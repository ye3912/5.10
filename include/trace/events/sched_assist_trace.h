/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2020 Oplus. All rights reserved.
 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM oplus_sched

#if !defined(_TRACE_OPLUS_SCHED_ASSIST_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_OPLUS_SCHED_ASSIST_H

#include <linux/sched.h>
#include <linux/tracepoint.h>

TRACE_EVENT(oplus_tp_sched_change_ux,

	TP_PROTO(int chg_ux, int target_cpu),

	TP_ARGS(chg_ux, target_cpu),

	TP_STRUCT__entry(
		__field(int, chg_ux)
		__field(int, target_cpu)
	),

	TP_fast_assign(
		__entry->chg_ux = chg_ux;
		__entry->target_cpu = target_cpu;
	),

	TP_printk("chg_ux=%d target_cpu=%d", __entry->chg_ux,
		  __entry->target_cpu)
);

TRACE_EVENT(oplus_tp_sched_switch_ux,

	TP_PROTO(int chg_ux, int target_cpu),

	TP_ARGS(chg_ux, target_cpu),

	TP_STRUCT__entry(
		__field(int, chg_ux)
		__field(int, target_cpu)
	),

	TP_fast_assign(
		__entry->chg_ux = chg_ux;
		__entry->target_cpu = target_cpu;
	),

	TP_printk("chg_ux=%d target_cpu=%d", __entry->chg_ux,
		  __entry->target_cpu)
);

#ifdef CONFIG_OPLUS_FEATURE_SCHED_SPREAD
TRACE_EVENT(sched_assist_spread_tasks,

	TP_PROTO(struct task_struct *p, int sched_type, int lowest_nr_cpu),

	TP_ARGS(p, sched_type, lowest_nr_cpu),

	TP_STRUCT__entry(
		__field(int, pid)
		__array(char, comm, TASK_COMM_LEN)
		__field(int, sched_type)
		__field(int, lowest_nr_cpu)
	),

	TP_fast_assign(
		__entry->pid = p->pid;
		memcpy(__entry->comm, p->comm, TASK_COMM_LEN);
		__entry->sched_type = sched_type;
		__entry->lowest_nr_cpu = lowest_nr_cpu;
	),

	TP_printk("comm=%-12s pid=%d sched_type=%d lowest_nr_cpu=%d",
		  __entry->comm, __entry->pid, __entry->sched_type,
		  __entry->lowest_nr_cpu)
);
#endif /* CONFIG_OPLUS_FEATURE_SCHED_SPREAD */

#ifdef CONFIG_LOCKING_PROTECT
DECLARE_EVENT_CLASS(sched_locking_template,

	TP_PROTO(struct task_struct *p, int lk_depth, int lk_nr),

	TP_ARGS(p, lk_depth, lk_nr),

	TP_STRUCT__entry(
		__array(char, comm, TASK_COMM_LEN)
		__field(int, pid)
		__field(int, lk_depth)
		__field(int, lk_nr)
	),

	TP_fast_assign(
		memcpy(__entry->comm, p->comm, TASK_COMM_LEN);
		__entry->pid = p->pid;
		__entry->lk_depth = lk_depth;
		__entry->lk_nr = lk_nr;
	),

	TP_printk("comm=%s pid=%d lk_depth=%d rq_lk_nr=%d",
		  __entry->comm, __entry->pid, __entry->lk_depth,
		  __entry->lk_nr)
);

DEFINE_EVENT(sched_locking_template, enqueue_locking_thread,
	TP_PROTO(struct task_struct *p, int lk_depth, int lk_nr),
	TP_ARGS(p, lk_depth, lk_nr));

DEFINE_EVENT(sched_locking_template, dequeue_locking_thread,
	TP_PROTO(struct task_struct *p, int lk_depth, int lk_nr),
	TP_ARGS(p, lk_depth, lk_nr));

DEFINE_EVENT(sched_locking_template, select_locking_thread,
	TP_PROTO(struct task_struct *p, int lk_depth, int lk_nr),
	TP_ARGS(p, lk_depth, lk_nr));
#endif /* CONFIG_LOCKING_PROTECT */

#endif /* _TRACE_OPLUS_SCHED_ASSIST_H */

/* This part must be outside protection */
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE sched_assist_trace
#include <trace/define_trace.h>
