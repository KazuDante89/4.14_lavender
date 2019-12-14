/*
 * CPUFreq governor based on scheduler-provided CPU utilization data.
 *
 * Copyright (C) 2016, Intel Corporation
 * Author: Rafael J. Wysocki <rafael.j.wysocki@intel.com>
 *
 * Copyright (C) 2018, The XPerience Project
 * Author: Carlos J. B. C <xXx.Reptar.Rawrr.xXx@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * v2.0 Use >= when aggregating CPU loads in a policy
 * - Switch from sprintf to scnprintf
 * - Add trace point for get_next_freq
 * - Avoid processing certain notifications
 * - Return to FIFO
 * v2.3
 * - Implement Energy Model
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/cpufreq.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <trace/events/power.h>
#include <linux/energy_model.h>

#include "sched.h"
#include "tune.h"

unsigned long boosted_cpu_util(int cpu);

/* Stub out fast switch routines present on mainline to reduce the backport
 * overhead. */
#define cpufreq_driver_fast_switch(x, y) 0
#define cpufreq_enable_fast_switch(x)
#define cpufreq_disable_fast_switch(x)
#define UP_RATE_LIMIT_US        (500)
#define DOWN_RATE_LIMIT_US	    (20000)
#define algov_up_rate_limit 500
#define algov_down_rate_limit 20000

struct algov_tunables {
	struct gov_attr_set attr_set;
	unsigned int up_rate_limit_us;
	unsigned int down_rate_limit_us;
	bool iowait_boost_enable;
};

struct algov_policy {
	struct cpufreq_policy *policy;

	struct algov_tunables *tunables;
	struct list_head tunables_hook;

	raw_spinlock_t update_lock;  /* For shared policies */
	u64 last_freq_update_time;
	s64 min_rate_limit_ns;
	s64 up_rate_delay_ns;
	s64 down_rate_delay_ns;
	unsigned int next_freq;
	unsigned int cached_raw_freq;

	/* The next fields are only needed if fast switch cannot be used. */
	struct irq_work irq_work;
	struct kthread_work work;
	struct mutex work_lock;
	struct kthread_worker worker;
	struct task_struct *thread;
	bool work_in_progress;

	bool need_freq_update;
#ifdef CONFIG_ENERGY_MODEL
	struct em_perf_domain *pd;
#endif
};

struct algov_cpu {
	struct update_util_data update_util;
	struct algov_policy *sg_policy;

	bool iowait_boost_pending;
	unsigned int iowait_boost;
	unsigned int iowait_boost_max;
	u64 last_update;

	/* The fields below are only needed when sharing a policy. */
	unsigned long util;
	unsigned long max;
	unsigned int flags;
	unsigned int cpu;

	/* The field below is for single-CPU policies only. */
#ifdef CONFIG_NO_HZ_COMMON
	unsigned long saved_idle_calls;
	unsigned long		previous_util;
#endif
};

static DEFINE_PER_CPU(struct algov_cpu, algov_cpu);
static DEFINE_PER_CPU(struct algov_tunables *, cached_tunables);
static unsigned int stale_ns;

/************************ Governor internals ***********************/

#ifdef CONFIG_ENERGY_MODEL
static void algov_policy_attach_pd(struct algov_policy *sg_policy)
{
	struct em_perf_domain *pd;
	struct cpufreq_policy *policy = sg_policy->policy;

	sg_policy->pd = NULL;
	pd = em_cpu_get(policy->cpu);
	if (!pd)
		return;

	if (cpumask_equal(policy->related_cpus, to_cpumask(pd->cpus)))
		sg_policy->pd = pd;
	else
		pr_warn("%s: Not all CPUs in schedalessa policy %u share the same perf domain, no perf domain for that policy will be registered\n",
			__func__, policy->cpu);
}

static struct em_perf_domain *algov_policy_get_pd(
						struct algov_policy *sg_policy)
{
	return sg_policy->pd;
}
#else /* CONFIG_ENERGY_MODEL */
static void algov_policy_attach_pd(struct algov_policy *sg_policy) {}
static struct em_perf_domain *algov_policy_get_pd(
						struct algov_policy *sg_policy)
{
	return NULL;
}
#endif /* CONFIG_ENERGY_MODEL */


