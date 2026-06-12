/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2018-2020 Oplus. All rights reserved.
 *
 * Dialog DA9313 charge-pump / divider driver for SM8250 (QCOM path).
 * Ported from 4.19 for 5.10 GKI — MTK conditionals stripped.
 */

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
#include "oplus_da9313.h"

static struct chip_da9313 *the_chip;
static DEFINE_MUTEX(da9313_i2c_access);

static int __da9313_read_reg(int reg, int *returnData)
{
	int ret = 0;
	int retry = 3;
	struct chip_da9313 *chip = the_chip;

	ret = i2c_smbus_read_byte_data(chip->client, reg);
	if (ret < 0) {
		while (retry > 0) {
			msleep(10);
			ret = i2c_smbus_read_byte_data(chip->client, reg);
			if (ret >= 0)
				break;
			retry--;
		}
	}

	if (ret < 0) {
		chg_err("i2c read fail: reg=0x%02x ret=%d\n", reg, ret);
		return ret;
	}

	*returnData = ret;
	return 0;
}

static int da9313_read_reg(int reg, int *returnData)
{
	int ret = 0;

	mutex_lock(&da9313_i2c_access);
	ret = __da9313_read_reg(reg, returnData);
	mutex_unlock(&da9313_i2c_access);

	return ret;
}

static int __da9313_write_reg(int reg, int val)
{
	int ret = 0;
	int retry = 3;
	struct chip_da9313 *chip = the_chip;

	ret = i2c_smbus_write_byte_data(chip->client, reg, val);
	if (ret < 0) {
		while (retry > 0) {
			msleep(10);
			ret = i2c_smbus_write_byte_data(chip->client, reg, val);
			if (ret >= 0)
				break;
			retry--;
		}
	}

	if (ret < 0)
		chg_err("i2c write fail: reg=0x%02x val=0x%02x ret=%d\n",
			reg, val, ret);

	return ret;
}

static int da9313_write_reg(int reg, int val)
{
	int ret = 0;

	mutex_lock(&da9313_i2c_access);
	ret = __da9313_write_reg(reg, val);
	mutex_unlock(&da9313_i2c_access);

	return ret;
}

static int da9313_hardware_init(void)
{
	int ret = 0;
	int data = 0;

	/* Auto PVC mode with 40mV drop, 20mV hysteresis */
	ret = da9313_read_reg(REG04_DA9313_ADDRESS, &data);
	if (ret < 0)
		return ret;

	switch (the_chip->hwid) {
	case HWID_SD77313:
		data &= ~REG04_SD77313_PVC_MODE_MASK;
		data |= REG04_SD77313_PVC_MODE_AUTO;
		break;
	case HWID_DA9313:
		data &= ~REG04_DA9313_PVC_MODE_MASK;
		data |= REG04_DA9313_PVC_MODE_AUTO;
		break;
	case HWID_MAX77932:
		data &= ~REG04_MAX77932_PVC_MODE_MASK;
		data |= REG04_MAX77932_PVC_MODE_AUTO;
		break;
	case HWID_MAX77938:
		data &= ~REG04_MAX77938_PVC_MODE_MASK;
		data |= REG04_MAX77938_PVC_MODE_AUTO;
		break;
	default:
		data &= ~REG04_DA9313_PVC_MODE_MASK;
		data |= REG04_DA9313_PVC_MODE_AUTO;
		break;
	}

	ret = da9313_write_reg(REG04_DA9313_ADDRESS, data);
	if (ret < 0)
		return ret;

	/* PVC drop = 40mV, hysteresis = 20mV */
	ret = da9313_read_reg(REG0E_DA9313_ADDRESS, &data);
	if (ret < 0)
		return ret;

	data &= ~REG0E_DA9313_PVC_DROP_MASK;
	data |= REG0E_DA9313_PVC_DROP_40MV;
	data &= ~REG0E_DA9313_PVC_HYST_MASK;
	data |= REG0E_DA9313_PVC_HYST_20MV;
	data &= ~REG0E_DA9313_PVC_MS_DROP_MASK;
	data |= REG0E_DA9313_PVC_MS_DROP_30MV;
	data &= ~REG0E_DA9313_PVC_MS_HYST_MASK;
	data |= REG0E_DA9313_PVC_MS_HYST_30MV;

	ret = da9313_write_reg(REG0E_DA9313_ADDRESS, data);
	if (ret < 0)
		return ret;

	chg_info("da9313 hardware init done, hwid=0x%02x\n", the_chip->hwid);
	return 0;
}

static int da9313_get_hwid(void)
{
	int ret;
	int data;
	struct chip_da9313 *chip = the_chip;

	ret = gpio_request(chip->da9313_hwid_gpio, "da9313_hwid");
	if (ret) {
		chg_err("failed to request hwid gpio\n");
		return HWID_DA9313;
	}

	data = gpio_get_value(chip->da9313_hwid_gpio);
	gpio_free(chip->da9313_hwid_gpio);

	return data;
}

static int da9313_resume(struct device *dev)
{
	if (!the_chip)
		return 0;

	return 0;
}

static int da9313_suspend(struct device *dev)
{
	if (!the_chip)
		return 0;

	return 0;
}

static const struct of_device_id da9313_match_table[] = {
	{ .compatible = "oplus,da9313-divider" },
	{ },
};

static int da9313_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct chip_da9313 *chip;
	int ret;

	chip = devm_kzalloc(&client->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->client = client;
	chip->dev = &client->dev;
	i2c_set_clientdata(client, chip);
	the_chip = chip;

	ret = da9313_get_hwid();
	if (ret >= 0)
		chip->hwid = ret;
	else
		chip->hwid = HWID_DA9313;

	chg_info("da9313 probed, hwid=0x%02x, addr=0x%02x\n",
		 chip->hwid, client->addr);

	ret = da9313_hardware_init();
	if (ret < 0)
		chg_err("da9313 hardware init failed: %d\n", ret);

	return 0;
}

static int da9313_remove(struct i2c_client *client)
{
	the_chip = NULL;
	return 0;
}

static const struct i2c_device_id da9313_id[] = {
	{ "da9313-divider", 0 },
	{ },
};

static struct i2c_driver da9313_driver = {
	.driver = {
		.name = "oplus_da9313",
		.of_match_table = da9313_match_table,
	},
	.probe = da9313_probe,
	.remove = da9313_remove,
	.id_table = da9313_id,
};

module_i2c_driver(da9313_driver);

MODULE_DESCRIPTION("OPLUS DA9313 charge pump driver");
MODULE_LICENSE("GPL v2");
