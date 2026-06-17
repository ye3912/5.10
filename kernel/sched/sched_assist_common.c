// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2020 Oplus. All rights reserved.
 *
 * sched_assist core implementation — ported from 4.19 to 5.10 GKI.
 *
 * 5.10 adaptations:
 *  - Removed LINUX_VERSION_CODE guards (always 5.10)
 *  - task_struct/rq direct field access → sa_ux_state()/sa_wrq() macros
 *  - fair_sched_class/rt_sched_class → direct reference (compiled into vmlinux)
 *  - struct file_operations → struct proc_ops for /proc nodes
 *  - p->state → READ_ONCE(p->__state)
 *  - p->mmap_sem → p->mmap_lock (renamed in 5.8)
 *  - kallsyms_lookup_name removed; systrace uses trace_printk directly
 *  - Missing Kconfig options (SCHED_TUNE, OPLUS_SS_LOCKER_OPT,
 *    OPLUS_FEATURE_AUDIO_CAMUX_OFF) → code blocks naturally excluded
 */

#include <linux/sched.h>
#include <linux/list.h>
#include <linux/jiffies.h>
#include <linux/reciprocal_div.h>
#include <trace/events/sched.h>
#include <../kernel/sched/sched.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <../fs/proc/internal.h>
#include <linux/thread_info.h>

#include <linux/sched_assist/sched_assist_common.h>
#include "sched_assist_internal.h"

#ifdef CONFIG_MMAP_LOCK_OPT
#include <linux/mm.h>
#include <linux/rwsem.h>
#endif

/* For 5.10, walt.h is in kernel/sched/walt/ */
#include <../kernel/sched/walt/walt.h>

#ifdef CONFIG_OPLUS_FEATURE_AUDIO_OPT
#include <linux/sched_assist/sched_assist_status.h>
#endif

#define CREATE_TRACE_POINTS
#include <trace/events/sched_assist_trace.h>

#define MS_TO_NS (1000000)
#define MAX_INHERIT_GRAN ((u64)(64 * MS_TO_NS))

/* Global variables */
int ux_min_sched_delay_granularity;
int ux_max_inherit_exist = 1000;
int ux_max_inherit_granularity = 32;
int ux_min_migration_delay = 10;
int ux_max_over_thresh = 2000;

/* WALT boost threshold — used by SF misfit and task misfit checks */
int sysctl_boost_task_threshold = 300;

/* for slide boost */
int sysctl_animation_type;
int sysctl_input_boost_enabled;
int sysctl_sched_assist_ib_duration_coedecay = 1;
u64 sched_assist_input_boost_duration;
int sched_assist_ib_duration_coedecay = 1;

#define S2NS_T 1000000
#define BGAPP 3

static int param_ux_debug;
module_param_named(debug, param_ux_debug, uint, 0644);

int global_debug_enabled;
static DEFINE_PER_CPU(int, prev_ux_state);
#if IS_ENABLED(CONFIG_OPLUS_FEATURE_SCHED_UX_PRIORITY)
static DEFINE_PER_CPU(int, prev_ux_priority);
#endif

struct ux_util_record sf_target[SF_GROUP_COUNT] = {
	{"surfaceflinger", 0, 0},
	{"RenderEngine", 0, 0},
};

pid_t sf_pid;
pid_t re_pid;

/* record important process tgid */
pid_t save_audio_tgid;
pid_t save_top_app_tgid;
unsigned int top_app_type;
struct cpumask nr_mask;

struct ux_sched_cputopo ux_sched_cputopo;

#ifdef CONFIG_OPLUS_FEATURE_SCHED_SPREAD
DEFINE_PER_CPU(struct task_count_rq, task_lb_count);
#endif

/*
 * Heavy load detection — ported from 4.19 kernel/special_opt/special_opt.c
 *
 * When sysctl_cpu_multi_thread is enabled, tasks with util > 80% of CPU
 * capacity are considered "heavy load". check_preempt_tick extends their
 * runtime and check_preempt_wakeup skips preemption for them.
 */
static int sysctl_cpu_multi_thread;
module_param_named(enable, sysctl_cpu_multi_thread, uint, 0644);

bool is_heavy_load_task(struct task_struct *p)
{
	int cpu;
	unsigned long thresh_load;
	struct reciprocal_value spc_rdiv = reciprocal_value(100);

	if (!sysctl_cpu_multi_thread || !p)
		return false;
	cpu = task_cpu(p);
	thresh_load = capacity_orig_of(cpu) * HEAVY_LOAD_SCALE;
	if (task_util(p) > reciprocal_divide(thresh_load, spc_rdiv))
		return true;
	return false;
}

/*
 * Forward declarations for functions defined later in this file.
 */
static void sched_init_ux_cputopo(void);

/*
 * Scene check helpers
 */
bool slide_scene(void)
{
	return sched_assist_scene(SA_SLIDE) || sched_assist_scene(SA_ANIM) ||
	       sched_assist_scene(SA_INPUT);
}

static bool sf_boost_scene(void)
{
	return sched_assist_scene(SA_SLIDE) || sched_assist_scene(SA_ANIM) ||
	       sched_assist_scene(SA_INPUT) || sched_assist_scene(SA_GPU_COMPOSITION);
}

/*
 * CPU topology initialization
 */
static inline void sched_init_ux_cputopo(void)
{
	int i = 0;

	ux_sched_cputopo.cls_nr = 0;
	for (; i < NR_CPUS; ++i) {
		cpumask_clear(&ux_sched_cputopo.sched_cls[i].cpus);
		ux_sched_cputopo.sched_cls[i].capacity = ULONG_MAX;
	}
}

/*
 * update_ux_sched_cputopo - build sorted CPU capacity clusters
 *
 * Called from sched_cpu_starting hook. Uses arch_scale_cpu_capacity()
 * to determine CPU capacity and groups CPUs into clusters sorted by
 * ascending capacity (silver → gold → prime).
 */
void update_ux_sched_cputopo(int cpu)
{
	unsigned long capacity = arch_scale_cpu_capacity(cpu);
	int i, j;

	if (cpu >= NR_CPUS)
		return;

	/* Find existing cluster with matching capacity */
	for (i = 0; i < ux_sched_cputopo.cls_nr; i++) {
		if (ux_sched_cputopo.sched_cls[i].capacity == capacity) {
			cpumask_set_cpu(cpu, &ux_sched_cputopo.sched_cls[i].cpus);
			return;
		}
	}

	/* New cluster — insert sorted by ascending capacity */
	for (i = 0; i < ux_sched_cputopo.cls_nr; i++) {
		if (capacity < ux_sched_cputopo.sched_cls[i].capacity)
			break;
	}

	/* Shift existing clusters right */
	for (j = ux_sched_cputopo.cls_nr; j > i; j--) {
		ux_sched_cputopo.sched_cls[j] = ux_sched_cputopo.sched_cls[j - 1];
	}

	ux_sched_cputopo.sched_cls[i].capacity = capacity;
	cpumask_clear(&ux_sched_cputopo.sched_cls[i].cpus);
	cpumask_set_cpu(cpu, &ux_sched_cputopo.sched_cls[i].cpus);
	ux_sched_cputopo.cls_nr++;
}

/*
 * Basic sched_entity helpers
 */
static inline struct task_struct *task_of(struct sched_entity *se)
{
	return container_of(se, struct task_struct, se);
}

static int entity_before(struct sched_entity *a, struct sched_entity *b)
{
	return (s64)(a->vruntime - b->vruntime) < 0;
}

static int entity_over(struct sched_entity *a, struct sched_entity *b)
{
	return (s64)(a->vruntime - b->vruntime) > (s64)ux_max_over_thresh * S2NS_T;
}

noinline int tracing_mark_write(const char *buf)
{
	trace_printk(buf);
	return 0;
}

void ux_state_systrace_c(unsigned int cpu, struct task_struct *p)
{
	if (unlikely(global_debug_enabled & DEBUG_SYSTRACE)) {
		int ux_state = sa_ux_state(p) & (SCHED_ASSIST_UX_MASK |
			POSSIBLE_UX_MASK | SA_TYPE_INHERIT |
			SA_TYPE_ID_CAMERA_PROVIDER | SA_TYPE_ID_ALLOCATOR_SER);

		if (per_cpu(prev_ux_state, cpu) != ux_state) {
			char buf[256];

			snprintf(buf, sizeof(buf), "C|9999|Cpu%d_ux_state|%d\n",
				 cpu, ux_state);
			tracing_mark_write(buf);
			per_cpu(prev_ux_state, cpu) = ux_state;
		}
	}
}

void sa_scene_systrace_c(void)
{
	if (unlikely(global_debug_enabled & DEBUG_SYSTRACE)) {
		static int prev_ux_scene;
		int assist_scene = sysctl_sched_assist_scene;

		if (prev_ux_scene != assist_scene) {
			char buf[64];

			snprintf(buf, sizeof(buf), "C|9999|Ux_Scene|%d\n",
				 sysctl_sched_assist_scene);
			tracing_mark_write(buf);
			prev_ux_scene = assist_scene;
		}
	}
}

extern const struct sched_class fair_sched_class;
extern const struct sched_class rt_sched_class;

bool should_force_adjust_vruntime(struct sched_entity *se)
{
	struct task_struct *se_task = NULL;

	if (!entity_is_task(se))
		return false;

	se_task = task_of(se);
	/* requeue runnable inherit ux task should be adjusted */
	if (se_task && (sa_ux_state(se_task) & SA_TYPE_INHERIT))
		return true;

	return false;
}

/*
 * test_task_identify_ux - check if task has a specific identity UX type
 *
 * Camera provider and allocator server tasks get special identity bits
 * that persist independently of UX scheduling decisions.
 */
bool test_task_identify_ux(struct task_struct *task, int id_type_ux)
{
	if (id_type_ux == SA_TYPE_ID_CAMERA_PROVIDER) {
		struct task_struct *grp_leader = task->group_leader;

#ifdef CONFIG_OPLUS_FEATURE_AUDIO_OPT
		if (save_audio_tgid != 0)
			return false;
#endif
		/* consider provider's HwBinder in configstream */
		if ((sa_ux_state(task) & SA_TYPE_LISTPICK) &&
		    (sa_ux_state(grp_leader) & SA_TYPE_ID_CAMERA_PROVIDER))
			return true;
		return (sa_ux_state(task) & SA_TYPE_ID_CAMERA_PROVIDER) &&
		       (sysctl_sched_assist_scene & SA_CAMERA);
	} else if (id_type_ux == SA_TYPE_ID_ALLOCATOR_SER) {
		if (task && (sa_ux_state(task) & SA_TYPE_ID_ALLOCATOR_SER) &&
		    (sysctl_sched_assist_scene & SA_CAMERA))
			return true;
	}

	return false;
}

inline bool test_task_ux(struct task_struct *task)
{
	if (unlikely(!sysctl_sched_assist_enabled))
		return false;

	if (!task)
		return false;

	if (task->sched_class != &fair_sched_class &&
	    task->sched_class != &rt_sched_class)
		return false;

	if (sa_ux_state(task) & (SA_TYPE_HEAVY | SA_TYPE_LIGHT |
				 SA_TYPE_ANIMATOR | SA_TYPE_LISTPICK |
				 SA_TYPE_ID_CAMERA_PROVIDER)) {
#if IS_ENABLED(CONFIG_OPLUS_FEATURE_SCHED_UX_PRIORITY)
		unsigned int limit = ux_task_exec_limit(task);

		if (sa_wts(task)->total_exec && (sa_wts(task)->total_exec > limit))
			return false;
#endif
		return true;
	}

	return false;
}

#ifdef CONFIG_OPLUS_FEATURE_SCHED_SPREAD
#define NR_IMBALANCE_THRESHOLD (24)
void update_rq_nr_imbalance(int cpu)
{
	int total_nr = 0;
	int i = -1;

	for_each_online_cpu(i) {
		total_nr += cpu_rq(i)->nr_running;
	}

	if (total_nr > NR_IMBALANCE_THRESHOLD * num_online_cpus())
		cpumask_set_cpu(cpu, &nr_mask);
	else
		cpumask_clear_cpu(cpu, &nr_mask);
}

bool should_force_spread_tasks(void)
{
	return is_spread_task_enabled() &&
	       !sched_assist_scene(SA_CAMERA) &&
	       !sched_assist_scene(SA_LAUNCH);
}

static int task_cgroup_id(struct task_struct *task)
{
	struct cgroup_subsys_state *css;
	int id = 0;

	rcu_read_lock();
	css = task_css(task, cpu_cgrp_id);
	if (css)
		id = css->id;
	rcu_read_unlock();

	return id;
}

int task_lb_sched_type(struct task_struct *tsk)
{
	int grp_id = task_cgroup_id(tsk);

	if (grp_id == SA_CGROUP_TOP_APP)
		return OPLUS_LB_TOP;
	if (grp_id == SA_CGROUP_FOREGROUND)
		return OPLUS_LB_FG;
	if (grp_id == SA_CGROUP_BACKGROUND)
		return OPLUS_LB_BG;

	return OPLUS_LB_FG;
}

#ifdef CONFIG_SCHED_WALT
static bool task_high_load(struct task_struct *tsk)
{
	unsigned long demand = scale_demand(sa_wts(tsk)->demand_scaled);
	unsigned long util = task_util(tsk);
	unsigned long threshold = sysctl_sched_assist_enabled * 10;

	return (demand >= threshold) || (util >= threshold);
}
#endif