static bool algov_should_update_freq(struct algov_policy *sg_policy, u64 time)
{
	s64 delta_ns;

	/*
	 * Since cpufreq_update_util() is called with rq->lock held for
	 * the @target_cpu, our per-cpu data is fully serialized.
	 *
	 * However, drivers cannot in general deal with cross-cpu
	 * requests, so while get_next_freq() will work, our
	 * algov_update_commit() call may not for the fast switching platforms.
	 *
	 * Hence stop here for remote requests if they aren't supported
	 * by the hardware, as calculating the frequency is pointless if
	 * we cannot in fact act on it.
	 *
	 * For the slow switching platforms, the kthread is always scheduled on
	 * the right set of CPUs and any CPU can find the next frequency and
	 * schedule the kthread.
	 */
	if (sg_policy->policy->fast_switch_enabled &&
	    !cpufreq_this_cpu_can_update(sg_policy->policy))
		return false;

	if (unlikely(sg_policy->need_freq_update))
		return true;

	delta_ns = time - sg_policy->last_freq_update_time;

	/* No need to recalculate next freq for min_rate_limit_us at least */
	return delta_ns >= sg_policy->min_rate_limit_ns;
}

static bool algov_up_down_rate_limit(struct algov_policy *sg_policy, u64 time,
				     unsigned int next_freq)
{
	s64 delta_ns;

	delta_ns = time - sg_policy->last_freq_update_time;

	if (next_freq > sg_policy->next_freq &&
	    delta_ns < sg_policy->up_rate_delay_ns)
			return true;

	if (next_freq < sg_policy->next_freq &&
	    delta_ns < sg_policy->down_rate_delay_ns)
			return true;

	return false;
}

static bool algov_update_next_freq(struct algov_policy *sg_policy, u64 time,
				   unsigned int next_freq)
{
        if (algov_up_down_rate_limit(sg_policy, time, next_freq)) {
                /* Reset cached freq as next_freq isn't changed */
                sg_policy->cached_raw_freq = 0;
                return false;
        }

	if (sg_policy->next_freq == next_freq)
		return false;

	if (sg_policy->next_freq > next_freq)
		next_freq = (sg_policy->next_freq + next_freq) >> 1;

	sg_policy->next_freq = next_freq;
	sg_policy->last_freq_update_time = time;

	return true;
}

static void algov_fast_switch(struct algov_policy *sg_policy, u64 time,
			      unsigned int next_freq)
{
	struct cpufreq_policy *policy = sg_policy->policy;

	if (!algov_update_next_freq(sg_policy, time, next_freq))
		return;

	next_freq = cpufreq_driver_fast_switch(policy, next_freq);
	if (!next_freq)
		return;

	policy->cur = next_freq;
	trace_cpu_frequency(next_freq, smp_processor_id());
}

static void algov_deferred_update(struct algov_policy *sg_policy, u64 time,
				  unsigned int next_freq)
{
	if (!algov_update_next_freq(sg_policy, time, next_freq))
		return;

	if (!sg_policy->work_in_progress) {
		sg_policy->work_in_progress = true;
		irq_work_queue(&sg_policy->irq_work);
	}
}

#ifdef CONFIG_NO_HZ_COMMON
static bool algov_cpu_is_busy(struct algov_cpu *sg_cpu)
{
	unsigned long idle_calls = tick_nohz_get_idle_calls();
	bool ret = idle_calls == sg_cpu->saved_idle_calls;

	sg_cpu->saved_idle_calls = idle_calls;
	return ret;
}
static void algov_cpu_is_busy_update(struct algov_cpu *sg_cpu,
				     unsigned long util)
{
	unsigned long idle_calls = tick_nohz_get_idle_calls_cpu(sg_cpu->cpu);
 	sg_cpu->saved_idle_calls = idle_calls;

	/*
	 * Make sure that this CPU will not be immediately considered as busy in
	 * cases where the CPU has already entered an idle state. In that case,
	 * the number of idle_calls will not vary anymore until it exits idle,
	 * which would lead algov_cpu_is_busy() to say that this CPU is busy,
	 * because it has not (re)entered idle since the last time we looked at
	 * it.
	 * Assuming cpu0 and cpu1 are in the same policy, that will make sure
	 * this sequence of events leads to right cpu1 business status from
	 * get_next_freq(cpu=1)
	 * cpu0: [enter idle] -> [get_next_freq] -> [doing nothing] -> [wakeup]
	 * cpu1:                ...              -> [get_next_freq] ->   ...
	 */
	if (util <= sg_cpu->previous_util)
		sg_cpu->saved_idle_calls--;

	sg_cpu->previous_util = util;
}
#else
static inline bool algov_cpu_is_busy(struct algov_cpu *sg_cpu) { return false; }
static void algov_cpu_is_busy_update(struct algov_cpu *sg_cpu
				     unsigned long util)
{}
#endif /* CONFIG_NO_HZ_COMMON */

