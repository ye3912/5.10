// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2018-2024 Oplus. All rights reserved.
 *
 * Boot mode detection driver — reads kernel cmdline to determine
 * the device boot state (normal, recovery, charging, factory, etc.).
 * Exports get_boot_mode() for other OPLUS drivers.
 *
 * Ported from 4.19 to 5.10 — see porting-plan.md.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/uaccess.h>
#include <linux/sysfs.h>
#include <soc/oplus/boot_mode.h>

#define MAX_CMD_LENGTH	32

static struct kobject *systeminfo_kobj;
static int ftm_mode = MSM_BOOT_MODE__NORMAL;

#if IS_MODULE(CONFIG_OPLUS_FEATURE_PROJECTINFO)
extern char oplus_ftm_mode[];
extern char startup_mode[];
extern char charger_present[];
extern char bootmode[];
#endif

#ifdef CONFIG_ARCH_LITO
static int hw_version;
#endif

static int __init board_ftm_mode_init(void)
{
#if IS_MODULE(CONFIG_OPLUS_FEATURE_PROJECTINFO)
	if (oplus_ftm_mode) {
		pr_err("oplus_ftm_mode from cmdline : %s\n", oplus_ftm_mode);
		if (strcmp(oplus_ftm_mode, "factory2") == 0)
			ftm_mode = MSM_BOOT_MODE__FACTORY;
		else if (strcmp(oplus_ftm_mode, "ftmwifi") == 0)
			ftm_mode = MSM_BOOT_MODE__WLAN;
		else if (strcmp(oplus_ftm_mode, "ftmmos") == 0)
			ftm_mode = MSM_BOOT_MODE__MOS;
		else if (strcmp(oplus_ftm_mode, "ftmrf") == 0)
			ftm_mode = MSM_BOOT_MODE__RF;
		else if (strcmp(oplus_ftm_mode, "ftmrecovery") == 0)
			ftm_mode = MSM_BOOT_MODE__RECOVERY;
		else if (strcmp(oplus_ftm_mode, "ftmsilence") == 0)
			ftm_mode = MSM_BOOT_MODE__SILENCE;
		else if (strcmp(oplus_ftm_mode, "ftmsau") == 0)
			ftm_mode = MSM_BOOT_MODE__SAU;
		else if (strcmp(oplus_ftm_mode, "ftmaging") == 0)
			ftm_mode = MSM_BOOT_MODE__AGING;
		else if (strcmp(oplus_ftm_mode, "ftmsafe") == 0)
			ftm_mode = MSM_BOOT_MODE__SAFE;
	}
#else
	char *substr;

	substr = strstr(boot_command_line, "oplus_ftm_mode=");
	if (substr) {
		substr += strlen("oplus_ftm_mode=");
		if (strncmp(substr, "factory2", 8) == 0)
			ftm_mode = MSM_BOOT_MODE__FACTORY;
		else if (strncmp(substr, "ftmwifi", 7) == 0)
			ftm_mode = MSM_BOOT_MODE__WLAN;
		else if (strncmp(substr, "ftmmos", 6) == 0)
			ftm_mode = MSM_BOOT_MODE__MOS;
		else if (strncmp(substr, "ftmrf", 5) == 0)
			ftm_mode = MSM_BOOT_MODE__RF;
		else if (strncmp(substr, "ftmrecovery", 12) == 0)
			ftm_mode = MSM_BOOT_MODE__RECOVERY;
		else if (strncmp(substr, "ftmsilence", 10) == 0)
			ftm_mode = MSM_BOOT_MODE__SILENCE;
		else if (strncmp(substr, "ftmsau", 6) == 0)
			ftm_mode = MSM_BOOT_MODE__SAU;
		else if (strncmp(substr, "ftmaging", 8) == 0)
			ftm_mode = MSM_BOOT_MODE__AGING;
		else if (strncmp(substr, "ftmsafe", 7) == 0)
			ftm_mode = MSM_BOOT_MODE__SAFE;
	}
#endif
	pr_err("board_ftm_mode_init ftm_mode=%d\n", ftm_mode);
	return 0;
}

/**
 * get_boot_mode() - Get the current device boot / FTM mode
 *
 * Determines the boot mode by parsing the kernel command-line
 * "oplus_ftm_mode=" parameter during early init.  The mode is cached
 * in @ftm_mode and returned on every call.
 *
 * Called by charging (oplus_qpnp_qg), display, keypad, and sensor
 * drivers that need to alter behaviour in factory / recovery / aging
 * modes.
 *
 * Context: any
 * Return:  MSM_BOOT_MODE__* enum value (MSM_BOOT_MODE__NORMAL = 0)
 */
int get_boot_mode(void)
{
	return ftm_mode;
}
EXPORT_SYMBOL_GPL(get_boot_mode);