void dec_task_lb(struct task_struct *tsk, struct rq *rq)
{
	int type = task_lb_sched_type(tsk);
	bool high = false;

#ifdef CONFIG_SCHED_WALT
	high = task_high_load(tsk);
#endif

	switch (type) {
	case OPLUS_LB_UX:
		if (high)
			per_cpu(task_lb_count, cpu_of(rq)).ux_high--;
		else
			per_cpu(task_lb_count, cpu_of(rq)).ux_low--;
		break;
	case OPLUS_LB_TOP:
		if (high)
			per_cpu(task_lb_count, cpu_of(rq)).top_high--;
		else
			per_cpu(task_lb_count, cpu_of(rq)).top_low--;
		break;
	case OPLUS_LB_FG:
		if (high)
			per_cpu(task_lb_count, cpu_of(rq)).foreground_high--;
		else
			per_cpu(task_lb_count, cpu_of(rq)).foreground_low--;
		break;
	case OPLUS_LB_BG:
		if (high)
			per_cpu(task_lb_count, cpu_of(rq)).background_high--;
		else
			per_cpu(task_lb_count, cpu_of(rq)).background_low--;
		break;
	default:
		break;
	}
}

void inc_task_lb(struct task_struct *tsk, struct rq *rq)
{
	int type = task_lb_sched_type(tsk);
	bool high = false;

#ifdef CONFIG_SCHED_WALT
	high = task_high_load(tsk);
#endif

	switch (type) {
	case OPLUS_LB_UX:
		if (high)
			per_cpu(task_lb_count, cpu_of(rq)).ux_high++;
		else
			per_cpu(task_lb_count, cpu_of(rq)).ux_low++;
		break;
	case OPLUS_LB_TOP:
		if (high)
			per_cpu(task_lb_count, cpu_of(rq)).top_high++;
		else
			per_cpu(task_lb_count, cpu_of(rq)).top_low++;
		break;
	case OPLUS_LB_FG:
		if (high)
			per_cpu(task_lb_count, cpu_of(rq)).foreground_high++;
		else
			per_cpu(task_lb_count, cpu_of(rq)).foreground_low++;
		break;
	case OPLUS_LB_BG:
		if (high)
			per_cpu(task_lb_count, cpu_of(rq)).background_high++;
		else
			per_cpu(task_lb_count, cpu_of(rq)).background_low++;
		break;
	default:
		break;
	}
}

void update_load_flag(struct task_struct *tsk, struct rq *rq)
{
#ifdef CONFIG_SCHED_WALT
	int curr_high_load = task_high_load(tsk) ? SA_HIGH_LOAD : SA_LOW_LOAD;
	int curr_task_type = task_lb_sched_type(tsk);

	if (sa_lb_state(tsk) != 0) {
		int prev_high_load = sa_lb_state(tsk) & 0x1;
		int prev_task_type = (sa_lb_state(tsk) >> 1) & 0x7;

		if (prev_high_load == curr_high_load &&
		    prev_task_type == curr_task_type)
			return;
		else
			dec_task_lb(tsk, rq);
	}
	inc_task_lb(tsk, rq);
	sa_lb_state(tsk) = (curr_task_type << 1) | curr_high_load;
#endif
}

void inc_ld_stats(struct task_struct *tsk, struct rq *rq)
{
	if (!is_spread_task_enabled())
		return;

	sa_ld_flag(tsk)++;
	inc_task_lb(tsk, rq);
	update_load_flag(tsk, rq);
}

void dec_ld_stats(struct task_struct *tsk, struct rq *rq)
{
	if (!is_spread_task_enabled())
		return;

	if (sa_ld_flag(tsk) > 0) {
		sa_ld_flag(tsk)--;
		dec_task_lb(tsk, rq);
	}
}

/*
 * Background weight tables for spread scheduling
 */
#define MAX_ADJ_NICE (5)
#define MAX_ADJ_PRIO (DEFAULT_PRIO + MAX_ADJ_NICE - 1)
static const int sched_prio_to_weight_bg[MAX_ADJ_NICE] = {
	716, 601, 510, 438, 380,
};
static const int sched_prio_to_wmult_bg[MAX_ADJ_NICE] = {
	5998558, 7146368, 8421505, 9805861, 11302546,
};

/* keep same as defined in sched/fair.c */
#define OPLUS_WMULT_CONST	(~0U)
#define OPLUS_WMULT_SHIFT	32

/* keep same as defined in sched/fair.c */
static void __oplus_update_inv_weight(struct load_weight *lw)
{
	unsigned long w;

	if (likely(lw->inv_weight))
		return;

	w = scale_load_down(lw->weight);

	if (BITS_PER_LONG > 32 && unlikely(w >= OPLUS_WMULT_CONST))
		lw->inv_weight = 1;
	else if (unlikely(!w))
		lw->inv_weight = OPLUS_WMULT_CONST;
	else
		lw->inv_weight = OPLUS_WMULT_CONST / w;
}

static struct load_weight sa_new_weight(struct sched_entity *se)
{
	struct task_struct *task = NULL;
	struct load_weight lw;
	int sched_type = -1;
	int idx = -1;

	if (!is_spread_task_enabled() || !entity_is_task(se))
		return se->load;

	task = container_of(se, struct task_struct, se);
	if ((task->static_prio < DEFAULT_PRIO) ||
	    (task->static_prio > MAX_ADJ_PRIO) ||
	    (task_util(task) < 51))
		return se->load;

	/* skip important process such as audio proc */
	if (save_audio_tgid && (task->tgid == save_audio_tgid))
		return se->load;

	/* we only adjust background group's load weight */
	sched_type = task_lb_sched_type(task);
	if (sched_type != OPLUS_LB_BG)
		return se->load;

	idx = task->static_prio - DEFAULT_PRIO;
	lw.weight = scale_load(sched_prio_to_weight_bg[idx]);
	lw.inv_weight = sched_prio_to_wmult_bg[idx];

	return lw;
}

u64 sa_calc_delta(struct sched_entity *se, u64 delta_exec,
		  unsigned long weight, struct load_weight *lw, bool calc_fair)
{
	u64 fact = 0;
	u32 inv_weight = 0;
	int shift = OPLUS_WMULT_SHIFT;
	struct load_weight sa_lw = sa_new_weight(se);

	if (calc_fair &&
	    (sa_lw.weight == lw->weight) && (lw->weight == NICE_0_LOAD))
		return delta_exec;

	__oplus_update_inv_weight(lw);

	if (calc_fair) { /* __calc_delta(delta, NICE_0_LOAD, &se->load) */
		fact = scale_load_down(weight);
		inv_weight = sa_lw.inv_weight;
	} else { /* __calc_delta(slice, sa_lw.weight, load) */
		fact = scale_load_down(sa_lw.weight);
		inv_weight = lw->inv_weight;
	}

	if (unlikely(fact >> 32)) {
		while (fact >> 32) {
			fact >>= 1;
			shift--;
		}
	}

	/* hint to use a 32x32->64 mul */
	fact = (u64)(u32)fact * inv_weight;

	while (fact >> 32) {
		fact >>= 1;
		shift--;
	}

	return mul_u64_u32_shr(delta_exec, fact, shift);
}

/*
 * printf_cpu_spread_nr_info - debug helper for spread info
 */
static void printf_cpu_spread_nr_info(void)
{
	int cpu;

	if (likely(!global_debug_enabled))
		return;

	for_each_online_cpu(cpu) {
		trace_printk("cpu=%d ux_h=%d ux_l=%d fg_h=%d fg_l=%d bg_h=%d bg_l=%d\n",
			cpu,
			per_cpu(task_lb_count, cpu).ux_high,
			per_cpu(task_lb_count, cpu).ux_low,
			per_cpu(task_lb_count, cpu).foreground_high,
			per_cpu(task_lb_count, cpu).foreground_low,
			per_cpu(task_lb_count, cpu).background_high,
			per_cpu(task_lb_count, cpu).background_low);
	}
}

/*
 * find_spread_lowest_nr_cpu - find CPU with lowest load for spread
 */
static int find_spread_lowest_nr_cpu(struct task_struct *p, int order_index,
				     int end_index, int skip_cpu, cpumask_t *cpus)
{
	int cpu = -1;
	int best_cpu = -1;
	int min_nr = INT_MAX;

	for_each_cpu(cpu, cpus) {
		int nr;

		if (cpu == skip_cpu)
			continue;
		if (!cpu_online(cpu) || cpu_isolated(cpu))
			continue;
		if (!cpumask_test_cpu(cpu, p->cpus_ptr))
			continue;

		nr = cpu_rq(cpu)->nr_running;
		if (nr < min_nr) {
			min_nr = nr;
			best_cpu = cpu;
		}
	}

	return best_cpu;
}

/*
 * sched_assist_spread_tasks - spread tasks across CPUs for load balance
 *
 * 5.10 QCOM path uses num_sched_clusters / cpu_array[][] from WALT.
 */
void sched_assist_spread_tasks(struct task_struct *p, cpumask_t new_allowed_cpus,
			       int order_index, int end_index, int skip_cpu,
			       cpumask_t *cpus, bool strict)
{
	int cluster, cpu;

	if (!is_spread_task_enabled())
		return;

	if (!num_sched_clusters || !cpu_array)
		return;

	for (cluster = 0; cluster < num_sched_clusters; cluster++) {
		for_each_cpu_and(cpu, &new_allowed_cpus,
				 &cpu_array[order_index][cluster]) {
			if (cpu == skip_cpu)
				continue;
			if (!cpu_online(cpu) || cpu_isolated(cpu))
				continue;
			if (!cpumask_test_cpu(cpu, p->cpus_ptr))
				continue;

			cpumask_set_cpu(cpu, cpus);
		}
	}

	if (cpumask_empty(cpus)) {
		/* Fallback: use all allowed CPUs */
		cpumask_copy(cpus, &new_allowed_cpus);
	}
}

void init_rq_cpu(int cpu)
{
	per_cpu(task_lb_count, cpu).ux_low = 0;
	per_cpu(task_lb_count, cpu).ux_high = 0;
	per_cpu(task_lb_count, cpu).top_low = 0;
	per_cpu(task_lb_count, cpu).top_high = 0;
	per_cpu(task_lb_count, cpu).foreground_low = 0;
	per_cpu(task_lb_count, cpu).foreground_high = 0;
	per_cpu(task_lb_count, cpu).background_low = 0;
	per_cpu(task_lb_count, cpu).background_high = 0;
}
#endif /* CONFIG_OPLUS_FEATURE_SCHED_SPREAD */

/*
 * ═══════════════════════════════════════════════════════════════
 * UX state / list core
 * ═══════════════════════════════════════════════════════════════
 */

inline int get_ux_state_type(struct task_struct *task)
{
	if (!task)
		return UX_STATE_INVALID;

	if (sa_ux_state(task) & SA_TYPE_INHERIT)
		return UX_STATE_INHERIT;

	if (sa_ux_state(task) & (SA_TYPE_HEAVY | SA_TYPE_LIGHT |
				 SA_TYPE_ANIMATOR | SA_TYPE_LISTPICK))
		return UX_STATE_SCHED_ASSIST;

	return UX_STATE_NONE;
}

static void insert_ux_task_into_list(struct rq *rq, struct task_struct *p)
{
	struct list_head *pos;
	bool exist = false;

	list_for_each(pos, sa_ux_thread_list(rq)) {
		if (pos == &sa_ux_entry(p)) {
			exist = true;
			WARN_ON_ONCE(1);
			break;
		}
	}
	if (!exist) {
		list_add_tail(&sa_ux_entry(p), sa_ux_thread_list(rq));
		get_task_struct(p);
	}
}

inline bool test_list_pick_ux(struct task_struct *task)
{
	return (sa_ux_state(task) & SA_TYPE_LISTPICK) ||
	       (sa_ux_state(task) & SA_TYPE_ONCE_UX) ||
	       test_task_identify_ux(task, SA_TYPE_ID_ALLOCATOR_SER);
}

void enqueue_ux_thread(struct rq *rq, struct task_struct *p)
{
	if (unlikely(!sysctl_sched_assist_enabled))
		return;

	if (!rq || !p || !list_empty(&sa_ux_entry(p)))
		return;

#ifdef CONFIG_OPLUS_CPU_AUDIO_PERF
	/* Audio hook — stub until audio subsystem is ported */
#endif
	sa_enqueue_time(p) = rq_clock(rq);
	if (test_list_pick_ux(p))
		insert_ux_task_into_list(rq, p);
}

void dequeue_ux_thread(struct rq *rq, struct task_struct *p)
{
	struct list_head *pos, *n;

	if (!rq || !p)
		return;

	sa_enqueue_time(p) = 0;
	if (!list_empty(&sa_ux_entry(p))) {
		list_for_each_safe(pos, n, sa_ux_thread_list(rq)) {
			if (pos == &sa_ux_entry(p)) {
				list_del_init(&sa_ux_entry(p));
				if (sa_ux_state(p) & SA_TYPE_ONCE_UX)
					sa_ux_state(p) &= ~SA_TYPE_ONCE_UX;
				put_task_struct(p);
				return;
			}
		}
	}
}

/*
 * pick_first_ux_thread - find the UX task with smallest vruntime
 *
 * NOTE: Fixed from 4.19 — stale entries now get put_task_struct()
 * to match the get_task_struct() in insert_ux_task_into_list().
 */
