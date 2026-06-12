// SPDX-License-Identifier: GPL-2.0-only
/*
 * Minimal OPLUS charger state machine for 5.10 Layer 3A.
 *
 * This file does not port the full 4.19 implementation.  It only provides
 * the smallest wired-charging runtime path: DT defaults, usb/battery psy
 * binding, gauge refresh, basic charger-online policy, and periodic update.
 */

#include <linux/module.h>
#include <linux/atomic.h>
#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/power_supply.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <linux/delay.h>

#include "oplus_charger.h"
#include "oplus_gauge.h"

static struct oplus_chg_chip *g_charger_chip;
static DEFINE_MUTEX(g_chg_lock);

static struct oplus_chg_chip *oplus_chg_try_get_chip(void)
{
	struct oplus_chg_chip *chip;

	mutex_lock(&g_chg_lock);
	chip = g_charger_chip;
	if (!chip || !chip->initialized) {
		chip = NULL;
	} else {
		atomic_inc(&chip->api_users);
	}
	mutex_unlock(&g_chg_lock);

	return chip;
}

static void oplus_chg_put_chip(struct oplus_chg_chip *chip)
{
	if (atomic_dec_and_test(&chip->api_users))
		wake_up_all(&chip->api_wq);
}

#define OPLUS_CHG_UPDATE_INTERVAL_MS	5000
#define OPLUS_CHG_DEFAULT_INPUT_MA	500
#define OPLUS_CHG_DEFAULT_FASTCHG_MA	1000
#define OPLUS_CHG_DEFAULT_FLOAT_MV	4400
#define OPLUS_CHG_TEMP_COOL_DECIDEGC	50
#define OPLUS_CHG_TEMP_WARM_DECIDEGC	450

/* Layer 3B: conservative temperature-band and safety defaults */
#define OPLUS_CHG_REMOVED_BAT_DECIDEGC	(-200)
#define OPLUS_CHG_COLD_BAT_DECIDEGC	(-30)
#define OPLUS_CHG_LITTLE_COLD_DECIDEGC	0
#define OPLUS_CHG_COOL_BAT_DECIDEGC	50
#define OPLUS_CHG_LITTLE_COOL_DECIDEGC	120
#define OPLUS_CHG_NORMAL_BAT_DECIDEGC	160
#define OPLUS_CHG_WARM_BAT_DECIDEGC	450
#define OPLUS_CHG_HOT_BAT_DECIDEGC	530
#define OPLUS_CHG_VBATT_HV_THR_MV	4450
#define OPLUS_CHG_VBATT_FULL_THR_MV	4350
#define OPLUS_CHG_RECHARGE_MV	100
#define OPLUS_CHG_ITERM_MA	150
#define OPLUS_CHG_MAX_CHG_TIME_SEC	(8 * 60 * 60)

/**
 * oplus_chg_ops_ready() - Check whether charger ops are minimally complete.
 * @chip: OPLUS charging chip context.
 *
 * Layer 3B tightened: requires all P0 callbacks needed by the normal-wired
 * state machine (enable, disable, input current, float voltage, fast-charge
 * current, charger detect, charger type, charger voltage, charge-enable read).
 *
 * Returns true when every required callback is non-NULL.
 */
static bool oplus_chg_ops_ready(struct oplus_chg_chip *chip)
{
	if (!chip || !chip->chg_ops)
		return false;

	if (!chip->chg_ops->charging_enable ||
	    !chip->chg_ops->charging_disable ||
	    !chip->chg_ops->input_current_write ||
	    !chip->chg_ops->float_voltage_write ||
	    !chip->chg_ops->charging_current_write_fast ||
	    !chip->chg_ops->check_chrdet_status ||
	    !chip->chg_ops->get_charger_type ||
	    !chip->chg_ops->get_charger_volt ||
	    !chip->chg_ops->get_charging_enable)
		return false;

	return true;
}

/**
 * oplus_chg_apply_default_limits() - Set conservative charging defaults.
 * @chip: OPLUS charging chip context.
 */
