// SPDX-License-Identifier: GPL-2.0-only
/*
 * binder sched_assist vendor_hook registration stubs
 *
 * Registers callbacks for Android vendor_hooks in binder.c.
 * These stubs will be populated with real logic when sched_assist
 * (kernel/sched_assist/) is ported from 4.19.
 *
 * Uses CONFIG_OPLUS_FEATURE_SCHED_ASSIST gate.
 */

#include <trace/hooks/binder.h>

/* ─── SCHED_ASSIST binder hooks (4) ───────────────────────── */

static void sa_binder_transaction(void *data,
				  struct binder_proc *target_proc,
				  struct binder_proc *proc,
				  struct binder_thread *thread,
				  struct binder_transaction_data *tr)
{
	/* TODO: sched_assist_binder_transaction() */
}

static void sa_binder_set_priority(void *data,
				   struct binder_transaction *t,
				   struct task_struct *task)
{
	/* TODO: FRAME_BOOST priority adjust */
}

static void sa_binder_wakeup_ilocked(void *data,
				     struct task_struct *task,
				     bool sync,
				     struct binder_proc *proc)
{
	/* TODO: HANS_FREEZE wakeup track */
}

static void sa_binder_special_task(void *data,
				   struct binder_transaction *t,
				   struct binder_proc *proc,
				   struct binder_thread *thread,
				   struct binder_work *w,
				   struct list_head *head,
				   bool sync, bool *special_task)
{
	/* TODO: OPLUS binder strategy special task */
}

/* ─── registration ───────────────────────────────────────── */

static int __init binder_assist_vh_init(void)
{
	register_trace_android_rvh_binder_transaction(
		sa_binder_transaction, NULL);
	register_trace_android_vh_binder_set_priority(
		sa_binder_set_priority, NULL);
	register_trace_android_vh_binder_wakeup_ilocked(
		sa_binder_wakeup_ilocked, NULL);
	register_trace_android_vh_binder_special_task(
		sa_binder_special_task, NULL);

	return 0;
}
late_initcall(binder_assist_vh_init);
