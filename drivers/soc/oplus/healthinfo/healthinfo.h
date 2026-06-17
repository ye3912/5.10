/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2020 Oplus. All rights reserved.
 *
 * Minimal healthinfo stub for sched_info / cpuloadmonitor.
 *
 * 4.19: drivers/soc/oplus/healthinfo/healthinfo.h (139 lines)
 * 5.10: Only the types used by fs/proc/sched_info/ are provided here.
 *       Full OHM (OHealth Monitor) stack is not yet ported.
 */

#ifndef _HEALTHINFO_H_
#define _HEALTHINFO_H_

#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/cpumask.h>

/* OHM sched type indices — must match full healthinfo.h */
#define OHM_SCHED_IOWAIT	0
#define OHM_SCHED_SCHEDLATENCY	1
#define OHM_SCHED_FSYNC		2
#define OHM_SCHED_EMMCIO	3
#define OHM_SCHED_DSTATE	4
#define OHM_SCHED_TOTAL		12

/* cgroup IDs for healthinfo — matches 4.19 healthinfo.h */
#define SA_CGROUP_SYS_BACKGROUND	1
#define SA_CGROUP_FOREGROUND		2
#define SA_CGROUP_BACKGROUND		3
#define SA_CGROUP_TOP_APP		4
#define SA_CGROUP_UX			9

struct sched_stat_common {
	u64 max_ms;
	u64 high_cnt;
	u64 low_cnt;
	u64 total_ms;
	u64 total_cnt;
};

struct long_wait_record {
	u32 pid;
	u32 priv;
	u64 timestamp;
	u64 timestamp_ns;
	u32 ms;
};

#define LWR_SHIFT	3
#define LWR_MASK	((1ULL << LWR_SHIFT) - 1)
#define LWR_SIZE	(1ULL << LWR_SHIFT)

struct sched_stat_para {
	bool ctrl;
	bool logon;
	bool trig;
	int low_thresh_ms;
	int high_thresh_ms;
	u64 delta_ms;
	spinlock_t lock;
	struct sched_stat_common all;
	struct sched_stat_common fg;
	struct sched_stat_common ux;
	struct sched_stat_common top;
	struct sched_stat_common bg;
	struct sched_stat_common sysbg;
	atomic_t lwr_index;
	struct long_wait_record last_n_lwr[LWR_SIZE];
};

extern struct sched_stat_para sched_para[OHM_SCHED_TOTAL];

extern void ohm_schedstats_record(int sched_type,
				  struct task_struct *task, u64 delta_ms);
extern int ohm_get_cur_cpuload(bool ctrl);
extern void ohm_action_trig_with_msg(int type, char *msg);

#endif /* _HEALTHINFO_H_ */