/**
 * get_next_freq - Compute a new frequency for a given cpufreq policy.
 * @sg_policy: schedalessa policy object to compute the new frequency for.
 * @util: Current CPU utilization.
 * @max: CPU capacity.
 * @busy: true if at least one CPU in the policy is busy, which means it had no
 *	idle time since its last frequency change.
 *
 * If the utilization is frequency-invariant, choose the new frequency to be
 * proportional to it, that is
 *
 * next_freq = C * max_freq * util / max
 *
 * Otherwise, approximate the would-be frequency-invariant utilization by
 * util_raw * (curr_freq / max_freq) which leads to
 *
 * next_freq = C * curr_freq * util_raw / max
 *
 * Take C = 1.25 for the frequency tipping point at (util / max) = 0.8.
 *
 * An energy-aware boost is then applied if busy is true. The boost will allow
 * selecting frequencies at most twice as costly in term of energy.
 *
 * The lowest driver-supported frequency which is equal or greater than the raw
 * next_freq (as calculated above) is returned, subject to policy min/max and
 * cpufreq driver limitations.
 */
static unsigned int get_next_freq(struct algov_policy *sg_policy,
				  unsigned long util, unsigned long max,
				  bool busy)
{
	struct cpufreq_policy *policy = sg_policy->policy;
	unsigned int freq = arch_scale_freq_invariant() ?
				policy->cpuinfo.max_freq : policy->cur;

	struct em_perf_domain *pd = algov_policy_get_pd(sg_policy);

	/*
	 * Maximum power we are ready to spend.
	 * When one CPU is busy in the policy, we apply a boost to help it reach
	 * the needed frequency faster.
	 */
	unsigned int cost_margin = busy ? 1024/2 : 0;

	freq = map_util_freq(util, freq, max);

	/*
	 * Try to get a higher frequency if one is available, given the extra
	 * power we are ready to spend.
	 */
	freq = em_pd_get_higher_freq(pd, freq, cost_margin);

	freq = (freq + (freq >> 2)) * util / max;

	if (freq == sg_policy->cached_raw_freq && !sg_policy->need_freq_update)
		return sg_policy->next_freq;

	sg_policy->need_freq_update = false;
	sg_policy->cached_raw_freq = freq;
	return cpufreq_driver_resolve_freq(policy, freq);
}

static inline bool use_pelt(void)
{
#ifdef CONFIG_SCHED_WALT
	return (!sysctl_sched_use_walt_cpu_util || walt_disabled);
#else
	return true;
#endif
}

static void algov_get_util(unsigned long *util, unsigned long *max, u64 time, int cpu)
{
	struct rq *rq = cpu_rq(cpu);
	unsigned long max_cap, rt;
	s64 delta;

	max_cap = arch_scale_cpu_capacity(NULL, cpu);

	sched_avg_update(rq);
	delta = time - rq->age_stamp;
	if (unlikely(delta < 0))
		delta = 0;
	rt = div64_u64(rq->rt_avg, sched_avg_period() + delta);
	rt = (rt * max_cap) >> SCHED_CAPACITY_SHIFT;

	*util = boosted_cpu_util(cpu);
	if (use_pelt())
		*util = *util + rt;

	*util = min(*util, max_cap);
	*max = max_cap;
}

static void algov_set_iowait_boost(struct algov_cpu *sg_cpu, u64 time)
{
	struct algov_policy *sg_policy = sg_cpu->sg_policy;

	if (!sg_policy->tunables->iowait_boost_enable)
		return;

	/* Clear iowait_boost if the CPU apprears to have been idle. */
	if (sg_cpu->iowait_boost) {
		s64 delta_ns = time - sg_cpu->last_update;

		if (delta_ns > TICK_NSEC) {
			sg_cpu->iowait_boost = 0;
			sg_cpu->iowait_boost_pending = false;
		}
	}

	if (sg_cpu->flags & SCHED_CPUFREQ_IOWAIT) {
		if (sg_cpu->iowait_boost_pending)
			return;

		sg_cpu->iowait_boost_pending = true;

		if (sg_cpu->iowait_boost) {
			sg_cpu->iowait_boost <<= 1;
			if (sg_cpu->iowait_boost > sg_cpu->iowait_boost_max)
				sg_cpu->iowait_boost = sg_cpu->iowait_boost_max;
		} else {
			sg_cpu->iowait_boost = sg_cpu->sg_policy->policy->min;
		}
	}
}

static void algov_iowait_boost(struct algov_cpu *sg_cpu, unsigned long *util,
			       unsigned long *max)
{
	unsigned int boost_util, boost_max;

	if (!sg_cpu->iowait_boost)
		return;

	if (sg_cpu->iowait_boost_pending) {
		sg_cpu->iowait_boost_pending = false;
	} else {
		sg_cpu->iowait_boost >>= 1;
		if (sg_cpu->iowait_boost < sg_cpu->sg_policy->policy->min) {
			sg_cpu->iowait_boost = 0;
			return;
		}
	}

	boost_util = sg_cpu->iowait_boost;
	boost_max = sg_cpu->iowait_boost_max;

	if (*util * boost_max < *max * boost_util) {
		*util = boost_util;
		*max = boost_max;
	}
}

