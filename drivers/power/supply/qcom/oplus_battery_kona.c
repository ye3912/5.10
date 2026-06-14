// SPDX-License-Identifier: GPL-2.0-only
/*
 * OPLUS SM8250/Kona SMB5 charger adapter.
 *
 * Anchored inside qpnp-smb5-main so it can access struct smb_charger
 * directly.  Populates struct oplus_chg_operations with real P0 SMB5
 * callbacks and calls oplus_chg_init() to start the Layer 3A/3B
 * charging state machine.
 */

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/iio/consumer.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/pinctrl/consumer.h>
#include <linux/delay.h>
#include <linux/pmic-voter.h>
#include <linux/pm_wakeup.h>
#include <linux/power_supply.h>
#include <linux/slab.h>

#include "smb5-lib.h"
#include "smb5-reg.h"
#include "oplus_battery_kona.h"
#include "../../oplus/oplus_charger.h"
#include <soc/oplus/boot_mode.h>

struct oplus_kona_chg {
	struct device *dev;
	struct smb_charger *chg;
	struct oplus_chg_chip oplus_chip;
	struct oplus_kona_gpio *gpio;

	/* IIO channels for chargerid and USB temp (not in smb_iio) */
	struct iio_channel *chgid_v_chan;
	struct iio_channel *usbtemp_v_chan;
	struct iio_channel *usbtemp_sup_v_chan;

	/* USB temperature monitoring thread */
	struct task_struct *usbtemp_kthread;
	wait_queue_head_t usbtemp_wq;
	struct wakeup_source *usbtemp_ws;
};

static DEFINE_MUTEX(oplus_kona_lock);
static struct oplus_kona_chg *oplus_kona_chip;

/**
 * oplus_kona_get_chg() - Return the active SMB5 charger context.
 *
 * The OPLUS callback ABI has no private context parameter, so this adapter
 * uses the single SM8250 SMB5 instance registered by smb5_probe().
 */
static struct smb_charger *oplus_kona_get_chg(void)
{
	struct smb_charger *chg;

	mutex_lock(&oplus_kona_lock);
	chg = oplus_kona_chip ? oplus_kona_chip->chg : NULL;
	mutex_unlock(&oplus_kona_lock);

	return chg;
}

/* ===== P0 Control Callbacks ===== */

/* Sanity caps — prevent integer overflow on mA→µA / mV→µV conversion */
#define KONA_MAX_CURRENT_MA	10000	/* 10 A */
#define KONA_MAX_VOLTAGE_MV	5000	/* 5 V */

static int oplus_kona_set_fastchg_current(int current_ma)
{
	struct smb_charger *chg = oplus_kona_get_chg();

	if (!chg || !chg->fcc_votable)
		return -ENODEV;
	if (current_ma < 0 || current_ma > KONA_MAX_CURRENT_MA)
		return -EINVAL;

	return vote(chg->fcc_votable, DEFAULT_VOTER, true, current_ma * 1000);
}

static int oplus_kona_set_input_current(int current_ma)
{
	struct smb_charger *chg = oplus_kona_get_chg();

	if (!chg)
		return -ENODEV;
	if (current_ma < 0 || current_ma > KONA_MAX_CURRENT_MA)
		return -EINVAL;

	return smblib_set_icl_current(chg, current_ma * 1000);
}

static int oplus_kona_set_float_voltage(int vfloat_mv)
{
	struct smb_charger *chg = oplus_kona_get_chg();

	if (!chg || !chg->fv_votable)
		return -ENODEV;
	if (vfloat_mv <= 0 || vfloat_mv > KONA_MAX_VOLTAGE_MV)
		return -EINVAL;

	return vote(chg->fv_votable, BATT_PROFILE_VOTER, true, vfloat_mv * 1000);
}

static int oplus_kona_charging_enable(void)
{
	struct smb_charger *chg = oplus_kona_get_chg();

	if (!chg || !chg->chg_disable_votable)
		return -ENODEV;

	return vote(chg->chg_disable_votable, DEFAULT_VOTER, false, 0);
}

static int oplus_kona_charging_disable(void)
{
	struct smb_charger *chg = oplus_kona_get_chg();

	if (!chg || !chg->chg_disable_votable)
		return -ENODEV;

	return vote(chg->chg_disable_votable, DEFAULT_VOTER, true, 0);
}

/* ===== P0 Status and Readback Callbacks ===== */

static int oplus_kona_get_charging_enable(void)
{
	struct smb_charger *chg = oplus_kona_get_chg();
	int rc;
	u8 val;

	if (!chg)
		return 0;

	rc = smblib_read(chg, CHARGING_ENABLE_CMD_REG, &val);
	if (rc < 0)
		return 0;

	return !!(val & CHARGING_ENABLE_CMD_BIT);
}

static bool oplus_kona_check_chrdet_status(void)
{
	struct smb_charger *chg = oplus_kona_get_chg();
	union power_supply_propval pval = { 0 };
	int rc;

	if (!chg)
		return false;

	rc = smblib_get_prop_usb_present(chg, &pval);
	if (rc < 0)
		return false;

	return !!pval.intval;
}

static int oplus_kona_get_charger_volt(void)
{
	struct smb_charger *chg = oplus_kona_get_chg();
	union power_supply_propval pval = { 0 };
	int rc;

	if (!chg || !oplus_kona_check_chrdet_status())
		return 0;

	rc = smblib_get_prop_usb_voltage_now(chg, &pval);
	if (rc < 0)
		return 0;

	return pval.intval / 1000;
}

static int oplus_kona_get_charger_current(void)
{
	struct smb_charger *chg = oplus_kona_get_chg();
	union power_supply_propval pval = { 0 };
	int rc;

	if (!chg)
		return 0;

	rc = smblib_get_prop_usb_current_now(chg, &pval);
	if (rc < 0)
		return 0;

	return pval.intval / 1000;
}

static int oplus_kona_get_charger_type(void)
{
	struct smb_charger *chg = oplus_kona_get_chg();

	if (!chg)
		return POWER_SUPPLY_TYPE_UNKNOWN;

	return chg->real_charger_type;
}

static int oplus_kona_read_full(void)
{
	struct smb_charger *chg = oplus_kona_get_chg();
	int rc;
	u8 val;

	if (!chg || !oplus_kona_check_chrdet_status())
		return 0;

	rc = smblib_read(chg, BATTERY_CHARGER_STATUS_1_REG, &val);
	if (rc < 0)
		return 0;

	val &= BATTERY_CHARGER_STATUS_MASK;
	return val == TERMINATE_CHARGE || val == INHIBIT_CHARGE;
}

/* ===== P0 Ops Table ===== */

/**
 * oplus_kona_set_typec_sinkonly() - Force Type-C to sink-only mode.
 *
 * Ported from 4.19 oplus_set_typec_sinkonly().
 * Writes EN_SNK_ONLY_BIT to TYPE_C_MODE_CFG_REG to disable source capability.
 */
static void oplus_kona_set_typec_sinkonly(void)
{
	struct smb_charger *chg = oplus_kona_get_chg();
	int rc;

	if (!chg)
		return;

	rc = smblib_masked_write(chg, TYPE_C_MODE_CFG_REG,
				 TYPEC_POWER_ROLE_CMD_MASK | TYPEC_TRY_MODE_MASK,
				 EN_SNK_ONLY_BIT);
	if (rc < 0)
		dev_err(chg->dev, "%s: failed, rc=%d\n", __func__, rc);
}

/* Forward declarations — functions defined after ops table */
static void oplus_kona_get_usbtemp_volt(struct oplus_chg_chip *chip);
static int oplus_kona_get_chargerid_volt(void);
static void oplus_kona_set_chargerid_switch_val(int value);
static int oplus_kona_chargerid_switch_val(void);
static bool oplus_kona_usbtemp_condition(struct oplus_chg_chip *chip);

/**
 * oplus_kona_get_boot_mode() - Read boot mode from SMEM or cmdline.
 *
 * Ported from 4.19 get_boot_mode().
 * Reads the OPLUS boot mode identifier used by the charging subsystem to
 * decide whether to enter ship mode, factory test, or normal charge.
 *
 * In 5.10, the boot_mode fields were stored in smb_charger which has
 * been redesigned.  Return 0 (normal boot) until OPLUS boot mode
 * infrastructure is fully ported.
 *
 * Return: 0 (normal boot).
 */
static int oplus_kona_get_boot_mode(void)
{
	/*
	 * 5.10 exports get_boot_mode() from drivers/soc/oplus/boot_mode.c
	 * (include/soc/oplus/boot_mode.h).  Returns MSM_BOOT_MODE__NORMAL etc.
	 */
	return get_boot_mode();
}

/**
 * oplus_kona_get_boot_reason() - Read SMEM boot reason for charger detection.
 *
 * Ported from 4.19 smbchg_get_boot_reason().
 * Returns the PMIC power-on reason (PON) to determine if the device booted
 * due to charger insertion, power key, or other wake sources.
 *
 * Return: boot reason register value, or 0 on error.
 */
static int oplus_kona_get_boot_reason(void)
{
	/*
	 * CHGR_PON_REASON_1_REG is a 4.19 downstream register not defined
	 * in 5.10 smb5-reg.h.  Return 0 (unknown reason) until the PON
	 * register mapping is ported.
	 */
	return 0;
}

