/*
 * (C) Copyright 2009-2010
 * Nokia Siemens Networks, michael.lawnick.ext@nsn.com
 *
 * Portions Copyright (C) 2010 - 2016 Cavium, Inc.
 *
 * This file contains the shared part of the driver for the i2c adapter in
 * Cavium Networks' OCTEON processors and ThunderX SOCs.
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>

#include "i2c-cavium.h"

static int reset_how;

/*
 * On some hardware IFLG is not visible in TWSI_CTL until after low-level IRQ,
 * so re-sample CTL a short time later to avoid stalls.
 */
static int irq_early_us = 80;
module_param(irq_early_us, int, 0644);
MODULE_PARM_DESC(irq_early_us, "Re-poll for IFLG after IRQ (us)");

static void octeon_i2c_disable_hlc(struct octeon_i2c *i2c)
{
	if (!i2c->hlc_enabled)
		return;

	i2c->hlc_enabled = false;
	octeon_i2c_write_sw(i2c, SW_TWSI_EOP_TWSI_CTL, TWSI_CTL_ENAB);
}

/* interrupt service routine */
irqreturn_t octeon_i2c_isr(int irq, void *dev_id)
{
	struct octeon_i2c *i2c = dev_id;

	i2c->int_dis(i2c);
	wake_up(&i2c->queue);

	return IRQ_HANDLED;
}

/*
 * Wait-helper which addresses the delayed-IFLAG problem by re-polling for
 * missing TWSI_CTL[IFLG] a few us later, when irq has signalled an event,
 * but none found. Skip this re-poll on the first (non-wakeup) call.
 */
static int poll_iflg(struct octeon_i2c *i2c, int *first_p)
{
	int iflg = octeon_i2c_test_iflg(i2c);

	if (iflg)
		return 1;
	if (*first_p)
		*first_p = 0;
	else {
		usleep_range(irq_early_us, 2 * irq_early_us);
		iflg = octeon_i2c_test_iflg(i2c);
	}
	return iflg;
}

/**
 * octeon_i2c_wait - wait for the IFLG to be set
 * @i2c: The struct octeon_i2c
 *
 * Returns 0 on success, otherwise a negative errno.
 */
static int octeon_i2c_wait(struct octeon_i2c *i2c)
{
	long time_left;
	int first = 1;

	if (i2c->broken_irq_mode) {
		/*
		 * Some chip revisions seem to not assert the irq in
		 * the interrupt controller.  So we must poll for the
		 * IFLG change.
		 */
		u64 end = get_jiffies_64() + i2c->adap.timeout;

		while (!octeon_i2c_test_iflg(i2c) &&
		       time_before64(get_jiffies_64(), end))
			udelay(50);

		return octeon_i2c_test_iflg(i2c) ? 0 : -ETIMEDOUT;
	}

	i2c->int_en(i2c);
	time_left = wait_event_timeout(i2c->queue, poll_iflg(i2c, &first),
				       i2c->adap.timeout);
	i2c->int_dis(i2c);

	if (time_left <= 0 && i2c->broken_irq_check &&
	    octeon_i2c_test_iflg(i2c)) {
		dev_err(i2c->dev,
			"broken irq connection detected, switching to polling mode.\n");
		i2c->broken_irq_mode = true;
		return 0;
	}
	if (!time_left) {
		dev_dbg(i2c->dev, "%s: timeout\n", __func__);
		return -ETIMEDOUT;
	}

	return 0;
}

/*
 * Cleanup low-level state & enable high-level.
 *
 * Returns -EAGAIN if low-level state could not be cleaned
 */
static int octeon_i2c_enable_hlc(struct octeon_i2c *i2c)
{
	int try = 0, ret = 0;
	u64 val;

	if (i2c->hlc_enabled)
		return 0;
	i2c->hlc_enabled = true;

	while (1) {
		val = octeon_i2c_read_ctl(i2c) & (TWSI_CTL_STA | TWSI_CTL_STP);
		if (!val)
			break;

		/* clear IFLG event */
		if (val & TWSI_CTL_IFLG)
			octeon_i2c_write_sw(i2c, SW_TWSI_EOP_TWSI_CTL, TWSI_CTL_ENAB);

		if (try++ > 100) {
			pr_err("%s: giving up\n", __func__);
			ret = -EAGAIN;
			break;
		}

		/* spin until any start/stop has finished */
		udelay(10);
	}

	octeon_i2c_write_sw(i2c, SW_TWSI_EOP_TWSI_CTL,
			    TWSI_CTL_CE | TWSI_CTL_AAK | TWSI_CTL_ENAB);
	return ret;
}

