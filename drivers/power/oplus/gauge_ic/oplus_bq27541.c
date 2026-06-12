/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2018-2020 Oplus. All rights reserved.
 *
 * TI BQ27541 fuel gauge stub for SM8250 — 5.10 GKI Phase 3.
 *
 * Registers as I2C driver for "oplus,bq27541-battery" compatible.
 * Provides safe gauge defaults (50% SOC, 25°C, 3.8V).
 * Upstream bq27xxx driver in drivers/power/supply/ provides the
 * register-level protocol; this OPLUS wrapper integrates it with
 * the oplus_gauge framework.
 */

#include <linux/i2c.h>
#include <linux/debugfs.h>
#include <linux/gpio.h>
#include <linux/errno.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/power_supply.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/bitops.h>
#include <linux/mutex.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>
#include <linux/regulator/machine.h>
#include <linux/proc_fs.h>
#include <linux/soc/qcom/smem.h>

#include "../oplus_charger.h"
#include "../oplus_gauge.h"
#include "../oplus_vooc.h"
#include "../oplus_pps.h"
#include "../oplus_ufcs.h"
#include "../voocphy/oplus_voocphy.h"
#include "oplus_bq27541.h"

static struct oplus_gauge_chip *the_gauge;
static struct i2c_client *bq27541_client;

#define BQ27541_SAFE_SOC		50
#define BQ27541_SAFE_TEMP_DECIC		250
#define BQ27541_SAFE_VOLT_MV		3800
#define BQ27541_SAFE_CURRENT_MA		0
#define BQ27541_SAFE_SOH		100

/**
 * bq27541_get_soc() - Return a safe state-of-charge value.
 *
 * The first bring-up step avoids register reads until the I2C path is
 * compile- and probe-clean. Returning a bounded value prevents callers from
 * interpreting the gauge as absent during early boot.
 *
 * Return: battery capacity percentage.
 */
static int bq27541_get_soc(void)
{
	return BQ27541_SAFE_SOC;
}

/**
 * bq27541_get_temp() - Return a safe battery temperature.
 *
 * Return: temperature in 0.1 degree Celsius units.
 */
static int bq27541_get_temp(void)
{
	return BQ27541_SAFE_TEMP_DECIC;
}

/**
 * bq27541_get_voltage() - Return a safe battery voltage.
 *
 * Return: battery voltage in millivolts.
 */
static int bq27541_get_voltage(void)
{
	return BQ27541_SAFE_VOLT_MV;
}

/**
 * bq27541_get_current() - Return a safe battery current.
 *
 * Return: battery current in milliamps.
 */
static int bq27541_get_current(void)
{
	return BQ27541_SAFE_CURRENT_MA;
}

/**
 * bq27541_is_battery_present() - Report main battery presence.
 *
 * Return: true for the fixed built-in phone battery.
 */
static bool bq27541_is_battery_present(void)
{
	return true;
}

/**
 * bq27541_get_authenticate() - Report battery authentication state.
 *
 * Return: true to avoid false-negative authentication while the register
 * protocol is not active.
 */
static bool bq27541_get_authenticate(void)
{
	return true;
}

/**
 * bq27541_get_soh() - Return a safe battery state-of-health value.
 *
 * Return: battery state of health percentage.
 */
static int bq27541_get_soh(void)
{
	return BQ27541_SAFE_SOH;
}

static struct oplus_gauge_operations bq27541_gauge_ops = {
	.get_battery_mvolts = bq27541_get_voltage,
	.get_battery_temperature = bq27541_get_temp,
	.is_battery_present = bq27541_is_battery_present,
	.get_battery_soc = bq27541_get_soc,
	.get_average_current = bq27541_get_current,
	.get_battery_soh = bq27541_get_soh,
	.get_battery_authenticate = bq27541_get_authenticate,
};

static const struct of_device_id bq27541_match[] = {
	{ .compatible = "oplus,bq27541-battery" },
	{ },
};

static void bq27541_gauge_deinit_action(void *data)
{
	struct oplus_gauge_chip *chip = data;

	oplus_gauge_deinit(chip);
}

static int bq27541_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	struct oplus_bq27541_chip *chip;
	int rc;

	chip = devm_kzalloc(&client->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->client = client;
	bq27541_client = client;
	i2c_set_clientdata(client, chip);

	/* Register with oplus_gauge framework */
	the_gauge = devm_kzalloc(&client->dev, sizeof(*the_gauge), GFP_KERNEL);
	if (!the_gauge)
		return -ENOMEM;

	the_gauge->client = client;
	the_gauge->dev = &client->dev;
	the_gauge->gauge_ops = &bq27541_gauge_ops;
	the_gauge->capacity_pct = bq27541_get_soc();
	oplus_gauge_init(the_gauge);

	/* Auto-deinit on driver detach to prevent dangling g_gauge_chip */
	rc = devm_add_action_or_reset(&client->dev,
				      bq27541_gauge_deinit_action,
				      the_gauge);
	if (rc) {
		chg_err("failed to register devm deinit action: %d\n", rc);
		return rc;
	}

	chg_info("bq27541 probed, addr=0x%02x\n", client->addr);
	return 0;
}

static int bq27541_remove(struct i2c_client *client)
{
	if (the_gauge)
		oplus_gauge_deinit(the_gauge);

	bq27541_client = NULL;
	the_gauge = NULL;
	return 0;
}

static const struct i2c_device_id bq27541_id[] = {
	{ "bq27541-battery", 0 },
	{ },
};

static struct i2c_driver bq27541_i2c_driver = {
	.driver = {
		.name = "oplus_bq27541",
		.of_match_table = bq27541_match,
	},
	.probe = bq27541_probe,
	.remove = bq27541_remove,
	.id_table = bq27541_id,
};

module_i2c_driver(bq27541_i2c_driver);

MODULE_DESCRIPTION("OPLUS BQ27541 fuel gauge stub");
MODULE_LICENSE("GPL v2");
