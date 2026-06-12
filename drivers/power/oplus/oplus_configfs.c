/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2020-2022 Oplus. All rights reserved.
 *
 * Stub — oplus_configfs framework.
 * Full implementation ~3598 lines deferred to Phase 3.
 */

#include <linux/configfs.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include "oplus_charger.h"
#include "oplus_configfs.h"

struct config_item *oplus_chg_cfg_add_item(struct config_group *group,
					   struct config_item_type *type)
{
	chg_debug("configfs add_item stub\n");
	return ERR_PTR(-ENOSYS);
}

void oplus_chg_cfg_del_item(struct config_item *item)
{
	chg_debug("configfs del_item stub\n");
}
EXPORT_SYMBOL_GPL(oplus_chg_cfg_add_item);
EXPORT_SYMBOL_GPL(oplus_chg_cfg_del_item);