static ssize_t ftmmode_show(struct kobject *kobj, struct kobj_attribute *attr,
			    char *buf)
{
	return sprintf(buf, "%d\n", ftm_mode);
}

static struct kobj_attribute ftmmode_attr = {
	.attr	= { .name = "ftmmode", .mode = 0444 },
	.show	= ftmmode_show,
};

static struct attribute *ftmmode_attrs[] = {
	&ftmmode_attr.attr,
	NULL,
};

static const struct attribute_group ftmmode_attr_group = {
	.attrs = ftmmode_attrs,
};

static char pwron_event[MAX_CMD_LENGTH + 1];

static int __init start_reason_init(void)
{
#if IS_MODULE(CONFIG_OPLUS_FEATURE_PROJECTINFO)
	if (startup_mode) {
		strcpy(pwron_event, startup_mode);
		pwron_event[strlen(startup_mode)] = '\0';
	}
#else
	char *substr;
	int i;

	substr = strstr(boot_command_line, "androidboot.startupmode=");
	if (!substr)
		return 0;
	substr += strlen("androidboot.startupmode=");
	for (i = 0; substr[i] != ' ' && i < MAX_CMD_LENGTH &&
	     substr[i] != '\0'; i++)
		pwron_event[i] = substr[i];
	pwron_event[i] = '\0';
#endif
	return 0;
}

static char boot_mode[MAX_CMD_LENGTH + 1];

bool qpnp_is_power_off_charging(void)
{
	return !strcmp(boot_mode, "charger");
}
EXPORT_SYMBOL_GPL(qpnp_is_power_off_charging);

bool op_is_monitorable_boot(void)
{
	if (ftm_mode != MSM_BOOT_MODE__NORMAL)
		return false;
	if (!strcmp(boot_mode, "normal") || !strcmp(boot_mode, "reboot") ||
	    !strcmp(boot_mode, "kernel") || !strcmp(boot_mode, "rtc"))
		return true;
	return false;
}
EXPORT_SYMBOL_GPL(op_is_monitorable_boot);

static char charger_reboot[MAX_CMD_LENGTH + 1];

bool qpnp_is_charger_reboot(void)
{
	return !strcmp(charger_reboot, "1");
}
EXPORT_SYMBOL_GPL(qpnp_is_charger_reboot);

static int __init oplus_charger_reboot_init(void)
{
#if IS_MODULE(CONFIG_OPLUS_FEATURE_PROJECTINFO)
	if (charger_present) {
		strcpy(charger_reboot, charger_present);
		charger_reboot[strlen(charger_present)] = '\0';
	}
#else
	char *substr;
	int i;

	substr = strstr(boot_command_line, "oplus_charger_present=");
	if (substr) {
		substr += strlen("oplus_charger_present=");
		for (i = 0; substr[i] != ' ' && i < MAX_CMD_LENGTH &&
		     substr[i] != '\0'; i++)
			charger_reboot[i] = substr[i];
		charger_reboot[i] = '\0';
	}
#endif
	return 0;
}

static int __init board_boot_mode_init(void)
{
#if IS_MODULE(CONFIG_OPLUS_FEATURE_PROJECTINFO)
	if (bootmode) {
		strcpy(boot_mode, bootmode);
		boot_mode[strlen(bootmode)] = '\0';
	}
#else
	char *substr;
	int i;

	substr = strstr(boot_command_line, "androidboot.mode=");
	if (substr) {
		substr += strlen("androidboot.mode=");
		for (i = 0; substr[i] != ' ' && i < MAX_CMD_LENGTH &&
		     substr[i] != '\0'; i++)
			boot_mode[i] = substr[i];
		boot_mode[i] = '\0';
	}
#endif
	return 0;
}

#ifdef CONFIG_ARCH_LITO
int get_hw_board_version(void)
{
	return hw_version;
}
EXPORT_SYMBOL_GPL(get_hw_board_version);

static int __init oplus_hw_version_init(char *str)
{
	if (kstrtoint(str, 0, &hw_version) < 0)
		hw_version = 0;
	pr_info("kernel get_hw_version %d\n", hw_version);
	return 0;
}
__setup("androidboot.hw_version=", oplus_hw_version_init);
#endif /* CONFIG_ARCH_LITO */

static int __init boot_mode_init(void)
{
	int rc = 0;

	board_boot_mode_init();
	board_ftm_mode_init();
	start_reason_init();
	oplus_charger_reboot_init();

	systeminfo_kobj = kobject_create_and_add("systeminfo", NULL);
	if (!systeminfo_kobj)
		return -ENOMEM;

	rc = sysfs_create_group(systeminfo_kobj, &ftmmode_attr_group);
	if (rc)
		kobject_put(systeminfo_kobj);

	return rc;
}
arch_initcall(boot_mode_init);

MODULE_LICENSE("GPL v2");