static int octeon_i2c_lost_arb(u8 code, int final_read)
{
	switch (code) {
	/* Arbitration lost */
	case STAT_LOST_ARB_38:
	case STAT_LOST_ARB_68:
	case STAT_LOST_ARB_78:
	case STAT_LOST_ARB_B0:
		return -EAGAIN;

	/* Being addressed as slave, should back off & listen */
	case STAT_SLAVE_60:
	case STAT_SLAVE_70:
	case STAT_GENDATA_ACK:
	case STAT_GENDATA_NAK:
		return -EIO;

	/* Core busy as slave */
	case STAT_SLAVE_80:
	case STAT_SLAVE_88:
	case STAT_SLAVE_A0:
	case STAT_SLAVE_A8:
	case STAT_SLAVE_LOST:
	case STAT_SLAVE_NAK:
	case STAT_SLAVE_ACK:
		return -EIO;

	/* ACK allowed on pre-terminal bytes only */
	case STAT_RXDATA_ACK:
		if (!final_read)
			return 0;
		return -EAGAIN;

	/* NAK allowed on terminal byte only */
	case STAT_RXDATA_NAK:
		if (final_read)
			return 0;
		return -EAGAIN;
	case STAT_TXDATA_NAK:
	case STAT_TXADDR_NAK:
	case STAT_RXADDR_NAK:
	case STAT_AD2W_NAK:
		return -EAGAIN;
	}
	return 0;
}

static int check_arb(struct octeon_i2c *i2c, int final_read)
{
	return octeon_i2c_lost_arb(octeon_i2c_read_sw(i2c,
			SW_TWSI_EOP_TWSI_STAT),	final_read);
}

/* send STOP to the bus */
static void octeon_i2c_stop(struct octeon_i2c *i2c)
{
	octeon_i2c_write_sw(i2c, SW_TWSI_EOP_TWSI_CTL,
			    TWSI_CTL_ENAB | TWSI_CTL_STP);
}

int octeon_i2c_initlowlevel(struct octeon_i2c *i2c)
{
	u8 status = 0;
	int tries;

	/* reset controller */
	octeon_i2c_write_sw(i2c, SW_TWSI_EOP_TWSI_RST, 0);

	for (tries = 10; tries && status != STAT_IDLE; tries--) {
		udelay(1);
		status = octeon_i2c_read_sw(i2c, SW_TWSI_EOP_TWSI_STAT);
	}

	if (status != STAT_IDLE) {
		dev_err(i2c->dev, "%s: TWSI_RST failed! (0x%x)\n",
			__func__, status);
		return -EIO;
	}

	/* toggle twice to force both teardowns */
	octeon_i2c_enable_hlc(i2c);
	octeon_i2c_disable_hlc(i2c);
	return 0;
}

/**
 * octeon_i2c_unblock - unblock the bus
 * @i2c: The struct octeon_i2c
 *
 * If there was a reset while a device was driving 0 to bus, bus is blocked.
 * We toggle it free manually by some clock cycles and send a stop.
 */
static void octeon_i2c_unblock(struct octeon_i2c *i2c)
{
	int state, i;

	octeon_i2c_disable_hlc(i2c);
	dev_dbg(i2c->dev, "%s\n", __func__);

	/* cycle 8+1 clocks with SDA high */
	for (i = 0; i < 9; i++) {
		octeon_i2c_write_int(i2c, 0);
		udelay(5);
		state = __raw_readq(i2c->twsi_base + TWSI_INT);
		if (state & (TWSI_INT_SDA | TWSI_INT_SCL))
			break;
		octeon_i2c_write_int(i2c, TWSI_INT_SCL_OVR);
		udelay(5);
	}
	/* hand-crank a STOP */
	octeon_i2c_write_int(i2c, TWSI_INT_SDA_OVR | TWSI_INT_SCL_OVR);
	udelay(5);
	octeon_i2c_write_int(i2c, TWSI_INT_SDA_OVR);
	udelay(5);
	octeon_i2c_write_int(i2c, 0);
}