/**
 * oplus_kona_get_rtc_soc() - Read shutdown SOC from persistent storage.
 *
 * Ported from 4.19 oplus_chg_get_shutdown_soc().
 * The OPLUS charging framework stores battery SOC at shutdown to provide
 * an accurate starting point on next boot before the gauge re-learns.
 *
 * Return: shutdown SOC (0-100), or 0 on error.
 */
static int oplus_kona_get_rtc_soc(void)
{
	struct oplus_kona_chg *kona;

	mutex_lock(&oplus_kona_lock);
	kona = oplus_kona_chip;
	mutex_unlock(&oplus_kona_lock);

	if (!kona)
		return 0;

	return kona->oplus_chip.shutdown_soc;
}

/**
 * oplus_kona_set_rtc_soc() - Persist battery SOC for next boot.
 * @val: SOC value (0-100) to store.
 *
 * Ported from 4.19 oplus_chg_backup_soc().
 *
 * Return: 0 on success.
 */
static int oplus_kona_set_rtc_soc(int val)
{
	struct oplus_kona_chg *kona;

	mutex_lock(&oplus_kona_lock);
	kona = oplus_kona_chip;
	mutex_unlock(&oplus_kona_lock);

	if (!kona)
		return -ENODEV;

	kona->oplus_chip.shutdown_soc = val;

	return 0;
}

/**
 * oplus_kona_get_ccdetect_online() - Read CC detection GPIO status.
 *
 * Ported from 4.19 oplus_ccdetect_check_is_gpio().
 * Reads the ccdetect_gpio level: 0 = charger connected, 1 = disconnected.
 * Falls back to PMIC USBIN status if GPIO is unavailable.
 *
 * Return: 1 if charger is connected, 0 otherwise.
 */
static int oplus_kona_get_ccdetect_online(void)
{
	struct oplus_kona_chg *kona;

	mutex_lock(&oplus_kona_lock);
	kona = oplus_kona_chip;
	mutex_unlock(&oplus_kona_lock);

	if (kona && kona->gpio &&
	    !IS_ERR_OR_NULL(kona->gpio->ccdetect_gpio)) {
		/* ccdetect: level 0 = cable inserted */
		return gpiod_get_value(kona->gpio->ccdetect_gpio) == 0 ? 1 : 0;
	}

	/* Fallback: use SMB5 PMIC USBIN present */
	return oplus_kona_check_chrdet_status() ? 1 : 0;
}

/**
 * oplus_kona_charger_suspend() - Suspend SMB5 charging.
 *
 * Ported from 4.19 oplus_charger_suspend().
 * Clears CHARGING_ENABLE_CMD_BIT to suspend charging.
 *
 * Return: 0 on success, negative errno on error.
 */
static int oplus_kona_charger_suspend(void)
{
	struct smb_charger *chg = oplus_kona_get_chg();

	if (!chg)
		return -ENODEV;

	return smblib_masked_write(chg, CHARGING_ENABLE_CMD_REG,
				   CHARGING_ENABLE_CMD_BIT, 0);
}

/**
 * oplus_kona_charger_unsuspend() - Resume SMB5 charging.
 *
 * Ported from 4.19 oplus_charger_unsuspend().
 * Sets CHARGING_ENABLE_CMD_BIT to resume charging.
 *
 * Return: 0 on success, negative errno on error.
 */
static int oplus_kona_charger_unsuspend(void)
{
	struct smb_charger *chg = oplus_kona_get_chg();

	if (!chg)
		return -ENODEV;

	return smblib_masked_write(chg, CHARGING_ENABLE_CMD_REG,
				   CHARGING_ENABLE_CMD_BIT,
				   CHARGING_ENABLE_CMD_BIT);
}

/**
 * oplus_kona_get_instant_vbatt() - Read instantaneous battery voltage.
 *
 * Ported from 4.19 oplus_get_instant_vbatt().
 * Reads VBAT via the SMB5 ADC path.
 *
 * Return: battery voltage in mV, or 0 on error.
 */
static int oplus_kona_get_instant_vbatt(void)
{
	struct smb_charger *chg = oplus_kona_get_chg();
	union power_supply_propval pval = { 0 };
	int rc;

	if (!chg)
		return 0;

	if (!chg->batt_psy)
		return 0;

	rc = power_supply_get_property(chg->batt_psy,
					POWER_SUPPLY_PROP_VOLTAGE_NOW,
					&pval);
	if (rc < 0) {
		dev_err(chg->dev, "%s: read fail, rc=%d\n", __func__, rc);
		return 0;
	}

	return pval.intval / 1000;
}

static struct oplus_chg_operations kona_smb5_chg_ops = {
	.charging_current_write_fast = oplus_kona_set_fastchg_current,
	.input_current_write = oplus_kona_set_input_current,
	.float_voltage_write = oplus_kona_set_float_voltage,
	.charging_enable = oplus_kona_charging_enable,
	.charging_disable = oplus_kona_charging_disable,
	.get_charging_enable = oplus_kona_get_charging_enable,
	.get_charger_type = oplus_kona_get_charger_type,
	.get_real_charger_type = oplus_kona_get_charger_type,
	.get_charger_volt = oplus_kona_get_charger_volt,
	.get_charger_current = oplus_kona_get_charger_current,
	.check_chrdet_status = oplus_kona_check_chrdet_status,
	.read_full = oplus_kona_read_full,
	.get_usbtemp_volt = oplus_kona_get_usbtemp_volt,
	.get_chargerid_volt = oplus_kona_get_chargerid_volt,
	.set_chargerid_switch_val = oplus_kona_set_chargerid_switch_val,
	.get_chargerid_switch_val = oplus_kona_chargerid_switch_val,
	.oplus_usbtemp_monitor_condition = oplus_kona_usbtemp_condition,
	.set_typec_sinkonly = oplus_kona_set_typec_sinkonly,
	.get_boot_mode = oplus_kona_get_boot_mode,
	.get_boot_reason = oplus_kona_get_boot_reason,
	.get_rtc_soc = oplus_kona_get_rtc_soc,
	.set_rtc_soc = oplus_kona_set_rtc_soc,
	.get_ccdetect_online = oplus_kona_get_ccdetect_online,
	.charger_suspend = oplus_kona_charger_suspend,
	.charger_unsuspend = oplus_kona_charger_unsuspend,
	.get_instant_vbatt = oplus_kona_get_instant_vbatt,
};

/* ===== OPLUS GPIO Management Functions ===== */

/**
 * oplus_kona_get_wired_chg_present() - Read wired charger present status.
 *
 * Ported from 4.19 oplus_get_wired_chg_present().
 * Reads the USBIN plug-in real-time status bit from the SMB5 PMIC
 * to determine if a wired charger is physically connected.
 *
 * Return: true if wired charger present, false otherwise.
 */
static __maybe_unused bool oplus_kona_get_wired_chg_present(void)
{
	struct smb_charger *chg = oplus_kona_get_chg();
	int rc;
	u8 stat;

	if (!chg)
		return false;

	rc = smblib_read(chg, USBIN_BASE + INT_RT_STS_OFFSET, &stat);
	if (rc < 0) {
		dev_err(chg->dev, "%s: read fail, rc=%d\n", __func__, rc);
		return false;
	}

	return !!(stat & USBIN_PLUGIN_RT_STS_BIT);
}

/**
 * oplus_kona_get_otg_switch_status() - Read OTG switch status.
 *
 * Ported from 4.19 oplus_get_otg_switch_status().
 * In 4.19 this reads chip->otg_switch from the global g_oplus_chip.
 * In 5.10 we track this locally via oplus_kona_chip.
 *
 * Return: true if OTG mode is active, false otherwise.
 */
static __maybe_unused bool oplus_kona_get_otg_switch_status(void)
{
	/* OTG switch is managed by the OPLUS layer; for now return false.
	 * Full OTG support will be added in a later phase once the
	 * typec_mux / OTG callbacks are fully plumbed through the
	 * kona adapter.
	 */
	return false;
}

/**
 * oplus_kona_set_otg_switch_status() - Set OTG switch status.
 * @value: true to enable OTG mode, false to disable.
 *
 * Ported from 4.19 oplus_set_otg_switch_status().
 * In 4.19 this toggles ccdetect enable/disable and stores chip->otg_switch.
 * Full OTG support will be plumbed in a later phase.
 */
static __maybe_unused void oplus_kona_set_otg_switch_status(bool value)
{
	struct oplus_kona_chg *kona;

	mutex_lock(&oplus_kona_lock);
	kona = oplus_kona_chip;
	mutex_unlock(&oplus_kona_lock);

	if (!kona)
		return;

	dev_info(kona->dev, "%s: otg_switch=%d (stub)\n", __func__, value);
}

/**
 * oplus_kona_set_idt_en_val() - Set IDT enable (wireless charger) GPIO.
 * @value: 0 for active (wireless charging enabled), 1 for inactive.
 *
 * Ported from 4.19 oplus_set_idt_en_val().
 * 4.19 API: gpio_direction_output(chg->idt_en_gpio, val) on int GPIO number.
 * 5.10 API: gpiod_set_value(kona->gpio->idt_en_gpio, val) on gpio_desc *.
 *
 * 4.19 logic: value==1 selects active pinctrl state, value==0 selects default.
 */
