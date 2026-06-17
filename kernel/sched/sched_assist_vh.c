// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2020 Oplus. All rights reserved.
 *
 * sched_assist vendor_hook registration
 *
 * Registers callbacks for Android vendor_hooks in fair.c and core.c.
 * These hooks call into sched_assist_common.c implementations.
 *
 * Uses CONFIG_OPLUS_FEATURE_SCHED_ASSIST gate.
 */

#include <linux/sched.h>
#include <trace/hooks/sched.h>
#include <linux/sched_assist/sched_assist_common.h>
#include "sched_assist_internal.h"

/* ─── fair.c hooks ──────────────────────────────────────────── */

static void sa_enqueue_task_fair(void *data, struct rq *rq,
				 struct task_struct *p, int flags)
{
#if IS_ENABLED(CONFIG_OPLUS_FEATURE_SCHED_UX_PRIORITY)
	enqueue_ux_thread_to_list(rq, p);
#else
	enqueue_ux_thread(rq, p);
#endif
}

static void sa_dequeue_task_fair(void *data, struct rq *rq,
				 struct task_struct *p, int flags)
{
#if IS_ENABLED(CONFIG_OPLUS_FEATURE_SCHED_UX_PRIORITY)
	dequeue_ux_thread_from_list(rq, p);
#else
	dequeue_ux_thread(rq, p);
#endif
}

static void sa_pick_next_entity(void *data, struct cfs_rq *cfs_rq,
				struct sched_entity *curr,
				struct sched_entity **se)
{
#ifdef CONFIG_OPLUS_FEATURE_AUDIO_OPT
	sched_assist_pick_next_entity(cfs_rq, se);
#endif
}

static void sa_replace_next_task_fair(void *data, struct rq *rq,
				      struct task_struct **p,
				      struct sched_entity **se,
				      bool *repick, bool simple,
				      struct task_struct *prev)
{
#if IS_ENABLED(CONFIG_OPLUS_FEATURE_SCHED_UX_PRIORITY)
	android_rvh_replace_next_task_fair_handler(rq, p, se, repick, simple);
#else
	pick_ux_thread(rq, p, se);
#endif
}

static void sa_check_preempt_tick(void *data, struct task_struct *p,
				  unsigned long *ideal_runtime,
				  bool *skip_preempt,
				  unsigned long delta_exec,
				  struct cfs_rq *cfs_rq,
				  struct sched_entity *curr,
				  unsigned int granularity)
{
	/*
	 * Ported from 4.19 fair.c:4633-4650
	 * Heavy load tasks get extended runtime and skip preempt checks.
	 */
	if (is_heavy_load_task(p))
		*ideal_runtime = HEAVY_LOAD_RUNTIME;
}

static void sa_check_preempt_wakeup(void *data, struct rq *rq,
				    struct task_struct *p,
				    bool *preempt, bool *nopreempt,
				    int wake_flags,
				    struct sched_entity *se,
				    struct sched_entity *pse,
				    int next_buddy_marked,
				    unsigned int granularity)
{
	struct task_struct *curr = rq->curr;

	if (!sysctl_sched_assist_enabled)
		return;

	/*
	 * Ported from 4.19 fair.c:8682-8685
	 * Heavy load current task should not be preempted.
	 */
	if (is_heavy_load_task(curr))
		return;

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_SCHED_UX_PRIORITY)
	oplus_check_preempt_wakeup_in_list(rq, p, curr, preempt, nopreempt);
#else
	if (should_ux_preempt_wakeup(p, curr))
		*preempt = true;
#endif
}