static struct task_struct *pick_first_ux_thread(struct rq *rq)
{
	struct list_head *ux_thread_list = sa_ux_thread_list(rq);
	struct list_head *pos = NULL;
	struct list_head *n = NULL;
	struct task_struct *leftmost_task = NULL;

	list_for_each_safe(pos, n, ux_thread_list) {
		struct walt_task_struct *wts = list_entry(pos,
			struct walt_task_struct, ux_entry);
		struct task_struct *temp = wts_to_ts(wts);

		/* ensure ux task is on current rq cpu, otherwise delete it */
		if (unlikely(task_cpu(temp) != rq->cpu)) {
			list_del_init(&sa_ux_entry(temp));
			put_task_struct(temp); /* Fixed: release ref on stale entry */
			continue;
		}

		if (leftmost_task == NULL) {
			leftmost_task = temp;
		} else if (entity_before(&temp->se, &leftmost_task->se)) {
			leftmost_task = temp;
		}
	}

	return leftmost_task;
}

void pick_ux_thread(struct rq *rq, struct task_struct **p, struct sched_entity **se)
{
	struct task_struct *ori_p = *p;
	struct task_struct *key_task;
	struct sched_entity *key_se;

	if (!rq || !ori_p || !se)
		return;

#ifdef CONFIG_OPLUS_FEATURE_AUDIO_OPT
	if (sa_oplus_task_info(ori_p).im_small)
		return;
#endif

	if ((sa_ux_state(ori_p) & SA_TYPE_ANIMATOR) || test_list_pick_ux(ori_p))
		return;

	if (!list_empty(sa_ux_thread_list(rq))) {
		key_task = pick_first_ux_thread(rq);
		/* in case that ux thread keeps running too long */
		if (key_task && entity_over(&key_task->se, &ori_p->se))
			return;

		if (key_task) {
			key_se = &key_task->se;
			if (key_se &&
			    (rq_clock(rq) >= sa_enqueue_time(key_task)) &&
			    rq_clock(rq) - sa_enqueue_time(key_task) >=
			    ((u64)ux_min_sched_delay_granularity * S2NS_T)) {
				*p = key_task;
				*se = key_se;
			}
		}
	}
}

/*
 * ═══════════════════════════════════════════════════════════════
 * Inherit UX bitfield helpers
 * ═══════════════════════════════════════════════════════════════
 */

#define INHERIT_UX_SEC_WIDTH   8
#define INHERIT_UX_MASK_BASE   0x00000000ff

#define inherit_ux_offset_of(type) (type * INHERIT_UX_SEC_WIDTH)
#define inherit_ux_mask_of(type) ((u64)(INHERIT_UX_MASK_BASE) << (inherit_ux_offset_of(type)))
#define inherit_ux_get_bits(value, type) ((value & inherit_ux_mask_of(type)) >> inherit_ux_offset_of(type))
#define inherit_ux_value(type, value) ((u64)value << inherit_ux_offset_of(type))

bool test_inherit_ux(struct task_struct *task, int type)
{
	u64 inherit_ux;

	if (!task)
		return false;

	inherit_ux = atomic64_read(&sa_inherit_ux(task));
	return inherit_ux_get_bits(inherit_ux, type) > 0;
}

static bool test_task_exist(struct task_struct *task, struct list_head *head)
{
	struct list_head *pos, *n;

	list_for_each_safe(pos, n, head) {
		if (pos == &sa_ux_entry(task))
			return true;
	}
	return false;
}

inline void inherit_ux_inc(struct task_struct *task, int type)
{
	atomic64_add(inherit_ux_value(type, 1), &sa_inherit_ux(task));
}

inline void inherit_ux_sub(struct task_struct *task, int type, int value)
{
	atomic64_sub(inherit_ux_value(type, value), &sa_inherit_ux(task));
}

static void __inherit_ux_dequeue(struct task_struct *task, int type, int value)
{
	struct rq_flags flags;
	bool exist = false;
	struct rq *rq = NULL;
	u64 inherit_ux = 0;

	rq = task_rq_lock(task, &flags);
	inherit_ux = atomic64_read(&sa_inherit_ux(task));
	if (inherit_ux <= 0) {
		task_rq_unlock(rq, task, &flags);
		return;
	}
	inherit_ux_sub(task, type, value);
	inherit_ux = atomic64_read(&sa_inherit_ux(task));
	if (inherit_ux > 0) {
		task_rq_unlock(rq, task, &flags);
		return;
	}
	sa_ux_depth(task) = 0;

	exist = test_task_exist(task, sa_ux_thread_list(rq));
	if (exist) {
		list_del_init(&sa_ux_entry(task));
		put_task_struct(task);
	}
	task_rq_unlock(rq, task, &flags);
}

void inherit_ux_dequeue(struct task_struct *task, int type)
{
	if (!task || type >= INHERIT_UX_MAX)
		return;
	__inherit_ux_dequeue(task, type, 1);
}

void inherit_ux_dequeue_refs(struct task_struct *task, int type, int value)
{
	if (!task || type >= INHERIT_UX_MAX)
		return;
	__inherit_ux_dequeue(task, type, value);
}

static void __inherit_ux_enqueue(struct task_struct *task, int type, int depth)
{
	struct rq_flags flags;
	bool exist = false;
	struct rq *rq = NULL;

	rq = task_rq_lock(task, &flags);

	if (unlikely(!list_empty(&sa_ux_entry(task)))) {
		task_rq_unlock(rq, task, &flags);
		return;
	}

	inherit_ux_inc(task, type);
	sa_inherit_ux_start(task) = jiffies_to_nsecs(jiffies);
	sa_ux_depth(task) = sa_ux_depth(task) > depth + 1 ?
			    sa_ux_depth(task) : depth + 1;
	if (READ_ONCE(task->__state) == TASK_RUNNING &&
	    task->sched_class == &fair_sched_class) {
		exist = test_task_exist(task, sa_ux_thread_list(rq));
		if (!exist) {
			get_task_struct(task);
			list_add_tail(&sa_ux_entry(task), sa_ux_thread_list(rq));
		}
	}
	task_rq_unlock(rq, task, &flags);
}

void inherit_ux_enqueue(struct task_struct *task, int type, int depth)
{
	if (!task || type >= INHERIT_UX_MAX)
		return;
	__inherit_ux_enqueue(task, type, depth);
}

inline bool test_task_ux_depth(int ux_depth)
{
	return ux_depth < UX_DEPTH_MAX;
}

inline bool test_set_inherit_ux(struct task_struct *tsk)
{
	return tsk && test_task_ux(tsk) && test_task_ux_depth(sa_ux_depth(tsk));
}

/*
 * ═══════════════════════════════════════════════════════════════
 * Initialization
 * ═══════════════════════════════════════════════════════════════
 */

void ux_init_rq_data(struct rq *rq)
{
	if (!rq)
		return;

	INIT_LIST_HEAD(sa_ux_thread_list(rq));
#if IS_ENABLED(CONFIG_OPLUS_FEATURE_SCHED_UX_PRIORITY)
	raw_spin_lock_init(sa_ux_list_lock(rq));
#endif
#ifdef CONFIG_OPLUS_FEATURE_SCHED_SPREAD
	cpumask_clear(&nr_mask);
	init_rq_cpu(cpu_of(rq));
#endif
}

int ux_prefer_cpu[NR_CPUS];

void ux_init_cpu_data(void)
{
	int i = 0;
	int min_cpu = 0, ux_cpu = 0;

	for (; i < nr_cpu_ids; ++i)
		ux_prefer_cpu[i] = -1;

	ux_cpu = cpumask_weight(topology_core_cpumask(min_cpu));
	if (ux_cpu == 0) {
		ux_warn("failed to init ux cpu data\n");
		return;
	}

	for (i = 0; i < nr_cpu_ids && ux_cpu < nr_cpu_ids; ++i)
		ux_prefer_cpu[i] = ux_cpu++;
}

bool test_ux_task_cpu(int cpu)
{
	return (cpu >= ux_prefer_cpu[0]);
}

bool test_ux_prefer_cpu(struct task_struct *tsk, int cpu)
{
	struct root_domain *rd = cpu_rq(smp_processor_id())->rd;

	if (cpu < 0)
		return false;

	if (tsk->pid == tsk->tgid) {
		/* 5.10 QCOM: wrd.max_cap_orig_cpu */
		return cpu >= rd->wrd.max_cap_orig_cpu;
	}

	return (cpu >= ux_prefer_cpu[0]);
}

void find_ux_task_cpu(struct task_struct *tsk, int *target_cpu)
{
	int i = 0;
	int temp_cpu = 0;
	struct rq *rq = NULL;

	for (i = (nr_cpu_ids - 1); i >= 0; --i) {
		temp_cpu = ux_prefer_cpu[i];
		if (temp_cpu <= 0 || temp_cpu >= nr_cpu_ids)
			continue;

		rq = cpu_rq(temp_cpu);
		if (!rq)
			continue;

		if (rq->curr->prio <= MAX_RT_PRIO)
			continue;

		if (!test_task_ux(rq->curr) && cpu_online(temp_cpu) &&
		    !cpu_isolated(temp_cpu) &&
		    cpumask_test_cpu(temp_cpu, tsk->cpus_ptr)) {
			*target_cpu = temp_cpu;
			return;
		}
	}
}

/*
 * ═══════════════════════════════════════════════════════════════
 * CPU capacity / misfit helpers
 * ═══════════════════════════════════════════════════════════════
 */

static inline bool oplus_is_min_capacity_cpu(int cpu)
{
	return capacity_orig_of(cpu) <=
	       ux_sched_cputopo.sched_cls[0].capacity;
}

#ifdef CONFIG_SCHED_WALT
bool sched_assist_task_misfit(struct task_struct *task, int cpu, int flag)
{
	unsigned long demand = scale_demand(sa_wts(task)->sum);
	unsigned long util = task_util(task);
	int num_mincpu = cpumask_weight(topology_core_cpumask(0));

	if ((demand >= sysctl_boost_task_threshold ||
	     util >= sysctl_boost_task_threshold) && cpu < num_mincpu)
		return true;

	return false;
}
#endif

/*
 * set_ux_task_cpu_common_by_prio - choose target CPU by UX priority
 *
 * Scans preferred CPUs, skipping those occupied by UX/RT tasks,
 * respecting the task's cpumask.
 */
int set_ux_task_cpu_common_by_prio(struct task_struct *task, int *target_cpu,
				   bool boost, bool prefer_idle, unsigned int type)
{
	int i;
	int cls_nr = ux_sched_cputopo.cls_nr - 1;

	if (!sysctl_sched_assist_enabled)
		return *target_cpu;

	if (cls_nr <= 0)
		return *target_cpu;

	for (i = cls_nr; i >= 0; i--) {
		int cpu;

		for_each_cpu(cpu, &ux_sched_cputopo.sched_cls[i].cpus) {
			struct rq *rq = cpu_rq(cpu);

			if (!cpu_online(cpu) || cpu_isolated(cpu))
				continue;
			if (!cpumask_test_cpu(cpu, task->cpus_ptr))
				continue;

			/* Skip CPUs with UX or RT tasks running */
			if (test_task_ux(rq->curr) || rq->curr->prio < MAX_RT_PRIO)
				continue;

			/* Skip if UX list is non-empty */
			if (!list_empty(sa_ux_thread_list(rq)))
				continue;

			*target_cpu = cpu;
			return *target_cpu;
		}
	}

	return *target_cpu;
}

/*
 * ═══════════════════════════════════════════════════════════════
 * SF / target comm / entity helpers
 * ═══════════════════════════════════════════════════════════════
 */

bool is_sf(struct task_struct *p)
{
	char sf_name[] = "surfaceflinger";

	return (strcmp(p->comm, sf_name) == 0) && (p->pid == p->tgid);
}

void drop_ux_task_cpus(struct task_struct *p, struct cpumask *lowest_mask)
{
	unsigned int cpu = cpumask_first(lowest_mask);

	while (cpu < nr_cpu_ids) {
		/* unlocked access */
		struct task_struct *task = READ_ONCE(cpu_rq(cpu)->curr);

		if ((sysctl_sched_assist_scene & SA_LAUNCH) &&
		    (sa_ux_state(task) & SA_TYPE_HEAVY))
			cpumask_clear_cpu(cpu, lowest_mask);

		if (test_task_ux(task) ||
		    !list_empty(&sa_ux_entry(task)) ||
		    (test_task_identify_ux(task, SA_TYPE_ID_CAMERA_PROVIDER) &&
		     oplus_is_min_capacity_cpu(cpu))) {
			cpumask_clear_cpu(cpu, lowest_mask);
		}

#ifdef CONFIG_SCHED_WALT
		if (sf_boost_scene() && is_sf(p)) {
			if (cpu < ux_prefer_cpu[0] ||
			    (sa_ux_state(task) & SA_TYPE_HEAVY))
				cpumask_clear_cpu(cpu, lowest_mask);
		}
#endif

		cpu = cpumask_next(cpu, lowest_mask);
	}
}

static inline bool test_sched_assist_ux_type(struct task_struct *task,
					     unsigned int sa_ux_type)
{
	return sa_ux_state(task) & sa_ux_type;
}

static inline u64 max_vruntime(u64 max_vrt, u64 vruntime)
{
	s64 delta = (s64)(vruntime - max_vrt);

	if (delta > 0)
		max_vrt = vruntime;

	return max_vrt;
}

