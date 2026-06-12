/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2020-2022 Oplus. All rights reserved.
 */

#ifndef _OPLUS_DEBUG_INFO_H
#define _OPLUS_DEBUG_INFO_H

#include <linux/kernel.h>

struct oplus_chg_chip;

enum oplus_chg_debug_module {
	DEBUG_MODULE_BASE,
	DEBUG_MODULE_CHARGER,
	DEBUG_MODULE_GAUGE,
	DEBUG_MODULE_VOOC,
	DEBUG_MODULE_WIRELESS,
};

extern int oplus_chg_debug_init(void);
extern void oplus_chg_debug_exit(void);

#endif /* _OPLUS_DEBUG_INFO_H */