static void algov_update_single(struct update_util_data *hook, u64 time,
				unsigned int flags)
{
	struct algov_cpu *sg_cpu = container_of(hook, struct algov_cpu, update_util);
	struct algov_policy *sg_policy = sg_cpu->sg_policy;
	struct cpufreq_policy *policy = sg_policy->policy;
	unsigned long util, max;
	unsigned int next_f;
	bool busy;

	algov_set_iowait_boost(sg_cpu, time);
	sg_cpu->last_update = time;

	/*
	 * For slow-switch systems, single policy requests can't run at the
	 * moment if update is in progress, unless we acquire update_lock.
	 */
	if (sg_policy->work_in_progress)
		return;

	if (!algov_should_update_freq(sg_policy, time))
		return;

	busy = use_pelt() && algov_cpu_is_busy(sg_cpu);
	algov_cpu_is_busy_update(sg_cpu, util);

	if (flags & SCHED_CPUFREQ_DL) {
		next_f = policy->cpuinfo.max_freq;
	} else {
		algov_get_util(&util, &max, time, sg_cpu->cpu);
		algov_iowait_boost(sg_cpu, &util, &max);
		next_f = get_next_freq(sg_policy, util, max, busy);
		/*
		 * Do not reduce the frequency if the CPU has not been idle
		 * recently, as the reduction is likely to be premature then.
		 */
		if (busy && next_f < sg_policy->next_freq &&
		    sg_policy->next_freq != UINT_MAX) {
			next_f = sg_policy->next_freq;

			/* Reset cached freq as next_freq has changed */
			sg_policy->cached_raw_freq = 0;
		}
	}

	/*
	 * This code runs under rq->lock for the target CPU, so it won't run
	 * concurrently on two different CPUs for the same target and it is not
	 * necessary to acquire the lock in the fast switch case.
	 */
	if (sg_policy->policy->fast_switch_enabled) {
		algov_fast_switch(sg_policy, time, next_f);
	} else {
		raw_spin_lock(&sg_policy->update_lock);
		algov_deferred_update(sg_policy, time, next_f);
		raw_spin_unlock(&sg_policy->update_lock);
	}
}

static unsigned int algov_next_freq_shared(struct algov_cpu *sg_cpu, u64 time)
{
	struct algov_policy *sg_policy = sg_cpu->sg_policy;
	struct cpufreq_policy *policy = sg_policy->policy;
	unsigned long util = 0, max = 1;
	unsigned int j;
	unsigned long sg_cpu_util = 0;
	bool busy = false;

	for_each_cpu(j, policy->cpus) {
		struct algov_cpu *j_sg_cpu = &per_cpu(algov_cpu, j);
		unsigned long j_util, j_max;
		s64 delta_ns;

		/*
		 * If the CPU utilization was last updated before the previous
		 * frequency update and the time elapsed between the last update
		 * of the CPU utilization and the last frequency update is long
		 * enough, don't take the CPU into account as it probably is
		 * idle now (and clear iowait_boost for it).
		 */
		delta_ns = time - j_sg_cpu->last_update;
		if (delta_ns > stale_ns) {
			j_sg_cpu->iowait_boost = 0;
			j_sg_cpu->iowait_boost_pending = false;
			continue;
		}
		if (j_sg_cpu->flags & SCHED_CPUFREQ_DL)
			return policy->cpuinfo.max_freq;

		j_util = j_sg_cpu->util;
		if (j_sg_cpu == sg_cpu)
			sg_cpu_util = j_util;
		j_max = j_sg_cpu->max;
		busy |= algov_cpu_is_busy(j_sg_cpu);
		if (j_util * max > j_max * util) {
			util = j_util;
			max = j_max;
		}

		algov_iowait_boost(j_sg_cpu, &util, &max);
	}

	/*
	 * Only update the business status if we are looking at the CPU for
	 * which a utilization change triggered a call to get_next_freq(). This
	 * way, we don't affect the "busy" status of CPUs that don't have any
	 * change in utilization.
	 */
	algov_cpu_is_busy_update(sg_cpu, sg_cpu_util);

	return get_next_freq(sg_policy, util, max, busy);
}