static __maybe_unused void oplus_kona_set_idt_en_val(int value)
{
	struct oplus_kona_chg *kona;
	struct oplus_kona_gpio *gpio;

	mutex_lock(&oplus_kona_lock);
	kona = oplus_kona_chip;
	mutex_unlock(&oplus_kona_lock);

	if (!kona || !kona->gpio)
		return;

	gpio = kona->gpio;

	if (IS_ERR_OR_NULL(gpio->idt_en_gpio)) {
		dev_err(kona->dev, "idt_en_gpio not exist, return\n");
		return;
	}

	if (IS_ERR_OR_NULL(gpio->idt_en_pinctrl)
		|| IS_ERR_OR_NULL(gpio->idt_en_active)
		|| IS_ERR_OR_NULL(gpio->idt_en_sleep)
		|| IS_ERR_OR_NULL(gpio->idt_en_default)) {
		dev_err(kona->dev, "idt_en pinctrl null, return\n");
		return;
	}

	if (value) {
		gpiod_set_value(gpio->idt_en_gpio, 1);
		pinctrl_select_state(gpio->idt_en_pinctrl,
				     gpio->idt_en_active);
	} else {
		gpiod_set_value(gpio->idt_en_gpio, 0);
		pinctrl_select_state(gpio->idt_en_pinctrl,
				     gpio->idt_en_default);
	}

	dev_info(kona->dev, "<~WPC~> set value:%d, gpio_val:%d\n",
		 value, gpiod_get_value(gpio->idt_en_gpio));
}

/**
 * oplus_kona_get_idt_en_val() - Read IDT enable (wireless charger) GPIO status.
 *
 * Ported from 4.19 oplus_get_idt_en_val().
 * 4.19 API: gpio_get_value(chg->idt_en_gpio) on int GPIO number.
 * 5.10 API: gpiod_get_value(kona->gpio->idt_en_gpio) on gpio_desc *.
 *
 * Return: GPIO value (0/1), or -1 on error.
 */
static __maybe_unused int oplus_kona_get_idt_en_val(void)
{
	struct oplus_kona_chg *kona;
	struct oplus_kona_gpio *gpio;

	mutex_lock(&oplus_kona_lock);
	kona = oplus_kona_chip;
	mutex_unlock(&oplus_kona_lock);

	if (!kona || !kona->gpio)
		return -1;

	gpio = kona->gpio;

	if (IS_ERR_OR_NULL(gpio->idt_en_gpio)) {
		dev_err(kona->dev, "idt_en_gpio not exist, return\n");
		return -1;
	}

	return gpiod_get_value(gpio->idt_en_gpio);
}

/**
 * oplus_kona_chargerid_switch_val() - Read charger ID switch GPIO.
 *
 * Ported from 4.19 smbchg_get_chargerid_switch_val().
 * In 4.19 this reads gpio_get_value(normalchg_gpio.chargerid_switch_gpio).
 *
 * The charger ID switch GPIO resides in the normalchg_gpio block,
 * not in our oplus_kona_gpio struct.  For the Kona adapter this
 * is a stub until the full normalchg GPIO infrastructure is plumbed.
 *
 * Return: GPIO value (0/1), or -1 on error.
 */
static int oplus_kona_chargerid_switch_val(void)
{
	/* Stub: chargerid_switch GPIO not yet plumbed in kona adapter.
	 * The 4.19 normalchg_gpio subsystem needs to be ported separately.
	 * Returning 0 (inactive) is the safe default.
	 */
	return 0;
}

/**
 * oplus_kona_set_chargerid_switch_val() - Set charger ID switch GPIO.
 * @value: 0 to clear, 1 to set the charger ID switch GPIO.
 *
 * Ported from 4.19 smbchg_set_chargerid_switch_val().
 * Stub until normalchg_gpio infrastructure is ported.
 */
static void oplus_kona_set_chargerid_switch_val(int value)
{
	/* Stub: chargerid_switch GPIO not yet plumbed in kona adapter. */
}

/**
 * oplus_kona_get_chargerid_volt() - Read charger ID voltage via IIO ADC.
 *
 * Ported from 4.19 smbchg_get_chargerid_volt().
 * In 4.19 this reads chg->iio.chgid_v_chan via iio_read_channel_processed().
 * In 5.10 the IIO channel is acquired by the SMB5 driver; access it through
 * the smb_charger iio handle.
 *
 * Return: charger ID voltage in mV, or 0 on error.
 */
static int oplus_kona_get_chargerid_volt(void)
{
	struct oplus_kona_chg *kona;
	int rc, chargerid_volt = 0;

	mutex_lock(&oplus_kona_lock);
	kona = oplus_kona_chip;
	mutex_unlock(&oplus_kona_lock);

	if (!kona || !kona->chg)
		return 0;

	if (IS_ERR_OR_NULL(kona->chgid_v_chan)) {
		/* chgid_v_chan not yet probed or DT property missing */
		return 0;
	}

	rc = iio_read_channel_processed(kona->chgid_v_chan,
					&chargerid_volt);
	if (rc < 0) {
		dev_err(kona->dev, "%s: iio read error, rc=%d\n",
			__func__, rc);
		return 0;
	}

	return chargerid_volt / 1000;
}

/**
 * oplus_kona_get_usbtemp_volt() - Read USB temperature thermistor voltages.
 *
 * Ported from 4.19 oplus_get_usbtemp_volt().
 * Reads two IIO ADC channels: usbtemp_v_chan (left/primary) and
 * usbtemp_sup_v_chan (right/supplementary).  Results are stored in
 * the oplus_chip for the USB temperature monitoring thread.
 *
 * @chip: oplus_chg_chip where usbtemp_volt_l and usbtemp_volt_r are updated.
 */
static void oplus_kona_get_usbtemp_volt(struct oplus_chg_chip *chip)
{
	struct oplus_kona_chg *kona;
	int rc, usbtemp_volt;

	if (!chip)
		return;

	mutex_lock(&oplus_kona_lock);
	kona = oplus_kona_chip;
	mutex_unlock(&oplus_kona_lock);

	if (!kona || !kona->chg)
		return;

	/* Primary/LHS USB temp ADC */
	if (!IS_ERR_OR_NULL(kona->usbtemp_v_chan)) {
		rc = iio_read_channel_processed(kona->usbtemp_v_chan,
						&usbtemp_volt);
		if (rc < 0) {
			dev_err(kona->dev,
				"%s: iio read usbtemp_v error, rc=%d\n",
				__func__, rc);
		} else {
			chip->usbtemp_volt_l = usbtemp_volt / 1000;
			chip->usb_temp_l = chip->usbtemp_volt_l;
		}
	}

	/* Supplementary/RHS USB temp ADC */
	if (!IS_ERR_OR_NULL(kona->usbtemp_sup_v_chan)) {
		rc = iio_read_channel_processed(kona->usbtemp_sup_v_chan,
						&usbtemp_volt);
		if (rc < 0) {
			dev_err(kona->dev,
				"%s: iio read usbtemp_sup_v error, rc=%d\n",
				__func__, rc);
		} else {
			chip->usbtemp_volt_r = usbtemp_volt / 1000;
			chip->usb_temp_r = chip->usbtemp_volt_r;
		}
	}
}

/* ===== USB Temperature Monitoring Thread ===== */

/* Forward declarations for internal thread helpers */
static int oplus_kona_usbtemp_dischg_action(struct oplus_chg_chip *chip);
static __maybe_unused void oplus_kona_usbtemp_clear_dischg(
			    struct oplus_chg_chip *chip);

/* Temperature thresholds in degC */
#define USB_20C		20
#define USB_30C		30
#define USB_40C		40
#define USB_57C		57
#define USB_50C		50
#define USB_55C		55
#define USB_100C	100

/* Monitoring intervals (ms) */
#define VBUS_VOLT_THRESHOLD	400
#define VBUS_MONITOR_INTERVAL	3000
#define MIN_MONITOR_INTERVAL	50
#define MAX_MONITOR_INTERVAL	50
#define RETRY_CNT_DELAY		5

#define USB_TEMP_HIGH	0x01

/**
 * oplus_kona_usbtemp_condition() - Check whether USB temp monitoring is needed.
 *
 * Ported from 4.19 oplus_usbtemp_condition().
 * Returns true if:
 *  - ccdetect GPIO indicates a device is connected (level == 0 when present)
 *  - OR the USBIN plug-in status register indicates VBUS is present
 * Returns false for Type-C sink/OTG modes.
 *
 * Return: true if monitoring is needed, false otherwise.
 */
static bool oplus_kona_usbtemp_condition(struct oplus_chg_chip *chip)
{
	struct smb_charger *chg = oplus_kona_get_chg();
	int rc, level;
	u8 stat;

	if (!chip || !chg)
		return false;

	/* Skip monitoring in Type-C sink modes (OTG) */
	if (chg->typec_mode >= QTI_POWER_SUPPLY_TYPEC_SINK &&
	    chg->typec_mode <= QTI_POWER_SUPPLY_TYPEC_POWERED_CABLE_ONLY)
		return false;

	/* Check ccdetect GPIO if available */
	if (oplus_kona_chip && oplus_kona_chip->gpio &&
	    !IS_ERR_OR_NULL(oplus_kona_chip->gpio->ccdetect_gpio)) {
		level = gpiod_get_value(oplus_kona_chip->gpio->ccdetect_gpio);
		/* ccdetect: level 0 means cable inserted */
		if (level == 1)
			return false;
		return true;
	}

	/* Fall back to PMIC USBIN plug-in status */
	rc = smblib_read(chg, USBIN_BASE + INT_RT_STS_OFFSET, &stat);
	if (rc < 0) {
		dev_err(chg->dev, "%s: read fail, rc=%d\n", __func__, rc);
		return false;
	}

	return !!(stat & USBIN_PLUGIN_RT_STS_BIT);
}

