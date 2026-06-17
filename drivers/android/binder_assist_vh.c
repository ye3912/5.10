// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2020 Oplus. All rights reserved.
 *
 * binder sched_assist vendor hooks — ported from 4.19 drivers/android/binder.c
 *
 * 4.19 directly modified binder.c with #ifdef OPLUS_FEATURE_SCHED_ASSIST.
 * 5.10 uses Android vendor hooks to inject the same behavior.
 *
 * Key porting decisions:
 *  - binder_proc.proc_type (SF detection): 4.19 added a field to binder_proc.
 *    5.10 cannot modify GKI structs. Use inline is_sf() check instead.
 *  - binder_transaction_priority main-thread check: 4.19 checked
 *    thread->looper & BINDER_LOOPER_STATE_ENTERED. The 5.10
 *    android_vh_binder_set_priority hook does not expose the binder_thread,
 *    so this check is omitted (conservative: always adjust priority).
 *  - Additional hooks registered: binder_wait_for_work (UX clear on idle),
 *    sync_txn_recvd (UX inherit on receive), binder_reply (UX clear on reply).
 */

#include <linux/sched.h>
#include <linux/sched_assist/sched_assist_common.h>
#include <linux/sched_assist/sched_assist_binder.h>
#include <trace/hooks/binder.h>

/*
 * is_sf() — detect surfaceflinger process (replaces 4.19 binder_proc.proc_type)
 *
 * 4.19 set proc->proc_type = 1 at binder_open() for SF.
 * 5.10 can't add fields to binder_proc; use comm check instead.
 */
static inline bool is_sf(struct task_struct *p)
{
	return p && strstr(p->comm, "surfaceflinger")
		&& (task_uid(p).val == 1000);
}

/*
 * is_binder_proc_sf() — check if binder proc belongs to SF
 * (matches 4.19 binder.c:3284-3288)
 */
static inline bool is_binder_proc_sf(struct binder_proc *proc)
{
	return proc && proc->tsk && is_sf(proc->tsk);
}

/* ─── SCHED_ASSIST binder hooks ──────────────────────────── */

/*
 * sa_binder_transaction — called at binder_transaction() entry
 *
 * Ported from 4.19 binder.c:3380-3385 and 3413-3419:
 * 1. If target is SF's proc (proc_type check), always inherit UX.
 * 2. Else if !oneway, inherit UX from current to target thread.
 * 3. If current is SF and target is UX group_leader on oneway, set_once_ux.
 */
static void sa_binder_transaction(void *data,
				  struct binder_proc *target_proc,
				  struct binder_proc *proc,
				  struct binder_thread *thread,
				  struct binder_transaction_data *tr)
{
#if IS_ENABLED(CONFIG_OPLUS_FEATURE_SCHED_ASSIST)
	if (!sysctl_sched_assist_enabled)
		return;

	if (!thread || !thread->task)
		return;

	/*
	 * 4.19: if (!oneway || proc->proc_type) binder_set_inherit_ux(...)
	 * 5.10 hook doesn't expose oneway flag directly; use TF_ONE_WAY from tr.
	 */
	if (!(tr->flags & TF_ONE_WAY) || is_binder_proc_sf(target_proc)) {
		binder_set_inherit_ux(thread->task, current);
	}

	/*
	 * 4.19:3413-3419: if is_sf(curr) && test_task_ux(grp_leader) && oneway
	 *                  → set_once_ux(thread->task)
	 */
	if ((tr->flags & TF_ONE_WAY) && is_sf(current)) {
		struct task_struct *grp_leader = thread->task->group_leader;

		if (grp_leader && test_task_ux(grp_leader))
			set_once_ux(thread->task);
	}
#endif /* CONFIG_OPLUS_FEATURE_SCHED_ASSIST */
}

/*
 * sa_binder_set_priority — called before setting binder thread priority
 *
 * 4.19 binder.c:1659-1665: skip priority change for main thread that
 * hasn't joined the binder pool (BINDER_LOOPER_STATE_ENTERED check).
 *
 * 5.10 hook doesn't expose binder_thread, so the looper state check
 * is omitted. Priority is always adjusted (conservative fallback).
 */
static void sa_binder_set_priority(void *data,
				   struct binder_transaction *t,
				   struct task_struct *task)
{
	/*
	 * TODO: If a future hook variant exposes binder_thread or looper
	 * state, add the main-thread pool-join check from 4.19 here.
	 * For now, this is intentionally empty — priority adjustment
	 * proceeds normally for all threads.
	 */
}

/*
 * sa_binder_wakeup_ilocked — called when a binder thread is woken
 *
 * 4.19 HANS_FREEZE wakeup tracking. No direct sched_assist logic here;
 * this hook is a registration point for future HANS integration.
 */