static void algov_update_shared(struct update_util_data *hook, u64 time,
				unsigned int flags)
{
	struct algov_cpu *sg_cpu = container_of(hook, struct algov_cpu, update_util);
	struct algov_policy *sg_policy = sg_cpu->sg_policy;
	unsigned long util, max;
	unsigned int next_f;

	algov_get_util(&util, &max, time, sg_cpu->cpu);

	raw_spin_lock(&sg_policy->update_lock);

	sg_cpu->util = util;
	sg_cpu->max = max;
	sg_cpu->flags = flags;

	algov_set_iowait_boost(sg_cpu, time);
	sg_cpu->last_update = time;

	if (algov_should_update_freq(sg_policy, time)) {
		if (flags & SCHED_CPUFREQ_DL)
			next_f = sg_policy->policy->cpuinfo.max_freq;
		else
			next_f = algov_next_freq_shared(sg_cpu, time);

		if (sg_policy->policy->fast_switch_enabled)
			algov_fast_switch(sg_policy, time, next_f);
		else
			algov_deferred_update(sg_policy, time, next_f);
	}

	raw_spin_unlock(&sg_policy->update_lock);
}

static void algov_work(struct kthread_work *work)
{
	struct algov_policy *sg_policy = container_of(work, struct algov_policy, work);
	unsigned int freq;
	unsigned long flags;

	/*
	 * Hold sg_policy->update_lock shortly to handle the case where:
	 * incase sg_policy->next_freq is read here, and then updated by
	 * algov_deferred_update() just before work_in_progress is set to false
	 * here, we may miss queueing the new update.
	 *
	 * Note: If a work was queued after the update_lock is released,
	 * algov_work() will just be called again by kthread_work code; and the
	 * request will be proceed before the algov thread sleeps.
	 */
	raw_spin_lock_irqsave(&sg_policy->update_lock, flags);
	freq = sg_policy->next_freq;
	sg_policy->work_in_progress = false;
	raw_spin_unlock_irqrestore(&sg_policy->update_lock, flags);

	mutex_lock(&sg_policy->work_lock);
	__cpufreq_driver_target(sg_policy->policy, freq, CPUFREQ_RELATION_L);
	mutex_unlock(&sg_policy->work_lock);
}

static void algov_irq_work(struct irq_work *irq_work)
{
	struct algov_policy *sg_policy;

	sg_policy = container_of(irq_work, struct algov_policy, irq_work);

	/*
	 * For RT and deadline tasks, the schedalessa governor shoots the
	 * frequency to maximum. Special care must be taken to ensure that this
	 * kthread doesn't result in the same behavior.
	 *
	 * This is (mostly) guaranteed by the work_in_progress flag. The flag is
	 * updated only at the end of the algov_work() function and before that
	 * the schedalessa governor rejects all other frequency scaling requests.
	 *
	 * There is a very rare case though, where the RT thread yields right
	 * after the work_in_progress flag is cleared. The effects of that are
	 * neglected for now.
	 */
	queue_kthread_work(&sg_policy->worker, &sg_policy->work);
}

/************************** sysfs interface ************************/

static struct algov_tunables *global_tunables;
static DEFINE_MUTEX(global_tunables_lock);

static inline struct algov_tunables *to_algov_tunables(struct gov_attr_set *attr_set)
{
	return container_of(attr_set, struct algov_tunables, attr_set);
}

static DEFINE_MUTEX(min_rate_lock);

static void update_min_rate_limit_us(struct algov_policy *sg_policy)
{
	mutex_lock(&min_rate_lock);
	sg_policy->min_rate_limit_ns = min(sg_policy->up_rate_delay_ns,
					   sg_policy->down_rate_delay_ns);
	mutex_unlock(&min_rate_lock);
}

static ssize_t up_rate_limit_us_show(struct gov_attr_set *attr_set, char *buf)
{
	struct algov_tunables *tunables = to_algov_tunables(attr_set);

        return scnprintf(buf, PAGE_SIZE, "%u\n", tunables->up_rate_limit_us);
}

static ssize_t down_rate_limit_us_show(struct gov_attr_set *attr_set, char *buf)
{
	struct algov_tunables *tunables = to_algov_tunables(attr_set);

	return scnprintf(buf, PAGE_SIZE, "%u\n", tunables->down_rate_limit_us);
}

static ssize_t up_rate_limit_us_store(struct gov_attr_set *attr_set,
				      const char *buf, size_t count)
{
	struct algov_tunables *tunables = to_algov_tunables(attr_set);
	struct algov_policy *sg_policy;
	unsigned int rate_limit_us;

	/* Don't let userspace change this */
	return count;

	if (kstrtouint(buf, 10, &rate_limit_us))
		return -EINVAL;

	tunables->up_rate_limit_us = rate_limit_us;

	list_for_each_entry(sg_policy, &attr_set->policy_list, tunables_hook) {
		sg_policy->up_rate_delay_ns = rate_limit_us * NSEC_PER_USEC;
		update_min_rate_limit_us(sg_policy);
	}

	return count;
}