/**
 * oplus_kona_usbtemp_dischg_action() - Disable charging on USB over-temperature.
 * @chip: oplus_chg_chip context.
 *
 * Ported from 4.19 oplus_usbtemp_dischg_action().
 * In 4.19 this:
 *   1. Suspends the charger
 *   2. Forces Type-C to sink-only mode
 *   3. Selects the dischg_enable pinctrl state
 *
 * In 5.10 the dischg pinctrl (normalchg_gpio.dischg_enable) is not yet
 * ported, so we use the SMB5 PMIC register interface to suspend charging
 * and force Type-C sink mode.
 *
 * Return: 0 on success.
 */
static int oplus_kona_usbtemp_dischg_action(struct oplus_chg_chip *chip)
{
	struct smb_charger *chg = oplus_kona_get_chg();
	int rc;

	if (!chg)
		return -ENODEV;

	/* Suspend charging via SMB5 PMIC */
	rc = smblib_masked_write(chg, CHARGING_ENABLE_CMD_REG,
				 CHARGING_ENABLE_CMD_BIT, 0);
	if (rc < 0)
		dev_err(chg->dev, "%s: fail to suspend charger, rc=%d\n",
			__func__, rc);

	/* Force Type-C to sink-only mode */
	rc = smblib_masked_write(chg, TYPE_C_MODE_CFG_REG,
				 TYPEC_POWER_ROLE_CMD_MASK | TYPEC_TRY_MODE_MASK,
				 EN_SNK_ONLY_BIT);
	if (rc < 0)
		dev_err(chg->dev, "%s: fail to set sink mode, rc=%d\n",
			__func__, rc);

	dev_warn(chg->dev, "%s: USB over-temp protection activated\n", __func__);

	return 0;
}

/**
 * oplus_kona_usbtemp_clear_dischg() - Clear USB over-temperature discharge.
 * @chip: oplus_chg_chip context.
 *
 * Ported from 4.19 oplus_usbtemp_clear_dischg().
 * Clears the USB_TEMP_HIGH status and re-enables charging.
 */
static __maybe_unused void oplus_kona_usbtemp_clear_dischg(struct oplus_chg_chip *chip)
{
	struct smb_charger *chg = oplus_kona_get_chg();
	int rc;

	if (!chg)
		return;

	dev_info(chg->dev, "%s: clearing USB over-temp discharge\n", __func__);

	/* Re-enable charging */
	rc = smblib_masked_write(chg, CHARGING_ENABLE_CMD_REG,
				 CHARGING_ENABLE_CMD_BIT,
				 CHARGING_ENABLE_CMD_BIT);
	if (rc < 0)
		dev_err(chg->dev, "%s: fail to re-enable charger, rc=%d\n",
			__func__, rc);
}

/**
 * oplus_kona_usbtemp_monitor_common() - Main USB temperature monitoring loop.
 * @data: oplus_chg_chip pointer.
 *
 * Ported from 4.19 oplus_usbtemp_monitor_common().
 *
 * Key 5.10 API migration:
 *   - wake_lock (removed in 5.10) -> pm_wakeup_event()
 *   - gpio_get_value(int) -> gpiod_get_value(struct gpio_desc *)
 *
 * The thread:
 *   1. Waits on oplus_usbtemp_wq for a wake-up
 *   2. Reads USB temp voltages via IIO (get_usbtemp_volt ops callback)
 *   3. Checks temp thresholds against max_temp and rising-too-fast conditions
 *   4. Triggers dischg_action on over-temp, then arms recovery timer
 *
 * Return: 0 when kthread_should_stop().
 */
static int oplus_kona_usbtemp_monitor_common(void *data)
{
	struct oplus_chg_chip *chip = data;
	struct smb_charger *chg;
	int delay = 0;
	int count = 0;
	int total_count = 0;
	int last_usb_temp_l = 25;
	int current_temp_l = 25;
	int last_usb_temp_r = 25;
	int current_temp_r = 25;
	int retry_cnt = 3, i;
	int count_r = 1, count_l = 1;
	bool condition1 = false;
	bool condition2 = false;
	struct oplus_kona_chg *kona;

	if (!chip)
		return 0;

	chg = oplus_kona_get_chg();
	if (!chg)
		return 0;

	/* Initialize wakeup source for 5.10 (replaces 4.19 wake_lock) */
	mutex_lock(&oplus_kona_lock);
	kona = oplus_kona_chip;
	mutex_unlock(&oplus_kona_lock);

	if (kona) {
		kona->usbtemp_ws = wakeup_source_register(chg->dev,
							  "oplus_usbtemp");
	} else {
		dev_err(chg->dev, "%s: kona chip disappeared during thread init\n",
			__func__);
		return 0;
	}

	dev_info(chg->dev, "%s: USB temp monitor thread started\n", __func__);

	while (!kthread_should_stop()) {
		wait_event_interruptible(kona->usbtemp_wq,
					 chip->usbtemp_check ||
					 kthread_should_stop());

		if (kthread_should_stop())
			break;

		/* Hold wakeup source while monitoring (replaces wake_lock) */
		if (kona->usbtemp_ws)
			__pm_wakeup_event(kona->usbtemp_ws,
					  MAX_MONITOR_INTERVAL * 10);

		if (chip->dischg_flag)
			goto dischg;

		if (!chip->chg_ops->get_usbtemp_volt) {
			/* get_usbtemp_volt not registered yet, retry */
			chip->usbtemp_check = false;
			msleep(500);
			continue;
		}

		/* Read USB temperatures via IIO ADC */
		chip->chg_ops->get_usbtemp_volt(chip);

		/* Determine monitoring interval */
		if ((chip->usb_temp_l < USB_40C) &&
		    (chip->usb_temp_r < USB_40C)) {
			delay = MAX_MONITOR_INTERVAL;
			total_count = 10;
		} else {
			delay = MIN_MONITOR_INTERVAL;
			total_count = (chip->usbtemp_temp_up_time_thr > 0) ?
				      chip->usbtemp_temp_up_time_thr : 30;
		}

		/* Condition 1: temp exceeded max threshold */
		if ((chip->usb_temp_l >= chip->usbtemp_max_temp_thr &&
		     chip->usb_temp_l < USB_100C) ||
		    (chip->usb_temp_r >= chip->usbtemp_max_temp_thr &&
		     chip->usb_temp_r < USB_100C)) {
			for (i = 1; i < retry_cnt; i++) {
				mdelay(RETRY_CNT_DELAY);
				chip->chg_ops->get_usbtemp_volt(chip);
				if (chip->usb_temp_r >=
				    chip->usbtemp_max_temp_thr &&
				    chip->usb_temp_r < USB_100C)
					count_r++;
				if (chip->usb_temp_l >=
				    chip->usbtemp_max_temp_thr &&
				    chip->usb_temp_l < USB_100C)
					count_l++;
			}
			if (count_r >= retry_cnt || count_l >= retry_cnt) {
				chip->dischg_flag = true;
				condition1 = true;
				dev_err(chg->dev,
					"%s: over-temp detected [%d, %d]\n",
					__func__, chip->usb_temp_l,
					chip->usb_temp_r);
			}
			count_r = 1;
			count_l = 1;
			count = 0;
			last_usb_temp_r = chip->usb_temp_r;
			last_usb_temp_l = chip->usb_temp_l;
		}

		if (condition1)
			goto dischg;

		/* Condition 2: temperature rising too fast */
		if (((chip->usb_temp_l - chip->tbatt_temp / 10) >= 8 &&
		     chip->usb_temp_l < USB_100C) ||
		    ((chip->usb_temp_r - chip->tbatt_temp / 10) >= 8 &&
		     chip->usb_temp_r < USB_100C)) {
			if (count == 0) {
				last_usb_temp_r = chip->usb_temp_r;
				last_usb_temp_l = chip->usb_temp_l;
			} else {
				current_temp_r = chip->usb_temp_r;
				current_temp_l = chip->usb_temp_l;
			}
			if ((current_temp_l - last_usb_temp_l) >= 3 ||
			    (current_temp_r - last_usb_temp_r) >= 3) {
				for (i = 1; i <= retry_cnt; i++) {
					mdelay(RETRY_CNT_DELAY);
					chip->chg_ops->get_usbtemp_volt(chip);
					if ((chip->usb_temp_r -
					     last_usb_temp_r) >= 3 &&
					    chip->usb_temp_r < USB_100C)
						count_r++;
					if ((chip->usb_temp_l -
					     last_usb_temp_l) >= 3 &&
					    chip->usb_temp_l < USB_100C)
						count_l++;
				}
				if (count_l >= retry_cnt ||
				    count_r >= retry_cnt) {
					chip->dischg_flag = true;
					condition2 = true;
					dev_err(chg->dev,
						"%s: temp rising too fast\n",
						__func__);
				}
				count_r = 1;
				count_l = 1;
			}
			count++;
			if (count > total_count)
				count = 0;
		} else {
			count = 0;
			last_usb_temp_r = chip->usb_temp_r;
			last_usb_temp_l = chip->usb_temp_l;
		}

dischg:
		/* Auto-recover if temps back to normal */
		if ((chip->usb_temp_l < USB_30C ||
		     chip->usb_temp_l > USB_100C) &&
		    (chip->usb_temp_r < USB_30C ||
		     chip->usb_temp_r > USB_100C)) {
			condition1 = false;
			condition2 = false;
			chip->dischg_flag = false;
		}

		/* Execute discharge action on trigger */
		if ((condition1 || condition2) && chip->dischg_flag) {
			oplus_kona_usbtemp_dischg_action(chip);
			condition1 = false;
			condition2 = false;
			count = 0;
			last_usb_temp_r = chip->usb_temp_r;
			last_usb_temp_l = chip->usb_temp_l;
		}

		chip->usbtemp_check = false;
		msleep(delay);
	}

	if (kona->usbtemp_ws) {
		wakeup_source_unregister(kona->usbtemp_ws);
		kona->usbtemp_ws = NULL;
	}

	dev_info(chg->dev, "%s: USB temp monitor thread stopped\n", __func__);

	return 0;
}

