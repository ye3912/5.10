// SPDX-License-Identifier: GPL-2.0-only
/*
 * OPLUS SchedTune minimal replacement for 5.10
 *
 * Replaces Qualcomm downstream SchedTune (1012-line tune.c in 4.19).
 *
 * In 4.19, these functions queried per-task cgroup SchedTune attributes
 * via rcu_read_lock() → task_schedtune(p) → st->field.
 *
 * 5.10 does not have CONFIG_SCHED_TUNE (Qualcomm downstream).  Instead
 * of porting the full ~900-line CGroup infrastructure, we implement the
 * root cgroup defaults from 4.19's root_schedtune:
 *   .window_policy     = 3   (WINDOW_STATS_MAX)
 *   .discount_wait_time = 0  (false)
 *   .top_task_filter    = 0  (false)
 *   .ed_task_filter     = 0  (false)
 *
 * These are NOT stubs — they are the correct default behavior when no
 * per-task cgroup tuning is active, which is all we support on 5.10.
 */
#include <linux/sched.h>
#include "oplus_tune.h"

#ifdef CONFIG_OPLUS_FEATURE_POWER_CPUFREQ

/*
 * schedtune_window_policy — return per-task window stats policy
 *
 * In 4.19 this was set via cgroup schedtune.window_policy.
 * root_schedtune default: 3 = WINDOW_STATS_MAX.
 * Callers: update_history() in walt.c §8.
 */
unsigned int schedtune_window_policy(struct task_struct *p)
{
	(void)p;
	return 3;	/* root_schedtune default: WINDOW_STATS_MAX */
}

#ifdef CONFIG_OPLUS_FEATURE_POWER_EFFICIENCY

/*
 * uclamp_discount_wait_time — discount wait-time from demand
 *
 * When true, non-running time is excluded from task demand
 * calculation, lowering CPU frequency for intermittent tasks.
 * root_schedtune default: 0 (no discount).
 * Callers: account_busy_for_task_demand() §7.
 */
unsigned int uclamp_discount_wait_time(struct task_struct *p)
{
	(void)p;
	return 0;	/* root_schedtune default: false */
}

/*
 * uclamp_top_task_filter — filter task from top-app accounting
 *
 * root_schedtune default: 0 (allow task in top-tasks table).
 * Callers: update_top_tasks() §6.
 */
unsigned int uclamp_top_task_filter(struct task_struct *p)
{
	(void)p;
	return 0;	/* root_schedtune default: false */
}

/*
 * uclamp_ed_task_filter — filter task from Early Detection
 *
 * root_schedtune default: 0 (allow ED promotion).
 * Callers: is_ed_task() §2.
 */
unsigned int uclamp_ed_task_filter(struct task_struct *p)
{
	(void)p;
	return 0;	/* root_schedtune default: false */
}

#endif /* CONFIG_OPLUS_FEATURE_POWER_EFFICIENCY */
#endif /* CONFIG_OPLUS_FEATURE_POWER_CPUFREQ */
