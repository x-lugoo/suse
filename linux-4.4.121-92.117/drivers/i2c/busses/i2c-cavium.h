#include <linux/atomic.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/i2c-smbus.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/pci.h>

/* Register offsets */
#if IS_ENABLED(CONFIG_I2C_THUNDERX)
	#define SW_TWSI			0x1000
	#define TWSI_INT		0x1010
	#define SW_TWSI_EXT		0x1018
#else
	#define SW_TWSI			0x00
	#define TWSI_INT		0x10
	#define SW_TWSI_EXT		0x18
#endif

/* Controller command patterns */
#define SW_TWSI_V		BIT_ULL(63)
#define SW_TWSI_EIA		BIT_ULL(61)
#define SW_TWSI_OP_TWSI_CLK	BIT_ULL(59)
#define SW_TWSI_OP_7		(0ULL << 57)
#define SW_TWSI_OP_7_IA		BIT_ULL(57)
#define SW_TWSI_OP_10		(2ULL << 57)
#define SW_TWSI_OP_10_IA	(3ULL << 57)
#define SW_TWSI_R		BIT_ULL(56)
#define SW_TWSI_SOVR		BIT_ULL(55)
#define SW_TWSI_SIZE_SHIFT	52
#define SW_TWSI_A_SHIFT		40
#define SW_TWSI_IA_SHIFT	32
#define SW_TWSI_EOP_TWSI_DATA	0x0C00000100000000ULL
#define SW_TWSI_EOP_TWSI_CTL	0x0C00000200000000ULL
#define SW_TWSI_EOP_TWSI_CLKCTL	0x0C00000300000000ULL
#define SW_TWSI_EOP_TWSI_STAT	0x0C00000300000000ULL
#define SW_TWSI_EOP_TWSI_RST	0x0C00000700000000ULL

/* Controller command and status bits */
#define TWSI_CTL_CE		0x80	/* High level controller enable */
#define TWSI_CTL_ENAB		0x40	/* Bus enable */
#define TWSI_CTL_STA		0x20	/* Master-mode start, HW clears when done */
#define TWSI_CTL_STP		0x10	/* Master-mode stop, HW clears when done */
#define TWSI_CTL_IFLG		0x08	/* HW event, SW writes 0 to ACK */
#define TWSI_CTL_AAK		0x04	/* Assert ACK */

/* Some status values */
#define STAT_ERROR		0x00
#define STAT_START		0x08
#define STAT_RSTART		0x10
#define STAT_TXADDR_ACK		0x18
#define STAT_TXADDR_NAK		0x20
#define STAT_TXDATA_ACK		0x28
#define STAT_TXDATA_NAK		0x30
#define STAT_LOST_ARB_38	0x38
#define STAT_RXADDR_ACK		0x40
#define STAT_RXADDR_NAK		0x48
#define STAT_RXDATA_ACK		0x50
#define STAT_RXDATA_NAK		0x58
#define STAT_SLAVE_60		0x60
#define STAT_LOST_ARB_68	0x68
#define STAT_SLAVE_70		0x70
#define STAT_LOST_ARB_78	0x78
#define STAT_SLAVE_80		0x80
#define STAT_SLAVE_88		0x88
#define STAT_GENDATA_ACK	0x90
#define STAT_GENDATA_NAK	0x98
#define STAT_SLAVE_A0		0xA0
#define STAT_SLAVE_A8		0xA8
#define STAT_LOST_ARB_B0	0xB0
#define STAT_SLAVE_LOST		0xB8
#define STAT_SLAVE_NAK		0xC0
#define STAT_SLAVE_ACK		0xC8
#define STAT_AD2W_ACK		0xD0
#define STAT_AD2W_NAK		0xD8
#define STAT_IDLE		0xF8

/* TWSI_INT values */
#define TWSI_INT_ST_INT		BIT_ULL(0)
#define TWSI_INT_TS_INT		BIT_ULL(1)
#define TWSI_INT_CORE_INT	BIT_ULL(2)
#define TWSI_INT_ST_EN		BIT_ULL(4)
#define TWSI_INT_TS_EN		BIT_ULL(5)
#define TWSI_INT_CORE_EN	BIT_ULL(6)
#define TWSI_INT_SDA_OVR	BIT_ULL(8)
#define TWSI_INT_SCL_OVR	BIT_ULL(9)
#define TWSI_INT_SDA		BIT_ULL(10)
#define TWSI_INT_SCL		BIT_ULL(11)

