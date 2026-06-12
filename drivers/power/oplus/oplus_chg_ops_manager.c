/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2020-2022 Oplus. All rights reserved.
 *
 * Ported from 4.19 for 5.10 GKI — linked-list ops registry.
 */

#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/of.h>
#include <linux/string.h>

#include "oplus_charger.h"
#include "oplus_chg_ops_manager.h"

#define CHG_OPS_DESC_NAME_MAX_LENTH 64

struct oplus_chg_ops_desc {
	struct list_head list;
	struct oplus_chg_operations *chg_ops;
	char name[CHG_OPS_DESC_NAME_MAX_LENTH];
};

struct oplus_chg_ops_mg_data {
	struct list_head chg_ops_list_head;
	spinlock_t chg_ops_list_lock;
	char chg_ops_name[CHG_OPS_DESC_NAME_MAX_LENTH];
};

static struct oplus_chg_ops_mg_data g_oplus_chg_ops_mg_data;
static bool ops_has_init;

static void oplus_chg_ops_manager_init(void)
{
	if (ops_has_init)
		return;

	INIT_LIST_HEAD(&g_oplus_chg_ops_mg_data.chg_ops_list_head);
	spin_lock_init(&g_oplus_chg_ops_mg_data.chg_ops_list_lock);
	ops_has_init = true;
}

static struct oplus_chg_ops_desc *oplus_chg_ops_desc_get(const char *name)
{
	struct oplus_chg_ops_desc *element, *loopup = NULL;

	if (list_empty(&g_oplus_chg_ops_mg_data.chg_ops_list_head)) {
		chg_err("chg_ops_list_head empty\n");
		return NULL;
	}

	spin_lock(&g_oplus_chg_ops_mg_data.chg_ops_list_lock);
	list_for_each_entry(element, &g_oplus_chg_ops_mg_data.chg_ops_list_head,
			    list) {
		if (!strncmp(name, element->name,
			     CHG_OPS_DESC_NAME_MAX_LENTH)) {
			loopup = element;
			break;
		}
	}
	spin_unlock(&g_oplus_chg_ops_mg_data.chg_ops_list_lock);

	return loopup;
}

/**
 * oplus_get_chg_ops_name_from_dt() - Load active charger ops name from DT.
 * @node: device-tree node containing the optional "oplus,chg_ops" string.
 *
 * Missing DT property intentionally selects "plat-pmic" for compatibility
 * with legacy OPLUS board files. The name is bounded with strscpy so the
 * global ops-manager state always remains NUL-terminated.
 */
void oplus_get_chg_ops_name_from_dt(struct device_node *node)
{
	const char *chg_ops_name_dt = "plat-pmic";
	char *chg_ops_name = g_oplus_chg_ops_mg_data.chg_ops_name;
	int rc;

	if (!ops_has_init)
		oplus_chg_ops_manager_init();

	if (node) {
		rc = of_property_read_string(node, "oplus,chg_ops",
					    &chg_ops_name_dt);
		if (rc)
			chg_ops_name_dt = "plat-pmic";
	}

	strscpy(chg_ops_name, chg_ops_name_dt,
		CHG_OPS_DESC_NAME_MAX_LENTH);

	chg_info("chg_ops_name: %s\n", chg_ops_name);
}

int oplus_chg_ops_register(const char *name,
			   struct oplus_chg_operations *chg_ops)
{
	struct oplus_chg_ops_desc *new_desc;

	if (!name || !chg_ops)
		return -EINVAL;

	if (!ops_has_init)
		oplus_chg_ops_manager_init();

	new_desc = oplus_chg_ops_desc_get(name);
	if (new_desc)
		return -EEXIST;

	new_desc = kzalloc(sizeof(*new_desc), GFP_KERNEL);
	if (!new_desc)
		return -ENOMEM;

	if (strscpy(new_desc->name, name,
		    CHG_OPS_DESC_NAME_MAX_LENTH) < 0) {
		kfree(new_desc);
		return -EINVAL;
	}

	new_desc->chg_ops = chg_ops;

	spin_lock(&g_oplus_chg_ops_mg_data.chg_ops_list_lock);
	list_add_tail(&new_desc->list,
		      &g_oplus_chg_ops_mg_data.chg_ops_list_head);
	spin_unlock(&g_oplus_chg_ops_mg_data.chg_ops_list_lock);

	chg_info("registered name: %s\n", new_desc->name);
	return 0;
}

void oplus_chg_ops_deinit(void)
{
	struct oplus_chg_ops_desc *entry, *tmp;

	spin_lock(&g_oplus_chg_ops_mg_data.chg_ops_list_lock);
	list_for_each_entry_safe(entry, tmp,
				 &g_oplus_chg_ops_mg_data.chg_ops_list_head,
				 list) {
		list_del(&entry->list);
		kfree(entry);
	}
	spin_unlock(&g_oplus_chg_ops_mg_data.chg_ops_list_lock);
}

struct oplus_chg_operations *oplus_chg_ops_get(void)
{
	struct oplus_chg_ops_desc *desc;

	if (!ops_has_init)
		oplus_chg_ops_manager_init();

	desc = oplus_chg_ops_desc_get(
		g_oplus_chg_ops_mg_data.chg_ops_name);
	if (desc)
		return desc->chg_ops;

	return NULL;
}

char *oplus_chg_ops_name_get(void)
{
	return g_oplus_chg_ops_mg_data.chg_ops_name;
}
