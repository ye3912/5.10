/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2020 Oplus. All rights reserved.
 *
 * OPLUS locking strategy per-task metadata.
 * Ported from 4.19 include/linux/sched.h (CONFIG_OPLUS_LOCKING_STRATEGY)
 * to 5.10 as a standalone header.
 *
 * In 4.19, struct locking_info was embedded directly in task_struct.
 * In 5.10 GKI, it is embedded in walt_task_struct which overlays
 * task_struct->android_vendor_data1. Access via sa_lkinfo(task) macro.
 */

#ifndef _OPLUS_SCHED_ASSIST_LOCKING_H_
#define _OPLUS_SCHED_ASSIST_LOCKING_H_

#include <linux/types.h>

struct task_struct;

#if IS_ENABLED(CONFIG_OPLUS_LOCKING_STRATEGY)

/**
 * struct locking_info - per-task locking metadata for UX priority boost
 *
 * Tracks optimistic spinning and wait-time information for mutex/rwsem
 * to determine whether a UX task should receive priority boost while
 * waiting on a lock held by another task.
 *
 * @waittime_stamp: timestamp when task started waiting on this lock
 * @opt_spin_start_time: timestamp when optimistic spinning started;
 *   a task can't spin on both mutex and rwsem simultaneously, so one
 *   common field suffices.
 * @holder: pointer to the task currently holding the lock
 * @ux_contrib: whether this task contributed UX priority boost to holder
 * @is_block_ux: whether task was UX when added to waiter list; helps
 *   check if any UX task is waiting on this mutex/rwsem.
 */
struct locking_info {
	u64			waittime_stamp;
	u64			opt_spin_start_time;
	struct task_struct	*holder;
	bool			ux_contrib;
	bool			is_block_ux;
};

#endif /* CONFIG_OPLUS_LOCKING_STRATEGY */

#endif /* _OPLUS_SCHED_ASSIST_LOCKING_H_ */