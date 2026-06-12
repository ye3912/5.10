/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2020-2022 Oplus. All rights reserved.
 *
 * Stub — oplus_chg_comm communication framework.
 * Full implementation ~2190 lines deferred to Phase 3.
 */

#include <linux/errno.h>
#include "oplus_charger.h"
#include "oplus_chg_comm.h"

static bool comm_registered;

int oplus_chg_comm_register(void)
{
	if (comm_registered) {
		chg_err("comm already registered\n");
		return -EALREADY;
	}
	comm_registered = true;
	chg_info("comm framework stub registered\n");
	return 0;
}

void oplus_chg_comm_unregister(void)
{
	if (!comm_registered) {
		chg_err("comm not registered\n");
		return;
	}
	comm_registered = false;
	chg_info("comm framework stub unregistered\n");
}
EXPORT_SYMBOL_GPL(oplus_chg_comm_register);
EXPORT_SYMBOL_GPL(oplus_chg_comm_unregister);
