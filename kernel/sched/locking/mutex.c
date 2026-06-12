// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2020 Oplus. All rights reserved.
 *
 * Ported from 4.19: kernel/sched_assist/sync/mutex.c
 * 5.10 adaptations:
 *  - Removed LINUX_VERSION_CODE guards (always 5.10 path)
 *  - __mutex_owner() uses 5.10 native MUTEX_FLAGS
 *  - sched_clock() available as-is in 5.10
 */

#include <linux/sched.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/sched/clock.h>
#include <linux/module.h>
#include <linux/kernel.h>

#include <linux/sched_assist/sched_assist_common.h>

#include "locking_main.h"

#ifdef CONFIG_OPLUS_LOCKING_OSQ
#include "mutex.h"
#endif
#ifdef CONFIG_OPLUS_LOCKING_MONITOR
#include "kern_lock_stat.h"
#endif

/* 5.10: __mutex_owner() is available in mutex.h, use it directly */

static void mutex_list_add_ux(struct list_head *entry, struct list_head *head)
{
	struct list_head *pos = NULL;
	struct list_head *n = NULL;
	struct mutex_waiter *waiter = NULL;

	list_for_each_safe(pos, n, head) {
		waiter = list_entry(pos, struct mutex_waiter, list);
		if (!test_task_ux(waiter->task)) {
			list_add(entry, waiter->list.prev);
			return;
		}
	}
	if (pos == head)
		list_add_tail(entry, head);
}

void mutex_list_add(struct task_struct *task, struct list_head *entry,
		    struct list_head *head, struct mutex *lock)
{
	bool is_ux = test_task_ux(task);

	if (!entry || !head || !lock)
		return;

	if (is_ux && !lock->ux_dep_task)
		mutex_list_add_ux(entry, head);
	else
		list_add_tail(entry, head);
}

void mutex_set_inherit_ux(struct mutex *lock, struct task_struct *task)
{
	bool is_ux = false;
	struct task_struct *owner = NULL;

	if (!lock)
		return;

	is_ux = test_set_inherit_ux(task);
	owner = __mutex_owner(lock);

	if (is_ux && !lock->ux_dep_task) {
		int type = get_ux_state_type(owner);

		if ((UX_STATE_NONE == type) || (UX_STATE_INHERIT == type)) {
			set_inherit_ux(owner, INHERIT_UX_MUTEX,
				       sa_ux_depth(task), sa_ux_state(task));
			lock->ux_dep_task = owner;
		}
	}
}

void mutex_unset_inherit_ux(struct mutex *lock, struct task_struct *task)
{
	if (lock && lock->ux_dep_task == task) {
		unset_inherit_ux(task, INHERIT_UX_MUTEX);
		lock->ux_dep_task = NULL;
	}
}

/* implement vendor hook in kernel/locking/mutex.c */
#ifdef CONFIG_OPLUS_LOCKING_OSQ
static void mutex_update_ux_cnt_when_add(struct mutex *lock)
{
	struct oplus_mutex *om = get_oplus_mutex(lock);

	if (unlikely(!current))
		return;

	/* Record the ux flag when task is added to waiter list */
	sa_lkinfo(current).is_block_ux = (sa_ux_state(current) & 0xf) ||
				      (current->prio < MAX_RT_PRIO);
	if (sa_lkinfo(current).is_block_ux)
		atomic_long_inc(&om->count);
}

static void mutex_update_ux_cnt_when_remove(struct mutex *lock)
{
	struct oplus_mutex *om = get_oplus_mutex(lock);

	if (unlikely(!current))
		return;

	/*
	 * Update the ux tasks when tasks exit the waiter list.
	 * We only care about the recorded ux flag, but don't
	 * care about the current ux flag. This avoids the corruption
	 * of the number caused by ux change when in waiter list.
	 */
	if (sa_lkinfo(current).is_block_ux) {
		/* Count == 0! Something is wrong! */
		WARN_ON_ONCE(!atomic_long_read(&om->count));
		atomic_long_add(-1, &om->count);
		sa_lkinfo(current).is_block_ux = false;
	}
}
#endif

