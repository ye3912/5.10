/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * OPLUS device info header stub for 5.10 port
 * Original: include/soc/oplus/device_info.h (4.19)
 */

#ifndef _DEVICE_INFO_H
#define _DEVICE_INFO_H

#include <linux/list.h>

#define INFO_LEN  24

typedef enum {
	DDR_TYPE_LPDDR1 = 0,
	DDR_TYPE_LPDDR2 = 2,
	DDR_TYPE_PCDDR2 = 3,
	DDR_TYPE_PCDDR3 = 4,
	DDR_TYPE_LPDDR3 = 5,
	DDR_TYPE_LPDDR4 = 6,
	DDR_TYPE_LPDDR4X = 7,
	DDR_TYPE_LPDDR5 = 8,
	DDR_TYPE_LPDDR5X = 9,
	DDR_TYPE_UNUSED = 0x7FFFFFFF
} DDR_TYPE __maybe_unused;

struct manufacture_info {
	char name[INFO_LEN];
	char *version;
	char *manufacture;
	char *fw_path;
};

struct o_hw_id {
	const char *label;
	const char *match;
	int id;
	struct list_head list;
};

struct o_ufsplus_status {
	int *hpb_status;
	int *tw_status;
};

int register_device_proc(char *name, char *version, char *vendor);
int register_device_proc_for_ufsplus(char *name, int *hpb_status, int *tw_status);
int register_devinfo(char *name, struct manufacture_info *info);
bool check_id_match(const char *label, const char *id_match, int id);

#endif  /* _DEVICE_INFO_H */
