/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2020-2022 Oplus. All rights reserved.
 *
 * Stub — oplus_chg_debug framework.
 * Full implementation ~2920 lines deferred to Phase 3.
 */

#include "oplus_charger.h"
#include "oplus_debug_info.h"

int oplus_chg_debug_init(void)
{
	chg_info("debug framework stub initialized\n");
	return 0;
}

void oplus_chg_debug_exit(void)
{
	chg_info("debug framework stub exited\n");
}
EXPORT_SYMBOL_GPL(oplus_chg_debug_init);
EXPORT_SYMBOL_GPL(oplus_chg_debug_exit);