static void sa_select_task_rq_fair(void *data, struct task_struct *p,
				   int prev_cpu, int sd_flag,
				   int wake_flags, int *new_cpu)
{
	/*
	 * Ported from 4.19 fair.c:find_energy_efficient_cpu() — 4 modification points.
	 *
	 * 5.10 hook semantics: *new_cpu is initialized to -1 by the caller.
	 *   - If handler sets *new_cpu >= 0: function returns immediately,
	 *     skipping EAS and default path entirely.
	 *   - If handler leaves *new_cpu = -1: scheduler continues normally.
	 *
	 * 4.19 semantics: EAS already selected a CPU, sched_assist adjusts result.
	 *
	 * Key difference: In 5.10, we can only OVERRIDE (set >= 0, skip EAS)
	 * or DEFER (leave -1, let EAS decide). We cannot "adjust after EAS".
	 *
	 * Strategy:
	 *   - SA_LAUNCH scene: override to preferred (big) CPU — skip EAS.
	 *   - Slide scene + heavy UX misfit: defer to EAS, cannot fix in hook.
	 *   - Other scenes: defer to EAS.
	 */
	int target_cpu;

	if (!sysctl_sched_assist_enabled)
		return;

	if (!test_task_ux(p))
		return;

	/*
	 * SA_LAUNCH scene: move UX task to preferred (big) CPU.
	 * 4.19: set_ux_task_to_prefer_cpu(p, &best_energy_cpu)
	 * In 5.10: override *new_cpu to skip EAS entirely.
	 */
	if (sched_assist_scene(SA_LAUNCH)) {
		target_cpu = *new_cpu;
		set_ux_task_to_prefer_cpu(p, &target_cpu);
		if (target_cpu >= 0) {
			*new_cpu = target_cpu;
			return;
		}
	}

	/*
	 * Slide scene misfit: In 4.19, this adjusts the EAS result.
	 * In 5.10 hook, we cannot "adjust after EAS" — *new_cpu = -1 means
	 * EAS hasn't run yet. Defer to EAS; the misfit check happens in
	 * select_task_rq_fair's default path via task_fits_max().
	 * No action possible in this hook for slide misfit.
	 */
}

static void sa_can_migrate_task(void *data, struct task_struct *p,
				int dst_cpu, int *can_migrate)
{
	/*
	 * Call chain: load_balance() → detach_tasks() → can_migrate_task()
	 * Hook: fair.c:7880 — trace_android_rvh_can_migrate_task(p, dst_cpu, &can_migrate)
	 *
	 * Hook fires FIRST, before all other checks (throttled, cpus_allowed,
	 * running, cache hot). If handler sets *can_migrate = 0, function
	 * returns 0 immediately (task cannot be migrated).
	 *
	 * Default: *can_migrate = 1 (caller initializes before hook).
	 *
	 * 4.19 can_migrate_task() has no OPLUS_FEATURE_SCHED_ASSIST modification.
	 * Only CONFIG_OPLUS_FEATURE_FRAME_BOOST (fbg_skip_migration) and
	 * CONFIG_OPLUS_FEATURE_ABNORMAL_FLAG — both separate features.
	 */
}

static void sa_migrate_queued_task(void *data, struct rq *rq,
				   struct rq_flags *rf,
				   struct task_struct *p,
				   int new_cpu, int *detached)
{
	/*
	 * Call chain: load_balance() → detach_tasks() → detach_task() → hook
	 * Hook: fair.c:7973 — trace_android_rvh_migrate_queued_task(rq, rf, p, dst_cpu, &detached)
	 *
	 * Hook fires BEFORE deactivate_task() and set_task_cpu().
	 * If handler sets *detached = 1, function skips default detach logic.
	 * Note: hook may temporarily drop rq lock (rq_unpin_lock).
	 *
	 * Default: *detached = 0 (caller initializes before hook).
	 *
	 * 4.19 detach_task()/detach_one_task() have no OPLUS_FEATURE_SCHED_ASSIST
	 * modification. No action needed — handler kept as registered hook placeholder.
	 */
}