/*
 * TWSI state seems stuck. Not sure if it's TWSI-engine state or something
 * else on bus. The initial _stop() is always harmless, it just resets state
 * machine, does not _transmit_ STOP unless engine was active.
 */
static int start_unstick(struct octeon_i2c *i2c)
{
	octeon_i2c_stop(i2c);

	/*
	 * Response is escalated over successive calls,
	 * as EAGAIN provokes retries from i2c/core.
	 */
	switch (reset_how++ % 4) {
	case 0:
		/* just the stop above */
		break;
	case 1:
		/*
		 * Controller refused to send start flag. May be a
		 * client is holding SDA low? Let's try to free it.
		 */
		octeon_i2c_unblock(i2c);
		break;
	case 2:
		/* re-init our TWSI hardware */
		octeon_i2c_initlowlevel(i2c);
		break;
	default:
		/* retry in caller */
		reset_how = 0;
		return -EAGAIN;
	}
	return 0;
}

/**
 * octeon_i2c_start - send START to the bus
 * @i2c: The struct octeon_i2c
 * @first: first msg in combined operation?
 *
 * Returns 0 on success, otherwise a negative errno.
 */
static int octeon_i2c_start(struct octeon_i2c *i2c, int first)
{
	int result;
	u8 data;

	octeon_i2c_disable_hlc(i2c);

	while (1) {
		octeon_i2c_write_sw(i2c, SW_TWSI_EOP_TWSI_CTL,
				    TWSI_CTL_ENAB | TWSI_CTL_STA);

		result = octeon_i2c_wait(i2c);
		data = octeon_i2c_read_sw(i2c, SW_TWSI_EOP_TWSI_STAT);

		switch (data) {
		case STAT_START:
		case STAT_RSTART:
			if (!first)
				return -EAGAIN;
			reset_how = 0;
			return 0;
		case STAT_RXADDR_ACK:
			if (first)
				return -EAGAIN;
			return start_unstick(i2c);
		/*
		 * case STAT_IDLE:
		 * case STAT_ERROR:
		 */
		default:
			if (!first)
				return -EAGAIN;
			start_unstick(i2c);
		}
	}
	return 0;
}

/* calculate and set clock divisors */
void octeon_i2c_setclock(struct octeon_i2c *i2c)
{
	int tclk, thp_base, inc, thp_idx, mdiv_idx, ndiv_idx, foscl, diff;
	int thp = 0x18, mdiv = 2, ndiv = 0, delta_hz = 1000000;

	for (ndiv_idx = 0; ndiv_idx < 8 && delta_hz != 0; ndiv_idx++) {
		/*
		 * An mdiv value of less than 2 seems to not work well
		 * with ds1337 RTCs, so we constrain it to larger values.
		 */
		for (mdiv_idx = 15; mdiv_idx >= 2 && delta_hz != 0; mdiv_idx--) {
			/*
			 * For given ndiv and mdiv values check the
			 * two closest thp values.
			 */
			tclk = i2c->twsi_freq * (mdiv_idx + 1) * 10;
			tclk *= (1 << ndiv_idx);
			thp_base = (i2c->sys_freq / (tclk * 2)) - 1;

			for (inc = 0; inc <= 1; inc++) {
				thp_idx = thp_base + inc;
				if (thp_idx < 5 || thp_idx > 0xff)
					continue;

				foscl = i2c->sys_freq / (2 * (thp_idx + 1));
				foscl = foscl / (1 << ndiv_idx);
				foscl = foscl / (mdiv_idx + 1) / 10;
				diff = abs(foscl - i2c->twsi_freq);
				if (diff < delta_hz) {
					delta_hz = diff;
					thp = thp_idx;
					mdiv = mdiv_idx;
					ndiv = ndiv_idx;
				}
			}
		}
	}
	octeon_i2c_write_sw(i2c, SW_TWSI_OP_TWSI_CLK, thp);
	octeon_i2c_write_sw(i2c, SW_TWSI_EOP_TWSI_CLKCTL, (mdiv << 3) | ndiv);
}

