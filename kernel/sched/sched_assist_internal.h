/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2020 Oplus. All rights reserved.
 *
 * sched_assist internal accessor macros - rq-side only.
 *
 * These macros access walt_rq fields which require kernel/sched/walt/walt.h.
 * They are NOT suitable for public headers (include/linux/) because that
 * would create a dependency from public headers on scheduler internals.
 *
 * Use these ONLY in kernel/sched/ code.
 */

#ifndef _SCHED_ASSIST_INTERNAL_H_
#define _SCHED_ASSIST_INTERNAL_H_

#include <linux/sched/sched_assist.h>
#include "walt/walt.h"

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_SCHED_ASSIST)

#define sa_ux_thread_list(rq)		sa_wrq(rq)->ux_thread_list
#define sa_ux_list_lock(rq)		sa_wrq(rq)->ux_list_lock

#if IS_ENABLED(CONFIG_LOCKING_PROTECT)
#define sa_rq_locking_task(rq)		sa_wrq(rq)->rq_locking_task
#define sa_locking_thread_list(rq)	sa_wrq(rq)->locking_thread_list
#endif

#endif /* CONFIG_OPLUS_FEATURE_SCHED_ASSIST */

#endif /* _SCHED_ASSIST_INTERNAL_H_ */