static void sa_newidle_balance(void *data, struct rq *this_rq,
			       struct rq_flags *rf, int *pulled_task,
			       int *done)
{
	/*
	 * Call chain: schedule() → pick_next_task() → newidle_balance() → hook
	 * Hook: fair.c:10931 — trace_android_rvh_sched_newidle_balance(this_rq, rf, &pulled_task, &done)
	 *
	 * Hook fires FIRST, before the main load balance loop.
	 * If handler sets *done = 1, function returns immediately with *pulled_task.
	 * If handler sets *pulled_task > 0, caller treats as "pulled a task".
	 *
	 * Default: *pulled_task = 0, *done = 0 (caller initializes before hook).
	 *
	 * 4.19 idle_balance() has no OPLUS_FEATURE_SCHED_ASSIST modification.
	 * No action needed — handler kept as registered hook placeholder.
	 */
}

static void sa_find_busiest_group(void *data,
				  struct sched_group *busiest,
				  struct rq *dst_rq, int *out_balance)
{
	/*
	 * Call chain: load_balance() → find_busiest_group() → hook
	 * Hook: fair.c:9555 — trace_android_rvh_find_busiest_group(busiest, dst_rq, &out_balance)
	 *
	 * Hook fires AFTER update_sd_lb_stats() computes statistics.
	 * Only in EAS path (sched_energy_enabled()).
	 * If handler sets *out_balance = 0, function jumps to out_balanced
	 * (no balancing needed).
	 *
	 * Default: *out_balance = 1 (caller initializes before hook).
	 *
	 * 4.19 find_busiest_group() has no OPLUS_FEATURE_SCHED_ASSIST modification.
	 * No action needed — handler kept as registered hook placeholder.
	 */
}

static void sa_find_busiest_queue(void *data, int dst_cpu,
				  struct sched_group *group,
				  struct cpumask *env_cpus,
				  struct rq **busiest, int *done)
{
	/*
	 * Call chain: load_balance() → find_busiest_queue() → hook
	 * Hook: fair.c:9679 — trace_android_rvh_find_busiest_queue(dst_cpu, group, env_cpus, &busiest, &done)
	 *
	 * Hook fires FIRST, before the CPU scan loop.
	 * If handler sets *done = 1, function returns immediately with *busiest.
	 * If handler sets *busiest to a specific rq, that's the result.
	 *
	 * Default: *busiest = NULL, *done = 0 (caller initializes before hook).
	 *
	 * 4.19 find_busiest_queue() has no OPLUS_FEATURE_SCHED_ASSIST modification.
	 * No action needed — handler kept as registered hook placeholder.
	 */
}

/* ─── core.c hooks ──────────────────────────────────────────── */

static void sa_try_to_wake_up(void *data, struct task_struct *p)
{
	/*
	 * 4.19 has no sched_assist modification in try_to_wake_up().
	 * No action needed — handler kept as registered hook placeholder.
	 */
}

static void sa_try_to_wake_up_success(void *data, struct task_struct *p)
{
	/*
	 * 4.19 has no sched_assist modification in try_to_wake_up success path.
	 * No action needed — handler kept as registered hook placeholder.
	 */
}

static void sa_sched_fork(void *data, struct task_struct *p)
{
	/*
	 * 4.19 has no sched_assist modification in sched_fork().
	 * init_task_ux_info() is called from fork.c, handled by sa_sched_fork_init().
	 * No action needed — handler kept as registered hook placeholder.
	 */
}

static void sa_sched_fork_init(void *data, struct task_struct *p)
{
	/* Initialize sched_assist fields for new task */
	sa_ux_state(p) = 0;
	atomic64_set(&sa_inherit_ux(p), 0);
	INIT_LIST_HEAD(&sa_ux_entry(p));
	sa_ux_depth(p) = 0;
	sa_enqueue_time(p) = 0;
	sa_inherit_ux_start(p) = 0;
	sa_sum_exec_baseline(p) = 0;
#ifdef CONFIG_OPLUS_UX_IM_FLAG
	sa_ux_im_flag(p) = 0;
#endif
}