static bool octeon_i2c_hlc_test_ready(struct octeon_i2c *i2c)
{
	u64 val = __raw_readq(i2c->twsi_base + SW_TWSI);

	return (val & SW_TWSI_V) == 0;
}

static void octeon_i2c_hlc_int_clear(struct octeon_i2c *i2c)
{
	/* clear ST/TS events, listen for neither */
	octeon_i2c_write_int(i2c, TWSI_INT_ST_INT | TWSI_INT_TS_INT);
}

/**
 * octeon_i2c_hlc_wait - wait for an HLC operation to complete
 * @i2c: The struct octeon_i2c
 *
 * Returns 0 on success, otherwise a negative errno.
 */
static int octeon_i2c_hlc_wait(struct octeon_i2c *i2c)
{
	int result;

	if (i2c->broken_irq_mode) {
		/*
		 * Some cn38xx boards did not assert the irq in
		 * the interrupt controller.  So we must poll for the
		 * IFLG change.
		 */
		u64 end = get_jiffies_64() + i2c->adap.timeout;

		while (!octeon_i2c_hlc_test_ready(i2c) &&
		       time_before64(get_jiffies_64(), end))
			udelay(50);

		return octeon_i2c_hlc_test_ready(i2c) ? 0 : -ETIMEDOUT;
	}

	i2c->hlc_int_en(i2c);
	result = wait_event_interruptible_timeout(i2c->queue,
						  octeon_i2c_hlc_test_ready(i2c),
						  i2c->adap.timeout);
	i2c->hlc_int_dis(i2c);
	if (!result)
		octeon_i2c_hlc_int_clear(i2c);
	if (result <= 0 && i2c->broken_irq_check &&
	    octeon_i2c_hlc_test_ready(i2c)) {
		dev_err(i2c->dev, "broken irq connection detected, switching to polling mode.\n");
			i2c->broken_irq_mode = true;
			return 0;
	}

	if (result < 0) {
		dev_dbg(i2c->dev, "%s: wait interrupted\n", __func__);
		return result;
	} else if (!result) {
		dev_dbg(i2c->dev, "%s: timeout\n", __func__);
		return -ETIMEDOUT;
	}
	return 0;
}

/**
 * octeon_i2c_read - receive data from the bus via low-level controller
 * @i2c: The struct octeon_i2c
 * @target: Target address
 * @data: Pointer to the location to store the data
 * @rlength: Length of the data
 * @phase: which phase of a combined operation.
 * @recv_len: flag for length byte
 *
 * The address is sent over the bus, then the data is read.
 *
 * Returns 0 on success, otherwise a negative errno.
 */
static int octeon_i2c_read(struct octeon_i2c *i2c, int target, u8 *data,
			   u16 *rlength, bool first, bool last, bool recv_len)
{
	u8 ctl = TWSI_CTL_ENAB | TWSI_CTL_AAK;
	int i, result, length = *rlength;
	u8 tmp;

	if (length < 1)
		return -EINVAL;

	result = octeon_i2c_start(i2c, first);
	if (result)
		return result;

	octeon_i2c_write_sw(i2c, SW_TWSI_EOP_TWSI_DATA, (target << 1) | 1);

	for (i = 0; i < length; ) {
		tmp = octeon_i2c_read_sw(i2c, SW_TWSI_EOP_TWSI_STAT);
		result = octeon_i2c_lost_arb(tmp, !(ctl & TWSI_CTL_AAK));
		if (result)
			return result;

		switch (tmp) {
		case STAT_RXDATA_ACK:
		case STAT_RXDATA_NAK:
			data[i++] = octeon_i2c_read_sw(i2c, SW_TWSI_EOP_TWSI_DATA);
		}


		/* NAK last recv'd byte, as a no-more-please */
		if (last && i == length - 1)
			ctl &= ~TWSI_CTL_AAK;

		/* clr iflg to allow next event */
		octeon_i2c_write_sw(i2c, SW_TWSI_EOP_TWSI_CTL, ctl);
		result = octeon_i2c_wait(i2c);
		if (result)
			return result;
		if (recv_len && i == 0) {
			if (data[i] > I2C_SMBUS_BLOCK_MAX + 1) {
				dev_err(i2c->dev,
					"%s: read len > I2C_SMBUS_BLOCK_MAX %d\n",
					__func__, data[i]);
				return -EIO;
			}
			length += data[i];
		}
	}
	*rlength = length;
	return 0;
}