/**
 * oplus_kona_usbtemp_thread_init() - Start the USB temperature monitoring thread.
 *
 * Ported from 4.19 oplus_usbtemp_thread_init().
 * 4.19 used g_oplus_chip global; 5.10 uses oplus_kona_chip.
 *
 * Creates a kernel thread running oplus_kona_usbtemp_monitor_common().
 */
static void oplus_kona_usbtemp_thread_init(void)
{
	struct oplus_kona_chg *kona;
	struct task_struct *kthread;

	mutex_lock(&oplus_kona_lock);
	kona = oplus_kona_chip;
	mutex_unlock(&oplus_kona_lock);

	if (!kona) {
		pr_err("[OPLUS_CHG] %s: kona chip not ready\n", __func__);
		return;
	}

	init_waitqueue_head(&kona->usbtemp_wq);

	kthread = kthread_run(oplus_kona_usbtemp_monitor_common,
			      &kona->oplus_chip, "usbtemp_kthread");
	if (IS_ERR(kthread)) {
		dev_err(kona->dev, "%s: failed to create usbtemp kthread, err=%ld\n",
			__func__, PTR_ERR(kthread));
		return;
	}

	kona->usbtemp_kthread = kthread;

	dev_info(kona->dev, "%s: USB temp thread initialized\n", __func__);
}

/**
 * oplus_kona_wake_up_usbtemp_thread() - Wake the USB temperature monitoring thread.
 *
 * Ported from 4.19 oplus_wake_up_usbtemp_thread().
 * Called from IRQ handlers and state transitions to re-evaluate USB temp.
 *
 * Key 5.10 migration: uses pm_wakeup_event() instead of the removed wake_lock API.
 */
static __maybe_unused void oplus_kona_wake_up_usbtemp_thread(void)
{
	struct oplus_kona_chg *kona;
	struct oplus_chg_chip *chip;

	mutex_lock(&oplus_kona_lock);
	kona = oplus_kona_chip;
	mutex_unlock(&oplus_kona_lock);

	if (!kona)
		return;

	chip = &kona->oplus_chip;

	if (chip->chg_ops && chip->chg_ops->oplus_usbtemp_monitor_condition) {
		chip->usbtemp_check =
			chip->chg_ops->oplus_usbtemp_monitor_condition(chip);
		if (chip->usbtemp_check)
			wake_up_interruptible(&kona->usbtemp_wq);
	}

	/*
	 * 4.19 used wake_lock_timeout() — removed in 5.10.
	 * 5.10 equivalent: pm_wakeup_event() to prevent suspend while
	 * the thread processes the temp check.
	 */
	if (chip->usbtemp_check)
		pm_wakeup_event(kona->dev, 2000 /* 2 seconds */);
}

/* ===== GPIO / Pinctrl DT Parsing ===== */

/**
 * oplus_kona_parse_ccdetect_dt() - Parse CC-detect GPIO from Device Tree.
 * @dev: device for devm allocations.
 * @gpio: pointer to oplus_kona_gpio structure to fill.
 *
 * DT property: "oplus,ccdetect-gpio"
 * - 4.19 used of_get_named_gpio() -> int; 5.10 uses devm_gpiod_get() -> gpio_desc *
 * - Pinctrl: ccdetect_active, ccdetect_sleep
 *
 * Return: 0 on success, negative errno on error.
 */
static int oplus_kona_parse_ccdetect_dt(struct device *dev,
					struct oplus_kona_gpio *gpio)
{
	struct gpio_desc *desc;

	desc = devm_gpiod_get(dev, "ccdetect", GPIOD_IN);
	if (IS_ERR(desc)) {
		int rc = PTR_ERR(desc);

		if (rc == -ENOENT) {
			/* GPIO is optional — gracefully skip */
			dev_info(dev, "ccdetect-gpio not specified in DT\n");
			gpio->ccdetect_gpio = NULL;
			return 0;
		}
		dev_err(dev, "Unable to get ccdetect-gpio: %d\n", rc);
		return rc;
	}
	gpio->ccdetect_gpio = desc;

	gpio->ccdetect_pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR_OR_NULL(gpio->ccdetect_pinctrl)) {
		dev_info(dev, "ccdetect pinctrl not found, skipping pinctrl\n");
		gpio->ccdetect_pinctrl = NULL;
		gpio->ccdetect_active = NULL;
		gpio->ccdetect_sleep = NULL;
		gpiod_direction_input(gpio->ccdetect_gpio);
		gpio->ccdetect_irq = gpiod_to_irq(gpio->ccdetect_gpio);
		dev_info(dev, "ccdetect GPIO parsed (no pinctrl), irq=%d\n",
			 gpio->ccdetect_irq);
		return 0;
	}

	gpio->ccdetect_active =
		pinctrl_lookup_state(gpio->ccdetect_pinctrl, "ccdetect_active");
	if (IS_ERR_OR_NULL(gpio->ccdetect_active)) {
		dev_info(dev, "ccdetect_active not found, skipping pinctrl\n");
		gpio->ccdetect_active = NULL;
		gpio->ccdetect_sleep = NULL;
		gpiod_direction_input(gpio->ccdetect_gpio);
		gpio->ccdetect_irq = gpiod_to_irq(gpio->ccdetect_gpio);
		dev_info(dev, "ccdetect GPIO parsed (partial pinctrl), irq=%d\n",
			 gpio->ccdetect_irq);
		return 0;
	}

	gpio->ccdetect_sleep =
		pinctrl_lookup_state(gpio->ccdetect_pinctrl, "ccdetect_sleep");
	if (IS_ERR_OR_NULL(gpio->ccdetect_sleep)) {
		dev_info(dev, "ccdetect_sleep not found, using active as fallback\n");
		gpio->ccdetect_sleep = gpio->ccdetect_active;
	}

	/* Set input direction and activate pinctrl */
	gpiod_direction_input(gpio->ccdetect_gpio);
	pinctrl_select_state(gpio->ccdetect_pinctrl, gpio->ccdetect_active);

	gpio->ccdetect_irq = gpiod_to_irq(gpio->ccdetect_gpio);
	dev_info(dev, "ccdetect GPIO parsed, irq=%d\n", gpio->ccdetect_irq);

	return 0;
}

/**
 * oplus_kona_parse_usbtemp_dt() - Parse USB temperature ADC pinmux from DT.
 * @dev: device for devm allocations.
 * @gpio: pointer to oplus_kona_gpio structure to fill.
 *
 * Three separate pinmux groups: gpio1_adc, gpio8_adc, gpio5_adc.
 * Each only has a pinctrl (no GPIO input — used as ADC channel).
 *
 * Return: 0 on success, negative errno on error.
 */
static int oplus_kona_parse_usbtemp_dt(struct device *dev,
				       struct oplus_kona_gpio *gpio)
{

