/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2018-2020 Oplus. All rights reserved.
 *
 * TI BQ27541 fuel gauge minimal header for SM8250 5.10 GKI.
 */

#ifndef __OPLUS_BQ27541_H__
#define __OPLUS_BQ27541_H__

/* Upstream bq27xxx driver in drivers/power/supply/ covers BQ27541.
 * This header provides OPLUS-specific register offsets and data structs
 * used by the OPLUS gauge integration layer.
 */

#define BQ27541_REG_TEMP		0x06
#define BQ27541_REG_VOLT		0x08
#define BQ27541_REG_FLAGS		0x0A
#define BQ27541_REG_NOM_CAP		0x0C
#define BQ27541_REG_FULL_CAP		0x0E
#define BQ27541_REG_REM_CAP		0x10
#define BQ27541_REG_AVG_CUR		0x14
#define BQ27541_REG_SOH			0x20
#define BQ27541_REG_CC			0x24
#define BQ27541_REG_FCC			0x26
#define BQ27541_REG_RM			0x28

struct oplus_bq27541_chip {
	struct i2c_client	*client;
	int			voltage;
	int			batt_current;
	int			temperature;
	int			soc;
	int			cc;
	int			fcc;
	int			rm;
	int			soh;
};

#endif /* __OPLUS_BQ27541_H__ */