/**
 * octeon_i2c_write - send data to the bus via low-level controller
 * @i2c: The struct octeon_i2c
 * @target: Target address
 * @data: Pointer to the data to be sent
 * @length: Length of the data
 * @last: is last msg in combined operation?
 *
 * The address is sent over the bus, then the data.
 *
 * Returns 0 on success, otherwise a negative errno.
 */
static int octeon_i2c_write(struct octeon_i2c *i2c, int target,
			    const u8 *data, int length, int first, int last)
{
	int i, result;

	result = octeon_i2c_start(i2c, first);
	if (result)
		return result;

	octeon_i2c_write_sw(i2c, SW_TWSI_EOP_TWSI_DATA, target << 1);
	octeon_i2c_write_sw(i2c, SW_TWSI_EOP_TWSI_CTL, TWSI_CTL_ENAB);

	result = octeon_i2c_wait(i2c);
	if (result)
		return result;

	for (i = 0; i < length; i++) {
		result = check_arb(i2c, false);
		if (result)
			return result;

		octeon_i2c_write_sw(i2c, SW_TWSI_EOP_TWSI_DATA, data[i]);
		octeon_i2c_write_sw(i2c, SW_TWSI_EOP_TWSI_CTL, TWSI_CTL_ENAB);

		result = octeon_i2c_wait(i2c);
		if (result)
			return result;
		result = check_arb(i2c, false);
		if (result)
			return result;
	}

	return 0;
}

/* high-level-controller pure read of up to 8 bytes */
static int octeon_i2c_simple_read(struct octeon_i2c *i2c, struct i2c_msg *msgs)
{
	int i, j, ret = 0;
	u64 cmd;

	octeon_i2c_enable_hlc(i2c);
	cmd = SW_TWSI_V | SW_TWSI_R | SW_TWSI_SOVR;
	/* SIZE */
	cmd |= (u64) (msgs[0].len - 1) << SW_TWSI_SIZE_SHIFT;
	/* A */
	cmd |= (u64) (msgs[0].addr & 0x7full) << SW_TWSI_A_SHIFT;

	if (msgs[0].flags & I2C_M_TEN)
		cmd |= SW_TWSI_OP_10;
	else
		cmd |= SW_TWSI_OP_7;

	octeon_i2c_hlc_int_clear(i2c);
	__raw_writeq(cmd, i2c->twsi_base + SW_TWSI);

	ret = octeon_i2c_hlc_wait(i2c);
	if (ret)
		goto err;

	cmd = __raw_readq(i2c->twsi_base + SW_TWSI);
	if ((cmd & SW_TWSI_R) == 0)
		return -EAGAIN;

	for (i = 0, j = msgs[0].len - 1; i  < msgs[0].len && i < 4; i++, j--)
		msgs[0].buf[j] = (cmd >> (8 * i)) & 0xff;

	if (msgs[0].len > 4) {
		cmd = __raw_readq(i2c->twsi_base + SW_TWSI_EXT);
		for (i = 0; i  < msgs[0].len - 4 && i < 4; i++, j--)
			msgs[0].buf[j] = (cmd >> (8 * i)) & 0xff;
	}

err:
	return ret;
}

