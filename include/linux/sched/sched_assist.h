/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2020 Oplus. All rights reserved.
 *
 * sched_assist accessor macros for 5.10 GKI.
 *
 * In 4.19, sched_assist fields (ux_state, inherit_ux, etc.) were direct
 * members of task_struct and rq. In 5.10 GKI, they must live in vendor
 * data space — specifically inside walt_task_struct (overlaying
 * task_struct->android_vendor_data1) and walt_rq (overlaying
 * rq->android_vendor_data1).
 *
 * These macros provide the bridge: sa_ux_state(p) replaces p->ux_state,
 * sa_ux_thread_list(rq) replaces rq->ux_thread_list, etc.
 *
 * All sched_assist .c code MUST use these macros instead of direct
 * task_struct/rq member access. Direct access to task->ux_state or
 * rq->ux_thread_list will NOT compile in 5.10 GKI.
 */

#ifndef _LINUX_SCHED_ASSIST_H_
#define _LINUX_SCHED_ASSIST_H_

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_SCHED_ASSIST)

#include <linux/sched/walt.h>

/*
 * Core cast macros — obtain walt_task_struct / walt_rq from task_struct / rq.
 * These mirror the WALT pattern: (struct walt_task_struct *)p->android_vendor_data1
 */
#define sa_wts(p)	((struct walt_task_struct *)(p)->android_vendor_data1)
#define sa_wrq(rq)	((struct walt_rq *)(rq)->android_vendor_data1)

/* Per-task sched_assist field accessors */
#define sa_ux_state(p)			sa_wts(p)->ux_state
#define sa_inherit_ux(p)		sa_wts(p)->inherit_ux
#define sa_ux_entry(p)			sa_wts(p)->ux_entry
#define sa_ux_depth(p)			sa_wts(p)->ux_depth
#define sa_enqueue_time(p)		sa_wts(p)->enqueue_time
#define sa_inherit_ux_start(p)		sa_wts(p)->inherit_ux_start
#define sa_sum_exec_baseline(p)	sa_wts(p)->sum_exec_baseline

#if IS_ENABLED(CONFIG_OPLUS_UX_IM_FLAG)
#define sa_ux_im_flag(p)		sa_wts(p)->ux_im_flag
#endif

#if IS_ENABLED(CONFIG_LOCKING_PROTECT)
#define sa_locking_entry(p)		sa_wts(p)->locking_entry
#define sa_locking_time_start(p)	sa_wts(p)->locking_time_start
#define sa_locking_depth(p)		sa_wts(p)->locking_depth
#endif

#if IS_ENABLED(CONFIG_OPLUS_LOCKING_STRATEGY)
#define sa_lkinfo(p)			sa_wts(p)->lkinfo
#endif

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_SCHED_SPREAD)
#define sa_lb_state(p)			sa_wts(p)->lb_state
#define sa_ld_flag(p)			sa_wts(p)->ld_flag
#endif

#if IS_ENABLED(CONFIG_MMAP_LOCK_OPT)
#define sa_ux_once(p)			sa_wts(p)->ux_once
#define sa_get_mmlock_ts(p)		sa_wts(p)->get_mmlock_ts
#define sa_get_mmlock(p)		sa_wts(p)->get_mmlock
#endif

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_AUDIO_OPT)
#define sa_oplus_task_info(p)		sa_wts(p)->oplus_task_info
#endif

/*
 * Per-rq accessors (sa_ux_thread_list, sa_ux_list_lock, sa_rq_locking_task,
 * sa_locking_thread_list) are in kernel/sched/sched_assist_internal.h
 * because they require struct walt_rq definition from kernel/sched/walt/walt.h.
 */

#else /* !CONFIG_OPLUS_FEATURE_SCHED_ASSIST */

/*
 * When sched_assist is disabled, provide no-op casts so that code which
 * conditionally uses these macros still compiles without #ifdef noise.
 * The actual field access will never execute because the calling code
 * is also guarded by CONFIG_OPLUS_FEATURE_SCHED_ASSIST.
 */
#define sa_wts(p)	((struct walt_task_struct *)(p)->android_vendor_data1)
#define sa_wrq(rq)	((struct walt_rq *)(rq)->android_vendor_data1)

#endif /* CONFIG_OPLUS_FEATURE_SCHED_ASSIST */

#endif /* _LINUX_SCHED_ASSIST_H_ */