	/* --- GPIO1 ADC --- */
	gpio->usbtemp_gpio1_adc_pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR_OR_NULL(gpio->usbtemp_gpio1_adc_pinctrl)) {
		dev_info(dev, "usbtemp_gpio1 pinctrl not found, skipping\n");
		gpio->usbtemp_gpio1_adc_pinctrl = NULL;
		gpio->usbtemp_gpio1_default = NULL;
		/* continue with remaining channels */
	} else {
		gpio->usbtemp_gpio1_default =
			pinctrl_lookup_state(gpio->usbtemp_gpio1_adc_pinctrl,
					     "gpio1_adc_default");
		if (IS_ERR_OR_NULL(gpio->usbtemp_gpio1_default)) {
			dev_info(dev, "usbtemp gpio1_adc_default not found, skipping\n");
			gpio->usbtemp_gpio1_default = NULL;
		} else {
			pinctrl_select_state(gpio->usbtemp_gpio1_adc_pinctrl,
					     gpio->usbtemp_gpio1_default);
		}
	}

	/* --- GPIO8 ADC --- */
	gpio->usbtemp_gpio8_adc_pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR_OR_NULL(gpio->usbtemp_gpio8_adc_pinctrl)) {
		dev_info(dev, "usbtemp_gpio8 pinctrl not found, skipping\n");
		gpio->usbtemp_gpio8_adc_pinctrl = NULL;
		gpio->usbtemp_gpio8_default = NULL;
	} else {
		gpio->usbtemp_gpio8_default =
			pinctrl_lookup_state(gpio->usbtemp_gpio8_adc_pinctrl,
					     "gpio8_adc_default");
		if (IS_ERR_OR_NULL(gpio->usbtemp_gpio8_default)) {
			dev_info(dev, "usbtemp gpio8_adc_default not found, skipping\n");
			gpio->usbtemp_gpio8_default = NULL;
		} else {
			pinctrl_select_state(gpio->usbtemp_gpio8_adc_pinctrl,
					     gpio->usbtemp_gpio8_default);
		}
	}

	/* --- GPIO5 ADC --- */
	gpio->usbtemp_gpio5_adc_pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR_OR_NULL(gpio->usbtemp_gpio5_adc_pinctrl)) {
		dev_info(dev, "usbtemp_gpio5 pinctrl not found, skipping\n");
		gpio->usbtemp_gpio5_adc_pinctrl = NULL;
		gpio->usbtemp_gpio5_default = NULL;
	} else {
		gpio->usbtemp_gpio5_default =
			pinctrl_lookup_state(gpio->usbtemp_gpio5_adc_pinctrl,
					     "gpio5_adc_default");
		if (IS_ERR_OR_NULL(gpio->usbtemp_gpio5_default)) {
			dev_info(dev, "usbtemp gpio5_adc_default not found, skipping\n");
			gpio->usbtemp_gpio5_default = NULL;
		} else {
			pinctrl_select_state(gpio->usbtemp_gpio5_adc_pinctrl,
					     gpio->usbtemp_gpio5_default);
		}
	}

	dev_info(dev, "usbtemp ADC GPIOs parsed\n");

	return 0;
}

/**
 * oplus_kona_parse_shipmode_dt() - Parse ship-mode ID GPIO from DT.
 * @dev: device for devm allocations.
 * @gpio: pointer to oplus_kona_gpio structure to fill.
 *
 * DT property: "oplus,shipmode-id-gpio"
 * Pinctrl: shipmode_id_active
 *
 * Return: 0 on success, negative errno on error.
 */
static int oplus_kona_parse_shipmode_dt(struct device *dev,
					struct oplus_kona_gpio *gpio)
{
	struct gpio_desc *desc;

	desc = devm_gpiod_get(dev, "shipmode-id", GPIOD_IN);
	if (IS_ERR(desc)) {
		int rc = PTR_ERR(desc);

		if (rc == -ENOENT) {
			dev_info(dev, "shipmode-id-gpio not specified in DT\n");
			gpio->shipmode_id_gpio = NULL;
			return 0;
		}
		dev_err(dev, "Unable to get shipmode-id-gpio: %d\n", rc);
		return rc;
	}
	gpio->shipmode_id_gpio = desc;

	gpio->shipmode_id_pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR_OR_NULL(gpio->shipmode_id_pinctrl)) {
		dev_info(dev, "shipmode_id pinctrl not found, skipping pinctrl\n");
		gpio->shipmode_id_pinctrl = NULL;
		gpio->shipmode_id_active = NULL;
		/* GPIO descriptor is still valid, continue */
		gpiod_direction_input(gpio->shipmode_id_gpio);
		dev_info(dev, "shipmode-id GPIO parsed (no pinctrl)\n");
		return 0;
	}

	gpio->shipmode_id_active =
		pinctrl_lookup_state(gpio->shipmode_id_pinctrl,
				     "shipmode_id_active");
	if (IS_ERR_OR_NULL(gpio->shipmode_id_active)) {
		/* fallback: DTS uses "ship_active" instead */
		gpio->shipmode_id_active =
			pinctrl_lookup_state(gpio->shipmode_id_pinctrl,
					     "ship_active");
		if (IS_ERR_OR_NULL(gpio->shipmode_id_active)) {
			dev_info(dev, "shipmode_id_active not found (both tried), skipping pinctrl\n");
			gpio->shipmode_id_active = NULL;
			gpiod_direction_input(gpio->shipmode_id_gpio);
			dev_info(dev, "shipmode-id GPIO parsed (no pinctrl)\n");
			return 0;
		}
		dev_info(dev, "shipmode_id pinctrl: using ship_active fallback\n");
	}

	gpiod_direction_input(gpio->shipmode_id_gpio);
	pinctrl_select_state(gpio->shipmode_id_pinctrl,
			     gpio->shipmode_id_active);

	dev_info(dev, "shipmode-id GPIO parsed\n");

	return 0;
}

/**
 * oplus_kona_parse_wired_conn_dt() - Parse wired-connector-detect GPIO from DT.
 * @dev: device for devm allocations.
 * @gpio: pointer to oplus_kona_gpio structure to fill.
 *
 * DT property: "oplus,wired-conn-gpio"
 * Pinctrl: wired_con_int_active, wired_con_int_sleep
 *
 * Return: 0 on success, negative errno on error.
 */
static int oplus_kona_parse_wired_conn_dt(struct device *dev,
					  struct oplus_kona_gpio *gpio)
{
	struct gpio_desc *desc;

	desc = devm_gpiod_get(dev, "wired-conn", GPIOD_IN);
	if (IS_ERR(desc)) {
		int rc = PTR_ERR(desc);

		if (rc == -ENOENT) {
			dev_info(dev, "wired-conn-gpio not specified in DT\n");
			gpio->wired_conn_gpio = NULL;
			return 0;
		}
		dev_err(dev, "Unable to get wired-conn-gpio: %d\n", rc);
		return rc;
	}
	gpio->wired_conn_gpio = desc;

	gpio->wired_conn_pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR_OR_NULL(gpio->wired_conn_pinctrl)) {
		dev_info(dev, "wired_conn pinctrl not found, skipping pinctrl\n");
		gpio->wired_conn_pinctrl = NULL;
		gpio->wired_conn_active = NULL;
		gpio->wired_conn_sleep = NULL;
		gpiod_direction_input(gpio->wired_conn_gpio);
		gpio->wired_conn_irq = gpiod_to_irq(gpio->wired_conn_gpio);
		dev_info(dev, "wired-conn GPIO parsed (no pinctrl), irq=%d\n",
			 gpio->wired_conn_irq);
		return 0;
	}

	gpio->wired_conn_active =
		pinctrl_lookup_state(gpio->wired_conn_pinctrl,
				     "wired_con_int_active");
	if (IS_ERR_OR_NULL(gpio->wired_conn_active)) {
		dev_info(dev, "wired_conn active pinctrl not found, skipping\n");
		gpio->wired_conn_active = NULL;
		gpio->wired_conn_sleep = NULL;
		gpiod_direction_input(gpio->wired_conn_gpio);
		gpio->wired_conn_irq = gpiod_to_irq(gpio->wired_conn_gpio);
		dev_info(dev, "wired-conn GPIO parsed (partial pinctrl), irq=%d\n",
			 gpio->wired_conn_irq);
		return 0;
	}

	gpio->wired_conn_sleep =
		pinctrl_lookup_state(gpio->wired_conn_pinctrl,
				     "wired_con_int_sleep");
	if (IS_ERR_OR_NULL(gpio->wired_conn_sleep)) {
		dev_info(dev, "wired_conn sleep not found, using active as fallback\n");
		gpio->wired_conn_sleep = gpio->wired_conn_active;
	}

	gpiod_direction_input(gpio->wired_conn_gpio);
	pinctrl_select_state(gpio->wired_conn_pinctrl,
			     gpio->wired_conn_active);

	gpio->wired_conn_irq = gpiod_to_irq(gpio->wired_conn_gpio);
	dev_info(dev, "wired-conn GPIO parsed, irq=%d\n", gpio->wired_conn_irq);

	return 0;
}

/**
 * oplus_kona_parse_otg_en_dt() - Parse OTG enable GPIO from DT.
 * @dev: device for devm allocations.
 * @gpio: pointer to oplus_kona_gpio structure to fill.
 *
 * DT property: "oplus,otg-en-gpio"
 * Pinctrl: otg_en_active, otg_en_sleep, otg_en_default
 *
 * Return: 0 on success, negative errno on error.
 */
static int oplus_kona_parse_otg_en_dt(struct device *dev,
				      struct oplus_kona_gpio *gpio)
{
	struct gpio_desc *desc;

	desc = devm_gpiod_get(dev, "otg-en", GPIOD_OUT_LOW);
	if (IS_ERR(desc)) {
		int rc = PTR_ERR(desc);

		if (rc == -ENOENT) {
			dev_info(dev, "otg-en-gpio not specified in DT\n");
			gpio->otg_en_gpio = NULL;
			return 0;
		}
		dev_err(dev, "Unable to get otg-en-gpio: %d\n", rc);
		return rc;
	}
	gpio->otg_en_gpio = desc;