/* high-level-controller pure write of up to 8 bytes */
static int octeon_i2c_simple_write(struct octeon_i2c *i2c, struct i2c_msg *msgs)
{
	int i, j, ret = 0;
	u64 cmd;

	octeon_i2c_enable_hlc(i2c);
	octeon_i2c_hlc_int_clear(i2c);

	ret = check_arb(i2c, false);
	if (ret)
		goto err;

	cmd = SW_TWSI_V | SW_TWSI_SOVR;
	/* SIZE */
	cmd |= (u64) (msgs[0].len - 1) << SW_TWSI_SIZE_SHIFT;
	/* A */
	cmd |= (u64) (msgs[0].addr & 0x7full) << SW_TWSI_A_SHIFT;

	if (msgs[0].flags & I2C_M_TEN)
		cmd |= SW_TWSI_OP_10;
	else
		cmd |= SW_TWSI_OP_7;

	for (i = 0, j = msgs[0].len - 1; i  < msgs[0].len && i < 4; i++, j--)
		cmd |= (u64)msgs[0].buf[j] << (8 * i);

	if (msgs[0].len > 4) {
		u64 ext = 0;

		for (i = 0; i < msgs[0].len - 4 && i < 4; i++, j--)
			ext |= (u64) msgs[0].buf[j] << (8 * i);
		writeqflush(ext, i2c->twsi_base + SW_TWSI_EXT);
	}

	writeqflush(cmd, i2c->twsi_base + SW_TWSI);

	ret = octeon_i2c_hlc_wait(i2c);
	if (ret)
		goto err;

	cmd = __raw_readq(i2c->twsi_base + SW_TWSI);
	if ((cmd & SW_TWSI_R) == 0)
		return -EAGAIN;

	ret = check_arb(i2c, false);

err:
	return ret;
}

/* high-level-controller composite write+read, msg0=addr, msg1=data */
static int octeon_i2c_ia_read(struct octeon_i2c *i2c, struct i2c_msg *msgs)
{
	int i, j, ret = 0;
	u64 cmd;

	octeon_i2c_enable_hlc(i2c);

	cmd = SW_TWSI_V | SW_TWSI_R | SW_TWSI_SOVR;
	/* SIZE */
	cmd |= (u64)(msgs[1].len - 1) << SW_TWSI_SIZE_SHIFT;
	/* A */
	cmd |= (u64)(msgs[0].addr & 0x7full) << SW_TWSI_A_SHIFT;

	if (msgs[0].flags & I2C_M_TEN)
		cmd |= SW_TWSI_OP_10_IA;
	else
		cmd |= SW_TWSI_OP_7_IA;

	if (msgs[0].len == 2) {
		u64 ext = 0;

		cmd |= SW_TWSI_EIA;
		ext = (u64) msgs[0].buf[0] << SW_TWSI_IA_SHIFT;
		cmd |= (u64) msgs[0].buf[1] << SW_TWSI_IA_SHIFT;
		__raw_writeq(ext, i2c->twsi_base + SW_TWSI_EXT);
	} else
		cmd |= (u64) msgs[0].buf[0] << SW_TWSI_IA_SHIFT;

	octeon_i2c_hlc_int_clear(i2c);
	writeqflush(cmd, i2c->twsi_base + SW_TWSI);

	ret = octeon_i2c_hlc_wait(i2c);
	if (ret)
		goto err;

	cmd = __raw_readq(i2c->twsi_base + SW_TWSI);
	if ((cmd & SW_TWSI_R) == 0)
		return -EAGAIN;

	for (i = 0, j = msgs[1].len - 1; i  < msgs[1].len && i < 4; i++, j--)
		msgs[1].buf[j] = (cmd >> (8 * i)) & 0xff;

	if (msgs[1].len > 4) {
		cmd = __raw_readq(i2c->twsi_base + SW_TWSI_EXT);
		for (i = 0; i  < msgs[1].len - 4 && i < 4; i++, j--)
			msgs[1].buf[j] = (cmd >> (8 * i)) & 0xff;
	}

err:
	return ret;
}