static void oplus_chg_apply_default_limits(struct oplus_chg_chip *chip)
{
	chip->input_current_limit_ma = OPLUS_CHG_DEFAULT_INPUT_MA;
	chip->fastchg_current_ma = OPLUS_CHG_DEFAULT_FASTCHG_MA;
	chip->float_voltage_mv = OPLUS_CHG_DEFAULT_FLOAT_MV;
}

/**
 * oplus_chg_limits_init() - Populate conservative normal-wired limits.
 * @chip: OPLUS charging chip context.
 */
static void oplus_chg_limits_init(struct oplus_chg_chip *chip)
{
	chip->limits.removed_bat_decidegc = OPLUS_CHG_REMOVED_BAT_DECIDEGC;
	chip->limits.cold_bat_decidegc = OPLUS_CHG_COLD_BAT_DECIDEGC;
	chip->limits.little_cold_bat_decidegc = OPLUS_CHG_LITTLE_COLD_DECIDEGC;
	chip->limits.cool_bat_decidegc = OPLUS_CHG_COOL_BAT_DECIDEGC;
	chip->limits.little_cool_bat_decidegc = OPLUS_CHG_LITTLE_COOL_DECIDEGC;
	chip->limits.normal_bat_decidegc = OPLUS_CHG_NORMAL_BAT_DECIDEGC;
	chip->limits.warm_bat_decidegc = OPLUS_CHG_WARM_BAT_DECIDEGC;
	chip->limits.hot_bat_decidegc = OPLUS_CHG_HOT_BAT_DECIDEGC;
	chip->limits.temp_cold_vfloat_mv = OPLUS_CHG_DEFAULT_FLOAT_MV;
	chip->limits.temp_little_cold_vfloat_mv = OPLUS_CHG_DEFAULT_FLOAT_MV;
	chip->limits.temp_cool_vfloat_mv = OPLUS_CHG_DEFAULT_FLOAT_MV;
	chip->limits.temp_little_cool_vfloat_mv = OPLUS_CHG_DEFAULT_FLOAT_MV;
	chip->limits.temp_normal_vfloat_mv = OPLUS_CHG_DEFAULT_FLOAT_MV;
	chip->limits.temp_warm_vfloat_mv = OPLUS_CHG_DEFAULT_FLOAT_MV;
	chip->limits.temp_cold_fastchg_current_ma = OPLUS_CHG_DEFAULT_INPUT_MA;
	chip->limits.temp_little_cold_fastchg_current_ma = OPLUS_CHG_DEFAULT_INPUT_MA;
	chip->limits.temp_cool_fastchg_current_ma_low = OPLUS_CHG_DEFAULT_INPUT_MA;
	chip->limits.temp_little_cool_fastchg_current_ma = OPLUS_CHG_DEFAULT_FASTCHG_MA;
	chip->limits.temp_normal_fastchg_current_ma = OPLUS_CHG_DEFAULT_FASTCHG_MA;
	chip->limits.temp_warm_fastchg_current_ma = OPLUS_CHG_DEFAULT_INPUT_MA;
	chip->limits.input_current_charger_ma = OPLUS_CHG_DEFAULT_INPUT_MA;
	chip->limits.vbatt_hv_thr = OPLUS_CHG_VBATT_HV_THR_MV;
	chip->limits.vbatt_full_thr = OPLUS_CHG_VBATT_FULL_THR_MV;
	chip->limits.recharge_mv = OPLUS_CHG_RECHARGE_MV;
	chip->limits.iterm_ma = OPLUS_CHG_ITERM_MA;
	chip->limits.max_chg_time_sec = OPLUS_CHG_MAX_CHG_TIME_SEC;
}

/**
 * oplus_chg_state_init() - Initialize Layer 3B charging state.
 * @chip: OPLUS charging chip context.
 */