static ssize_t down_rate_limit_us_store(struct gov_attr_set *attr_set,
					const char *buf, size_t count)
{
	struct algov_tunables *tunables = to_algov_tunables(attr_set);
	struct algov_policy *sg_policy;
	unsigned int rate_limit_us;

	/* Don't let userspace change this */
	return count;

	if (kstrtouint(buf, 10, &rate_limit_us))
		return -EINVAL;

	tunables->down_rate_limit_us = rate_limit_us;

	list_for_each_entry(sg_policy, &attr_set->policy_list, tunables_hook) {
		sg_policy->down_rate_delay_ns = rate_limit_us * NSEC_PER_USEC;
		update_min_rate_limit_us(sg_policy);
	}

	return count;
}

static struct governor_attr up_rate_limit_us = __ATTR_RW(up_rate_limit_us);
static struct governor_attr down_rate_limit_us = __ATTR_RW(down_rate_limit_us);

static ssize_t iowait_boost_enable_show(struct gov_attr_set *attr_set,
					char *buf)
{
	struct algov_tunables *tunables = to_algov_tunables(attr_set);

	return scnprintf(buf, PAGE_SIZE, "%u\n", tunables->iowait_boost_enable);
}

static ssize_t iowait_boost_enable_store(struct gov_attr_set *attr_set,
					 const char *buf, size_t count)
{
	struct algov_tunables *tunables = to_algov_tunables(attr_set);
	bool enable;

	if (kstrtobool(buf, &enable))
		return -EINVAL;

	tunables->iowait_boost_enable = enable;

	return count;
}

static struct governor_attr iowait_boost_enable = __ATTR_RW(iowait_boost_enable);

static struct attribute *algov_attributes[] = {
	&up_rate_limit_us.attr,
	&down_rate_limit_us.attr,
	&iowait_boost_enable.attr,
	NULL
};

static struct kobj_type algov_tunables_ktype = {
	.default_attrs = algov_attributes,
	.sysfs_ops = &governor_sysfs_ops,
};

/********************** cpufreq governor interface *********************/
#ifndef CONFIG_CPU_FREQ_DEFAULT_GOV_SCHEDALESSA
static
#endif
struct cpufreq_governor cpufreq_gov_schedalessa;

static struct algov_policy *algov_policy_alloc(struct cpufreq_policy *policy)
{
	struct algov_policy *sg_policy;

	sg_policy = kzalloc(sizeof(*sg_policy), GFP_KERNEL);
	if (!sg_policy)
		return NULL;

	sg_policy->policy = policy;
	raw_spin_lock_init(&sg_policy->update_lock);
	return sg_policy;
}

static void algov_policy_free(struct algov_policy *sg_policy)
{
	kfree(sg_policy);
}

static int algov_kthread_create(struct algov_policy *sg_policy)
{
	struct task_struct *thread;
	struct sched_param param = { .sched_priority = MAX_USER_RT_PRIO / 2 };
	struct cpufreq_policy *policy = sg_policy->policy;
	int ret;

	/* kthread only required for slow path */
	if (policy->fast_switch_enabled)
		return 0;

	init_kthread_work(&sg_policy->work, algov_work);
	init_kthread_worker(&sg_policy->worker);
	thread = kthread_create(kthread_worker_fn, &sg_policy->worker,
				"algov:%d",
				cpumask_first(policy->related_cpus));
	if (IS_ERR(thread)) {
		pr_err("failed to create algov thread: %ld\n", PTR_ERR(thread));
		return PTR_ERR(thread);
	}

	ret = sched_setscheduler_nocheck(thread, SCHED_FIFO, &param);
	if (ret) {
		kthread_stop(thread);
		pr_warn("%s: failed to set SCHED_FIFO\n", __func__);
		return ret;
	}

	sg_policy->thread = thread;
	kthread_bind_mask(thread, policy->related_cpus);
	init_irq_work(&sg_policy->irq_work, algov_irq_work);
	mutex_init(&sg_policy->work_lock);

	wake_up_process(thread);

	return 0;
}

static void algov_kthread_stop(struct algov_policy *sg_policy)
{
	/* kthread only required for slow path */
	if (sg_policy->policy->fast_switch_enabled)
		return;

	flush_kthread_worker(&sg_policy->worker);
	kthread_stop(sg_policy->thread);
	mutex_destroy(&sg_policy->work_lock);
}