	gpio->otg_en_pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR_OR_NULL(gpio->otg_en_pinctrl)) {
		dev_info(dev, "otg_en pinctrl not found, skipping pinctrl\n");
		gpio->otg_en_pinctrl = NULL;
		gpio->otg_en_active = NULL;
		gpio->otg_en_sleep = NULL;
		gpio->otg_en_default = NULL;
		gpiod_direction_output(gpio->otg_en_gpio, 0);
		dev_info(dev, "otg-en GPIO parsed (no pinctrl)\n");
		return 0;
	}

	gpio->otg_en_active =
		pinctrl_lookup_state(gpio->otg_en_pinctrl, "otg_en_active");
	if (IS_ERR_OR_NULL(gpio->otg_en_active)) {
		dev_info(dev, "otg_en_active not found, skipping pinctrl states\n");
		gpio->otg_en_active = NULL;
		gpio->otg_en_sleep = NULL;
		gpio->otg_en_default = NULL;
		gpiod_direction_output(gpio->otg_en_gpio, 0);
		dev_info(dev, "otg-en GPIO parsed (partial pinctrl)\n");
		return 0;
	}

	gpio->otg_en_sleep =
		pinctrl_lookup_state(gpio->otg_en_pinctrl, "otg_en_sleep");
	if (IS_ERR_OR_NULL(gpio->otg_en_sleep)) {
		dev_info(dev, "otg_en_sleep not found, skipping pinctrl\n");
		gpio->otg_en_sleep = NULL;
		gpio->otg_en_default = NULL;
		gpiod_direction_output(gpio->otg_en_gpio, 0);
		dev_info(dev, "otg-en GPIO parsed (partial pinctrl)\n");
		return 0;
	}

	gpio->otg_en_default =
		pinctrl_lookup_state(gpio->otg_en_pinctrl, "otg_en_default");
	if (IS_ERR_OR_NULL(gpio->otg_en_default)) {
		dev_info(dev, "otg_en_default not found, using sleep as default\n");
		gpio->otg_en_default = gpio->otg_en_sleep;
	}

	/* 4.19 default: select sleep state at init */
	pinctrl_select_state(gpio->otg_en_pinctrl, gpio->otg_en_sleep);

	dev_info(dev, "otg-en GPIO parsed\n");

	return 0;
}

/**
 * oplus_kona_parse_idt_en_dt() - Parse IDT enable (wireless charger) GPIO from DT.
 * @dev: device for devm allocations.
 * @gpio: pointer to oplus_kona_gpio structure to fill.
 *
 * DT property: "oplus,idt-en-gpio"
 * Pinctrl: idt_en_active, idt_en_sleep, idt_en_default
 *
 * Return: 0 on success, negative errno on error.
 */
static int oplus_kona_parse_idt_en_dt(struct device *dev,
				      struct oplus_kona_gpio *gpio)
{
	struct gpio_desc *desc;

	desc = devm_gpiod_get(dev, "idt-en", GPIOD_OUT_LOW);
	if (IS_ERR(desc)) {
		int rc = PTR_ERR(desc);

		if (rc == -ENOENT) {
			dev_info(dev, "idt-en-gpio not specified in DT\n");
			gpio->idt_en_gpio = NULL;
			return 0;
		}
		dev_err(dev, "Unable to get idt-en-gpio: %d\n", rc);
		return rc;
	}
	gpio->idt_en_gpio = desc;

	gpio->idt_en_pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR_OR_NULL(gpio->idt_en_pinctrl)) {
		dev_info(dev, "idt_en pinctrl not found, skipping pinctrl\n");
		gpio->idt_en_pinctrl = NULL;
		gpio->idt_en_active = NULL;
		gpio->idt_en_sleep = NULL;
		gpio->idt_en_default = NULL;
		gpiod_direction_output(gpio->idt_en_gpio, 0);
		dev_info(dev, "idt-en GPIO parsed (no pinctrl)\n");
		return 0;
	}

	gpio->idt_en_active =
		pinctrl_lookup_state(gpio->idt_en_pinctrl, "idt_en_active");
	if (IS_ERR_OR_NULL(gpio->idt_en_active)) {
		dev_info(dev, "idt_en_active not found, skipping pinctrl states\n");
		gpio->idt_en_active = NULL;
		gpio->idt_en_sleep = NULL;
		gpio->idt_en_default = NULL;
		gpiod_direction_output(gpio->idt_en_gpio, 0);
		dev_info(dev, "idt-en GPIO parsed (partial pinctrl)\n");
		return 0;
	}

	gpio->idt_en_sleep =
		pinctrl_lookup_state(gpio->idt_en_pinctrl, "idt_en_sleep");
	if (IS_ERR_OR_NULL(gpio->idt_en_sleep)) {
		dev_info(dev, "idt_en_sleep not found, skipping pinctrl\n");
		gpio->idt_en_sleep = NULL;
		gpio->idt_en_default = NULL;
		gpiod_direction_output(gpio->idt_en_gpio, 0);
		dev_info(dev, "idt-en GPIO parsed (partial pinctrl)\n");
		return 0;
	}

	gpio->idt_en_default =
		pinctrl_lookup_state(gpio->idt_en_pinctrl, "idt_en_default");
	if (IS_ERR_OR_NULL(gpio->idt_en_default)) {
		dev_info(dev, "idt_en_default not found, using sleep as default\n");
		gpio->idt_en_default = gpio->idt_en_sleep;
	}

	gpiod_direction_output(gpio->idt_en_gpio, 0);
	pinctrl_select_state(gpio->idt_en_pinctrl, gpio->idt_en_default);

	dev_info(dev, "idt-en GPIO parsed\n");

	return 0;
}

/**
 * oplus_kona_parse_wrx_en_dt() - Parse WRX enable (wireless receiver) GPIO from DT.
 * @dev: device for devm allocations.
 * @gpio: pointer to oplus_kona_gpio structure to fill.
 *
 * DT property: "oplus,wrx-en-gpio"
 * Pinctrl: wrx_en_active, wrx_en_sleep, wrx_en_default
 *
 * Return: 0 on success, negative errno on error.
 */
static int oplus_kona_parse_wrx_en_dt(struct device *dev,
				      struct oplus_kona_gpio *gpio)
{
	struct gpio_desc *desc;

	desc = devm_gpiod_get(dev, "wrx-en", GPIOD_OUT_LOW);
	if (IS_ERR(desc)) {
		int rc = PTR_ERR(desc);

		if (rc == -ENOENT) {
			dev_info(dev, "wrx-en-gpio not specified in DT\n");
			gpio->wrx_en_gpio = NULL;
			return 0;
		}
		dev_err(dev, "Unable to get wrx-en-gpio: %d\n", rc);
		return rc;
	}
	gpio->wrx_en_gpio = desc;

	gpio->wrx_en_pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR_OR_NULL(gpio->wrx_en_pinctrl)) {
		dev_info(dev, "wrx_en pinctrl not found, skipping pinctrl\n");
		gpio->wrx_en_pinctrl = NULL;
		gpio->wrx_en_active = NULL;
		gpio->wrx_en_sleep = NULL;
		gpio->wrx_en_default = NULL;
		gpiod_direction_output(gpio->wrx_en_gpio, 0);
		dev_info(dev, "wrx-en GPIO parsed (no pinctrl)\n");
		return 0;
	}

	gpio->wrx_en_active =
		pinctrl_lookup_state(gpio->wrx_en_pinctrl, "wrx_en_active");
	if (IS_ERR_OR_NULL(gpio->wrx_en_active)) {
		dev_info(dev, "wrx_en_active not found, skipping pinctrl\n");
		gpio->wrx_en_active = NULL;
		gpio->wrx_en_sleep = NULL;
		gpio->wrx_en_default = NULL;
		gpiod_direction_output(gpio->wrx_en_gpio, 0);
		dev_info(dev, "wrx-en GPIO parsed (partial pinctrl)\n");
		return 0;
	}

	gpio->wrx_en_sleep =
		pinctrl_lookup_state(gpio->wrx_en_pinctrl, "wrx_en_sleep");
	if (IS_ERR_OR_NULL(gpio->wrx_en_sleep)) {
		dev_info(dev, "wrx_en_sleep not found, skipping pinctrl\n");
		gpio->wrx_en_sleep = NULL;
		gpio->wrx_en_default = NULL;
		gpiod_direction_output(gpio->wrx_en_gpio, 0);
		dev_info(dev, "wrx-en GPIO parsed (partial pinctrl)\n");
		return 0;
	}

	gpio->wrx_en_default =
		pinctrl_lookup_state(gpio->wrx_en_pinctrl, "wrx_en_default");
	if (IS_ERR_OR_NULL(gpio->wrx_en_default)) {
		dev_info(dev, "wrx_en_default not found, using sleep as default\n");
		gpio->wrx_en_default = gpio->wrx_en_sleep;
	}

	gpiod_direction_output(gpio->wrx_en_gpio, 0);
	pinctrl_select_state(gpio->wrx_en_pinctrl, gpio->wrx_en_default);

	dev_info(dev, "wrx-en GPIO parsed\n");

	return 0;
}

/**
 * oplus_kona_parse_wrx_otg_dt() - Parse WRX OTG (wireless receiver OTG) GPIO from DT.
 * @dev: device for devm allocations.
 * @gpio: pointer to oplus_kona_gpio structure to fill.
 *
 * DT property: "oplus,wrx-otg-gpio"
 * Pinctrl: wrx_otg_active, wrx_otg_sleep
 *
 * Return: 0 on success, negative errno on error.
 */
static int oplus_kona_parse_wrx_otg_dt(struct device *dev,
				       struct oplus_kona_gpio *gpio)
{
	struct gpio_desc *desc;

