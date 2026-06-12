/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2020 Oplus. All rights reserved.
 *
 * OPLUS sched_assist task statistics for audio optimization.
 * Ported from 4.19 kernel/sched_assist/sched_assist_status.h
 * to 5.10 as a standalone header.
 *
 * In 4.19, struct task_info was embedded directly in task_struct.
 * In 5.10 GKI, it is embedded in walt_task_struct which overlays
 * task_struct->android_vendor_data1. Access via sa_oplus_task_info(task).
 */

#ifndef _OPLUS_SCHED_ASSIST_STATUS_H_
#define _OPLUS_SCHED_ASSIST_STATUS_H_

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_AUDIO_OPT)

enum task_stats_type {
	TST_SLEEP = 0,		/* update sleeping time when enq-wakeup */
	TST_RUNNABLE,		/* update runnable time when enq_deq */
	TST_EXEC,		/* update exec time when deq-sleep */
	TST_SCHED_TYPE_TATOL,	/* total type */
};

#define TASK_INFO_SAMPLE (4)

/**
 * struct task_info - per-task scheduling statistics for audio optimization
 *
 * @sa_info: circular buffer of recent scheduling time samples per type
 * @im_small: whether this task is classified as "small" for IM scheduling
 */
struct task_info {
	u64	sa_info[TST_SCHED_TYPE_TATOL][TASK_INFO_SAMPLE];
	bool	im_small;
};

extern int sysctl_sched_impt_tgid;

#endif /* CONFIG_OPLUS_FEATURE_AUDIO_OPT */

#endif /* _OPLUS_SCHED_ASSIST_STATUS_H_ */