static void oplus_chg_state_init(struct oplus_chg_chip *chip)
{
	oplus_chg_limits_init(chip);
	chip->tbatt_status = BATTERY_STATUS__NORMAL;
	chip->charging_state = CHARGING_STATUS_CCCV;
	chip->charger_exist = false;
	chip->batt_exist = true;
	chip->batt_full = false;
	chip->real_batt_full = false;
	chip->sw_full = false;
	chip->hw_full = false;
	chip->vbatt_over = false;
	chip->chging_over_time = false;
	chip->charging_enabled = false;
	chip->in_rechging = false;
	chip->batt_volt = 0;
	chip->temperature = 0;
	chip->icharging = 0;
	chip->batt_volt_min = 0;
	chip->charger_volt = 0;
	chip->charger_current_ma = 0;
	chip->total_time = 0;
	chip->recharge_count = 0;
}

/**
 * oplus_chg_parse_svooc_dt() - Parse VOOC/SVOOC DT properties (Layer 3A no-op).
 * @chip: OPLUS charging chip context.
 *
 * Layer 3A does not implement VOOC.  This function validates the chip
 * pointer and returns success so the init path remains stable.
 *
 * Return: 0 on success, -EINVAL if chip or dev is NULL.
 */
int oplus_chg_parse_svooc_dt(struct oplus_chg_chip *chip)
{
	if (!chip || !chip->dev)
		return -EINVAL;

	return 0;
}
EXPORT_SYMBOL_GPL(oplus_chg_parse_svooc_dt);

/**
 * oplus_chg_parse_charger_dt() - Parse charger DT properties and apply defaults.
 * @chip: OPLUS charging chip context.
 *
 * Layer 3A applies frozen conservative defaults.  DT-based overrides are
 * deferred to Layer 3B.
 *
 * Return: 0 on success, -EINVAL if chip or dev is NULL.
 */
int oplus_chg_parse_charger_dt(struct oplus_chg_chip *chip)
{
	if (!chip || !chip->dev)
		return -EINVAL;

	chip->update_interval_ms = OPLUS_CHG_UPDATE_INTERVAL_MS;
	oplus_chg_apply_default_limits(chip);

	return 0;
}
EXPORT_SYMBOL_GPL(oplus_chg_parse_charger_dt);

/**
 * oplus_chg_get_battery_data() - Read all gauge data into chip fields.
 * @chip: OPLUS charging chip context.
 *
 * Layer 3B expanded refresh: reads SOC, voltage, temperature, current,
 * authentication, and SOH from the Layer 1 gauge provider.
 */
static void oplus_chg_get_battery_data(struct oplus_chg_chip *chip)
{
	chip->batt_soc = oplus_gauge_get_batt_soc();
	chip->batt_mv = oplus_gauge_get_batt_mvolts();
	chip->batt_volt = chip->batt_mv;
	chip->batt_volt_min = chip->batt_mv;
	chip->batt_temp = oplus_gauge_get_batt_temperature();
	chip->temperature = chip->batt_temp;
	chip->batt_current_ma = oplus_gauge_get_batt_current();
	chip->icharging = chip->batt_current_ma;
	chip->batt_auth = oplus_gauge_get_batt_authenticate();
	chip->batt_soh = oplus_gauge_get_batt_soh();
}

/**
 * oplus_chg_bind_power_supplies() - Bind usb and battery power supplies.
 * @chip: OPLUS charging chip context.
 *
 * Return: 0 when both psy are bound, -EPROBE_DEFER if either is missing.
 */
static int oplus_chg_bind_power_supplies(struct oplus_chg_chip *chip)
{
	if (!chip->usb_psy)
		chip->usb_psy = power_supply_get_by_name("usb");

	if (!chip->batt_psy)
		chip->batt_psy = power_supply_get_by_name("battery");

	if (!chip->usb_psy || !chip->batt_psy)
		return -EPROBE_DEFER;

	return 0;
}

/**
 * oplus_charger_detect_check() - Detect charger presence and type.
 * @chip: OPLUS charging chip context.
 *
 * Layer 3B expanded charger detection: reads charger existence, type,
 * voltage, and optional current from P0 charger ops.
 */