#define MALI_THREAD_NAME "mali-cmar-backe"
#define LAUNCHER_THREAD_NAME "ndroid.launcher"
#define WECHAT_THREAD_NAME "com.tencent.mm"
#define ALLOCATOR_THREAD_NAME "allocator-servi"
#define CAMERA_PROVIDER_NAME "provider@2.4-se"

#define CAMERA_MAINTHREAD_NAME "com.oplus.camera"
#define OPLUS_CAMERA_MAINTHREAD_NAME "om.oplus.camera"
#define CAMERA_PREMR_NAME "previewManagerR"
#define CAMERA_PREPT_NAME "PreviewProcessT"
#define CAMERA_HALCONT_NAME "Camera Hal Cont"
#define CAMERA_IMAGEPROC_NAME "ImageProcessThr"

extern pid_t sf_pid;
extern pid_t re_pid;

void sched_assist_target_comm(struct task_struct *task)
{
	struct task_struct *grp_leader = task->group_leader;

	if (unlikely(!sysctl_sched_assist_enabled))
		return;

	if (!grp_leader || (get_ux_state_type(task) != UX_STATE_NONE))
		return;

	if (strstr(task->comm, "surfaceflinger") &&
	    strstr(grp_leader->comm, "surfaceflinger")) {
		sf_pid = task->pid;
		return;
	}

	if (strstr(task->comm, "RenderEngine") &&
	    strstr(grp_leader->comm, "surfaceflinger")) {
		re_pid = task->pid;
		return;
	}

	/* QCOM platform: no mali threads */

	if (strstr(grp_leader->comm, CAMERA_PROVIDER_NAME) &&
	    strstr(task->comm, CAMERA_PROVIDER_NAME)) {
		sa_ux_state(task) |= SA_TYPE_ID_CAMERA_PROVIDER;
		return;
	}

	if ((strstr(grp_leader->comm, CAMERA_MAINTHREAD_NAME) ||
	     strstr(grp_leader->comm, OPLUS_CAMERA_MAINTHREAD_NAME)) &&
	    (strstr(task->comm, CAMERA_PREMR_NAME) ||
	     strstr(task->comm, CAMERA_PREPT_NAME) ||
	     strstr(task->comm, CAMERA_HALCONT_NAME) ||
	     strstr(task->comm, CAMERA_IMAGEPROC_NAME))) {
		sa_ux_state(task) |= SA_TYPE_LIGHT;
		return;
	}

	if (!strncmp(grp_leader->comm, ALLOCATOR_THREAD_NAME, TASK_COMM_LEN) ||
	    !strncmp(task->comm, ALLOCATOR_THREAD_NAME, TASK_COMM_LEN)) {
		sa_ux_state(task) |= SA_TYPE_ID_ALLOCATOR_SER;
	}

	/* set audio task ux for com.tencent.mm */
	if (!strncmp(grp_leader->comm, WECHAT_THREAD_NAME, TASK_COMM_LEN) &&
	    (!strncmp(task->comm, "TPDecoder", 9) ||
	     !strncmp(task->comm, "MediaCodec_loop", TASK_COMM_LEN) ||
	     !strncmp(task->comm, "VoipEngine", TASK_COMM_LEN))) {
		sa_ux_state(task) |= SA_TYPE_LIGHT;
		return;
	}
}

/*
 * ═══════════════════════════════════════════════════════════════
 * Entity placement and wakeup preemption
 * ═══════════════════════════════════════════════════════════════
 */

#ifdef CONFIG_FAIR_GROUP_SCHED
#define oplus_entity_is_task(se)	(!se->my_q)
#else
#define oplus_entity_is_task(se)	(1)
#endif

void place_entity_adjust_ux_task(struct cfs_rq *cfs_rq,
				 struct sched_entity *se, int initial)
{
	u64 vruntime = cfs_rq->min_vruntime;
	unsigned long thresh = sysctl_sched_latency;
	unsigned long launch_adjust = 0;
	struct task_struct *se_task = NULL;

	if (unlikely(!sysctl_sched_assist_enabled))
		return;

	if (!oplus_entity_is_task(se) || initial)
		return;

	if (sysctl_sched_assist_scene & SA_LAUNCH)
		launch_adjust = sysctl_sched_latency;

	se_task = task_of(se);

#ifdef CONFIG_MMAP_LOCK_OPT
	if (sa_ux_once(se_task)) {
		vruntime -= thresh;
		se->vruntime = vruntime;
		sa_ux_once(se_task) = 0;
		return;
	}
#endif

	if (test_sched_assist_ux_type(se_task, SA_TYPE_ANIMATOR)) {
		vruntime -= 3 * thresh + (thresh >> 1);
		se->vruntime = vruntime - (launch_adjust >> 1);
		return;
	}

	if (test_sched_assist_ux_type(se_task, SA_TYPE_LIGHT | SA_TYPE_HEAVY)) {
		vruntime -= 2 * thresh;
		se->vruntime = vruntime - (launch_adjust >> 1);
		return;
	}

	if (test_task_identify_ux(se_task, SA_TYPE_ID_CAMERA_PROVIDER)) {
		vruntime -= 2 * thresh + (thresh >> 1);
		se->vruntime = vruntime - (launch_adjust >> 1);
		return;
	}
}

bool should_ux_preempt_wakeup(struct task_struct *wake_task,
			      struct task_struct *curr_task)
{
	bool wake_ux = false;
	bool curr_ux = false;

	if (!sysctl_sched_assist_enabled)
		return false;

	wake_ux = test_task_ux(wake_task) || test_list_pick_ux(wake_task) ||
		  test_task_identify_ux(wake_task, SA_TYPE_ID_CAMERA_PROVIDER);
	curr_ux = test_task_ux(curr_task) || test_list_pick_ux(curr_task) ||
		  test_task_identify_ux(curr_task, SA_TYPE_ID_CAMERA_PROVIDER);

	/* ux can preempt cfs */
	if (wake_ux && !curr_ux)
		return true;

	/* animator ux can preempt un-animator */
	if ((sa_ux_state(wake_task) & SA_TYPE_ANIMATOR) &&
	    !(sa_ux_state(curr_task) & SA_TYPE_ANIMATOR))
		return true;

	/* heavy type can be preempted by other type */
	if (wake_ux && (sa_ux_state(curr_task) & SA_TYPE_HEAVY)) {
		bool preempt = !(sa_ux_state(wake_task) & SA_TYPE_HEAVY) &&
			       !test_task_identify_ux(wake_task, SA_TYPE_ID_CAMERA_PROVIDER);
		return preempt;
	}

	return false;
}

bool should_ux_task_skip_further_check(struct sched_entity *se)
{
	return oplus_entity_is_task(se) &&
	       test_sched_assist_ux_type(task_of(se), SA_TYPE_ANIMATOR);
}

static inline bool is_ux_task_prefer_cpu(struct task_struct *task, int cpu)
{
	struct ux_sched_cputopo ux_cputopo = ux_sched_cputopo;
	int cls_nr = ux_cputopo.cls_nr - 1;

	if (cpu < 0)
		return false;

	/* only one cluster or init failed */
	if (unlikely(cls_nr <= 0))
		return true;

	if (cpu_rq(cpu)->curr &&
	    test_sched_assist_ux_type(cpu_rq(cpu)->curr, SA_TYPE_HEAVY))
		return false;

	if (test_sched_assist_ux_type(task, SA_TYPE_HEAVY))
		return capacity_orig_of(cpu) >= ux_cputopo.sched_cls[cls_nr].capacity;

	return true;
}

bool should_ux_task_skip_cpu(struct task_struct *task, unsigned int cpu)
{
	struct ux_sched_cputopo ux_cputopo = ux_sched_cputopo;
	int cls_nr = ux_cputopo.cls_nr - 1;
	bool is_skip_rt_cpu = true;
	int silver_core_nr = 0;

	if (!sysctl_sched_assist_enabled)
		return false;

	if (test_task_identify_ux(task, SA_TYPE_ID_CAMERA_PROVIDER)) {
		if (cpu_rq(cpu)->curr && test_task_ux(cpu_rq(cpu)->curr))
			return true;
	}

	if (!test_task_ux(task))
		return false;

	if ((sysctl_sched_assist_scene & SA_LAUNCH) &&
	    !is_ux_task_prefer_cpu(task, cpu))
		return true;

	if (cls_nr > 0)
		silver_core_nr = cpumask_weight(&ux_cputopo.sched_cls[0].cpus);

#define SILVER_NR_6 (6)
	/* avoid ux being squeezed to small core by rt in 6+2 architecture */
	if ((sysctl_sched_assist_scene & SA_ANIM) &&
	    (silver_core_nr == SILVER_NR_6) && !oplus_is_min_capacity_cpu(cpu))
		is_skip_rt_cpu = false;

	if (!(sysctl_sched_assist_scene & SA_LAUNCH) ||
	    !test_sched_assist_ux_type(task, SA_TYPE_HEAVY)) {
		if (is_skip_rt_cpu && cpu_rq(cpu)->rt.rt_nr_running)
			return true;

		/* avoid placing turbo ux into cpu which has animator or list ux */
		if (cpu_rq(cpu)->curr &&
		    (test_sched_assist_ux_type(cpu_rq(cpu)->curr, SA_TYPE_ANIMATOR) ||
		     !list_empty(sa_ux_thread_list(cpu_rq(cpu)))))
			return true;
	}

	return false;
}

void set_ux_task_to_prefer_cpu_v1(struct task_struct *task,
				  int *orig_target_cpu, bool *cond)
{
	struct rq *rq = NULL;
	struct ux_sched_cputopo ux_cputopo = ux_sched_cputopo;
	int cls_nr = ux_cputopo.cls_nr - 1;
	int cpu = 0;

	if (!sysctl_sched_assist_enabled ||
	    !(sysctl_sched_assist_scene & SA_LAUNCH))
		return;

	if (unlikely(cls_nr <= 0))
		return;

	if (is_ux_task_prefer_cpu(task, *orig_target_cpu))
		return;
	*cond = true;
retry:
	for_each_cpu(cpu, &ux_cputopo.sched_cls[cls_nr].cpus) {
		rq = cpu_rq(cpu);
		if (test_sched_assist_ux_type(rq->curr, SA_TYPE_HEAVY))
			continue;

		if (rq->curr->prio < MAX_RT_PRIO)
			continue;

		if (cpu_online(cpu) && !cpu_isolated(cpu) &&
		    cpumask_test_cpu(cpu, task->cpus_ptr)) {
			*orig_target_cpu = cpu;
			return;
		}
	}

	cls_nr = cls_nr - 1;
	if (cls_nr > 0)
		goto retry;
}

/*
 * set_ux_task_to_prefer_cpu - find preferred CPU for UX task
 *
 * NOTE: Fixed from 4.19 — the original had `!cpu_isolated(cpu)` which
 * would skip normal online non-isolated CPUs. Corrected to
 * `cpu_online(cpu) && !cpu_isolated(cpu)`.
 */
void set_ux_task_to_prefer_cpu(struct task_struct *task, int *orig_target_cpu)
{
	struct rq *rq = NULL;
	struct ux_sched_cputopo ux_cputopo = ux_sched_cputopo;
	int cls_nr = ux_cputopo.cls_nr - 1;
	int cpu = 0;

	if (!sysctl_sched_assist_enabled ||
	    !(sysctl_sched_assist_scene & SA_LAUNCH))
		return;

	if (unlikely(cls_nr <= 0))
		return;

	if (is_ux_task_prefer_cpu(task, *orig_target_cpu))
		return;
retry:
	for_each_cpu(cpu, &ux_cputopo.sched_cls[cls_nr].cpus) {
		rq = cpu_rq(cpu);

		/* Fixed: was !cpu_isolated(cpu) in 4.19, which rejected normal CPUs */
		if (!cpu_online(cpu) || cpu_isolated(cpu))
			continue;

		if (!cpumask_test_cpu(cpu, task->cpus_ptr))
			continue;

		/* Skip CPUs with UX or RT tasks */
		if (sa_ux_state(rq->curr) & (SCHED_ASSIST_UX_MASK | POSSIBLE_UX_MASK | SA_TYPE_INHERIT))
			continue;

		if (rq->curr->prio < MAX_RT_PRIO)
			continue;

		/* Skip CPUs with UX list entries */
		if (!list_empty(sa_ux_thread_list(rq)))
			continue;

		if (rt_rq_is_runnable(&rq->rt))
			continue;

		/* An available CPU was found */
		*orig_target_cpu = cpu;
		return;
	}

	cls_nr = cls_nr - 1;
	if (cls_nr > 0)
		goto retry;
}

/*
 * ═══════════════════════════════════════════════════════════════
 * Boost kill
 * ═══════════════════════════════════════════════════════════════
 */

static int boost_kill = 1;
module_param_named(boost_kill, boost_kill, uint, 0644);

int get_grp(struct task_struct *p)
{
	struct cgroup_subsys_state *css;

	if (p == NULL)
		return false;
	rcu_read_lock();
	css = task_css(p, cpu_cgrp_id);
	if (!css) {
		rcu_read_unlock();
		return false;
	}
	rcu_read_unlock();

	return css->id;
}

void oplus_boost_kill_signal(int sig, struct task_struct *cur,
			     struct task_struct *p)
{
	struct task_struct *tmp;