/* high-level-controller composite write+write, m[0]len<=2, m[1]len<=8 */
static int octeon_i2c_ia_write(struct octeon_i2c *i2c, struct i2c_msg *msgs)
{
	bool set_ext = false;
	int i, j, ret = 0;
	u64 cmd, ext = 0;

	octeon_i2c_enable_hlc(i2c);

	cmd = SW_TWSI_V | SW_TWSI_SOVR;
	/* SIZE */
	cmd |= (u64) (msgs[1].len - 1) << SW_TWSI_SIZE_SHIFT;
	/* A */
	cmd |= (u64) (msgs[0].addr & 0x7full) << SW_TWSI_A_SHIFT;

	if (msgs[0].flags & I2C_M_TEN)
		cmd |= SW_TWSI_OP_10_IA;
	else
		cmd |= SW_TWSI_OP_7_IA;

	if (msgs[0].len == 2) {
		cmd |= SW_TWSI_EIA;
		ext |= (u64) msgs[0].buf[0] << SW_TWSI_IA_SHIFT;
		set_ext = true;
		cmd |= (u64) msgs[0].buf[1] << SW_TWSI_IA_SHIFT;
	} else
		cmd |= (u64) msgs[0].buf[0] << SW_TWSI_IA_SHIFT;

	for (i = 0, j = msgs[1].len - 1; i  < msgs[1].len && i < 4; i++, j--)
		cmd |= (u64) msgs[1].buf[j] << (8 * i);

	if (msgs[1].len > 4) {
		for (i = 0; i < msgs[1].len - 4 && i < 4; i++, j--)
			ext |= (u64)msgs[1].buf[j] << (8 * i);
		set_ext = true;
	}
	if (set_ext)
		writeqflush(ext, i2c->twsi_base + SW_TWSI_EXT);

	octeon_i2c_hlc_int_clear(i2c);
	writeqflush(cmd, i2c->twsi_base + SW_TWSI);

	ret = octeon_i2c_hlc_wait(i2c);
	if (ret)
		goto err;

	cmd = octeon_i2c_read_sw64(i2c, SW_TWSI_EOP_TWSI_STAT);
	if ((cmd & SW_TWSI_R) == 0)
		return -EAGAIN;
	ret = octeon_i2c_lost_arb(cmd, false);

err:
	return ret;
}

/**
 * octeon_i2c_xfer - The driver's master_xfer function
 * @adap: Pointer to the i2c_adapter structure
 * @msgs: Pointer to the messages to be processed
 * @num: Length of the MSGS array
 *
 * Returns the number of messages processed, or a negative errno on failure.
 */
int octeon_i2c_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs,
			   int num)
{
	struct octeon_i2c *i2c = i2c_get_adapdata(adap);
	int i, ret = 0;

	if (num == 1) {
		if (msgs[0].len > 0 && msgs[0].len <= 8) {
			if (msgs[0].flags & I2C_M_RD)
				ret = octeon_i2c_simple_read(i2c, msgs);
			else
				ret = octeon_i2c_simple_write(i2c, msgs);
			goto out;
		}
	} else if (num == 2) {
		if ((msgs[0].flags & I2C_M_RD) == 0 &&
		    (msgs[1].flags & I2C_M_RECV_LEN) == 0 &&
		    msgs[0].len > 0 && msgs[0].len <= 2 &&
		    msgs[1].len > 0 && msgs[1].len <= 8 &&
		    msgs[0].addr == msgs[1].addr) {
			if (msgs[1].flags & I2C_M_RD)
				ret = octeon_i2c_ia_read(i2c, msgs);
			else
				ret = octeon_i2c_ia_write(i2c, msgs);
			goto out;
		}
	}

	for (i = 0; ret == 0 && i < num; i++) {
		struct i2c_msg *pmsg = &msgs[i];
		bool last = (i == (num - 1));

		dev_dbg(i2c->dev,
			"Doing %s %d byte(s) to/from 0x%02x - %d of %d messages\n",
			 pmsg->flags & I2C_M_RD ? "read" : "write",
			 pmsg->len, pmsg->addr, i + 1, num);
		if (pmsg->flags & I2C_M_RD)
			ret = octeon_i2c_read(i2c, pmsg->addr, pmsg->buf,
					      &pmsg->len, !i, last,
					      pmsg->flags & I2C_M_RECV_LEN);
		else
			ret = octeon_i2c_write(i2c, pmsg->addr, pmsg->buf,
					       pmsg->len, !i, last);
	}
	octeon_i2c_stop(i2c);

out:
	return ret ? -EAGAIN : num;
}