static struct algov_tunables *algov_tunables_alloc(struct algov_policy *sg_policy)
{
	struct algov_tunables *tunables;

	tunables = kzalloc(sizeof(*tunables), GFP_KERNEL);
	if (tunables) {
		gov_attr_set_init(&tunables->attr_set, &sg_policy->tunables_hook);
		if (!have_governor_per_policy())
			global_tunables = tunables;
	}
	return tunables;
}

static void algov_tunables_save(struct cpufreq_policy *policy,
		struct algov_tunables *tunables)
{
	int cpu;
	struct algov_tunables *cached = per_cpu(cached_tunables, policy->cpu);

	if (!have_governor_per_policy())
		return;

	if (!cached) {
		cached = kzalloc(sizeof(*tunables), GFP_KERNEL);
		if (!cached) {
			pr_warn("Couldn't allocate tunables for caching\n");
			return;
		}
		for_each_cpu(cpu, policy->related_cpus)
			per_cpu(cached_tunables, cpu) = cached;
	}

	cached->up_rate_limit_us = tunables->up_rate_limit_us;
	cached->down_rate_limit_us = tunables->down_rate_limit_us;
}

static void algov_tunables_free(struct algov_tunables *tunables)
{
	if (!have_governor_per_policy())
		global_tunables = NULL;

	kfree(tunables);
}

static void algov_tunables_restore(struct cpufreq_policy *policy)
{
	struct algov_policy *sg_policy = policy->governor_data;
	struct algov_tunables *tunables = sg_policy->tunables;
	struct algov_tunables *cached = per_cpu(cached_tunables, policy->cpu);

	if (!cached)
		return;

	tunables->up_rate_limit_us = cached->up_rate_limit_us;
	tunables->down_rate_limit_us = cached->down_rate_limit_us;
	sg_policy->up_rate_delay_ns =
		tunables->up_rate_limit_us * NSEC_PER_USEC;
	sg_policy->down_rate_delay_ns =
		tunables->down_rate_limit_us * NSEC_PER_USEC;
	sg_policy->min_rate_limit_ns = min(sg_policy->up_rate_delay_ns,
					   sg_policy->down_rate_delay_ns);
}

static int algov_init(struct cpufreq_policy *policy)
{
	struct algov_policy *sg_policy;
	struct algov_tunables *tunables;
	int ret = 0;

	/* State should be equivalent to EXIT */
	if (policy->governor_data)
		return -EBUSY;

	cpufreq_enable_fast_switch(policy);

	sg_policy = algov_policy_alloc(policy);
	if (!sg_policy) {
		ret = -ENOMEM;
		goto disable_fast_switch;
	}

	ret = algov_kthread_create(sg_policy);
	if (ret)
		goto free_sg_policy;

	mutex_lock(&global_tunables_lock);

	if (global_tunables) {
		if (WARN_ON(have_governor_per_policy())) {
			ret = -EINVAL;
			goto stop_kthread;
		}

		policy->governor_data = sg_policy;
		sg_policy->tunables = global_tunables;

		gov_attr_set_get(&global_tunables->attr_set, &sg_policy->tunables_hook);
		goto out;
	}

	tunables = algov_tunables_alloc(sg_policy);
	if (!tunables) {
		ret = -ENOMEM;
		goto stop_kthread;
	}

	if (policy->up_transition_delay_us && policy->down_transition_delay_us) {
		tunables->up_rate_limit_us = policy->up_transition_delay_us;
		tunables->down_rate_limit_us = policy->down_transition_delay_us;
	} else {
		unsigned int lat;

                tunables->up_rate_limit_us = UP_RATE_LIMIT_US;
                tunables->down_rate_limit_us = DOWN_RATE_LIMIT_US;
		lat = policy->cpuinfo.transition_latency / NSEC_PER_USEC;
		if (lat) {
                        tunables->up_rate_limit_us *= lat;
                        tunables->down_rate_limit_us *= lat;
                }
	}

	/* Hard-code some sane rate-limit values */
	tunables->up_rate_limit_us = algov_up_rate_limit;
	tunables->down_rate_limit_us = algov_down_rate_limit;

	tunables->iowait_boost_enable = false;

	policy->governor_data = sg_policy;
	sg_policy->tunables = tunables;
	stale_ns = walt_ravg_window + (walt_ravg_window >> 3);

	algov_tunables_restore(policy);

	ret = kobject_init_and_add(&tunables->attr_set.kobj, &algov_tunables_ktype,
				   get_governor_parent_kobj(policy), "%s",
				   cpufreq_gov_schedalessa.name);
	if (ret)
		goto fail;

out:
	mutex_unlock(&global_tunables_lock);
	return 0;

fail:
	kobject_put(&tunables->attr_set.kobj);
	policy->governor_data = NULL;
	algov_tunables_free(tunables);

stop_kthread:
	algov_kthread_stop(sg_policy);

free_sg_policy:
	mutex_unlock(&global_tunables_lock);

	algov_policy_free(sg_policy);

disable_fast_switch:
	cpufreq_disable_fast_switch(policy);

	pr_err("initialization failed (error %d)\n", ret);
	return ret;
}

