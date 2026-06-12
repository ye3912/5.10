/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * OPLUS SchedTune minimal replacement for 5.10
 *
 * Provides the interface expected by WALT OPLUS hooks without
 * the full Qualcomm downstream SchedTune cgroup subsystem.
 *
 * All Power Efficiency functions return 0 (disabled) by default.
 * Tuning via sysctl / vendor_hooks to be added in later phases.
 */
#ifndef _OPLUS_TUNE_H
#define _OPLUS_TUNE_H

struct task_struct;

/*
 * Window policy override — used in update_history() to select
 * demand calculation strategy (recent / max / avg / max_recent).
 * Returns 0 to preserve system-wide sysctl_sched_window_stats_policy.
 */
unsigned int schedtune_window_policy(struct task_struct *p);

/*
 * Wait-time discount — when set, a task's non-running time is
 * excluded from demand calculation, reducing frequency boost
 * for background / intermittent tasks.
 */
unsigned int uclamp_discount_wait_time(struct task_struct *p);

/*
 * Top-app task filter — excludes certain tasks from top-task
 * accounting even when they appear in the top bucket.
 */
unsigned int uclamp_top_task_filter(struct task_struct *p);

/*
 * Early-detection task filter — prevents a task from being
 * promoted to ED (Early Detection) status, capping its
 * frequency-request impact.
 */
unsigned int uclamp_ed_task_filter(struct task_struct *p);

#endif /* _OPLUS_TUNE_H */
