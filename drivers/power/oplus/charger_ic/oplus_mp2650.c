/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2018-2020 Oplus. All rights reserved.
 *
 * MPS MP2650 charger IC stub for SM8250 — 5.10 GKI Phase 3.
 *
 * Registers `struct oplus_chg_operations` with oplus_chg_ops_manager
 * as "mp2650-charger".  All hardware ops return safe defaults.
 * Full driver (~3680L) deferred to Phase 4+.
 */

#include <linux/interrupt.h>
#include <linux/i2c.h>
#include <linux/debugfs.h>
#include <linux/gpio.h>
#include <linux/errno.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/power_supply.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/bitops.h>
#include <linux/mutex.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>
#include <linux/regulator/machine.h>
#include <linux/rtc.h>
#include <linux/proc_fs.h>
#include <soc/oplus/device_info.h>

#include "../oplus_vooc.h"
#include "../oplus_gauge.h"
#include "oplus_mp2650.h"
#include "../oplus_charger.h"
#include "../oplus_chg_ops_manager.h"
#include "../oplus_pps.h"
#include "../oplus_battery_log.h"

static struct oplus_chg_operations mp2650_ops;
static struct i2c_client *mp2650_client;

static const struct of_device_id mp2650_match[] = {
	{ .compatible = "oplus,mp2650-charger" },
	{ },
};

static int mp2650_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	int ret;

	mp2650_client = client;

	chg_info("mp2650 probed, addr=0x%02x\n", client->addr);

	ret = oplus_chg_ops_register("mp2650-charger", &mp2650_ops);
	if (ret)
		chg_err("failed to register mp2650 ops: %d\n", ret);

	return ret;
}

static int mp2650_remove(struct i2c_client *client)
{
	mp2650_client = NULL;
	return 0;
}

static const struct i2c_device_id mp2650_id[] = {
	{ "mp2650-charger", 0 },
	{ },
};

static struct i2c_driver mp2650_i2c_driver = {
	.driver = {
		.name = "oplus_mp2650",
		.of_match_table = mp2650_match,
	},
	.probe = mp2650_probe,
	.remove = mp2650_remove,
	.id_table = mp2650_id,
};

module_i2c_driver(mp2650_i2c_driver);

MODULE_DESCRIPTION("OPLUS MP2650 charger stub");
MODULE_LICENSE("GPL v2");
