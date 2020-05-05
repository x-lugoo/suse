/*
 * Copyright (C) 2013 - 2015 Linaro Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/init.h>
#include <linux/efi.h>
#include <linux/libfdt.h>

#define UEFI_PARAM(name, prop, field)			   \
	{						   \
		{ name },				   \
		{ prop },				   \
		offsetof(struct efi_fdt_params, field),    \
		FIELD_SIZEOF(struct efi_fdt_params, field) \
	}

static __initdata struct {
	const char name[32];
	const char propname[32];
	int offset;
	int size;
} dt_params[] = {
	UEFI_PARAM("System Table", "linux,uefi-system-table", system_table),
	UEFI_PARAM("MemMap Address", "linux,uefi-mmap-start", mmap),
	UEFI_PARAM("MemMap Size", "linux,uefi-mmap-size", mmap_size),
	UEFI_PARAM("MemMap Desc. Size", "linux,uefi-mmap-desc-size", desc_size),
	UEFI_PARAM("MemMap Desc. Version", "linux,uefi-mmap-desc-ver", desc_ver)
};

bool __init efi_get_fdt_params(void *fdt, struct efi_fdt_params *params)
{
	const void *prop;
	int node, i;

	pr_info("Getting EFI parameters from FDT:\n");

	node = fdt_path_offset(fdt, "/chosen");
	if (node < 0) {
		pr_err("/chosen node not found!\n");
		return false;
	}

	prop = fdt_getprop(fdt, node, "bootargs", NULL);
	params->verbose = prop && strstr(prop, "uefi_debug");

	for (i = 0; i < ARRAY_SIZE(dt_params); i++) {
		void *dest;
		int len;
		u64 val;

		prop = fdt_getprop(fdt, node, dt_params[i].propname, &len);
		if (!prop)
			goto not_found;
		dest = (void *)params + dt_params[i].offset;

		if (dt_params[i].size == sizeof(u32))
			val = *(u32 *)dest = be32_to_cpup(prop);
		else
			val = *(u64 *)dest = be64_to_cpup(prop);

		if (params->verbose)
			pr_info("  %s: 0x%0*llx\n", dt_params[i].name,
				dt_params[i].size * 2, val);
	}
	return true;

not_found:
	if (i == 0)
		pr_info("UEFI not found.\n");
	else
		pr_err("Can't find '%s' in device tree!\n", dt_params[i].name);
	return false;
}
