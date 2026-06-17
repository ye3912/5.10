// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2020 Oplus. All rights reserved.
 *
 * Minimal healthinfo stub for sched_info / cpuloadmonitor.
 *
 * 4.19: drivers/soc/oplus/healthinfo/healthinfo.c (1372 lines)
 * 5.10: Only provides sched_para[] definition and stub functions.
 *       Full OHM (OHealth Monitor) stack is not yet ported.
 */

#include <soc/oplus/healthinfo.h>

/* Global sched stat array — used by osi_cpuloadmonitor.c */
struct sched_stat_para sched_para[OHM_SCHED_TOTAL];

/* Stub: record sched event (not yet implemented) */
void ohm_schedstats_record(int sched_type,
			   struct task_struct *task, u64 delta_ms)
{
}

/* Stub: get current CPU load (not yet implemented) */
int ohm_get_cur_cpuload(bool ctrl)
{
	return 0;
}

/* Stub: trigger action with message (not yet implemented) */
void ohm_action_trig_with_msg(int type, char *msg)
{
}
