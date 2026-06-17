/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2020 Oplus. All rights reserved.
 */

#ifndef _FRAME_BOOST_H_
#define _FRAME_BOOST_H_

#include <linux/sched/walt.h>
#include "frame_info.h"
#include "frame_group.h"
#include "cluster_boost.h"

/*
 * Per-task accessor macros for frame_boost fields in walt_task_struct.
 *
 * 4.19 added fbg_state/fbg_depth/fbg_running/fbg_list/preferred_cluster_id
 * directly to task_struct. 5.10 GKI cannot modify task_struct; these fields
 * live in walt_task_struct overlaying task_struct->android_vendor_data1.
 */
static inline struct walt_task_struct *fbg_wts(struct task_struct *p)
{
	return (struct walt_task_struct *) p->android_vendor_data1;
}

#define fbg_state(p)		(fbg_wts(p)->fbg_state)
#define fbg_depth(p)		(fbg_wts(p)->fbg_depth)
#define fbg_running(p)		(fbg_wts(p)->fbg_running)
#define fbg_list(p)		(fbg_wts(p)->fbg_list)
#define fbg_preferred_cluster_id(p)	(fbg_wts(p)->preferred_cluster_id)

#define ofb_debug(fmt, ...) \
	pr_info("[frame boost][%s]"fmt, __func__, ##__VA_ARGS__)

#define ofb_err(fmt, ...) \
	pr_err("[frame boost][%s]"fmt, __func__, ##__VA_ARGS__)

#define SLIDE_SCENE    (1 << 0)
#define INPUT_SCENE    (1 << 1)

enum STUNE_BOOST_TYPE {
	BOOST_DEF_MIGR = 0,
	BOOST_DEF_FREQ,
	BOOST_UTIL_FRAME_RATE,
	BOOST_UTIL_MIN_THRESHOLD,
	BOOST_UTIL_MIN_OBTAIN_VIEW,
	BOOST_UTIL_MIN_TIMEOUT,
	BOOST_SF_MIGR,
	BOOST_SF_FREQ,
	BOOST_SF_MIGR_NONGPU,
	BOOST_SF_FREQ_NONGPU,
	BOOST_SF_MIGR_GPU,
	BOOST_SF_FREQ_GPU,
	BOOST_ED_TASK_MID_DURATION,
	BOOST_ED_TASK_MID_UTIL,
	BOOST_ED_TASK_MAX_DURATION,
	BOOST_ED_TASK_MAX_UTIL,
	BOOST_ED_TASK_TIME_OUT_DURATION,
	BOOST_MAX_TYPE,
};

struct fbg_vendor_hook {
	void (*update_freq)(struct rq *rq, unsigned int flags);
};

extern unsigned int sysctl_frame_boost_enable;
extern unsigned int sysctl_frame_boost_debug;
extern int stune_boost[BOOST_MAX_TYPE];
extern struct fbg_vendor_hook fbg_hook;

void fbg_set_stune_boost(int value, unsigned int type);
int fbg_get_stune_boost(unsigned int type);
void fbg_sysctl_init(void);
#endif /* _FRAME_BOOST_H_ */