static void oplus_charger_detect_check(struct oplus_chg_chip *chip)
{
	chip->charger_exist = chip->chg_ops->check_chrdet_status();
	chip->charger_online = chip->charger_exist;

	if (!chip->charger_exist) {
		chip->charger_type = POWER_SUPPLY_TYPE_UNKNOWN;
		chip->charger_volt = 0;
		chip->charger_current_ma = 0;
		return;
	}

	chip->charger_type = chip->chg_ops->get_charger_type();
	chip->charger_volt = chip->chg_ops->get_charger_volt();
	if (chip->chg_ops->get_charger_current)
		chip->charger_current_ma = chip->chg_ops->get_charger_current();
}

/**
 * oplus_chg_check_tbatt_is_good() - Classify battery temperature into bands.
 * @chip: OPLUS charging chip context.
 *
 * Ported from 4.19 oplus_chg_check_tbatt_is_good() (L7041-7125), pruned
 * to normal-wired temperature bands only.
 *
 * Return: true when temperature allows charging, false otherwise.
 */
static bool oplus_chg_check_tbatt_is_good(struct oplus_chg_chip *chip)
{
	int batt_temp = chip->temperature;

	if (batt_temp <= chip->limits.removed_bat_decidegc) {
		chip->tbatt_status = BATTERY_STATUS__REMOVED;
		chip->batt_exist = false;
		return false;
	}

	chip->batt_exist = true;
	if (batt_temp > chip->limits.hot_bat_decidegc) {
		chip->tbatt_status = BATTERY_STATUS__HIGH_TEMP;
		return false;
	}
	if (batt_temp < chip->limits.cold_bat_decidegc) {
		chip->tbatt_status = BATTERY_STATUS__LOW_TEMP;
		return false;
	}
	if (batt_temp >= chip->limits.warm_bat_decidegc)
		chip->tbatt_status = BATTERY_STATUS__WARM_TEMP;
	else if (batt_temp >= chip->limits.normal_bat_decidegc)
		chip->tbatt_status = BATTERY_STATUS__NORMAL;
	else if (batt_temp >= chip->limits.little_cool_bat_decidegc)
		chip->tbatt_status = BATTERY_STATUS__LITTLE_COOL_TEMP;
	else if (batt_temp >= chip->limits.cool_bat_decidegc)
		chip->tbatt_status = BATTERY_STATUS__COOL_TEMP;
	else if (batt_temp >= chip->limits.little_cold_bat_decidegc)
		chip->tbatt_status = BATTERY_STATUS__LITTLE_COLD_TEMP;
	else
		chip->tbatt_status = BATTERY_STATUS__COLD_TEMP;

	return true;
}

/**
 * oplus_chg_check_vbatt_is_good() - Check battery voltage against HV threshold.
 * @chip: OPLUS charging chip context.
 *
 * Ported from 4.19 oplus_chg_check_vbatt_is_good() (L7370-7389).
 *
 * Return: true when voltage is safe, false on overvoltage.
 */
static bool oplus_chg_check_vbatt_is_good(struct oplus_chg_chip *chip)
{
	if (chip->batt_volt >= chip->limits.vbatt_hv_thr) {
		chip->vbatt_over = true;
		return false;
	}

	chip->vbatt_over = false;
	return true;
}

/**
 * oplus_chg_check_time_is_good() - Check charge time against safety limit.
 * @chip: OPLUS charging chip context.
 *
 * Ported from 4.19 oplus_chg_check_time_is_good() (L7391-7411).
 *
 * Return: true when time is within limit, false on overtime.
 */
static bool oplus_chg_check_time_is_good(struct oplus_chg_chip *chip)
{
	if (chip->limits.max_chg_time_sec < 0) {
		chip->chging_over_time = false;
		return true;
	}

	if (chip->total_time >= chip->limits.max_chg_time_sec) {
		chip->total_time = chip->limits.max_chg_time_sec;
		chip->chging_over_time = true;
		return false;
	}

	chip->chging_over_time = false;
	return true;
}