struct octeon_i2c {
	wait_queue_head_t queue;
	struct i2c_adapter adap;
	struct clk *clk;
	int irq;
	int hlc_irq;		/* For cn7890 only */
	u32 twsi_freq;
	int sys_freq;
	void __iomem *twsi_base;
	struct device *dev;
	bool hlc_enabled;
	bool broken_irq_mode;
	bool broken_irq_check;
	void (*int_en)(struct octeon_i2c *);
	void (*int_dis)(struct octeon_i2c *);
	void (*hlc_int_en)(struct octeon_i2c *);
	void (*hlc_int_dis)(struct octeon_i2c *);
	atomic_t int_en_cnt;
	atomic_t hlc_int_en_cnt;
#if IS_ENABLED(CONFIG_I2C_THUNDERX)
	struct msix_entry i2c_msix;
#endif
#if IS_ENABLED(CONFIG_I2C_SMBUS)
	struct i2c_smbus_alert_setup alert_data;
	struct i2c_client *ara;
#endif
};

static inline void writeqflush(u64 val, void __iomem *addr)
{
	__raw_writeq(val, addr);
	__raw_readq(addr);	/* wait for write to land */
}

/**
 * octeon_i2c_write_sw - write an I2C core register
 * @i2c: The struct octeon_i2c
 * @eop_reg: Register selector
 * @data: Value to be written
 *
 * The I2C core registers are accessed indirectly via the SW_TWSI CSR.
 */
static inline void octeon_i2c_write_sw(struct octeon_i2c *i2c, u64 eop_reg, u32 data)
{
	u64 tmp;

	__raw_writeq(SW_TWSI_V | eop_reg | data, i2c->twsi_base + SW_TWSI);
	do {
		tmp = __raw_readq(i2c->twsi_base + SW_TWSI);
	} while ((tmp & SW_TWSI_V) != 0);
}

/**
 * octeon_i2c_read_sw64 - read an I2C core register
 * @i2c: The struct octeon_i2c
 * @eop_reg: Register selector
 *
 * Returns the data.
 *
 * The I2C core registers are accessed indirectly via the SW_TWSI CSR.
 */
static inline u64 octeon_i2c_read_sw64(struct octeon_i2c *i2c, u64 eop_reg)
{
	u64 tmp;

	__raw_writeq(SW_TWSI_V | eop_reg | SW_TWSI_R, i2c->twsi_base + SW_TWSI);
	do {
		tmp = __raw_readq(i2c->twsi_base + SW_TWSI);
	} while ((tmp & SW_TWSI_V) != 0);

	return tmp;
}

/**
 * octeon_i2c_read_sw - read lower bits of an I2C core register
 * @i2c: The struct octeon_i2c
 * @eop_reg: Register selector
 *
 * Returns the data.
 *
 * The I2C core registers are accessed indirectly via the SW_TWSI CSR.
 */
static inline u8 octeon_i2c_read_sw(struct octeon_i2c *i2c, u64 eop_reg)
{
	return octeon_i2c_read_sw64(i2c, eop_reg) & 0xFF;
}

/**
 * octeon_i2c_write_int - write the TWSI_INT register
 * @i2c: The struct octeon_i2c
 * @data: Value to be written
 */
static inline void octeon_i2c_write_int(struct octeon_i2c *i2c, u64 data)
{
	writeqflush(data, i2c->twsi_base + TWSI_INT);
}

static inline u64 octeon_i2c_read_ctl(struct octeon_i2c *i2c)
{
	return octeon_i2c_read_sw64(i2c, SW_TWSI_EOP_TWSI_CTL);
}

static inline int octeon_i2c_test_iflg(struct octeon_i2c *i2c)
{
	return (octeon_i2c_read_ctl(i2c) & TWSI_CTL_IFLG) != 0;
}

/* Prototypes */
int octeon_i2c_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num);
irqreturn_t octeon_i2c_isr(int irq, void *dev_id);
int octeon_i2c_initlowlevel(struct octeon_i2c *i2c);
void octeon_i2c_setclock(struct octeon_i2c *i2c);