static int algov_exit(struct cpufreq_policy *policy)
{
	struct algov_policy *sg_policy = policy->governor_data;
	struct algov_tunables *tunables = sg_policy->tunables;
	unsigned int count;

	cpufreq_disable_fast_switch(policy);

	mutex_lock(&global_tunables_lock);

	count = gov_attr_set_put(&tunables->attr_set, &sg_policy->tunables_hook);
	policy->governor_data = NULL;
	if (!count) {
		algov_tunables_save(policy, tunables);
		algov_tunables_free(tunables);
	}

	mutex_unlock(&global_tunables_lock);

	algov_kthread_stop(sg_policy);
	algov_policy_free(sg_policy);

	cpufreq_disable_fast_switch(policy);
	return 0;
}

static int algov_start(struct cpufreq_policy *policy)
{
	struct algov_policy *sg_policy = policy->governor_data;
	unsigned int cpu;

	sg_policy->up_rate_delay_ns =
		sg_policy->tunables->up_rate_limit_us * NSEC_PER_USEC;
	sg_policy->down_rate_delay_ns =
		sg_policy->tunables->down_rate_limit_us * NSEC_PER_USEC;
	update_min_rate_limit_us(sg_policy);
	sg_policy->last_freq_update_time = 0;
	sg_policy->next_freq = 0;
	sg_policy->work_in_progress = false;
	sg_policy->need_freq_update = false;
	sg_policy->cached_raw_freq = 0;

	for_each_cpu(cpu, policy->cpus) {
		struct algov_cpu *sg_cpu = &per_cpu(algov_cpu, cpu);

		memset(sg_cpu, 0, sizeof(*sg_cpu));
		sg_cpu->sg_policy = sg_policy;
		sg_cpu->cpu = cpu;
		sg_cpu->flags = SCHED_CPUFREQ_DL;
		sg_cpu->iowait_boost_max = policy->cpuinfo.max_freq;
	}

	for_each_cpu(cpu, policy->cpus) {
		struct algov_cpu *sg_cpu = &per_cpu(algov_cpu, cpu);

		cpufreq_add_update_util_hook(cpu, &sg_cpu->update_util,
					     policy_is_shared(policy) ?
							algov_update_shared :
							algov_update_single);

	algov_policy_attach_pd(sg_policy);

	}
	return 0;
}

static int algov_stop(struct cpufreq_policy *policy)
{
	struct algov_policy *sg_policy = policy->governor_data;
	unsigned int cpu;

	for_each_cpu(cpu, policy->cpus)
		cpufreq_remove_update_util_hook(cpu);

	synchronize_sched();

	if (!policy->fast_switch_enabled) {
		irq_work_sync(&sg_policy->irq_work);
		kthread_cancel_work_sync(&sg_policy->work);
	}
	return 0;
}

static int algov_limits(struct cpufreq_policy *policy)
{
	struct algov_policy *sg_policy = policy->governor_data;

	if (!policy->fast_switch_enabled) {
		mutex_lock(&sg_policy->work_lock);
		cpufreq_policy_apply_limits(policy);
		mutex_unlock(&sg_policy->work_lock);
	}

	sg_policy->need_freq_update = true;

	return 0;
}

static int cpufreq_schedalessa_cb(struct cpufreq_policy *policy,
				unsigned int event)
{
	switch(event) {
	case CPUFREQ_GOV_POLICY_INIT:
		return algov_init(policy);
	case CPUFREQ_GOV_POLICY_EXIT:
		return algov_exit(policy);
	case CPUFREQ_GOV_START:
		return algov_start(policy);
	case CPUFREQ_GOV_STOP:
		return algov_stop(policy);
	case CPUFREQ_GOV_LIMITS:
		return algov_limits(policy);
	default:
		BUG();
	}
}

#ifndef CONFIG_CPU_FREQ_DEFAULT_GOV_SCHEDALESSA
static
#endif
struct cpufreq_governor cpufreq_gov_schedalessa = {
	.name = "schedalessa",
	.governor = cpufreq_schedalessa_cb,
	.owner = THIS_MODULE,
};

static int __init algov_register(void)
{
	return cpufreq_register_governor(&cpufreq_gov_schedalessa);
}
fs_initcall(algov_register);