	if (p == NULL)
		return;
	if (sig == SIGKILL && boost_kill && get_grp(p) == BGAPP &&
	    p->group_leader && p->group_leader->pid == p->pid) {
		tmp = p;
		rcu_read_lock();
		/* walk all threads for each process */
		do {
			set_user_nice(tmp, -20);
			cpumask_copy(&tmp->cpus_mask, cpu_possible_mask);
			tmp->nr_cpus_allowed = cpumask_weight(cpu_possible_mask);
		} while_each_thread(p, tmp);
		rcu_read_unlock();
	}
}

/*
 * ═══════════════════════════════════════════════════════════════
 * Inherit UX state management
 * ═══════════════════════════════════════════════════════════════
 */

#if !IS_ENABLED(CONFIG_OPLUS_FEATURE_SCHED_UX_PRIORITY)
static void requeue_runnable_task(struct task_struct *p)
{
	bool queued, running;
	struct rq_flags rf;
	struct rq *rq;

	rq = task_rq_lock(p, &rf);
	queued = task_on_rq_queued(p);
	running = task_current(rq, p);

	if (!queued || running) {
		task_rq_unlock(rq, p, &rf);
		return;
	}

	update_rq_clock(rq);
	deactivate_task(rq, p, DEQUEUE_NOCLOCK);
	activate_task(rq, p, ENQUEUE_NOCLOCK);
	resched_curr(rq);

	task_rq_unlock(rq, p, &rf);
}
#endif

void set_inherit_ux(struct task_struct *task, int type, int depth, int inherit_val)
{
	struct rq_flags flags;
	struct rq *rq = NULL;
	int old_state = 0;
	int new_state;
#if !IS_ENABLED(CONFIG_OPLUS_FEATURE_SCHED_UX_PRIORITY)
	bool list_pick = false;
#endif

	if (!task || type >= INHERIT_UX_MAX)
		return;

	rq = task_rq_lock(task, &flags);

	if (task->sched_class != &fair_sched_class) {
		task_rq_unlock(rq, task, &flags);
		return;
	}

	inherit_ux_inc(task, type);
	sa_ux_depth(task) = depth + 1;
	old_state = sa_ux_state(task);
	new_state = (inherit_val & SCHED_ASSIST_UX_MASK) | SA_TYPE_INHERIT;
	/* identify type like allocator ux, keep it, but cannot inherit */
	if (old_state & SA_TYPE_ID_ALLOCATOR_SER)
		new_state |= SA_TYPE_ID_ALLOCATOR_SER;
	if (old_state & SA_TYPE_ID_CAMERA_PROVIDER)
		new_state |= SA_TYPE_ID_CAMERA_PROVIDER;
	sa_inherit_ux_start(task) = jiffies_to_nsecs(jiffies);

	sched_assist_systrace_pid(task->tgid, new_state, "ux_state %d", task->pid);

	oplus_set_ux_state_lock(task, new_state, false);

#if !IS_ENABLED(CONFIG_OPLUS_FEATURE_SCHED_UX_PRIORITY)
	list_pick = test_list_pick_ux(task);
	if (list_pick && task->on_rq && list_empty(&sa_ux_entry(task))) {
		insert_ux_task_into_list(rq, task);
	}
#endif
	task_rq_unlock(rq, task, &flags);

#if !IS_ENABLED(CONFIG_OPLUS_FEATURE_SCHED_UX_PRIORITY)
	/* requeue runnable task to ensure vruntime adjust */
	if (!list_pick)
		requeue_runnable_task(task);
#endif
}

void reset_inherit_ux(struct task_struct *inherit_task,
		      struct task_struct *ux_task, int reset_type)
{
	struct rq_flags flags;
	struct rq *rq;
	int reset_depth = 0;
	int reset_inherit = 0;
	int ux_state;

	if (!inherit_task || !ux_task || reset_type >= INHERIT_UX_MAX)
		return;

	reset_inherit = sa_ux_state(ux_task);
	reset_depth = sa_ux_depth(ux_task);
	/* animator ux is important, so we just reset in this type */
	if (!test_inherit_ux(inherit_task, reset_type) ||
	    !test_sched_assist_ux_type(ux_task, SA_TYPE_ANIMATOR))
		return;

	rq = task_rq_lock(inherit_task, &flags);

	sa_ux_depth(inherit_task) = reset_depth + 1;
	/* identify type like allocator ux, keep it, but cannot inherit */
	if (reset_inherit & SA_TYPE_ID_ALLOCATOR_SER)
		reset_inherit &= ~SA_TYPE_ID_ALLOCATOR_SER;
	if (reset_inherit & SA_TYPE_ID_CAMERA_PROVIDER)
		reset_inherit &= ~SA_TYPE_ID_CAMERA_PROVIDER;
	ux_state = (sa_ux_state(inherit_task) & ~SCHED_ASSIST_UX_MASK) | reset_inherit;

	oplus_set_ux_state_lock(inherit_task, ux_state, false);

	sched_assist_systrace_pid(inherit_task->tgid,
				  sa_ux_state(inherit_task),
				  "ux_state %d", inherit_task->pid);

	task_rq_unlock(rq, inherit_task, &flags);
}

void unset_inherit_ux_value(struct task_struct *task, int type, int value)
{
	struct rq_flags flags;
	struct rq *rq;
	s64 inherit_ux;
	int ux_state;

	if (!task || type >= INHERIT_UX_MAX)
		return;

	rq = task_rq_lock(task, &flags);

	inherit_ux_sub(task, type, value);
	inherit_ux = atomic64_read(&sa_inherit_ux(task));
	if (inherit_ux > 0) {
		task_rq_unlock(rq, task, &flags);
		return;
	}
	if (inherit_ux < 0)
		atomic64_set(&sa_inherit_ux(task), 0);
	sa_ux_depth(task) = 0;
	/* identify type like allocator ux, keep it, but cannot inherit */
	ux_state = sa_ux_state(task);
	ux_state &= (SA_TYPE_ID_ALLOCATOR_SER | SA_TYPE_ID_CAMERA_PROVIDER);

	oplus_set_ux_state_lock(task, ux_state, false);

	sched_assist_systrace_pid(task->tgid, sa_ux_state(task),
				  "ux_state %d", task->pid);

	task_rq_unlock(rq, task, &flags);
}

void unset_inherit_ux(struct task_struct *task, int type)
{
	unset_inherit_ux_value(task, type, 1);
}

void inc_inherit_ux_refs(struct task_struct *task, int type)
{
	struct rq_flags flags;
	struct rq *rq;

	rq = task_rq_lock(task, &flags);
	inherit_ux_inc(task, type);
	task_rq_unlock(rq, task, &flags);
}

/*
 * ═══════════════════════════════════════════════════════════════
 * SF group / misfit helpers
 * ═══════════════════════════════════════════════════════════════
 */

bool task_is_sf_group(struct task_struct *p)
{
	return (p->pid == sf_pid) || (p->pid == re_pid);
}

void sf_task_util_record(struct task_struct *p)
{
	char comm_now[TASK_COMM_LEN];
	unsigned long walt_util;
	int i = 0;
	int len = 0;

	memset(comm_now, '\0', sizeof(comm_now));
	walt_util = sa_wts(p)->demand_scaled;

	if (unlikely(task_is_sf_group(p))) {
		strcpy(comm_now, p->comm);
		len = strlen(comm_now);
		for (i = 0; i < SF_GROUP_COUNT; i++) {
			if (!strncmp(comm_now, sf_target[i].val, len))
				sf_target[i].util = walt_util;
		}
	}
}

bool sf_task_misfit(struct task_struct *p)
{
	unsigned long util = 0;
	int i;

	if (task_is_sf_group(p)) {
		for (i = 0; i < SF_GROUP_COUNT; i++)
			util += sf_target[i].util;
		if ((util > sysctl_boost_task_threshold) && sf_boost_scene())
			return true;
		else
			return false;
	}
	return false;
}

bool oplus_task_misfit(struct task_struct *p, int cpu)
{
#ifdef CONFIG_SCHED_WALT
	int num_mincpu = cpumask_weight(topology_core_cpumask(0));

	if ((scale_demand(sa_wts(p)->sum) >= sysctl_boost_task_threshold ||
	     task_util(p) >= sysctl_boost_task_threshold) && cpu < num_mincpu)
		return true;
#endif
	return false;
}

void kick_min_cpu_from_mask(struct cpumask *lowest_mask)
{
	unsigned int cpu = cpumask_first(lowest_mask);

	while (cpu < nr_cpu_ids) {
		if (cpu < ux_prefer_cpu[0])
			cpumask_clear_cpu(cpu, lowest_mask);
		cpu = cpumask_next(cpu, lowest_mask);
	}
}

bool ux_skip_sync_wakeup(struct task_struct *task, int *sync)
{
	bool ret = false;

	if (test_sched_assist_ux_type(task, SA_TYPE_ANIMATOR)) {
		*sync = 0;
		ret = true;
	}

	return ret;
}

/*
 * ═══════════════════════════════════════════════════════════════
 * Audio optimization (CONFIG_OPLUS_FEATURE_AUDIO_OPT)
 * ═══════════════════════════════════════════════════════════════
 */

#ifdef CONFIG_OPLUS_FEATURE_AUDIO_OPT
#define FIT_SMALL_THREASH 3
#define RUNNABLE_MIN_THREASH 20000000
#define RUNNING_MIN_THREASH 3000000

#define TASK_SMALL (0)

static inline bool trace_state_group(struct task_struct *tsk)
{
	return (save_audio_tgid && tsk->tgid == save_audio_tgid);
}

bool sched_assist_pick_next_entity(struct cfs_rq *cfs_rq, struct sched_entity **se)
{
	if (cfs_rq->next && oplus_entity_is_task(cfs_rq->next)) {
		struct task_struct *tsk = task_of(cfs_rq->next);

		if (sa_oplus_task_info(tsk).im_small) {
			*se = cfs_rq->next;
			return true;
		}
	}

	return false;
}

void update_task_sched_stat_common(struct task_struct *tsk, u64 delta_ns,
				   int stats_type)
{
	int i = TASK_INFO_SAMPLE - 1;

	while (i > 0) {
		sa_oplus_task_info(tsk).sa_info[stats_type][i] =
			sa_oplus_task_info(tsk).sa_info[stats_type][i - 1];
		i--;
	}
	sa_oplus_task_info(tsk).sa_info[stats_type][0] = delta_ns;
}

u64 task_info_sum(struct task_struct *task, int type)
{
	int i = 0;
	u64 sum = 0;

	for (i = 0; i < TASK_INFO_SAMPLE; i++)
		sum += sa_oplus_task_info(task).sa_info[type][i];

	return sum;
}

void try_to_mark_task_type(struct task_struct *tsk, int type)
{
	int i = 0;
	int fit_small = 0;
	u64 sum_running = 0;

	switch (type) {
	case TASK_SMALL:
		if ((sa_oplus_task_info(tsk).sa_info[TST_EXEC][TASK_INFO_SAMPLE - 1] != 0) &&
		    (sa_oplus_task_info(tsk).sa_info[TST_SLEEP][TASK_INFO_SAMPLE - 1] != 0)) {
			sum_running = task_info_sum(tsk, TST_EXEC);
			while (i < TASK_INFO_SAMPLE) {
				if ((sa_oplus_task_info(tsk).sa_info[TST_EXEC][i] < RUNNING_MIN_THREASH) &&
				    ((sa_oplus_task_info(tsk).sa_info[TST_EXEC][i] * 5) <
				     sa_oplus_task_info(tsk).sa_info[TST_SLEEP][i])) {
					fit_small++;
				}

				if (fit_small >= FIT_SMALL_THREASH) {
					sa_oplus_task_info(tsk).im_small = true;
					goto out;
				}
				i++;
			}
		}
		sa_oplus_task_info(tsk).im_small = false;
		break;
	default:
		break;
	}
out:
	return;
}

void update_sa_task_stats(struct task_struct *tsk, u64 delta_ns, int stats_type)
{
	if (unlikely(!tsk) || unlikely(!sysctl_sched_assist_enabled))
		return;

	if (sa_oplus_task_info(tsk).im_small &&
	    (!trace_state_group(tsk) || !sched_assist_scene(SA_CAMERA))) {
		memset(&sa_oplus_task_info(tsk), 0, sizeof(struct task_info));
		return;
	}

	if (!sched_assist_scene(SA_CAMERA) || !trace_state_group(tsk))
		return;

	update_task_sched_stat_common(tsk, delta_ns, stats_type);
	try_to_mark_task_type(tsk, TASK_SMALL);
}

void sched_assist_update_record(struct task_struct *p, u64 delta_ns, int stats_type)
{
	update_sa_task_stats(p, delta_ns, stats_type);
}
#endif /* CONFIG_OPLUS_FEATURE_AUDIO_OPT */

/*
 * ═══════════════════════════════════════════════════════════════
 * Systrace / tracing helpers
 * ═══════════════════════════════════════════════════════════════
 */

/*
 * sched_assist_systrace_pid - emit systrace marker
 *
 * 5.10 adaptation: removed kallsyms_lookup_name("tracing_mark_write")
 * which is not exported in GKI. Uses trace_printk() directly, which
 * writes to the ftrace ring buffer and is visible in systrace.
 */