/**
 * oplus_chg_protection_check() - Run all safety checks.
 * @chip: OPLUS charging chip context.
 *
 * Return: true when all checks pass, false if any check fails.
 */
static bool oplus_chg_protection_check(struct oplus_chg_chip *chip)
{
	if (!oplus_chg_check_tbatt_is_good(chip))
		return false;
	if (!oplus_chg_check_vbatt_is_good(chip))
		return false;
	if (!oplus_chg_check_time_is_good(chip))
		return false;

	return true;
}

/**
 * oplus_chg_get_float_voltage() - Select float voltage based on temperature band.
 * @chip: OPLUS charging chip context.
 *
 * Return: float voltage in mV.
 */
static int oplus_chg_get_float_voltage(struct oplus_chg_chip *chip)
{
	switch (chip->tbatt_status) {
	case BATTERY_STATUS__COLD_TEMP:
		return chip->limits.temp_cold_vfloat_mv;
	case BATTERY_STATUS__LITTLE_COLD_TEMP:
		return chip->limits.temp_little_cold_vfloat_mv;
	case BATTERY_STATUS__COOL_TEMP:
		return chip->limits.temp_cool_vfloat_mv;
	case BATTERY_STATUS__LITTLE_COOL_TEMP:
		return chip->limits.temp_little_cool_vfloat_mv;
	case BATTERY_STATUS__WARM_TEMP:
		return chip->limits.temp_warm_vfloat_mv;
	case BATTERY_STATUS__NORMAL:
	default:
		return chip->limits.temp_normal_vfloat_mv;
	}
}

/**
 * oplus_chg_get_charging_current() - Select charge current based on temperature band.
 * @chip: OPLUS charging chip context.
 *
 * Returns conservative input current when battery is not authenticated.
 *
 * Return: charge current in mA.
 */
static int oplus_chg_get_charging_current(struct oplus_chg_chip *chip)
{
	if (!chip->batt_auth)
		return chip->limits.input_current_charger_ma;

	switch (chip->tbatt_status) {
	case BATTERY_STATUS__COLD_TEMP:
		return chip->limits.temp_cold_fastchg_current_ma;
	case BATTERY_STATUS__LITTLE_COLD_TEMP:
		return chip->limits.temp_little_cold_fastchg_current_ma;
	case BATTERY_STATUS__COOL_TEMP:
		return chip->limits.temp_cool_fastchg_current_ma_low;
	case BATTERY_STATUS__LITTLE_COOL_TEMP:
		return chip->limits.temp_little_cool_fastchg_current_ma;
	case BATTERY_STATUS__WARM_TEMP:
		return chip->limits.temp_warm_fastchg_current_ma;
	case BATTERY_STATUS__NORMAL:
	default:
		return chip->limits.temp_normal_fastchg_current_ma;
	}
}

/**
 * oplus_chg_apply_normal_wired_limits() - Program hardware with selected limits.
 * @chip: OPLUS charging chip context.
 *
 * Return: 0 on success, or a negative errno from charger ops.
 */
static int oplus_chg_apply_normal_wired_limits(struct oplus_chg_chip *chip)
{
	int ret;

	chip->input_current_limit_ma = chip->limits.input_current_charger_ma;
	chip->float_voltage_mv = oplus_chg_get_float_voltage(chip);
	chip->fastchg_current_ma = oplus_chg_get_charging_current(chip);

	ret = chip->chg_ops->input_current_write(chip->input_current_limit_ma);
	if (ret)
		return ret;
	ret = chip->chg_ops->float_voltage_write(chip->float_voltage_mv);
	if (ret)
		return ret;
	return chip->chg_ops->charging_current_write_fast(chip->fastchg_current_ma);
}

/**
 * oplus_chg_check_sw_full() - Software full-charge detection.
 * @chip: OPLUS charging chip context.
 *
 * Return: true when SOC >= 100 and voltage >= full threshold.
 */
