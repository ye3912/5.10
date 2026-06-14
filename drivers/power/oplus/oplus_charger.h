/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * OPLUS charger header stub for 5.10 port
 * Original: drivers/power/oplus/oplus_charger.h (4.19)
 *
 * Provides minimal declarations needed by oplus_qpnp_qg.c
 * Full OPLUS charger framework integration is deferred.
 */

#ifndef _OPLUS_CHARGER_H_
#define _OPLUS_CHARGER_H_

#include <linux/power_supply.h>
#include <linux/atomic.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <linux/version.h>

/* Forward declarations — full definitions ported in Phase 3 */
struct oplus_chg_operations;

/* ===== Layer 3B normal-wired state enums ===== */
enum oplus_chg_tbatt_status {
	BATTERY_STATUS__NORMAL = 0,
	BATTERY_STATUS__REMOVED,
	BATTERY_STATUS__LOW_TEMP,
	BATTERY_STATUS__HIGH_TEMP,
	BATTERY_STATUS__COLD_TEMP,
	BATTERY_STATUS__LITTLE_COLD_TEMP,
	BATTERY_STATUS__COOL_TEMP,
	BATTERY_STATUS__LITTLE_COOL_TEMP,
	BATTERY_STATUS__WARM_TEMP,
	BATTERY_STATUS__INVALID,
};

enum oplus_chg_charging_status {
	CHARGING_STATUS_CCCV = 0x01,
	CHARGING_STATUS_FULL = 0x02,
	CHARGING_STATUS_FAIL = 0x03,
};

/*
 * struct oplus_chg_limits — normal-wired charging limits.
 *
 * Temperature bands, voltage thresholds, current limits, and time safety
 * constants.  Populated by oplus_chg_limits_init() with conservative
 * defaults; DT overrides deferred to later layers.
 */
struct oplus_chg_limits {
	int removed_bat_decidegc;
	int cold_bat_decidegc;
	int little_cold_bat_decidegc;
	int cool_bat_decidegc;
	int little_cool_bat_decidegc;
	int normal_bat_decidegc;
	int warm_bat_decidegc;
	int hot_bat_decidegc;
	int temp_cold_vfloat_mv;
	int temp_little_cold_vfloat_mv;
	int temp_cool_vfloat_mv;
	int temp_little_cool_vfloat_mv;
	int temp_normal_vfloat_mv;
	int temp_warm_vfloat_mv;
	int temp_cold_fastchg_current_ma;
	int temp_little_cold_fastchg_current_ma;
	int temp_cool_fastchg_current_ma_low;
	int temp_little_cool_fastchg_current_ma;
	int temp_normal_fastchg_current_ma;
	int temp_warm_fastchg_current_ma;
	int input_current_charger_ma;
	int vbatt_hv_thr;
	int vbatt_full_thr;
	int recharge_mv;
	int iterm_ma;
	int max_chg_time_sec;
};

/*
 * struct oplus_chg_chip — OPLUS charging runtime state.
 *
 * Layer 3A provided the minimal wired-charging fields.  Layer 3B extends
 * with safety, full/recharge, and temperature-band state.
 */
struct oplus_chg_chip {
	struct device *dev;
	struct oplus_chg_operations *chg_ops;
	struct power_supply *usb_psy;
	struct power_supply *batt_psy;
	struct delayed_work update_work;
	atomic_t api_users;
	wait_queue_head_t api_wq;
	bool initialized;
	bool charger_online;
	bool batt_auth;
	int charger_type;
	int batt_soc;
	int batt_mv;
	int batt_temp;
	int batt_current_ma;
	int batt_soh;
	int input_current_limit_ma;
	int float_voltage_mv;
	int fastchg_current_ma;
	unsigned int update_interval_ms;
	/* Layer 3B: safety, full/recharge, temperature-band state */
	struct oplus_chg_limits limits;
	enum oplus_chg_tbatt_status tbatt_status;
	enum oplus_chg_charging_status charging_state;
	bool charger_exist;
	bool batt_exist;
	bool batt_full;
	bool real_batt_full;
	bool sw_full;
	bool hw_full;
	bool vbatt_over;
	bool chging_over_time;
	bool charging_enabled;
	bool in_rechging;
	int batt_volt;
	int temperature;
	int icharging;
	int batt_volt_min;
	int charger_volt;
	int charger_current_ma;
	int total_time;
	int recharge_count;
	/* USB temperature monitoring (ported from 4.19 oplus_chg_chip) */
	int shutdown_soc;
	int usbtemp_volt_l;
	int usbtemp_volt_r;
	bool usbtemp_check;
	bool dischg_flag;
	int usb_temp_l;
	int usb_temp_r;
	int usbtemp_temp_up_time_thr;
	int usbtemp_max_temp_thr;
	int tbatt_temp;
};
#include <linux/thermal.h>
#include "oplus_power_supply_ext.h"

/* ===== OPLUS charger externs used by QG driver ===== */
extern int oplus_chg_get_ffc_status(void);
extern bool oplus_vooc_get_fastchg_ing(void);