static void sa_binder_wakeup_ilocked(void *data,
				     struct task_struct *task,
				     bool sync,
				     struct binder_proc *proc)
{
	/* Reserved for HANS_FREEZE wakeup tracking — not yet ported */
}

/*
 * sa_binder_special_task — called to determine if transaction is "special"
 *
 * 4.19 CONFIG_OPLUS_BINDER_STRATEGY: obwork_restrict() special task logic.
 * Not directly part of sched_assist UX inheritance. Stub for future use.
 */
static void sa_binder_special_task(void *data,
				   struct binder_transaction *t,
				   struct binder_proc *proc,
				   struct binder_thread *thread,
				   struct binder_work *w,
				   struct list_head *head,
				   bool sync, bool *special_task)
{
	/* Reserved for CONFIG_OPLUS_BINDER_STRATEGY — not yet ported */
}

/* ─── Additional hooks (not in original stub) ────────────── */

/*
 * sa_binder_wait_for_work — called when binder thread waits for work
 *
 * Ported from 4.19 binder.c:4772-4778: when a binder thread becomes
 * idle (do_proc_work=true), unset inherit_ux. This prevents stale
 * UX inheritance from persisting after the transaction completes.
 */
static void sa_binder_wait_for_work(void *data,
				    bool do_proc_work,
				    struct binder_thread *tsk,
				    struct binder_proc *proc)
{
#if IS_ENABLED(CONFIG_OPLUS_FEATURE_SCHED_ASSIST)
	if (!sysctl_sched_assist_enabled)
		return;

	if (do_proc_work && tsk && tsk->task)
		binder_unset_inherit_ux(tsk->task);
#endif /* CONFIG_OPLUS_FEATURE_SCHED_ASSIST */
}

/*
 * sa_binder_sync_txn_recvd — called when a sync transaction is received
 *
 * Ported from 4.19 binder.c:5110-5114: when a binder thread reads a
 * transaction from a sender, inherit UX from the sender's task.
 *
 * 5.10 hook params: (task_struct *tsk, task_struct *from)
 *   - tsk: the receiving binder thread
 *   - from: the sending task
 */
static void sa_binder_sync_txn_recvd(void *data,
				     struct task_struct *tsk,
				     struct task_struct *from)
{
#if IS_ENABLED(CONFIG_OPLUS_FEATURE_SCHED_ASSIST)
	if (!sysctl_sched_assist_enabled)
		return;

	if (tsk && from)
		binder_set_inherit_ux(tsk, from);
#endif /* CONFIG_OPLUS_FEATURE_SCHED_ASSIST */
}

/*
 * sa_binder_reply — called at binder reply path
 *
 * Ported from 4.19 binder.c:4111-4115: after replying, unset inherit_ux
 * on the replying thread (no longer blocked by the original caller).
 *
 * Note: The 4.19 code only unsets if !proc->proc_type (not SF).
 * In 5.10 we use is_binder_proc_sf() inline check.
 */
static void sa_binder_reply(void *data,
			    struct binder_proc *target_proc,
			    struct binder_proc *proc,
			    struct binder_thread *thread,
			    struct binder_transaction_data *tr)
{
#if IS_ENABLED(CONFIG_OPLUS_FEATURE_SCHED_ASSIST)
	if (!sysctl_sched_assist_enabled)
		return;

	if (thread && thread->task && !is_binder_proc_sf(proc))
		binder_unset_inherit_ux(thread->task);
#endif /* CONFIG_OPLUS_FEATURE_SCHED_ASSIST */
}

/* ─── registration ───────────────────────────────────────── */

static int __init binder_assist_vh_init(void)
{
	/* Core transaction hooks */
	register_trace_android_rvh_binder_transaction(
		sa_binder_transaction, NULL);
	register_trace_android_vh_binder_set_priority(
		sa_binder_set_priority, NULL);

	/* Wakeup and special task hooks */
	register_trace_android_vh_binder_wakeup_ilocked(
		sa_binder_wakeup_ilocked, NULL);
	register_trace_android_vh_binder_special_task(
		sa_binder_special_task, NULL);

	/* UX lifecycle hooks (ported from 4.19 binder.c) */
	register_trace_android_vh_binder_wait_for_work(
		sa_binder_wait_for_work, NULL);
	register_trace_android_vh_sync_txn_recvd(
		sa_binder_sync_txn_recvd, NULL);
	register_trace_android_vh_binder_reply(
		sa_binder_reply, NULL);

	return 0;
}
late_initcall(binder_assist_vh_init);