static bool oplus_chg_check_sw_full(struct oplus_chg_chip *chip)
{
	return chip->batt_soc >= 100 &&
		chip->batt_volt >= chip->limits.vbatt_full_thr;
}

/**
 * oplus_chg_check_status_full() - Handle full and recharge logic.
 * @chip: OPLUS charging chip context.
 *
 * Ported from 4.19 oplus_chg_check_status_full() (L11167-11426), pruned
 * to normal-wired full/recharge without protocol branches.
 */
static void oplus_chg_check_status_full(struct oplus_chg_chip *chip)
{
	int hw_full = 0;

	if (chip->chg_ops->read_full)
		hw_full = chip->chg_ops->read_full();

	chip->hw_full = hw_full > 0;
	chip->sw_full = oplus_chg_check_sw_full(chip);
	chip->batt_full = chip->hw_full || chip->sw_full;

	if (chip->batt_full) {
		chip->charging_state = CHARGING_STATUS_FULL;
		chip->real_batt_full = true;
		chip->chg_ops->charging_disable();
		chip->charging_enabled = false;
		return;
	}

	if (chip->real_batt_full &&
	    chip->batt_volt <= chip->limits.vbatt_full_thr - chip->limits.recharge_mv) {
		chip->real_batt_full = false;
		chip->in_rechging = true;
		chip->charging_state = CHARGING_STATUS_CCCV;
	}
}

/**
 * oplus_chg_update_work() - Periodic charging update handler.
 * @work: work_struct embedded in oplus_chg_chip.update_work.
 *
 * Layer 3B ordered flow (pruned from 4.19 oplus_chg_update_work L12847-12884):
 *   bind psy → charger detect → battery data → disable when offline
 *   → protection checks → check full → apply limits → enable charging
 *   → increment total_time → requeue.
 */
static void oplus_chg_update_work(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct oplus_chg_chip *chip = container_of(dwork,
					struct oplus_chg_chip, update_work);
	bool allow_charge;
	int ret;

	if (!chip->initialized)
		return;

	if (!oplus_chg_ops_ready(chip))
		goto requeue;

	if (oplus_chg_bind_power_supplies(chip))
		goto requeue;

	oplus_charger_detect_check(chip);
	oplus_chg_get_battery_data(chip);

	if (!chip->charger_exist) {
		chip->total_time = 0;
		chip->chg_ops->charging_disable();
		chip->charging_enabled = false;
		goto requeue;
	}

	allow_charge = oplus_chg_protection_check(chip);
	if (!allow_charge) {
		chip->charging_state = CHARGING_STATUS_FAIL;
		chip->chg_ops->charging_disable();
		chip->charging_enabled = false;
		goto requeue;
	}

	oplus_chg_check_status_full(chip);
	if (chip->charging_state == CHARGING_STATUS_FULL && !chip->in_rechging)
		goto requeue;

	ret = oplus_chg_apply_normal_wired_limits(chip);
	if (ret) {
		chip->charging_state = CHARGING_STATUS_FAIL;
		goto requeue;
	}

	ret = chip->chg_ops->charging_enable();
	if (ret) {
		chip->charging_state = CHARGING_STATUS_FAIL;
		goto requeue;
	}

	chip->charging_enabled = true;
	chip->charging_state = CHARGING_STATUS_CCCV;
	chip->total_time += chip->update_interval_ms / 1000;

requeue:
	if (chip->initialized)
		schedule_delayed_work(&chip->update_work,
			msecs_to_jiffies(chip->update_interval_ms));
}

/**
 * oplus_chg_init() - Initialize the OPLUS charger runtime.
 * @chip: OPLUS charging chip context with dev and chg_ops populated.
 *
 * Validates ops readiness, initializes Layer 3B state, parses DT defaults,
 * initializes the periodic update work, and schedules the first update.
 *
 * Return: 0 on success, or a negative errno.
 */
