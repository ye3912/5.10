/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2020-2022 Oplus. All rights reserved.
 *
 * Usage: IC drivers call oplus_chg_ops_register("plat-pmic", &ops) early.
 * The framework resolves the winner via oplus_chg_ops_get().
 */

#ifndef _OPLUS_CHG_OPS_MANAGER_H
#define _OPLUS_CHG_OPS_MANAGER_H

#include <linux/of.h>

struct oplus_chg_operations;

extern void oplus_get_chg_ops_name_from_dt(struct device_node *node);
extern int oplus_chg_ops_register(const char *name,
				  struct oplus_chg_operations *chg_ops);
extern void oplus_chg_ops_deinit(void);
extern struct oplus_chg_operations *oplus_chg_ops_get(void);
extern char *oplus_chg_ops_name_get(void);

#endif /* _OPLUS_CHG_OPS_MANAGER_H */