/* ===== OPLUS power supply type extensions ===== */
enum oplus_power_supply_type {
	POWER_SUPPLY_TYPE_USB_HVDCP = 13,		/* High Voltage DCP */
	POWER_SUPPLY_TYPE_USB_HVDCP_3,			/* Efficient High Voltage DCP */
	POWER_SUPPLY_TYPE_USB_HVDCP_3P5,		/* Efficient High Voltage DCP */
	POWER_SUPPLY_TYPE_USB_FLOAT,			/* Floating charger */
	POWER_SUPPLY_TYPE_USB_PD_SDP,			/* USB With PD Port */
};

enum oplus_power_supply_usb_type {
	POWER_SUPPLY_USB_TYPE_PD_SDP = 17,		/* USB With PD Port */
};

/* ===== Logging macros ===== */
#define chg_info(fmt, ...) \
	printk(KERN_INFO "[OPLUS_CHG][%s]" fmt, __func__, ##__VA_ARGS__)
#define chg_debug(fmt, ...) \
	printk(KERN_NOTICE "[OPLUS_CHG][%s]" fmt, __func__, ##__VA_ARGS__)
#define chg_err(fmt, ...) \
	printk(KERN_ERR "[OPLUS_CHG][%s]" fmt, __func__, ##__VA_ARGS__)

/* ===== OPLUS RTC helpers for 5.10+ ===== */
#include <uapi/linux/rtc.h>
#include <linux/rtc.h>

#ifdef __KERNEL__
#ifndef _STRUCT_TIMESPEC
#define _STRUCT_TIMESPEC
struct timespec {
	__kernel_old_time_t tv_sec;
	long tv_nsec;
};
#endif

struct timeval {
	__kernel_old_time_t tv_sec;
	__kernel_suseconds_t tv_usec;
};

struct itimerspec {
	struct timespec it_interval;
	struct timespec it_value;
};

struct itimerval {
	struct timeval it_interval;
	struct timeval it_value;
};
#endif

static inline void rtc_time_to_tm(unsigned long time, struct rtc_time *tm)
{
	rtc_time64_to_tm(time, tm);
}

static inline int rtc_tm_to_time(struct rtc_time *tm, unsigned long *time)
{
	*time = rtc_tm_to_time64(tm);
	return 0;
}

#if __BITS_PER_LONG == 64
static inline struct timespec
timespec64_to_timespec(const struct timespec64 ts64)
{
	return *(const struct timespec *)&ts64;
}

static inline struct timespec64 timespec_to_timespec64(const struct timespec ts)
{
	return *(const struct timespec64 *)&ts;
}
#else
static inline struct timespec
timespec64_to_timespec(const struct timespec64 ts64)
{
	struct timespec ret;
	ret.tv_sec = (time_t)ts64.tv_sec;
	ret.tv_nsec = ts64.tv_nsec;
	return ret;
}

static inline struct timespec64 timespec_to_timespec64(const struct timespec ts)
{
	struct timespec64 ret;
	ret.tv_sec = ts.tv_sec;
	ret.tv_nsec = ts.tv_nsec;
	return ret;
}
#endif

static inline void getnstimeofday(struct timespec *ts)
{
	struct timespec64 ts64;
	ktime_get_real_ts64(&ts64);
	*ts = timespec64_to_timespec(ts64);
}

/* ===== OPLUS charge stop/critical log types ===== */
typedef enum {
	CRITICAL_LOG_NORMAL = 0,
	CRITICAL_LOG_UNABLE_CHARGING,
	CRITICAL_LOG_BATTTEMP_ABNORMAL,
	CRITICAL_LOG_VCHG_ABNORMAL,
	CRITICAL_LOG_VBAT_TOO_HIGH,
	CRITICAL_LOG_CHARGING_OVER_TIME,
	CRITICAL_LOG_VOOC_WATCHDOG,
	CRITICAL_LOG_VOOC_BAD_CONNECTED,
	CRITICAL_LOG_VOOC_BTB,
	CRITICAL_LOG_VOOC_FW_UPDATE_ERR,
	CRITICAL_LOG_VBAT_OVP,
} OPLUS_CHG_CRITICAL_LOG;

/*
 * struct oplus_chg_operations — charger IC hardware operations table.
 *
 * Populated by charger-IC drivers (bq2597x, smb1398, etc.) at probe time
 * via oplus_chg_ops_register().  The framework resolves the active instance
 * through oplus_chg_ops_get().
 *
 * Ported from 4.19 oplus_charger.h (L1551-L1670).  ~120 function pointers.
 * All entries default to NULL; IC drivers fill in the relevant subset.
 */
struct oplus_chg_operations {
	void (*get_props_from_adsp_by_buffer)(void);
	int (*get_charger_cycle)(void);
	void (*get_usbtemp_volt)(struct oplus_chg_chip *chip);
	void (*set_typec_sinkonly)(void);
	void (*set_typec_cc_open)(void);
	void (*really_suspend_charger)(bool en);
	bool (*oplus_usbtemp_monitor_condition)(struct oplus_chg_chip *chip);
	int (*recovery_usbtemp)(void *data);
	void (*dump_registers)(void);
	int (*kick_wdt)(void);
	int (*hardware_init)(void);
	int (*charging_current_write_fast)(int cur);
	int (*set_wls_boost_en)(bool enable);
	int (*wls_set_boost_en)(bool en);
	int (*wls_set_boost_vol)(int vol_mv);
	int (*input_current_ctrl_by_vooc_write)(int cur);
	void (*set_aicl_point)(int vbatt);
	int (*input_current_write)(int cur);
	int (*float_voltage_write)(int cur);
	int (*term_current_set)(int cur);
	int (*charging_enable)(void);
	int (*charging_disable)(void);
	int (*get_charging_enable)(void);
	int (*charger_suspend)(void);
	int (*charger_unsuspend)(void);
	bool (*charger_suspend_check)(void);
	int (*set_rechg_vol)(int vol);
	int (*reset_charger)(void);
	int (*read_full)(void);
	int (*otg_enable)(void);
	int (*otg_disable)(void);
	int (*set_charging_term_disable)(void);
	bool (*check_charger_resume)(void);
	int (*get_charger_type)(void);
	int (*get_charger_volt)(void);
	int (*get_ibus)(void);
	int (*get_charger_current)(void);
	int (*get_real_charger_type)(void);
	int (*get_chargerid_volt)(void);
	void (*set_chargerid_switch_val)(int value);
	int (*get_chargerid_switch_val)(void);
	bool (*check_chrdet_status)(void);
	int (*get_boot_mode)(void);
	int (*get_boot_reason)(void);
	int (*get_instant_vbatt)(void);
	int (*get_rtc_soc)(void);
	int (*set_rtc_soc)(int val);
	void (*set_power_off)(void);
	void (*usb_connect)(void);
	void (*usb_disconnect)(void);
	void (*get_platform_gauge_curve)(int index_curve);
	int (*get_chg_current_step)(void);
	bool (*need_to_check_ibatt)(void);
	int (*get_dyna_aicl_result)(void);
	bool (*get_shortc_hw_gpio_status)(void);
	void (*check_is_iindpm_mode)(void);
	int (*oplus_chg_get_pd_type)(void);
	int (*oplus_chg_pd_setup)(void);
	int (*oplus_chg_pps_setup)(int vbus_mv, int ibus_ma);
	u32 (*oplus_chg_get_pps_status)(void);
	int (*oplus_chg_get_max_cur)(int vbus_mv);
	int (*get_charger_subtype)(void);
	int (*set_qc_config)(void);
	void (*em_mode_enable)(void);
	int (*enable_qc_detect)(void);
	int (*input_current_write_without_aicl)(int current_ma);
	int (*wls_input_current_write)(int current_ma);
	int (*set_charger_vsys_threshold)(int val);
	int (*enable_burst_mode)(bool enable);
	void (*oplus_chg_wdt_enable)(bool wdt_enable);
	void (*adsp_voocphy_set_match_temp)(void);
	int (*oplus_chg_set_high_vbus)(bool en);
	int (*oplus_chg_set_hz_mode)(bool en);
	int (*enable_shipmode)(bool en);
	bool (*check_pdphy_ready)(void);
	int (*pdo_5v)(void);
	int (*set_enable_volatile_writes)(void);
	int (*set_complete_charge_timeout)(int val);
	int (*set_prechg_voltage_threshold)(void);
	int (*set_prechg_current)(int ipre_mA);
	int (*set_vindpm_vol)(int vol);
	void (*rerun_wls_aicl)(void);
	int (*disable_buck_switch)(void);
	int (*disable_async_mode)(void);
	int (*set_switching_frequency)(void);
	int (*set_mps_otg_current)(void);
	int (*set_mps_otg_voltage)(bool is_9v);
	int (*set_mps_second_otg_voltage)(bool is_750mv);
	int (*set_wdt_timer)(int reg);
	int (*set_voltage_slew_rate)(int value);
	int (*otg_wait_vbus_decline)(void);
	void (*vooc_timeout_callback)(bool);
	void (*force_pd_to_dcp)(void);
	bool (*get_otg_enable)(void);
	bool (*check_qchv_condition)(void);
	int (*set_bcc_curr_to_voocphy)(int bcc_curr);
	bool (*is_support_qcpd)(void);
	int (*get_subboard_temp)(void);
	int (*get_ccdetect_online)(void);
	int (*check_cc_mode)(void);
	void (*set_prswap)(bool);
	int (*check_chg_plugin)(void);
	int (*get_cp_tsbus)(void);
	int (*get_cp_tsbat)(void);
	int (*get_abnormal_adapter_disconnect_cnt)(void);
};

/* ===== Layer 3A exported entry points ===== */
int oplus_chg_init(struct oplus_chg_chip *chip);
void oplus_chg_deinit(struct oplus_chg_chip *chip);
int oplus_chg_parse_svooc_dt(struct oplus_chg_chip *chip);
int oplus_chg_parse_charger_dt(struct oplus_chg_chip *chip);

/* ===== Layer 3B exported update-work controls ===== */
void oplus_chg_cancel_update_work_sync(void);
void oplus_chg_restart_update_work(void);
bool oplus_chg_wake_update_work(void);

#endif /* _OPLUS_CHARGER_H */