void sched_assist_systrace_pid(pid_t pid, int val, const char *fmt, ...)
{
	char log[256];
	va_list args;
	int len;

	if (likely(!param_ux_debug))
		return;

	memset(log, ' ', sizeof(log));
	va_start(args, fmt);
	len = vsnprintf(log, sizeof(log), fmt, args);
	va_end(args);

	if (unlikely(len < 0))
		return;
	else if (unlikely(len == 256))
		log[255] = '\0';

	preempt_disable();
	trace_printk("C|%d|%s|%d\n", pid, log, val);
	preempt_enable();
}

#ifdef CONFIG_OPLUS_FEATURE_AUDIO_OPT
void sched_assist_im_systrace_c(struct task_struct *tsk, int tst_type)
{
	if (likely(!param_ux_debug))
		return;

	if (!trace_state_group(tsk))
		return;

	preempt_disable();
	if (tst_type != -1) {
		trace_printk("C|10001|short_run_target_%d|%d\n",
			     tsk->pid, sa_oplus_task_info(tsk).im_small ? 1 : 0);
	} else if (tst_type == -1) {
		trace_printk("C|10001|short_run_target_buddy_%d|%d\n", tsk->pid, 1);
		trace_printk("C|10001|short_run_target_buddy_%d|%d\n", tsk->pid, 0);
	}
	preempt_enable();
}
#endif /* CONFIG_OPLUS_FEATURE_AUDIO_OPT */

/*
 * ═══════════════════════════════════════════════════════════════
 * Proc nodes — /proc/oplus_scheduler/sched_assist/*
 *
 * 5.10 adaptation: struct file_operations → struct proc_ops
 * ═══════════════════════════════════════════════════════════════
 */

static int proc_ux_state_show(struct seq_file *m, void *v)
{
	struct inode *inode = m->private;
	struct task_struct *p;

	p = get_proc_task(inode);
	if (!p)
		return -ESRCH;

	task_lock(p);
	seq_printf(m, "%d\n", sa_ux_state(p));
	task_unlock(p);
	put_task_struct(p);
	return 0;
}

static int proc_ux_state_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, proc_ux_state_show, inode);
}

static ssize_t proc_ux_state_write(struct file *file, const char __user *buf,
				   size_t count, loff_t *ppos)
{
	struct task_struct *task;
	char buffer[PROC_NUMBUF];
	int err, ux_state;
#if IS_ENABLED(CONFIG_OPLUS_FEATURE_SCHED_UX_PRIORITY)
	int ux_orig;
#endif

	memset(buffer, 0, sizeof(buffer));

	if (count > sizeof(buffer) - 1)
		count = sizeof(buffer) - 1;
	if (copy_from_user(buffer, buf, count))
		return -EFAULT;

	err = kstrtoint(strstrip(buffer), 0, &ux_state);
	if (err)
		return err;

	task = get_proc_task(file_inode(file));
	if (!task)
		return -ESRCH;

	if (ux_state < 0) {
		put_task_struct(task);
		return -EINVAL;
	}

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_SCHED_UX_PRIORITY)
	ux_orig = sa_ux_state(task);
	if (ux_state == SA_OPT_CLEAR) {
		/* clear all ux type but animator and specific ux type */
		ux_orig &= (SA_TYPE_ANIMATOR | SA_TYPE_ID_ALLOCATOR_SER |
			    SA_TYPE_ID_CAMERA_PROVIDER);
		oplus_set_ux_state_lock(task, ux_orig, true);
	} else if (ux_state & SA_OPT_SET) {
		/* set target ux type and clear set opt */
		if (ux_state & SA_OPT_SET_PRIORITY)
			ux_orig &= ~(SCHED_ASSIST_UX_PRIORITY_MASK);
		ux_orig |= ux_state & ~(SA_OPT_SET | SA_OPT_SET_PRIORITY);
		oplus_set_ux_state_lock(task, ux_orig, true);
	} else if (ux_orig & ux_state) {
		/* reset target ux type */
		ux_orig &= ~ux_state;
		oplus_set_ux_state_lock(task, ux_orig, true);
	}
#else
	/* remove priority bit of ux state */
	ux_state &= (0x00FFFDFF);
	if (ux_state == SA_OPT_CLEAR) {
		sa_ux_state(task) &= ~(SA_TYPE_LISTPICK | SA_TYPE_HEAVY | SA_TYPE_LIGHT);
	} else if (ux_state & SA_OPT_SET) {
		sa_ux_state(task) |= ux_state & (~SA_OPT_SET);
	} else if (sa_ux_state(task) & ux_state) {
		sa_ux_state(task) &= ~ux_state;
	}
#endif
	sched_assist_systrace_pid(task->tgid, sa_ux_state(task),
				  "ux_state %d", task->pid);

	put_task_struct(task);
	return count;
}

static ssize_t proc_ux_state_read(struct file *file, char __user *buf,
				  size_t count, loff_t *ppos)
{
	char buffer[256];
	struct task_struct *task = NULL;
	int ux_state = -1;
	size_t len = 0;
	int grp_id = 0;

	task = get_proc_task(file_inode(file));
	if (!task)
		return -ESRCH;

	ux_state = sa_ux_state(task);
#ifdef CONFIG_OPLUS_FEATURE_SCHED_SPREAD
	grp_id = task_cgroup_id(task);
#endif

#ifdef CONFIG_OPLUS_UX_IM_FLAG
	len = snprintf(buffer, sizeof(buffer),
		"pid=%d ux_state=0x%08x inherit=%llx(fu:%d mu:%d rw:%d bi:%d) grp_id=%d ux_im_flag=%d\n",
		task->pid, ux_state,
		(u64)atomic64_read(&sa_inherit_ux(task)),
		test_inherit_ux(task, INHERIT_UX_FUTEX),
		test_inherit_ux(task, INHERIT_UX_MUTEX),
		test_inherit_ux(task, INHERIT_UX_RWSEM),
		test_inherit_ux(task, INHERIT_UX_BINDER),
		grp_id, sa_ux_im_flag(task));
#else
	len = snprintf(buffer, sizeof(buffer),
		"pid=%d ux_state=0x%08x inherit=%llx(fu:%d mu:%d rw:%d bi:%d) grp_id=%d\n",
		task->pid, ux_state,
		(u64)atomic64_read(&sa_inherit_ux(task)),
		test_inherit_ux(task, INHERIT_UX_FUTEX),
		test_inherit_ux(task, INHERIT_UX_MUTEX),
		test_inherit_ux(task, INHERIT_UX_RWSEM),
		test_inherit_ux(task, INHERIT_UX_BINDER),
		grp_id);
#endif

	put_task_struct(task);
	return simple_read_from_buffer(buf, count, ppos, buffer, len);
}

