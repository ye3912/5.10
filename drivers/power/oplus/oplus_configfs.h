/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2020-2022 Oplus. All rights reserved.
 */

#ifndef _OPLUS_CONFIGFS_H
#define _OPLUS_CONFIGFS_H

#include <linux/configfs.h>

struct oplus_chg_cfg_item {
	struct config_item item;
	void *data;
};

extern struct config_item *oplus_chg_cfg_add_item(
	struct config_group *group,
	struct config_item_type *type);
extern void oplus_chg_cfg_del_item(struct config_item *item);

#endif /* _OPLUS_CONFIGFS_H */