static void sa_wake_up_new_task(void *data, struct task_struct *p)
{
	/*
	 * 4.19 has no sched_assist modification in wake_up_new_task().
	 * sched_assist_target_comm() is called from fs/exec.c (both 4.19 and 5.10).
	 * No action needed here — handler kept as registered hook placeholder.
	 */
}

/* ─── Additional hooks ──────────────────────────────────────── */

static void sa_tick_entry(void *data, struct rq *rq)
{
#if IS_ENABLED(CONFIG_OPLUS_FEATURE_SCHED_UX_PRIORITY)
	android_vh_scheduler_tick_handler(rq);
#endif
}

static void sa_sched_cpu_starting(void *data, int cpu)
{
	update_ux_sched_cputopo(cpu);
	ux_init_cpu_data();
}

static void sa_place_entity(void *data, struct cfs_rq *cfs_rq,
			    struct sched_entity *se, int initial,
			    u64 *vruntime)
{
	if (should_force_adjust_vruntime(se))
		place_entity_adjust_ux_task(cfs_rq, se, initial);
}

static void sa_sched_setaffinity(void *data, struct task_struct *p,
				 const struct cpumask *in_mask, int *retval)
{
	if (oplus_sched_ban_setaffinity(p, in_mask))
		*retval = -EPERM;
}

static void sa_free_task(void *data, struct task_struct *p)
{
	/* Clean up any remaining UX list references */
	if (!list_empty(&sa_ux_entry(p))) {
		list_del_init(&sa_ux_entry(p));
		/* Note: put_task_struct not needed here — task is being freed */
	}
}

/* ─── registration ──────────────────────────────────────────── */

static int __init sched_assist_vh_init(void)
{
	/* fair.c hooks */
	register_trace_android_rvh_enqueue_task_fair(
		sa_enqueue_task_fair, NULL);
	register_trace_android_rvh_dequeue_task_fair(
		sa_dequeue_task_fair, NULL);
	register_trace_android_rvh_pick_next_entity(
		sa_pick_next_entity, NULL);
	register_trace_android_rvh_replace_next_task_fair(
		sa_replace_next_task_fair, NULL);
	register_trace_android_rvh_check_preempt_tick(
		sa_check_preempt_tick, NULL);
	register_trace_android_rvh_check_preempt_wakeup(
		sa_check_preempt_wakeup, NULL);
	register_trace_android_rvh_select_task_rq_fair(
		sa_select_task_rq_fair, NULL);
	register_trace_android_rvh_can_migrate_task(
		sa_can_migrate_task, NULL);
	register_trace_android_rvh_migrate_queued_task(
		sa_migrate_queued_task, NULL);
	register_trace_android_rvh_sched_newidle_balance(
		sa_newidle_balance, NULL);
	register_trace_android_rvh_find_busiest_group(
		sa_find_busiest_group, NULL);
	register_trace_android_rvh_find_busiest_queue(
		sa_find_busiest_queue, NULL);

	/* core.c hooks */
	register_trace_android_rvh_try_to_wake_up(
		sa_try_to_wake_up, NULL);
	register_trace_android_rvh_try_to_wake_up_success(
		sa_try_to_wake_up_success, NULL);
	register_trace_android_rvh_sched_fork(
		sa_sched_fork, NULL);
	register_trace_android_rvh_sched_fork_init(
		sa_sched_fork_init, NULL);
	register_trace_android_rvh_wake_up_new_task(
		sa_wake_up_new_task, NULL);

	/* Additional hooks for sched_assist */
	register_trace_android_rvh_tick_entry(
		sa_tick_entry, NULL);
	register_trace_android_rvh_sched_cpu_starting(
		sa_sched_cpu_starting, NULL);
	register_trace_android_rvh_place_entity(
		sa_place_entity, NULL);
	register_trace_android_rvh_sched_setaffinity(
		sa_sched_setaffinity, NULL);
	register_trace_android_rvh_free_task(
		sa_free_task, NULL);

	return 0;
}
late_initcall(sched_assist_vh_init);