static const struct proc_ops proc_ux_state_operations = {
	.proc_open	= proc_ux_state_open,
	.proc_write	= proc_ux_state_write,
	.proc_read	= proc_ux_state_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

/*
 * sysctl_sched_assist_scene_handler - handle /proc/sys/kernel/sched_assist_scene
 */
int sysctl_sched_assist_scene_handler(struct ctl_table *table, int write,
				      void __user *buffer, size_t *lenp,
				      loff_t *ppos)
{
	int result, save_sa;
	static DEFINE_MUTEX(sa_scene_mutex);

	mutex_lock(&sa_scene_mutex);

	save_sa = sysctl_sched_assist_scene;
	result = proc_dointvec(table, write, buffer, lenp, ppos);

	if (!write)
		goto out;

	if (sysctl_sched_assist_scene == SA_SCENE_OPT_CLEAR)
		goto out;

	if (sysctl_sched_assist_scene & SA_SCENE_OPT_SET) {
		save_sa |= sysctl_sched_assist_scene & (~SA_SCENE_OPT_SET);
	} else if (save_sa & sysctl_sched_assist_scene) {
		save_sa &= ~sysctl_sched_assist_scene;
	}

	sysctl_sched_assist_scene = save_sa;
	sched_assist_systrace(sysctl_sched_assist_scene, "scene");
out:
	mutex_unlock(&sa_scene_mutex);

	return result;
}

/*
 * ═══════════════════════════════════════════════════════════════
 * MMAP lock optimization (CONFIG_MMAP_LOCK_OPT)
 * ═══════════════════════════════════════════════════════════════
 */

#ifdef CONFIG_MMAP_LOCK_OPT
int sysctl_uxchain_v2;

void uxchain_rwsem_wake(struct task_struct *tsk, struct rw_semaphore *sem)
{
	int set_ux_once;

	if (current->mm) {
		set_ux_once = (sem == &(current->mm->mmap_lock));
		if (set_ux_once && sysctl_uxchain_v2)
			sa_ux_once(tsk) = 1;
	}
}

void uxchain_rwsem_down(struct rw_semaphore *sem)
{
	if (current->mm && sem == &(current->mm->mmap_lock) && sysctl_uxchain_v2) {
		sa_get_mmlock(current) = 1;
		sa_get_mmlock_ts(current) = sched_clock();
	}
}

void uxchain_rwsem_up(struct rw_semaphore *sem)
{
	if (current->mm && sem == &(current->mm->mmap_lock) &&
	    sa_get_mmlock(current) == 1 && sysctl_uxchain_v2) {
		sa_get_mmlock(current) = 0;
	}
}
#endif

/*
 * ═══════════════════════════════════════════════════════════════
 * IM mali / cgroup boost
 * ═══════════════════════════════════════════════════════════════
 */

#define TOP_APP_GROUP_ID	4

bool im_mali(struct task_struct *p)
{
	return strstr(p->comm, "mali-cmar-backe");
}

bool cgroup_check_set_sched_assist_boost(struct task_struct *p)
{
	return im_mali(p);
}

int get_st_group_id(struct task_struct *task)
{
	/* CONFIG_SCHED_TUNE not available in 5.10 — return 0 */
	return 0;
}

void cgroup_set_sched_assist_boost_task(struct task_struct *p)
{
	if (cgroup_check_set_sched_assist_boost(p)) {
		int ux_state = sa_ux_state(p);

		if (get_st_group_id(p) == TOP_APP_GROUP_ID)
			oplus_set_ux_state_lock(p, (ux_state | SA_TYPE_HEAVY), true);
		else
			oplus_set_ux_state_lock(p, (ux_state & ~SA_TYPE_HEAVY), true);
	}
}

/*
 * ═══════════════════════════════════════════════════════════════
 * IM flag proc nodes (CONFIG_OPLUS_UX_IM_FLAG)
 * ═══════════════════════════════════════════════════════════════
 */

static ssize_t proc_sched_impt_task_write(struct file *file,
					  const char __user *buf,
					  size_t count, loff_t *ppos)
{
	char temp_buf[32];
	char *temp_str, *token;
	char in_str[2][16];
	int cnt, err, pid;

	static DEFINE_MUTEX(impt_thd_mutex);

	mutex_lock(&impt_thd_mutex);

	memset(temp_buf, 0, sizeof(temp_buf));

	if (count > sizeof(temp_buf) - 1) {
		mutex_unlock(&impt_thd_mutex);
		return -EFAULT;
	}

	if (copy_from_user(temp_buf, buf, count)) {
		mutex_unlock(&impt_thd_mutex);
		return -EFAULT;
	}

	cnt = 0;
	temp_str = strstrip(temp_buf);
	while ((token = strsep(&temp_str, " ")) && *token && (cnt < 2)) {
		strscpy(in_str[cnt], token, sizeof(in_str[cnt]));
		cnt += 1;
	}

	if (cnt != 2) {
		mutex_unlock(&impt_thd_mutex);
		return -EFAULT;
	}

	err = kstrtoint(strstrip(in_str[1]), 0, &pid);
	if (err) {
		mutex_unlock(&impt_thd_mutex);
		return err;
	}

	if (pid < 0 || pid > PID_MAX_DEFAULT) {
		mutex_unlock(&impt_thd_mutex);
		return -EINVAL;
	}

	/* set top app */
	if (!strncmp(in_str[0], "fg", 2)) {
		save_top_app_tgid = pid;
		top_app_type = 0;
		if (!strncmp(in_str[0], "fgLauncher", 10))
			top_app_type = 1; /* 1 is launcher */
		goto out;
	}

	/* set audio app */
	if (!strncmp(in_str[0], "au", 2)) {
		save_audio_tgid = pid;
		goto out;
	}

out:
	mutex_unlock(&impt_thd_mutex);

	return count;
}

static ssize_t proc_sched_impt_task_read(struct file *file, char __user *buf,
					 size_t count, loff_t *ppos)
{
	char buffer[32];
	size_t len = 0;

	len = snprintf(buffer, sizeof(buffer), "top(%d %u) au(%d)\n",
		       save_top_app_tgid, top_app_type, save_audio_tgid);

	return simple_read_from_buffer(buf, count, ppos, buffer, len);
}

static ssize_t proc_debug_enabled_write(struct file *file,
					const char __user *buf,
					size_t count, loff_t *ppos)
{
	char buffer[8];
	int err, val;

	memset(buffer, 0, sizeof(buffer));

	if (count > sizeof(buffer) - 1)
		count = sizeof(buffer) - 1;

	if (copy_from_user(buffer, buf, count))
		return -EFAULT;

	buffer[count] = '\0';
	err = kstrtoint(strstrip(buffer), 10, &val);
	if (err)
		return err;

	global_debug_enabled = val;

	return count;
}

static ssize_t proc_debug_enabled_read(struct file *file, char __user *buf,
				       size_t count, loff_t *ppos)
{
	char buffer[20];
	size_t len = 0;

	len = snprintf(buffer, sizeof(buffer), "debug_enabled=%d\n",
		       global_debug_enabled);

	return simple_read_from_buffer(buf, count, ppos, buffer, len);
}

static const struct proc_ops proc_debug_enabled_pops = {
	.proc_write	= proc_debug_enabled_write,
	.proc_read	= proc_debug_enabled_read,
	.proc_lseek	= default_llseek,
};

static const struct proc_ops proc_sched_impt_task_pops = {
	.proc_write	= proc_sched_impt_task_write,
	.proc_read	= proc_sched_impt_task_read,
	.proc_lseek	= default_llseek,
};

#ifdef CONFIG_OPLUS_UX_IM_FLAG
enum {
	OPT_STR_TYPE = 0,
	OPT_STR_PID,
	OPT_STR_VAL,
	OPT_STR_MAX = 3,
};
#define MAX_SET 128
static pid_t global_im_flag_pid = -1;

static int im_flag_set_handle(struct task_struct *task, int im_flag)
{
	/* check if flag is for app task */
	if (IM_FLAG_FORBID_SET_CPU_AFFINITY == im_flag) {
		int uid = task_uid(task).val;

		if ((uid >= FIRST_APPLICATION_UID) && (uid <= LAST_APPLICATION_UID))
			im_flag = IM_FLAG_FORBID_SET_CPU_AFFINITY_IN_KERNEL;
	}

#ifdef CONFIG_OPLUS_CPU_AUDIO_PERF
	/* Audio perf hook — stub until audio subsystem is ported */
#endif

	sa_ux_im_flag(task) = im_flag;

	switch (sa_ux_im_flag(task)) {
	case IM_FLAG_LAUNCHER_NON_UX_RENDER: {
		int ux_state = sa_ux_state(task) | SA_TYPE_HEAVY;

		oplus_set_ux_state_lock(task, ux_state, true);
		}
		break;
	default:
		break;
	}

	return 0;
}

static ssize_t proc_im_flag_write(struct file *file, const char __user *buf,
				  size_t count, loff_t *ppos)
{
	char buffer[MAX_SET];
	char *str, *token;
	char opt_str[OPT_STR_MAX][16];
	int cnt = 0;
	int pid = 0;
	int im_flag = 0;
	int err = 0;

	memset(buffer, 0, sizeof(buffer));

	if (count > sizeof(buffer) - 1)
		count = sizeof(buffer) - 1;

	if (copy_from_user(buffer, buf, count))
		return -EFAULT;

	buffer[count] = '\0';
	str = strstrip(buffer);
	while ((token = strsep(&str, " ")) && *token && (cnt < OPT_STR_MAX)) {
		strscpy(opt_str[cnt], token, sizeof(opt_str[cnt]));
		cnt += 1;
	}

	if ((cnt != OPT_STR_MAX) && (cnt != OPT_STR_MAX - 1))
		return -EFAULT;

	if (cnt != OPT_STR_MAX) {
		if (cnt == (OPT_STR_MAX - 1) &&
		    !strncmp(opt_str[OPT_STR_TYPE], "r", 1)) {
			err = kstrtoint(strstrip(opt_str[OPT_STR_PID]), 10, &pid);
			if (err)
				return err;

			if (pid > 0 && pid <= PID_MAX_DEFAULT)
				global_im_flag_pid = pid;
		}
		return count;
	}

	err = kstrtoint(strstrip(opt_str[OPT_STR_PID]), 10, &pid);
	if (err)
		return err;

	err = kstrtoint(strstrip(opt_str[OPT_STR_VAL]), 10, &im_flag);
	if (err)
		return err;

	if (!strncmp(opt_str[OPT_STR_TYPE], "p", 1)) {
		struct task_struct *task = NULL;

		if (pid > 0 && pid <= PID_MAX_DEFAULT) {
			rcu_read_lock();
			task = find_task_by_vpid(pid);
			if (task)
				get_task_struct(task);
			rcu_read_unlock();

			if (task) {
				im_flag_set_handle(task, im_flag);
				put_task_struct(task);
			} else {
				ux_debug("Can not find task with pid=%d", pid);
			}
		}
	}

	return count;
}

static ssize_t proc_im_flag_read(struct file *file, char __user *buf,
				 size_t count, loff_t *ppos)
{
	char buffer[256];
	size_t len = 0;
	struct task_struct *task = NULL;

	rcu_read_lock();
	task = find_task_by_vpid(global_im_flag_pid);
	if (task) {
		get_task_struct(task);
		len = snprintf(buffer, sizeof(buffer),
			       "comm=%s pid=%d tgid=%d ux_im_flag=%d\n",
			       task->comm, task->pid, task->tgid,
			       sa_ux_im_flag(task));
		put_task_struct(task);
	} else {
		len = snprintf(buffer, sizeof(buffer), "Can not find task\n");
	}
	rcu_read_unlock();

	return simple_read_from_buffer(buf, count, ppos, buffer, len);
}

static inline bool can_access_im_flag_app(struct task_struct *task)
{
	return task->tgid == current->tgid;
}

/*
 * proc_im_flag_app_write - only accepts app changing im flag of its child threads.
 * Audio app uses this to change im flag.
 */
static ssize_t proc_im_flag_app_write(struct file *file,
				      const char __user *buf,
				      size_t count, loff_t *ppos)
{
	char buffer[MAX_SET];
	char *str, *token;
	char opt_str[OPT_STR_MAX][8];
	int cnt = 0;
	int pid = 0;
	int im_flag = 0;
	int err = 0;
	static DEFINE_MUTEX(sa_im_mutex);

	memset(buffer, 0, sizeof(buffer));

	if (count > sizeof(buffer) - 1)
		count = sizeof(buffer) - 1;

	if (copy_from_user(buffer, buf, count))
		return -EFAULT;

	buffer[count] = '\0';
	str = strstrip(buffer);
	while ((token = strsep(&str, " ")) && *token && (cnt < OPT_STR_MAX)) {
		strscpy(opt_str[cnt], token, sizeof(opt_str[cnt]));
		cnt += 1;
	}

	if (cnt != OPT_STR_MAX) {
		if (cnt == (OPT_STR_MAX - 1) &&
		    !strncmp(opt_str[OPT_STR_TYPE], "r", 1)) {
			err = kstrtoint(strstrip(opt_str[OPT_STR_PID]), 10, &pid);
			if (err)
				return err;

			if (pid > 0 && pid <= PID_MAX_DEFAULT)
				global_im_flag_pid = pid;

			return count;
		} else {
			return -EFAULT;
		}
	}

	err = kstrtoint(strstrip(opt_str[OPT_STR_PID]), 10, &pid);
	if (err)
		return err;

	err = kstrtoint(strstrip(opt_str[OPT_STR_VAL]), 10, &im_flag);
	if (err)
		return err;

	mutex_lock(&sa_im_mutex);
	if (!strncmp(opt_str[OPT_STR_TYPE], "p", 1)) {
		struct task_struct *task = NULL;

		if (pid > 0 && pid <= PID_MAX_DEFAULT) {
			rcu_read_lock();
			task = find_task_by_vpid(pid);
			if (task && can_access_im_flag_app(task)) {
				get_task_struct(task);
				im_flag_set_handle(task, im_flag);
				put_task_struct(task);
			} else {
				ux_debug("Can not find task with pid=%d", pid);
			}
			rcu_read_unlock();
		}
	}

	mutex_unlock(&sa_im_mutex);
	return count;
}

static ssize_t proc_im_flag_app_read(struct file *file, char __user *buf,
				     size_t count, loff_t *ppos)
{
	char buffer[256];
	size_t len = 0;
	struct task_struct *task = NULL;

	task = find_task_by_vpid(global_im_flag_pid);
	if (task && can_access_im_flag_app(task)) {
		get_task_struct(task);
		len = snprintf(buffer, sizeof(buffer),
			       "comm=%s pid=%d tgid=%d im_flag=%d\n",
			       task->comm, task->pid, task->tgid,
			       sa_ux_im_flag(task));
		put_task_struct(task);
	} else {
		len = snprintf(buffer, sizeof(buffer), "Can not find task\n");
	}

	return simple_read_from_buffer(buf, count, ppos, buffer, len);
}

static const struct proc_ops proc_im_flag_pops = {
	.proc_write	= proc_im_flag_write,
	.proc_read	= proc_im_flag_read,
	.proc_lseek	= default_llseek,
};

static const struct proc_ops proc_im_flag_app_pops = {
	.proc_write	= proc_im_flag_app_write,
	.proc_read	= proc_im_flag_app_read,
	.proc_lseek	= default_llseek,
};

#endif /* CONFIG_OPLUS_UX_IM_FLAG */

/*
 * ═══════════════════════════════════════════════════════════════
 * Init — create /proc/oplus_scheduler/sched_assist/*
 * ═══════════════════════════════════════════════════════════════
 */

#define OPLUS_SCHEDULER_PROC_DIR	"oplus_scheduler"
#define OPLUS_SCHEDASSIST_PROC_DIR	"sched_assist"
struct proc_dir_entry *d_oplus_scheduler;
struct proc_dir_entry *d_sched_assist;

static int __init oplus_sched_assist_init(void)
{
	struct proc_dir_entry *proc_node;

	d_oplus_scheduler = proc_mkdir(OPLUS_SCHEDULER_PROC_DIR, NULL);
	if (!d_oplus_scheduler) {
		ux_err("failed to create proc dir oplus_scheduler\n");
		goto err_dir_scheduler;
	}

	d_sched_assist = proc_mkdir(OPLUS_SCHEDASSIST_PROC_DIR, d_oplus_scheduler);
	if (!d_sched_assist) {
		ux_err("failed to create proc dir sched_assist\n");
		goto err_dir_sa;
	}

	proc_node = proc_create("debug_enabled", 0666, d_sched_assist,
				&proc_debug_enabled_pops);
	if (!proc_node) {
		ux_err("failed to create proc node debug_enabled\n");
		goto err_creat_debug_enabled;
	}

	proc_node = proc_create("sched_impt_task", 0666, d_sched_assist,
				&proc_sched_impt_task_pops);
	if (!proc_node) {
		ux_err("failed to create proc node sched_impt_task\n");
#ifdef CONFIG_OPLUS_UX_IM_FLAG
		goto err_node_assist;
#else
		goto err_node_impt;
#endif
	}

#ifdef CONFIG_OPLUS_UX_IM_FLAG
	proc_node = proc_create("im_flag", 0666, d_sched_assist,
				&proc_im_flag_pops);
	if (!proc_node) {
		ux_err("failed to create proc node im_flag\n");
		goto err_node_assist;
	}

	proc_node = proc_create("im_flag_app", 0666, d_sched_assist,
				&proc_im_flag_app_pops);
	if (!proc_node) {
		ux_err("failed to create proc node im_flag_app\n");
		remove_proc_entry("im_flag_app", d_sched_assist);
	}
#endif

	return 0;

#ifdef CONFIG_OPLUS_UX_IM_FLAG
err_node_assist:
#else
err_node_impt:
#endif
	remove_proc_entry(OPLUS_SCHEDASSIST_PROC_DIR, d_oplus_scheduler);

err_creat_debug_enabled:
	remove_proc_entry(OPLUS_SCHEDASSIST_PROC_DIR, d_oplus_scheduler);

err_dir_sa:
	remove_proc_entry(OPLUS_SCHEDULER_PROC_DIR, NULL);

err_dir_scheduler:
	return -ENOENT;
}

device_initcall(oplus_sched_assist_init);

/*
 * ═══════════════════════════════════════════════════════════════
 * UX priority (CONFIG_OPLUS_FEATURE_SCHED_UX_PRIORITY)
 * ═══════════════════════════════════════════════════════════════
 */

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_SCHED_UX_PRIORITY)

void ux_priority_systrace_c(unsigned int cpu, struct task_struct *p)
{
	if (unlikely(global_debug_enabled & DEBUG_SYSTRACE)) {
		int ux_priority = (sa_ux_state(p) & SCHED_ASSIST_UX_PRIORITY_MASK) >>
				  SCHED_ASSIST_UX_PRIORITY_SHIFT;

		if (per_cpu(prev_ux_priority, cpu) != ux_priority) {
			char buf[256];

			snprintf(buf, sizeof(buf), "C|9998|Cpu%d_ux_priority|%d\n",
				 cpu, ux_priority);
			tracing_mark_write(buf);
			per_cpu(prev_ux_priority, cpu) = ux_priority;
		}
	}
}

unsigned int ux_task_exec_limit(struct task_struct *p)
{
	int ux_state = sa_ux_state(p);
	unsigned int exec_limit = UX_EXEC_SLICE;

	/*
	 * SS_LOCK_OWNER has lowest ux prio and lowest ux exectime.
	 * There may be many ss_lockers and we don't know if they really
	 * matter. Anyway, there's no harm in being careful.
	 */
#ifdef CONFIG_OPLUS_UX_IM_FLAG
	if ((sa_ux_im_flag(p) == IM_FLAG_SS_LOCK_OWNER) &&
	    (ux_state & SA_TYPE_LISTPICK))
		return exec_limit;
#endif

	if (sched_assist_scene(SA_LAUNCH) && !(ux_state & SA_TYPE_INHERIT)) {
		exec_limit *= 25;
		return exec_limit;
	}

	if (ux_state & SA_TYPE_ANIMATOR)
		exec_limit *= 8;
	else if (ux_state & SA_TYPE_LIGHT)
		exec_limit *= 2;
	else if (ux_state & SA_TYPE_HEAVY)
		exec_limit *= 8;
	else if (ux_state & SA_TYPE_LISTPICK)
		exec_limit *= 25;

	return exec_limit;
}

