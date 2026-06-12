/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * OPLUS VOOC protocol stub for 5.10 GKI — Phase 3.
 * Full VOOC/SuperVOOC framework ~10K+ lines deferred.
 */
#ifndef _OPLUS_VOOC_H
#define _OPLUS_VOOC_H

#include <linux/types.h>

#define VOOC_CHARGING_UNSUPPORTED	0
#define VOOC_CHARGING_NORMAL		1
#define VOOC_CHARGING_FAST		2

static inline bool oplus_vooc_get_allow_reading(void) { return false; }
bool oplus_vooc_get_fastchg_ing(void);
static inline bool oplus_vooc_get_fastchg_started(void) { return false; }
static inline int oplus_vooc_get_adapter_update_status(void) { return 0; }

#endif /* _OPLUS_VOOC_H */