	desc = devm_gpiod_get(dev, "wrx-otg", GPIOD_OUT_LOW);
	if (IS_ERR(desc)) {
		int rc = PTR_ERR(desc);

		if (rc == -ENOENT) {
			dev_info(dev, "wrx-otg-gpio not specified in DT\n");
			gpio->wrx_otg_gpio = NULL;
			return 0;
		}
		dev_err(dev, "Unable to get wrx-otg-gpio: %d\n", rc);
		return rc;
	}
	gpio->wrx_otg_gpio = desc;

	gpio->wrx_otg_pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR_OR_NULL(gpio->wrx_otg_pinctrl)) {
		dev_info(dev, "wrx_otg pinctrl not found, skipping pinctrl\n");
		gpio->wrx_otg_pinctrl = NULL;
		gpio->wrx_otg_active = NULL;
		gpio->wrx_otg_sleep = NULL;
		gpiod_direction_output(gpio->wrx_otg_gpio, 0);
		dev_info(dev, "wrx-otg GPIO parsed (no pinctrl)\n");
		return 0;
	}

	gpio->wrx_otg_active =
		pinctrl_lookup_state(gpio->wrx_otg_pinctrl, "wrx_otg_active");
	if (IS_ERR_OR_NULL(gpio->wrx_otg_active)) {
		dev_info(dev, "wrx_otg_active not found, skipping pinctrl\n");
		gpio->wrx_otg_active = NULL;
		gpio->wrx_otg_sleep = NULL;
		gpiod_direction_output(gpio->wrx_otg_gpio, 0);
		dev_info(dev, "wrx-otg GPIO parsed (partial pinctrl)\n");
		return 0;
	}

	gpio->wrx_otg_sleep =
		pinctrl_lookup_state(gpio->wrx_otg_pinctrl, "wrx_otg_sleep");
	if (IS_ERR_OR_NULL(gpio->wrx_otg_sleep)) {
		dev_info(dev, "wrx_otg_sleep not found, using active as fallback\n");
		gpio->wrx_otg_sleep = gpio->wrx_otg_active;
	}

	gpiod_direction_output(gpio->wrx_otg_gpio, 0);
	pinctrl_select_state(gpio->wrx_otg_pinctrl, gpio->wrx_otg_sleep);

	dev_info(dev, "wrx-otg GPIO parsed\n");

	return 0;
}

/**
 * oplus_kona_gpio_parse_dt() - Parse all GPIO/pinctrl properties from DT.
 * @dev: device for devm allocations.
 * @kona: kona charger context to populate.
 *
 * Top-level dispatcher: calls each individual parse function.
 * Errors from optional GPIOs are silently ignored; errors from required
 * infrastructure propagate upward.
 *
 * Return: 0 on success, negative errno on error.
 */
static int oplus_kona_gpio_parse_dt(struct device *dev,
				    struct oplus_kona_chg *kona)
{
	int rc;

	kona->gpio = devm_kzalloc(dev, sizeof(*kona->gpio), GFP_KERNEL);
	if (!kona->gpio)
		return -ENOMEM;

	rc = oplus_kona_parse_ccdetect_dt(dev, kona->gpio);
	if (rc < 0)
		return rc;

	rc = oplus_kona_parse_usbtemp_dt(dev, kona->gpio);
	if (rc < 0)
		return rc;

	rc = oplus_kona_parse_shipmode_dt(dev, kona->gpio);
	if (rc < 0)
		return rc;

	rc = oplus_kona_parse_wired_conn_dt(dev, kona->gpio);
	if (rc < 0)
		return rc;

	rc = oplus_kona_parse_otg_en_dt(dev, kona->gpio);
	if (rc < 0)
		return rc;

	rc = oplus_kona_parse_idt_en_dt(dev, kona->gpio);
	if (rc < 0)
		return rc;

	rc = oplus_kona_parse_wrx_en_dt(dev, kona->gpio);
	if (rc < 0)
		return rc;

	rc = oplus_kona_parse_wrx_otg_dt(dev, kona->gpio);
	if (rc < 0)
		return rc;

	dev_info(dev, "OPLUS Kona GPIO DT parsing complete\n");

	return 0;
}

/* ===== Cleanup ===== */

static void oplus_battery_kona_deinit_action(void *data)
{
	struct oplus_kona_chg *kona = data;

	/* Stop USB temperature monitoring thread */
	if (kona->usbtemp_kthread) {
		kthread_stop(kona->usbtemp_kthread);
		kona->usbtemp_kthread = NULL;
	}

	/* Release OPLUS-specific IIO channels */
	if (!IS_ERR_OR_NULL(kona->chgid_v_chan))
		iio_channel_release(kona->chgid_v_chan);
	if (!IS_ERR_OR_NULL(kona->usbtemp_v_chan))
		iio_channel_release(kona->usbtemp_v_chan);
	if (!IS_ERR_OR_NULL(kona->usbtemp_sup_v_chan))
		iio_channel_release(kona->usbtemp_sup_v_chan);

	oplus_chg_deinit(&kona->oplus_chip);

	mutex_lock(&oplus_kona_lock);
	if (oplus_kona_chip == kona)
		oplus_kona_chip = NULL;
	mutex_unlock(&oplus_kona_lock);
}

/* ===== Init ===== */

/**
 * oplus_battery_kona_init() - Initialize OPLUS Kona SMB5 charger adapter.
 * @dev: device owning the SMB5 charger.
 * @chg: verified SMB5 charger context from smb5_probe().
 *
 * Allocates adapter state, fills oplus_chg_chip with real P0 ops, and
 * calls oplus_chg_init() to start the charging state machine.
 *
 * Return: 0 on success, or a negative errno.
 */
int oplus_battery_kona_init(struct device *dev, struct smb_charger *chg)
{
	struct oplus_kona_chg *kona;
	int rc;

	if (!dev || !chg)
		return -EINVAL;

	kona = devm_kzalloc(dev, sizeof(*kona), GFP_KERNEL);
	if (!kona)
		return -ENOMEM;

	kona->dev = dev;
	kona->chg = chg;
	kona->oplus_chip.dev = dev;
	kona->oplus_chip.chg_ops = &kona_smb5_chg_ops;
	kona->oplus_chip.usb_temp_l = 25;
	kona->oplus_chip.usb_temp_r = 25;
	kona->oplus_chip.tbatt_temp = 250;
	kona->oplus_chip.usbtemp_temp_up_time_thr = 30;
	kona->oplus_chip.usbtemp_max_temp_thr = USB_57C;

	mutex_lock(&oplus_kona_lock);
	if (oplus_kona_chip) {
		mutex_unlock(&oplus_kona_lock);
		return -EBUSY;
	}
	oplus_kona_chip = kona;
	mutex_unlock(&oplus_kona_lock);

	rc = devm_add_action_or_reset(dev, oplus_battery_kona_deinit_action,
				      kona);
	if (rc < 0)
		return rc;

	/* GPIO / Pinctrl DT parsing */
	rc = oplus_kona_gpio_parse_dt(dev, kona);
	if (rc < 0)
		return rc;

	/* Acquire OPLUS-specific IIO channels from DT io-channel-names.
	 * These are NOT part of the upstream smb_iio struct — they are
	 * OPLUS-only ADC channels declared in the 8T device tree.
	 * Ported from 4.19 oplus_battery_msm8250.c probe path.
	 */
	rc = of_property_match_string(dev->of_node, "io-channel-names",
				      "chgID_voltage_adc");
	if (rc >= 0) {
		kona->chgid_v_chan = iio_channel_get(dev,
						     "chgID_voltage_adc");
		if (IS_ERR(kona->chgid_v_chan)) {
			rc = PTR_ERR(kona->chgid_v_chan);
			if (rc != -EPROBE_DEFER)
				dev_err(dev, "chgid_v_chan get error, %d\n",
					rc);
			kona->chgid_v_chan = NULL;
		}
	}

	rc = of_property_match_string(dev->of_node, "io-channel-names",
				      "usb_temp_adc");
	if (rc >= 0) {
		kona->usbtemp_v_chan = iio_channel_get(dev, "usb_temp_adc");
		if (IS_ERR(kona->usbtemp_v_chan)) {
			rc = PTR_ERR(kona->usbtemp_v_chan);
			if (rc != -EPROBE_DEFER)
				dev_err(dev, "usb_temp_adc get error, %d\n",
					rc);
			kona->usbtemp_v_chan = NULL;
		}
	}

	rc = of_property_match_string(dev->of_node, "io-channel-names",
				      "usb_supplementary_temp_adc");
	if (rc >= 0) {
		kona->usbtemp_sup_v_chan = iio_channel_get(dev,
					"usb_supplementary_temp_adc");
		if (IS_ERR(kona->usbtemp_sup_v_chan)) {
			rc = PTR_ERR(kona->usbtemp_sup_v_chan);
			if (rc != -EPROBE_DEFER)
				dev_err(dev,
					"usb_supplementary_temp_adc get error, %d\n",
					rc);
			kona->usbtemp_sup_v_chan = NULL;
		}
	}

	rc = oplus_chg_init(&kona->oplus_chip);
	if (rc < 0)
		return rc;

	/* Start USB temperature monitoring thread */
	oplus_kona_usbtemp_thread_init();

	dev_info(dev, "OPLUS Kona SMB5 charger adapter initialized\n");

	return 0;
}
EXPORT_SYMBOL_GPL(oplus_battery_kona_init);