static int prio_higher(int a, int b)
{
	int a_type = a & SCHED_ASSIST_UX_MASK;
	int b_type = b & SCHED_ASSIST_UX_MASK;

	if (a_type == b_type)
		return 0;

	/* Animator > Light > Heavy > Listpick */
	if (a_type & SA_TYPE_ANIMATOR)
		return 1;
	if (b_type & SA_TYPE_ANIMATOR)
		return -1;
	if (a_type & SA_TYPE_LIGHT)
		return 1;
	if (b_type & SA_TYPE_LIGHT)
		return -1;
	if (a_type & SA_TYPE_HEAVY)
		return 1;
	if (b_type & SA_TYPE_HEAVY)
		return -1;

	return 0;
}

static void insert_ux_task_into_list_ordered(struct rq *rq, struct task_struct *p)
{
	struct list_head *pos;
	bool exist = false;

	list_for_each(pos, sa_ux_thread_list(rq)) {
		struct walt_task_struct *wts = list_entry(pos,
			struct walt_task_struct, ux_entry);
		struct task_struct *temp = wts_to_ts(wts);

		if (pos == &sa_ux_entry(p)) {
			exist = true;
			BUG_ON(1);
			break;
		}

		if (prio_higher(sa_ux_state(p), sa_ux_state(temp)) > 0) {
			list_add_tail(&sa_ux_entry(p), pos);
			get_task_struct(p);
			return;
		}
	}

	if (!exist) {
		list_add_tail(&sa_ux_entry(p), sa_ux_thread_list(rq));
		get_task_struct(p);
	}
}

void enqueue_ux_thread_to_list(struct rq *rq, struct task_struct *p)
{
	if (unlikely(!sysctl_sched_assist_enabled))
		return;

	if (!rq || !p || !list_empty(&sa_ux_entry(p)))
		return;

#ifdef CONFIG_OPLUS_CPU_AUDIO_PERF
	/* Audio hook — stub until audio subsystem is ported */
#endif
	sa_enqueue_time(p) = rq_clock(rq);
	raw_spin_lock(sa_ux_list_lock(rq));
	insert_ux_task_into_list_ordered(rq, p);
	raw_spin_unlock(sa_ux_list_lock(rq));
	get_task_struct(p);
	sa_sum_exec_baseline(p) = 0;
}

void dequeue_ux_thread_from_list(struct rq *rq, struct task_struct *p)
{
	unsigned long flags;
	u64 inherit_ux_start;
	u64 now;

	if (!rq || !p)
		return;

	sa_enqueue_time(p) = 0;
	raw_spin_lock_irqsave(sa_ux_list_lock(rq), flags);
	if (!list_empty(&sa_ux_entry(p))) {
		list_del_init(&sa_ux_entry(p));
		/* Check inherit timeout before clearing */
		inherit_ux_start = sa_inherit_ux_start(p);
		now = jiffies_to_nsecs(jiffies);
		if (inherit_ux_start &&
		    (now - inherit_ux_start < MAX_INHERIT_GRAN)) {
			/* inherit still valid — clear all non-ID bits */
			sa_ux_state(p) &= (SA_TYPE_ID_ALLOCATOR_SER |
					   SA_TYPE_ID_CAMERA_PROVIDER);
		} else {
			/* inherit expired or not inherit — clear inherit+once */
			sa_ux_state(p) &= ~(SA_TYPE_INHERIT | SA_TYPE_ONCE_UX);
		}
		sa_ux_depth(p) = 0;
		atomic64_set(&sa_inherit_ux(p), 0);
		put_task_struct(p);
	}
	if (READ_ONCE(p->__state) != TASK_RUNNING)
		sa_wts(p)->total_exec = 0;
	raw_spin_unlock_irqrestore(sa_ux_list_lock(rq), flags);
}

/*
 * account_ux_runtime - update UX task runtime accounting
 *
 * Must be called with rq->lock held.
 */
void account_ux_runtime(struct rq *rq, struct task_struct *p)
{
	struct sched_entity *se = &p->se;
	s64 delta_exec;

	if (!rq || !p)
		return;

	lockdep_assert_held(&rq->lock);

	if (!(rq->clock_update_flags & RQCF_UPDATED))
		update_rq_clock(rq);

	delta_exec = se->sum_exec_runtime - sa_sum_exec_baseline(p);
	if (delta_exec < 0)
		delta_exec = 0;

	/* Skip tiny deltas — same as 4.19 UX_EXEC_SLICE minimum */
	if (delta_exec < UX_EXEC_SLICE)
		return;

	sa_sum_exec_baseline(p) = se->sum_exec_runtime;
	sa_wts(p)->total_exec += delta_exec;

	/* Check if task exceeded exec limit */
	if (ux_task_exec_limit(p) && sa_wts(p)->total_exec > ux_task_exec_limit(p)) {
		raw_spin_lock(sa_ux_list_lock(rq));
		if (!list_empty(&sa_ux_entry(p))) {
			list_del_init(&sa_ux_entry(p));
			put_task_struct(p);
		}
		raw_spin_unlock(sa_ux_list_lock(rq));
	}
}

void oplus_check_preempt_wakeup_in_list(struct rq *rq,
					struct task_struct *wake_task,
					struct task_struct *curr_task,
					bool *preempt, bool *nopreempt)
{
	unsigned long flags;
	bool wake_ux = false;
	bool curr_ux = false;

	if (!rq || !wake_task || !curr_task)
		return;

	if (unlikely(!sysctl_sched_assist_enabled))
		return;

	wake_ux = test_task_ux(wake_task) || test_list_pick_ux(wake_task) ||
		  test_task_identify_ux(wake_task, SA_TYPE_ID_CAMERA_PROVIDER);
	curr_ux = test_task_ux(curr_task) || test_list_pick_ux(curr_task) ||
		  test_task_identify_ux(curr_task, SA_TYPE_ID_CAMERA_PROVIDER);

	if (wake_ux && !curr_ux) {
		*preempt = true;
		return;
	}

	if (!wake_ux && curr_ux) {
		*nopreempt = true;
		return;
	}

	if (wake_ux && curr_ux) {
		/* Higher priority UX wins */
		if (prio_higher(sa_ux_state(wake_task), sa_ux_state(curr_task)) > 0) {
			*preempt = true;
			return;
		}
	}

	/* Update current UX runtime under lock */
	raw_spin_lock_irqsave(sa_ux_list_lock(rq), flags);
	if (task_current(rq, curr_task) && test_task_ux(curr_task))
		account_ux_runtime(rq, curr_task);
	raw_spin_unlock_irqrestore(sa_ux_list_lock(rq), flags);
}

void android_vh_scheduler_tick_handler(struct rq *rq)
{
	unsigned long flags;
	struct task_struct *curr;

	if (!rq)
		return;

	curr = rq->curr;
	if (!curr)
		return;

	if (!test_task_ux(curr))
		return;

	raw_spin_lock_irqsave(sa_ux_list_lock(rq), flags);
	if (list_empty(sa_ux_thread_list(rq))) {
		raw_spin_unlock_irqrestore(sa_ux_list_lock(rq), flags);
		return;
	}
	if (task_current(rq, curr))
		account_ux_runtime(rq, curr);
	raw_spin_unlock_irqrestore(sa_ux_list_lock(rq), flags);
}

/*
 * oplus_replace_next_task_fair - pick UX task for next scheduling
 *
 * Picks the first valid UX task from the ordered list, cleaning stale
 * entries (CPU-mismatched or non-UX) with proper put_task_struct().
 */
static void oplus_replace_next_task_fair(struct rq *rq, struct task_struct **p,
					 struct sched_entity **se, bool *repick,
					 bool simple)
{
	unsigned long flags;
	struct list_head *pos, *n;
	struct task_struct *task = NULL;

	if (unlikely(!sysctl_sched_assist_enabled))
		return;

	raw_spin_lock_irqsave(sa_ux_list_lock(rq), flags);

	if (list_empty(sa_ux_thread_list(rq))) {
		raw_spin_unlock_irqrestore(sa_ux_list_lock(rq), flags);
		return;
	}

#ifdef CONFIG_OPLUS_FEATURE_AUDIO_OPT
	/* Audio opt: check if current task is im_small */
	if (*p && sa_oplus_task_info(*p).im_small) {
		raw_spin_unlock_irqrestore(sa_ux_list_lock(rq), flags);
		return;
	}
#endif

	list_for_each_safe(pos, n, sa_ux_thread_list(rq)) {
		struct walt_task_struct *wts = list_entry(pos,
			struct walt_task_struct, ux_entry);
		struct task_struct *temp = wts_to_ts(wts);

		/* Clean stale entries */
		if (task_cpu(temp) != rq->cpu) {
			WARN_ON(1);
			list_del_init(&sa_ux_entry(temp));
			put_task_struct(temp);
			continue;
		}

		if (!test_task_ux(temp) ||
		    READ_ONCE(temp->__state) != TASK_RUNNING) {
			list_del_init(&sa_ux_entry(temp));
			put_task_struct(temp);
			continue;
		}

		/* Check exec limit */
		if (ux_task_exec_limit(temp) &&
		    sa_wts(temp)->total_exec > ux_task_exec_limit(temp))
			continue;

		task = temp;
		break;
	}

	if (task) {
		*p = task;
		*se = &task->se;
		*repick = true;
	}
	raw_spin_unlock_irqrestore(sa_ux_list_lock(rq), flags);
}

void android_rvh_replace_next_task_fair_handler(struct rq *rq,
						struct task_struct **p,
						struct sched_entity **se,
						bool *repick, bool simple)
{
	oplus_replace_next_task_fair(rq, p, se, repick, simple);

#if IS_ENABLED(CONFIG_LOCKING_PROTECT)
	if (!*repick) {
		/* pick_locking_thread() — stub until locking subsystem is ported */
	}
#endif
}

/*
 * oplus_set_ux_state_lock - set UX state with proper locking
 *
 * UX synchronization rules:
 * - When setting/changing UX: acquire rq lock, then ux_list_lock
 * - When clearing/dequeue: only ux_list_lock needed
 * - List empty check is atomic under ux_list_lock
 */
void oplus_set_ux_state_lock(struct task_struct *t, int ux_state, bool need_lock_rq)
{
	struct rq_flags rf;
	struct rq *rq;
	unsigned long flags;
	int old_state;

	if (!t)
		return;

	if (need_lock_rq) {
		rq = task_rq_lock(t, &rf);

		if (t->sched_class != &fair_sched_class) {
			task_rq_unlock(rq, t, &rf);
			return;
		}

		old_state = sa_ux_state(t);
		if (old_state == ux_state) {
			task_rq_unlock(rq, t, &rf);
			return;
		}

		raw_spin_lock_irqsave(sa_ux_list_lock(rq), flags);
		sa_ux_state(t) = ux_state;

		/* If no UX bits set, remove from list */
		if (!(ux_state & (SCHED_ASSIST_UX_MASK | POSSIBLE_UX_MASK |
				  SA_TYPE_INHERIT | SA_TYPE_ID_CAMERA_PROVIDER |
				  SA_TYPE_ID_ALLOCATOR_SER))) {
			if (!list_empty(&sa_ux_entry(t))) {
				list_del_init(&sa_ux_entry(t));
				put_task_struct(t);
			}
		} else {
			/* Reorder in list if task is on rq */
			if (task_on_rq_queued(t)) {
				if (!list_empty(&sa_ux_entry(t))) {
					list_del_init(&sa_ux_entry(t));
					/* Re-insert at correct position */
					insert_ux_task_into_list_ordered(rq, t);
				} else {
					/* New entry — take reference */
					get_task_struct(t);
					insert_ux_task_into_list_ordered(rq, t);
				}
			}
			sa_sum_exec_baseline(t) = 0;
		}

		raw_spin_unlock_irqrestore(sa_ux_list_lock(rq), flags);
		task_rq_unlock(rq, t, &rf);
	} else {
		sa_ux_state(t) = ux_state;
	}
}

#endif /* IS_ENABLED(CONFIG_OPLUS_FEATURE_SCHED_UX_PRIORITY) */

/*
 * oplus_sched_ban_setaffinity - ban setaffinity for IM flag tasks
 *
 * Application UID tasks with IM_FLAG_FORBID_SET_CPU_AFFINITY_IN_KERNEL
 * are protected from kernel-side affinity changes.
 */
bool oplus_sched_ban_setaffinity(struct task_struct *task,
				 const struct cpumask *new_mask)
{
#ifdef CONFIG_OPLUS_UX_IM_FLAG
	int uid;

	if (!task)
		return false;

	uid = task_uid(task).val;
	if (uid < FIRST_APPLICATION_UID || uid > LAST_APPLICATION_UID)
		return false;

	rcu_read_lock();
	if (sa_ux_im_flag(task) == IM_FLAG_FORBID_SET_CPU_AFFINITY_IN_KERNEL) {
		rcu_read_unlock();
		return true;
	}
	/* Also check group leader */
	if (task->group_leader &&
	    sa_ux_im_flag(task->group_leader) == IM_FLAG_FORBID_SET_CPU_AFFINITY_IN_KERNEL) {
		rcu_read_unlock();
		return true;
	}
	rcu_read_unlock();
#endif
	return false;
}