void locking_vh_alter_mutex_list_add(struct mutex *lock,
			struct mutex_waiter *waiter, struct list_head *list,
			bool *already_on_list)
{
#ifdef CONFIG_OPLUS_LOCKING_OSQ
	if (likely(locking_opt_enable(LK_OSQ_ENABLE)))
		mutex_update_ux_cnt_when_add(lock);
#endif
}

void locking_vh_mutex_wait_start(struct mutex *lock)
{
#ifdef CONFIG_OPLUS_LOCKING_MONITOR
	monitor_mutex_wait_start(lock);
#endif
}

void locking_vh_mutex_wait_finish(struct mutex *lock)
{
#ifdef CONFIG_OPLUS_LOCKING_MONITOR
	monitor_mutex_wait_finish(lock);
#endif
#ifdef CONFIG_OPLUS_LOCKING_OSQ
	mutex_update_ux_cnt_when_remove(lock);
#endif
}

#ifdef CONFIG_OPLUS_LOCKING_OSQ
static int mutex_opt_spin_time_threshold = NSEC_PER_USEC * 60;
static int mutex_ux_opt_spin_time_threshold = NSEC_PER_USEC * 100;
static int mutex_opt_spin_total_cnt = 0;
static int mutex_opt_spin_timeout_exit_cnt = 0;
module_param(mutex_opt_spin_time_threshold, int, 0644);
module_param(mutex_ux_opt_spin_time_threshold, int, 0644);
module_param(mutex_opt_spin_total_cnt, int, 0444);
module_param(mutex_opt_spin_timeout_exit_cnt, int, 0444);

void locking_vh_mutex_opt_spin_start(struct mutex *lock,
		bool *time_out, int *cnt)
{
	u64 delta;

	if (unlikely(IS_ERR_OR_NULL(current)))
		return;

	if (!sa_lkinfo(current).opt_spin_start_time) {
		/* Note: Should use atomic operations? */
		mutex_opt_spin_total_cnt++;
		sa_lkinfo(current).opt_spin_start_time = sched_clock();
		return;
	}

	if (unlikely(!locking_opt_enable(LK_OSQ_ENABLE)))
		return;

	++(*cnt);
	/*
	 * Check whether time is out every 16 times in order
	 * to reduce the cost of calling sched_clock()
	 */
	if ((*cnt) & 0xf)
		return;

	delta = sched_clock() - sa_lkinfo(current).opt_spin_start_time;
	if (((sa_ux_state(current) & 0xf) &&
	     delta > mutex_ux_opt_spin_time_threshold) ||
	    (!(sa_ux_state(current) & 0xf) &&
	     delta > mutex_opt_spin_time_threshold)) {
		/* Note: Should use atomic operations? */
		mutex_opt_spin_timeout_exit_cnt++;
		*time_out = true;
	}
}

void locking_vh_mutex_opt_spin_finish(struct mutex *lock, bool taken)
{
#ifdef CONFIG_OPLUS_INTERNAL_VERSION
	u64 delta;
#endif

	if (unlikely(IS_ERR_OR_NULL(current)))
		return;

	if (likely(sa_lkinfo(current).opt_spin_start_time)) {
#ifdef CONFIG_OPLUS_INTERNAL_VERSION
		delta = sched_clock() - sa_lkinfo(current).opt_spin_start_time;
		handle_wait_stats(OSQ_MUTEX, delta);
#endif
		sa_lkinfo(current).opt_spin_start_time = 0;
	}
}

#define MAX_VALID_COUNT (1000)
void locking_vh_mutex_can_spin_on_owner(struct mutex *lock, int *retval)
{
	struct oplus_mutex *om = get_oplus_mutex(lock);
	long block_ux_cnt;

	if (unlikely(!locking_opt_enable(LK_OSQ_ENABLE) || !current))
		return;

	/* ux and rt task just go */
	if ((sa_ux_state(current) & 0xf) || current->prio < MAX_RT_PRIO)
		return;

	/*
	 * If some ux or rt task is in the waiter list,
	 * non-ux can't optimistic spin.
	 */
	block_ux_cnt = atomic_long_read(&om->count);
	if (block_ux_cnt > 0 && block_ux_cnt < MAX_VALID_COUNT)
		*retval = 0;
}
#endif