int oplus_chg_init(struct oplus_chg_chip *chip)
{
	int rc;

	if (!chip || !chip->dev)
		return -EINVAL;

	if (!oplus_chg_ops_ready(chip))
		return -ENODEV;

	oplus_chg_state_init(chip);

	rc = oplus_chg_parse_svooc_dt(chip);
	if (rc < 0)
		return rc;

	rc = oplus_chg_parse_charger_dt(chip);
	if (rc < 0)
		return rc;

	atomic_set(&chip->api_users, 0);
	init_waitqueue_head(&chip->api_wq);

	INIT_DELAYED_WORK(&chip->update_work, oplus_chg_update_work);
	chip->initialized = true;

	mutex_lock(&g_chg_lock);
	if (g_charger_chip) {
		chip->initialized = false;
		mutex_unlock(&g_chg_lock);
		return -EBUSY;
	}
	g_charger_chip = chip;
	mutex_unlock(&g_chg_lock);

	schedule_delayed_work(&chip->update_work, 0);
	chg_info("charger path initialized\n");

	return 0;
}
EXPORT_SYMBOL_GPL(oplus_chg_init);

/**
 * oplus_chg_deinit() - Stop and detach an OPLUS charger runtime.
 * @chip: OPLUS charging chip context previously passed to oplus_chg_init().
 *
 * The update worker requeues itself while initialized is true.  Clear the flag
 * before cancel_delayed_work_sync() so a concurrently running worker exits
 * without scheduling itself again.  Only clear g_charger_chip when it still
 * points to the same chip to avoid tearing down a newer runtime by mistake.
 */
void oplus_chg_deinit(struct oplus_chg_chip *chip)
{
	if (!chip)
		return;

	mutex_lock(&g_chg_lock);
	if (g_charger_chip == chip)
		g_charger_chip = NULL;
	chip->initialized = false;
	mutex_unlock(&g_chg_lock);

	if (wait_event_timeout(chip->api_wq,
			       atomic_read(&chip->api_users) == 0,
			       msecs_to_jiffies(5000)) == 0)
		WARN_ONCE(1, "oplus_chg: api_users leaked, forcing deinit\n");

	cancel_delayed_work_sync(&chip->update_work);

	if (chip->usb_psy) {
		power_supply_put(chip->usb_psy);
		chip->usb_psy = NULL;
	}
	if (chip->batt_psy) {
		power_supply_put(chip->batt_psy);
		chip->batt_psy = NULL;
	}

}
EXPORT_SYMBOL_GPL(oplus_chg_deinit);

/**
 * oplus_chg_cancel_update_work_sync() - Cancel the periodic update work synchronously.
 */
void oplus_chg_cancel_update_work_sync(void)
{
	struct oplus_chg_chip *chip = oplus_chg_try_get_chip();

	if (!chip)
		return;

	cancel_delayed_work_sync(&chip->update_work);
	oplus_chg_put_chip(chip);
}
EXPORT_SYMBOL_GPL(oplus_chg_cancel_update_work_sync);

/**
 * oplus_chg_restart_update_work() - Restart the periodic update work immediately.
 */
void oplus_chg_restart_update_work(void)
{
	struct oplus_chg_chip *chip = oplus_chg_try_get_chip();

	if (!chip)
		return;

	schedule_delayed_work(&chip->update_work, 0);
	oplus_chg_put_chip(chip);
}
EXPORT_SYMBOL_GPL(oplus_chg_restart_update_work);

/**
 * oplus_chg_wake_update_work() - Wake the update work immediately.
 *
 * Return: true when work was kicked, false when g_charger_chip is NULL.
 */
bool oplus_chg_wake_update_work(void)
{
	struct oplus_chg_chip *chip = oplus_chg_try_get_chip();

	if (!chip)
		return false;

	mod_delayed_work(system_wq, &chip->update_work, 0);
	oplus_chg_put_chip(chip);
	return true;
}
EXPORT_SYMBOL_GPL(oplus_chg_wake_update_work);

MODULE_DESCRIPTION("OPLUS charger state machine for 5.10 Layer 3B");
MODULE_LICENSE("GPL");
