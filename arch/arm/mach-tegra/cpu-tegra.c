/*
 * arch/arm/mach-tegra/cpu-tegra.c
 *
 * Copyright (C) 2010 Google, Inc.
 *
 * Author:
 *	Colin Cross <ccross@google.com>
 *	Based on arch/arm/plat-omap/cpu-omap.c, (C) 2005 Nokia Corporation
 *
 * Copyright (C) 2010-2012 NVIDIA CORPORATION. All rights reserved.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/sched.h>
#include <linux/cpufreq.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/err.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/suspend.h>
#include <linux/debugfs.h>
#include <linux/cpu.h>
#include <linux/pm_qos_params.h>
#include <linux/earlysuspend.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/cpu_debug.h>

#include <asm/system.h>

#include <mach/clk.h>
#include <mach/edp.h>
#include "board.h"
#include "clock.h"
#include "cpu-tegra.h"
#include "dvfs.h"
#include "pm.h"
#include "tegra_pmqos.h"

#define CPU_FREQ_DEBUG 0

#ifdef CONFIG_TEGRA_MPDECISION
/* mpdecision notifier */
extern int mpdecision_gmode_notifier(void);
#endif

// make these vals modifiable in realtime
unsigned int T3_CPU_MIN_FREQ = DEF_T3_CPU_MIN_FREQ;

/* EliteKernel Extreme Powersaving*/
unsigned int tegra_pmqos_powersave = 0;
unsigned int tegra_pmqos_audio = 0;

#ifdef CONFIG_TEGRA_CPUQUIET
extern void tegra_cpuquiet_force_gmode(void);
#endif

unsigned int tegra_pmqos_boost_freq = T3_CPU_FREQ_BOOST;
struct work_struct ril_suspend_resume_work;

/* frequency cap used during suspend (screen off)*/
static unsigned int suspend_cap_freq = SUSPEND_CPU_FREQ_MAX;
static unsigned int suspend_cap_cpu_num = SUSPEND_CPU_NUM_MAX;
/* disable speed changes during early suspend and resume handlers */
static bool in_earlysuspend = false;

// maxwen: assumes 4 cores!
unsigned int tegra_pmqos_cpu_freq_limits[CONFIG_NR_CPUS] = {0, 0, 0, 0};
unsigned int tegra_pmqos_cpu_freq_limits_min[CONFIG_NR_CPUS] = {0, 0, 0, 0};

/* tegra throttling and edp governors require frequencies in the table
   to be in ascending order */
static struct cpufreq_frequency_table *freq_table;

static struct clk *cpu_clk;
static struct clk *cpu_g_clk;
static struct clk *emc_clk;

static unsigned long policy_max_speed[CONFIG_NR_CPUS];
static unsigned long target_cpu_speed[CONFIG_NR_CPUS];
static DEFINE_MUTEX(tegra_cpu_lock);
static bool is_suspended;
static int suspend_index;

#ifdef CONFIG_TEGRA3_VARIANT_CPU_OVERCLOCK
int enable_oc = 0;
#endif

/* maximum cpu freq */
unsigned int tegra_cpu_freq_max(unsigned int cpu)
{
#ifdef CONFIG_TEGRA3_VARIANT_CPU_OVERCLOCK
	if (enable_oc)
		return T3_CPU_FREQ_MAX_OC;
#endif	
	if (cpu==0)
		return T3_CPU_FREQ_MAX_0;
	return T3_CPU_FREQ_MAX;
}

// maxwen: see tegra_cpu_init
// values can be changed in sysfs interface of cpufreq
// for scaling_max_freq_limit
static inline unsigned int get_cpu_freq_limit(unsigned int cpu)
{
	BUG_ON(cpu > 3);
	if(tegra_pmqos_cpu_freq_limits[cpu]!=0){
		return tegra_pmqos_cpu_freq_limits[cpu];
	}
	return tegra_cpu_freq_max(cpu);
}

static inline unsigned int get_cpu_freq_limit_min(unsigned int cpu)
{
	BUG_ON(cpu > 3);
	if(tegra_pmqos_cpu_freq_limits_min[cpu]!=0){
		return tegra_pmqos_cpu_freq_limits_min[cpu];
	}
	return T3_CPU_MIN_FREQ;
}

unsigned int tegra_get_suspend_boost_freq(void)
{
	return min((unsigned int)T3_CPU_FREQ_BOOST, get_cpu_freq_limit(0));
}

static bool force_policy_max;

static int force_policy_max_set(const char *arg, const struct kernel_param *kp)
{
	int ret;
	bool old_policy = force_policy_max;

	mutex_lock(&tegra_cpu_lock);

	ret = param_set_bool(arg, kp);
	if ((ret == 0) && (old_policy != force_policy_max))
		tegra_cpu_set_speed_cap(NULL);

	mutex_unlock(&tegra_cpu_lock);
	return ret;
}

static int force_policy_max_get(char *buffer, const struct kernel_param *kp)
{
	return param_get_bool(buffer, kp);
}

static struct kernel_param_ops policy_ops = {
	.set = force_policy_max_set,
	.get = force_policy_max_get,
};
module_param_cb(force_policy_max, &policy_ops, &force_policy_max, 0644);

static int suspend_cap_freq_set(const char *arg, const struct kernel_param *kp)
{
	int tmp;
	
	if (1 != sscanf(arg, "%d", &tmp))
		return -EINVAL;

    suspend_cap_freq = tmp;
    pr_info("suspend_cap_freq %d\n", suspend_cap_freq);
	return 0;
}

static int suspend_cap_freq_get(char *buffer, const struct kernel_param *kp)
{
	return param_get_uint(buffer, kp);
}

static struct kernel_param_ops suspend_cap_freq_ops = {
	.set = suspend_cap_freq_set,
	.get = suspend_cap_freq_get,
};
module_param_cb(suspend_cap_freq, &suspend_cap_freq_ops, &suspend_cap_freq, 0644);

static int suspend_cap_cpu_num_set(const char *arg, const struct kernel_param *kp)
{
	int tmp;
	
	if (1 != sscanf(arg, "%d", &tmp))
		return -EINVAL;

	if (tmp == PM_QOS_MAX_ONLINE_CPUS_DEFAULT_VALUE)
		tmp = CONFIG_NR_CPUS;

	if (tmp < 1 || tmp > CONFIG_NR_CPUS)
		return -EINVAL;
			
    suspend_cap_cpu_num = tmp;
    pr_info("suspend_cap_cpu_num %d\n", suspend_cap_cpu_num);
	return 0;
}

static int suspend_cap_cpu_num_get(char *buffer, const struct kernel_param *kp)
{
	return param_get_uint(buffer, kp);
}

static struct kernel_param_ops suspend_cap_cpu_num_ops = {
	.set = suspend_cap_cpu_num_set,
	.get = suspend_cap_cpu_num_get,
};
module_param_cb(suspend_cap_cpu_num, &suspend_cap_cpu_num_ops, &suspend_cap_cpu_num, 0644);

static unsigned int cpu_user_cap = 0;

static inline void _cpu_user_cap_set_locked(void)
{
#ifndef CONFIG_TEGRA_CPU_CAP_EXACT_FREQ
	if (cpu_user_cap != 0) {
		int i;
		for (i = 0; freq_table[i].frequency != CPUFREQ_TABLE_END; i++) {
			if (freq_table[i].frequency > cpu_user_cap)
				break;
		}
		i = (i == 0) ? 0 : i - 1;
		cpu_user_cap = freq_table[i].frequency;
	}
#endif
	tegra_cpu_set_speed_cap(NULL);
}

void tegra_cpu_user_cap_set(unsigned int speed_khz)
{
	mutex_lock(&tegra_cpu_lock);

	cpu_user_cap = speed_khz;
	_cpu_user_cap_set_locked();

	mutex_unlock(&tegra_cpu_lock);
}

static int cpu_user_cap_set(const char *arg, const struct kernel_param *kp)
{
	int ret;

	mutex_lock(&tegra_cpu_lock);

	ret = param_set_uint(arg, kp);
	if (ret == 0)
		_cpu_user_cap_set_locked();

	mutex_unlock(&tegra_cpu_lock);
	return ret;
}

static int cpu_user_cap_get(char *buffer, const struct kernel_param *kp)
{
	return param_get_uint(buffer, kp);
}

static struct kernel_param_ops cpu_user_cap_ops = {
	.set = cpu_user_cap_set,
	.get = cpu_user_cap_get,
};
module_param_cb(cpu_user_cap, &cpu_user_cap_ops, &cpu_user_cap, 0644);

static unsigned int user_cap_speed(unsigned int requested_speed)
{
	if ((cpu_user_cap) && (requested_speed > cpu_user_cap))
		return cpu_user_cap;
	return requested_speed;
}

static unsigned int pmqos_cap_speed(unsigned int requested_speed)
{
	unsigned int ret = requested_speed;
	if(pm_qos_request(PM_QOS_CPU_FREQ_MIN) > requested_speed)
	{
		ret = pm_qos_request(PM_QOS_CPU_FREQ_MIN);
	}
	if(pm_qos_request(PM_QOS_CPU_FREQ_MAX) < requested_speed)
	{
		ret =  pm_qos_request(PM_QOS_CPU_FREQ_MAX);
	}
	return ret;
}

static int ril_boost = 0;

static int ril_boost_set(const char *arg, const struct kernel_param *kp)
{
	return schedule_work(&ril_suspend_resume_work);
}

static int ril_boost_get(char *buffer, const struct kernel_param *kp)
{
	return param_get_uint(buffer, kp);
}

static struct kernel_param_ops ril_boost_ops = {
	.set = ril_boost_set,
	.get = ril_boost_get,
};

module_param_cb(ril_boost, &ril_boost_ops, &ril_boost, 0644);

static int perf_early_suspend = 0;

static int perf_early_suspend_set(const char *arg, const struct kernel_param *kp)
{
	pr_info("perf_early_suspend not supported\n");

	return 0;
}

static int perf_early_suspend_get(char *buffer, const struct kernel_param *kp)
{
	return 0;
}
static struct kernel_param_ops perf_early_suspend_ops = {
	.set = perf_early_suspend_set,
	.get = perf_early_suspend_get,
};

module_param_cb(perf_early_suspend, &perf_early_suspend_ops, &perf_early_suspend, 0644);

#ifdef CONFIG_TEGRA3_VARIANT_CPU_OVERCLOCK
static int enable_oc_set(const char *arg, const struct kernel_param *kp)
{
    int ret = param_set_int(arg, kp);
	if (ret)
		return ret;
    pr_info("enable_oc %d\n", enable_oc);
	return 0;
}

static int enable_oc_get(char *buffer, const struct kernel_param *kp)
{
	return param_get_uint(buffer, kp);
}

static struct kernel_param_ops enable_oc_ops = {
	.set = enable_oc_set,
	.get = enable_oc_get,
};

module_param_cb(enable_oc, &enable_oc_ops, &enable_oc, 0644);
#endif

/* disable edp limitations */
static unsigned int no_edp_limit = 0;
module_param(no_edp_limit, uint, 0644);

/* disable thermal throttling limitations */
unsigned int no_thermal_throttle_limit = 0;
module_param(no_thermal_throttle_limit, uint, 0644);

#ifdef CONFIG_TEGRA_THERMAL_THROTTLE

static ssize_t show_throttle(struct cpufreq_policy *policy, char *buf)
{
	return sprintf(buf, "%u\n", tegra_is_throttling());
}

cpufreq_freq_attr_ro(throttle);
#endif /* CONFIG_TEGRA_THERMAL_THROTTLE */

#ifdef CONFIG_TEGRA_EDP_LIMITS

static const struct tegra_edp_limits *cpu_edp_limits;
static int cpu_edp_limits_size;

static const unsigned int *system_edp_limits;
static bool system_edp_alarm;

static int edp_thermal_index;
static cpumask_t edp_cpumask;
static unsigned int edp_limit;

unsigned int tegra_get_edp_limit(void)
{
	return edp_limit;
}

static unsigned int edp_predict_limit(unsigned int cpus)
{
	unsigned int limit = 0;

	BUG_ON(cpus == 0);
	if (cpu_edp_limits) {
		BUG_ON(edp_thermal_index >= cpu_edp_limits_size);
#ifdef CONFIG_TEGRA3_VARIANT_CPU_OVERCLOCK
        if(enable_oc)
		    limit = cpu_edp_limits[edp_thermal_index].freq_limits_oc[cpus - 1];
        else
		    limit = cpu_edp_limits[edp_thermal_index].freq_limits[cpus - 1];
#else
		limit = cpu_edp_limits[edp_thermal_index].freq_limits[cpus - 1];
#endif
	}
	if (system_edp_limits && system_edp_alarm)
		limit = min(limit, system_edp_limits[cpus - 1]);

	return limit;
}

static void edp_update_limit(void)
{
	unsigned int limit = edp_predict_limit(cpumask_weight(&edp_cpumask));

#ifdef CONFIG_TEGRA_EDP_EXACT_FREQ
	edp_limit = limit;
#else
	unsigned int i;
	for (i = 0; freq_table[i].frequency != CPUFREQ_TABLE_END; i++) {
		if (freq_table[i].frequency > limit) {
			break;
		}
	}
	BUG_ON(i == 0);	/* min freq above the limit or table empty */
	edp_limit = freq_table[i-1].frequency;
#endif
}

static unsigned int edp_governor_speed(unsigned int requested_speed)
{
    /* ignore EDP (regulator max output) limitation */
    if (unlikely(no_edp_limit))
        return requested_speed;

	if ((!edp_limit) || (requested_speed <= edp_limit))
		return requested_speed;
	else
		return edp_limit;
}

int tegra_edp_update_thermal_zone(int temperature)
{
	int i;
	int ret = 0;
	int nlimits = cpu_edp_limits_size;
	int index;

	if (!cpu_edp_limits)
		return -EINVAL;

#if CPU_FREQ_DEBUG
	pr_info("tegra_edp_update_thermal_zone\n");
#endif

	index = nlimits - 1;

	if (temperature < cpu_edp_limits[0].temperature) {
		index = 0;
	} else {
		for (i = 0; i < (nlimits - 1); i++) {
			if (temperature >= cpu_edp_limits[i].temperature &&
			   temperature < cpu_edp_limits[i + 1].temperature) {
				index = i + 1;
				break;
			}
		}
	}

	mutex_lock(&tegra_cpu_lock);
	edp_thermal_index = index;

	/* Update cpu rate if cpufreq (at least on cpu0) is already started;
	   alter cpu dvfs table for this thermal zone if necessary */
	tegra_cpu_dvfs_alter(edp_thermal_index, &edp_cpumask, true, 0);
	if (target_cpu_speed[0]) {
		edp_update_limit();
		tegra_cpu_set_speed_cap(NULL);
	}
	tegra_cpu_dvfs_alter(edp_thermal_index, &edp_cpumask, false, 0);
	mutex_unlock(&tegra_cpu_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(tegra_edp_update_thermal_zone);

int tegra_system_edp_alarm(bool alarm)
{
	int ret = -ENODEV;

#if CPU_FREQ_DEBUG
	pr_info("tegra_system_edp_alarm\n");
#endif

	mutex_lock(&tegra_cpu_lock);
	system_edp_alarm = alarm;

	/* Update cpu rate if cpufreq (at least on cpu0) is already started
	   and cancel emergency throttling after either edp limit is applied
	   or alarm is canceled */
	if (target_cpu_speed[0]) {
		edp_update_limit();
		ret = tegra_cpu_set_speed_cap(NULL);
	}
	if (!ret || !alarm)
		tegra_edp_throttle_cpu_now(0);

	mutex_unlock(&tegra_cpu_lock);

	return ret;
}

bool tegra_cpu_edp_favor_up(unsigned int n, int mp_overhead)
{
	unsigned int current_limit, next_limit;

	if (n == 0)
		return true;

	if (n >= ARRAY_SIZE(cpu_edp_limits->freq_limits))
		return false;

	current_limit = edp_predict_limit(n);
	next_limit = edp_predict_limit(n + 1);

	return ((next_limit * (n + 1)) >=
		(current_limit * n * (100 + mp_overhead) / 100));
}

bool tegra_cpu_edp_favor_down(unsigned int n, int mp_overhead)
{
	unsigned int current_limit, next_limit;

	if (n <= 1)
		return false;

	if (n > ARRAY_SIZE(cpu_edp_limits->freq_limits))
		return true;

	current_limit = edp_predict_limit(n);
	next_limit = edp_predict_limit(n - 1);

	return ((next_limit * (n - 1) * (100 + mp_overhead) / 100)) >
		(current_limit * n);
}

static int tegra_cpu_edp_notify(
	struct notifier_block *nb, unsigned long event, void *hcpu)
{
	int ret = 0;
	unsigned int cpu_speed, new_speed;
	int cpu = (long)hcpu;

	switch (event) {
	case CPU_UP_PREPARE:
#if CPU_FREQ_DEBUG
		pr_info("tegra_cpu_edp_notify CPU_UP_PREPARE\n");
#endif		
		mutex_lock(&tegra_cpu_lock);
		cpu_set(cpu, edp_cpumask);
		edp_update_limit();

		cpu_speed = tegra_getspeed(0);
		new_speed = edp_governor_speed(cpu_speed);
		#if CPU_FREQ_DEBUG
		pr_info("DBG_NS(01): %d kHz\n", new_speed);
#endif
		if (new_speed < cpu_speed) {
			ret = tegra_cpu_set_speed_cap(NULL);
			pr_info("tegra_cpu_edp_notify:%s cpu:%d force EDP limit %u kHz"
				"\n", ret ? " failed to " : " ", cpu, new_speed);
		}
		if (!ret)
			ret = tegra_cpu_dvfs_alter(
				edp_thermal_index, &edp_cpumask, false, event);
		if (ret) {
			cpu_clear(cpu, edp_cpumask);
			edp_update_limit();
		}
		mutex_unlock(&tegra_cpu_lock);
		break;
	case CPU_DEAD:
#if CPU_FREQ_DEBUG
		pr_info("tegra_cpu_edp_notify CPU_UP_PREPARE\n");
#endif

		mutex_lock(&tegra_cpu_lock);
		cpu_clear(cpu, edp_cpumask);
		tegra_cpu_dvfs_alter(
			edp_thermal_index, &edp_cpumask, true, event);
		edp_update_limit();
		tegra_cpu_set_speed_cap(NULL);
		mutex_unlock(&tegra_cpu_lock);
		break;
	}
	return notifier_from_errno(ret);
}

static struct notifier_block tegra_cpu_edp_notifier = {
	.notifier_call = tegra_cpu_edp_notify,
};

static void tegra_cpu_edp_init(bool resume)
{
	tegra_get_system_edp_limits(&system_edp_limits);
	tegra_get_cpu_edp_limits(&cpu_edp_limits, &cpu_edp_limits_size);

	if (!(cpu_edp_limits || system_edp_limits)) {
		if (!resume)
			pr_info("tegra_cpu_edp_init: no EDP table is provided\n");
		return;
	}

	/* FIXME: use the highest temperature limits if sensor is not on-line?
	 * If thermal zone is not set yet by the sensor, edp_thermal_index = 0.
	 * Boot frequency allowed SoC to get here, should work till sensor is
	 * initialized.
	 */
	edp_cpumask = *cpu_online_mask;
	edp_update_limit();

	if (!resume) {
		register_hotcpu_notifier(&tegra_cpu_edp_notifier);
		pr_info("tegra_cpu_edp_init: init EDP limit: %u MHz\n", edp_limit/1000);
	}
}

static void tegra_cpu_edp_exit(void)
{
	if (!(cpu_edp_limits || system_edp_limits))
		return;

	unregister_hotcpu_notifier(&tegra_cpu_edp_notifier);
}

#ifdef CONFIG_DEBUG_FS

static int system_edp_alarm_get(void *data, u64 *val)
{
	*val = (u64)system_edp_alarm;
	return 0;
}
static int system_edp_alarm_set(void *data, u64 val)
{
	if (val > 1) {	/* emulate emergency throttling */
		tegra_edp_throttle_cpu_now(val);
		return 0;
	}
	return tegra_system_edp_alarm((bool)val);
}
DEFINE_SIMPLE_ATTRIBUTE(system_edp_alarm_fops,
			system_edp_alarm_get, system_edp_alarm_set, "%llu\n");

static int __init tegra_edp_debug_init(struct dentry *cpu_tegra_debugfs_root)
{
	if (!debugfs_create_file("edp_alarm", 0644, cpu_tegra_debugfs_root,
				 NULL, &system_edp_alarm_fops))
		return -ENOMEM;

	return 0;
}
#endif

#else	/* CONFIG_TEGRA_EDP_LIMITS */
#define edp_governor_speed(requested_speed) (requested_speed)
#define tegra_cpu_edp_init(resume)
#define tegra_cpu_edp_exit()
#define tegra_edp_debug_init(cpu_tegra_debugfs_root) (0)
#endif	/* CONFIG_TEGRA_EDP_LIMITS */

#ifdef CONFIG_DEBUG_FS

static struct dentry *cpu_tegra_debugfs_root;

static int __init tegra_cpu_debug_init(void)
{
	cpu_tegra_debugfs_root = debugfs_create_dir("cpu-tegra", 0);

	if (!cpu_tegra_debugfs_root)
		return -ENOMEM;

	if (tegra_edp_debug_init(cpu_tegra_debugfs_root))
		goto err_out;

	return 0;

err_out:
	debugfs_remove_recursive(cpu_tegra_debugfs_root);
	return -ENOMEM;
}

static void __exit tegra_cpu_debug_exit(void)
{
	debugfs_remove_recursive(cpu_tegra_debugfs_root);
}

late_initcall(tegra_cpu_debug_init);
module_exit(tegra_cpu_debug_exit);
#endif /* CONFIG_DEBUG_FS */

int tegra_verify_speed(struct cpufreq_policy *policy)
{
	return cpufreq_frequency_table_verify(policy, freq_table);
}

unsigned int tegra_getspeed(unsigned int cpu)
{
	unsigned long rate;

	if (cpu >= CONFIG_NR_CPUS || !ACCESS_ONCE(cpu_clk))
		return 0;

	rate = clk_get_rate(cpu_clk) / 1000;
	return rate;
}

int tegra_update_cpu_speed(unsigned long rate)
{
	int ret = 0;
	struct cpufreq_freqs freqs;
#ifndef CONFIG_TEGRA_CPUQUIET
	unsigned long rate_save = rate;
	int status = 1;
#endif
	
	// dont allow changes while in early suspend boost mode
	if (in_earlysuspend)
		return ret;
		
	freqs.old = tegra_getspeed(0);
	freqs.new = rate;

	rate = clk_round_rate(cpu_clk, rate * 1000);
	if (!IS_ERR_VALUE(rate))
		freqs.new = rate / 1000;

#ifndef CONFIG_TEGRA_CPUQUIET
	if (rate_save > T3_LP_MAX_FREQ) {
		if (is_lp_cluster()) {
#if CPU_FREQ_DEBUG			
			pr_info("tegra_update_cpu_speed: LP off %d %d %ld\n", freqs.old, freqs.new, rate_save);
#endif
			/* set rate to max of LP mode */
			ret = clk_set_rate(cpu_clk, T3_LP_MAX_FREQ * 1000);
#ifndef CONFIG_TEGRA_MPDECISION
			/* change to g mode */
			clk_set_parent(cpu_clk, cpu_g_clk);
#else
            /*
             * the above variant is now no longer preferred since
             * mpdecision would not know about this. Notify mpdecision
             * instead to switch to G mode
             */
             status = mpdecision_gmode_notifier();
             if (status == 0)
             	pr_err("tegra_update_cpu_speed: %s: couldn't switch to gmode (freq)", __func__ );
#endif
			/* restore the target frequency, and
			 * let the rest of the function handle
			 * the frequency scale up
			 */
			freqs.new = rate_save;
		}
	}
#endif

	if (freqs.old == freqs.new){
		return ret;
	}
	
	/*
	 * Vote on memory bus frequency based on cpu frequency
	 * This sets the minimum frequency, display or avp may request higher
	 */
	if (freqs.old < freqs.new) {
		ret = tegra_update_mselect_rate(freqs.new);
		if (ret) {
			pr_err("tegra_update_cpu_speed: Failed to scale mselect for cpu"
			       " frequency %u kHz\n", freqs.new);
			return ret;
		}
		ret = clk_set_rate(emc_clk, tegra_emc_to_cpu_ratio(freqs.new));
		if (ret) {
			pr_err("tegra_update_cpu_speed: Failed to scale emc for cpu"
			       " frequency %u kHz\n", freqs.new);
			return ret;
		}
	}

	for_each_online_cpu(freqs.cpu)
		cpufreq_notify_transition(&freqs, CPUFREQ_PRECHANGE);


	ret = clk_set_rate(cpu_clk, freqs.new * 1000);
	if (ret) {
		pr_err("tegra_update_cpu_speed: Failed to set cpu frequency to %d kHz\n",
			freqs.new);
		return ret;
	} 
#if CPU_FREQ_DEBUG			
	else {
		pr_info("tegra_update_cpu_speed: old=%d new=%d\n", freqs.old, tegra_getspeed(0));
	}
#endif

	for_each_online_cpu(freqs.cpu)
		cpufreq_notify_transition(&freqs, CPUFREQ_POSTCHANGE);

	if (freqs.old > freqs.new) {
		clk_set_rate(emc_clk, tegra_emc_to_cpu_ratio(freqs.new));
		tegra_update_mselect_rate(freqs.new);
	}

	return 0;
}

unsigned int tegra_count_slow_cpus(unsigned long speed_limit)
{
	unsigned int cnt = 0;
	int i;

	for_each_online_cpu(i)
		if (target_cpu_speed[i] <= speed_limit)
			cnt++;
	return cnt;
}

unsigned int tegra_get_slowest_cpu_n(void) {
	unsigned int cpu = nr_cpu_ids;
	unsigned long rate = ULONG_MAX;
	int i;

	for_each_online_cpu(i)
		if ((i > 0) && (rate > target_cpu_speed[i])) {
			cpu = i;
			rate = target_cpu_speed[i];
		}
	return cpu;
}

unsigned long tegra_cpu_lowest_speed(void) {
	unsigned long rate = ULONG_MAX;
	int i;

	for_each_online_cpu(i)
		rate = min(rate, target_cpu_speed[i]);
	return rate;
}

unsigned long tegra_cpu_highest_speed(void) {
	unsigned long policy_max = ULONG_MAX;
	unsigned long rate = 0;
	int i;

	for_each_online_cpu(i) {
		if (force_policy_max)
			policy_max = min(policy_max, policy_max_speed[i]);
		rate = max(rate, target_cpu_speed[i]);
	}
	rate = min(rate, policy_max);
	return rate;
}

#if defined(CONFIG_BEST_TRADE_HOTPLUG)
#define __NO_CPU_KICKING__                              (~0u>>1)
#define _buy_bargain(which_side, ...) \
    _buy_bargain_at_##which_side##_store (__VA_ARGS__)
#define _sell_bargain(which_side, ...) \
    _sell_bargain_to_##which_side##_customer (__VA_ARGS__)

extern bool valid_bargain_speed (unsigned int freq);
extern unsigned long p2pconv (unsigned int speed, int active_cpus);
extern int get_perf_votes (int);
extern unsigned long cpu_nr_running (int);
extern bool bthp_empty (int);
extern unsigned int tracked_tasks_nr (int cpu);
extern void get_bthp_lb_info (unsigned long long *, unsigned long *);
extern void cpus_load_stats (int, unsigned long *,
                             unsigned long *,
                             unsigned long *);
extern bool lb_prophet_up (int cpu, unsigned long *total_cc,
                           unsigned long *min_tradable_cc);
extern bool lb_prophet_down (int cpu, int cpu_to_be_down,
                             unsigned long *total_cc,
                             unsigned long *max_added_cc);
extern unsigned int bthp_supported_min_speed (void);
extern bool valid_power_diff (int diff_val);
extern unsigned int valid_max_power (void);
extern unsigned int round_or_level_up_speed (unsigned int speed);
extern bool valid_power_value (unsigned int power);
extern unsigned int g2lp_bottom_freq (void);
extern unsigned int lp2g_top_freq (void);
extern bool on_plugging (void);
extern bool can_do_bthp (void);
extern bool bthp_do_hotplug (int core_num_diff,
                             int dest_core,
                             cputime64_t *last_time,
                             unsigned int *debounce_interval,
                             unsigned int target_speed,
                             unsigned int *decision_cpu_speed_loc);
extern bool bthp_cpu_num_catchup (void);
extern bool is_bthp_en (void);

static unsigned int bthp_debounce_time_up = 70000000; /* 70 ms */
module_param(bthp_debounce_time_up, uint, 0644);

static unsigned int bthp_debounce_time_down = 100000000; /* 100 ms */
module_param(bthp_debounce_time_down, uint, 0644);

static unsigned int bthp_debounce_time_lp = 200000000; /* 200 ms */
module_param(bthp_debounce_time_lp, uint, 0644);

static unsigned int bthp_relax = 0; /* relieve aggregated BTHP eval. */
module_param(bthp_relax, uint, 0644);

/* the intention of mips UP ratio for each task's reschedule in rq */
unsigned int mips_aggressive_factor = 6;
module_param(mips_aggressive_factor, uint, 0644);
EXPORT_SYMBOL (mips_aggressive_factor);
#endif

// maxwen: apply all limits to a frequency
static unsigned int get_scaled_freq (unsigned int target_freq)
{
    /* chip-dependent, such as thermal throttle, edp, and user-defined freq. cap */
    #if CPU_FREQ_DEBUG
		pr_info("DBG_TF(01): %d kHz\n", target_freq);
	#endif
	target_freq = pmqos_cap_speed (target_freq);
    #if CPU_FREQ_DEBUG
		pr_info("DBG_TF(02): %d kHz\n", target_freq);
	#endif
    target_freq = tegra_throttle_governor_speed (target_freq);
    #if CPU_FREQ_DEBUG
		pr_info("DBG_TF(03): %d kHz\n", target_freq);
	#endif
	unsigned int edp_freq = edp_governor_speed (target_freq);
	if(edp_freq >= 1000)
	{
		target_freq = edp_governor_speed (target_freq);
	}
    #if CPU_FREQ_DEBUG
		pr_info("DBG_TF(04): %d kHz\n", target_freq);
	#endif
	target_freq = user_cap_speed (target_freq);
	#if CPU_FREQ_DEBUG
		pr_info("DBG_TF(05): %d kHz\n", target_freq);
	#endif
	
    return target_freq;
}

unsigned int best_core_to_turn_up (void) {
    /* mitigate high temperature, 0 -> 3 -> 2 -> 1 */
    if (!cpu_online (3))
        return 3;

    if (!cpu_online (2))
        return 2;

    if (!cpu_online (1))
        return 1;

    /* NOT found, return >= nr_cpu_id */
    return nr_cpu_ids;
}
EXPORT_SYMBOL(best_core_to_turn_up);

#if defined(CONFIG_BEST_TRADE_HOTPLUG)

DEFINE_PER_CPU(unsigned long, last_freq_update_jiffies) = {0UL};

struct {
    atomic_t kicking;
    int kick_cpu;
    cputime64_t last_kick_time;
    cputime64_t last_hotplug_time;
    cputime64_t last_lp_kick_time;
    unsigned int *up_debounce_time;
    unsigned int *down_debounce_time;
    unsigned int *tolp_debounce_time;

    struct {
        int cpu;
        unsigned int base_clk;
        unsigned int big_hammer;
        cputime64_t last_oc_time;
    } oc;

} bthp_kicker = {
    .kicking = ATOMIC_INIT(0),
    .kick_cpu = __NO_CPU_KICKING__,
    .last_kick_time = 0ULL,
    .last_hotplug_time = 0ULL,
    .last_lp_kick_time = 0ULL,
    .up_debounce_time = &bthp_debounce_time_up,
    .down_debounce_time = &bthp_debounce_time_down,
    .tolp_debounce_time = &bthp_debounce_time_lp,
    .oc.cpu = __NO_CPU_KICKING__,
    .oc.base_clk = 50000000,
    .oc.big_hammer = 1,
    .oc.last_oc_time = 0ULL
};

struct bthp_cpu {
    struct {
        unsigned int min_freq; /* 0 means unlimited */
        unsigned int max_freq; /* 0 means unlimited */
    } policy_qos;
};

DEFINE_PER_CPU(struct bthp_cpu, bthp_cpu) = {.policy_qos = {0}};

/* different levels for mp_overhead
 * bigger value represents "more easily to DOWN, but more hardly to UP"
 */
static unsigned int bthp_mp_overhead[NR_CPUS] = {
/*  1<->2,  2<->3,  3<->4  */
    10,     10,     10,             [3 ... NR_CPUS-1] = 10 /* default to 10%*/
};
module_param_array(bthp_mp_overhead, uint, NULL, 0644);

static bool big2_mp_overhead_cpuup = 0;
module_param(big2_mp_overhead_cpuup, bool, 0644);

static bool big2_mp_overhead_cpudown = 1;
module_param(big2_mp_overhead_cpudown, bool, 0644);

static unsigned int perfvote_factor = 0;
module_param(perfvote_factor, uint, 0644);

static unsigned int oc_big_hammer_limit = 100;
module_param(oc_big_hammer_limit, uint, 0644);

static bool oc_disabled = true;
module_param(oc_disabled, bool, 0644);

static bool bthp_optimistic_up = true;
module_param(bthp_optimistic_up, bool, 0644);

bool is_optimistic_up (void) {
	return bthp_optimistic_up;
}
EXPORT_SYMBOL(is_optimistic_up);

void lp_ticket_reset (
   cputime64_t new_time_ticket
   )
{
    bthp_kicker.last_lp_kick_time = new_time_ticket;
}
EXPORT_SYMBOL(lp_ticket_reset);

bool lp_ticket_claim (
   cputime64_t claim_time
   )
{
    if (time_before64 (bthp_kicker.last_lp_kick_time,
                       bthp_kicker.last_hotplug_time)) {
        bthp_kicker.last_lp_kick_time = claim_time;
        return false;
    }

    if (time_after64 (claim_time,
                      bthp_kicker.last_lp_kick_time +
                      *bthp_kicker.tolp_debounce_time)) {
        return true;
    }

    return false;
}
EXPORT_SYMBOL(lp_ticket_claim);

static unsigned int new_comer_as_perf_crit_factor = 1;
module_param(new_comer_as_perf_crit_factor, uint, 0644);

static unsigned int perf_critical_elevator = 0;
module_param(perf_critical_elevator, uint, 0644);

#define __BTHP_DECISION__DO_NOTHING__           0
#define __BTHP_DECISION__ANY__                  __BTHP_DECISION__DO_NOTHING__
#define __BTHP_DECISION__CPU_UP__               1
#define __BTHP_DECISION__CPU_DOWN__            -1
#define BTHP_DECISION(dec)                      __BTHP_DECISION__##dec##__

struct bthp_params {
    int cpu;
    int active_cpus;
    unsigned int orig_speed;
    int diff_speed;
    unsigned int orig_power;
    int diff_power;
    int best_diff_power;
    unsigned int best_target_speed;
    int best_hotplug_decision; /* 0: do nothing; 1: cpu_up; -1: cpu_down */
    int dest_core;

    struct {
        unsigned int min_freq;
        unsigned int max_freq;
        int min_cpus;
        int max_cpus;
        int perf_up_votes;
        bool perf_critical;
    } qos;
};

void update_bthp_policy_qos (
    int cpu,
    unsigned int min_freq,
    unsigned int max_freq
    )
{
    if (cpu >= NR_CPUS)
        return;

    per_cpu(bthp_cpu, cpu).policy_qos.min_freq = min_freq;
    per_cpu(bthp_cpu, cpu).policy_qos.max_freq = max_freq;
}
EXPORT_SYMBOL (update_bthp_policy_qos);

static unsigned int _cpu_target_freq (
    unsigned int target_freq,
    unsigned int relation
    )
{
	struct cpufreq_frequency_table optimal = {
		.index = ~0,
		.frequency = 0,
	};
	struct cpufreq_frequency_table suboptimal = {
		.index = ~0,
		.frequency = 0,
	};
	unsigned int i;

    /* NOT ready yet */
    if (!ACCESS_ONCE (freq_table))
        return 0;

	switch (relation) {
	case CPUFREQ_RELATION_H:
		suboptimal.frequency = ~0;
		break;
	case CPUFREQ_RELATION_L:
		optimal.frequency = ~0;
		break;
	}

	for (i = 0; (freq_table[i].frequency != CPUFREQ_TABLE_END); i++) {
		unsigned int freq = freq_table[i].frequency;
		if (freq == CPUFREQ_ENTRY_INVALID)
			continue;
		switch (relation) {
		case CPUFREQ_RELATION_H:
			if (freq <= target_freq) {
				if (freq >= optimal.frequency) {
					optimal.frequency = freq;
					optimal.index = i;
				}
			} else {
				if (freq <= suboptimal.frequency) {
					suboptimal.frequency = freq;
					suboptimal.index = i;
				}
			}
			break;
		case CPUFREQ_RELATION_L:
			if (freq >= target_freq) {
				if (freq <= optimal.frequency) {
					optimal.frequency = freq;
					optimal.index = i;
				}
			} else {
				if (freq >= suboptimal.frequency) {
					suboptimal.frequency = freq;
					suboptimal.index = i;
				}
			}
			break;
		}
	}
	if (optimal.index > i) {
		if (suboptimal.index > i)
			return 0;
		return freq_table[suboptimal.index].frequency;

	} else
		return freq_table[optimal.index].frequency;

	return 0;
}

static bool better_perf (
    unsigned int i_speed,
    int i_cpus,
    unsigned int competitor_speed,
    int competitor_cpus
    )
{
    unsigned int mp_levels = abs (i_cpus - competitor_cpus);
    unsigned int mp_overhead_idx = (i_cpus < competitor_cpus?
                                    i_cpus-1: competitor_cpus-1
                                    );
    unsigned int i_perf = i_speed * i_cpus;
    unsigned int c_perf = competitor_speed * competitor_cpus;

    if (i_cpus <= 0 || i_cpus > NR_CPUS ||
        competitor_cpus <= 0 || competitor_cpus > NR_CPUS) {
        return false;
    }

    for (; mp_levels > 0; mp_levels--) {
        if (i_cpus < competitor_cpus) {
            i_perf =
                (i_perf * (100 + bthp_mp_overhead[mp_overhead_idx++])) / 100;

        } else {
            c_perf =
                (c_perf * (100 + bthp_mp_overhead[mp_overhead_idx++])) / 100;
        }
    }

    return (i_perf > c_perf);
}

static unsigned int best_core_to_turn_down (void) {
	unsigned int bthp_get_slowest_cpu_n (void);

    /* NOT found, return >= nr_cpu_id */
    return tegra_get_slowest_cpu_n ();
}

static unsigned int big_two_mp_adjustment (
    int i_cpu,
    unsigned int i_cpu_speed,
    int mp_type,
    unsigned int speed_added_to_big2,
    int exp_cpu,
    unsigned int nr_online_cpus
    )
{
	unsigned int cpu = NR_CPUS;
	unsigned int rate = UINT_MAX;
	int i;

    /* find the big two */
	for_each_online_cpu(i)
		if ((i != i_cpu) && (i != exp_cpu) && (rate > target_cpu_speed[i])) {
			cpu = i;
			rate = target_cpu_speed[i];
		}

    if (cpu != NR_CPUS) {
        switch (mp_type) {
        case BTHP_DECISION(CPU_UP):
            if (big2_mp_overhead_cpuup) {
                rate =
                    (rate * (100 + bthp_mp_overhead[nr_online_cpus-1])) / 100;
            }

            i_cpu_speed =
                max(i_cpu_speed,
                    get_scaled_freq (rate + speed_added_to_big2));
            break;

        case BTHP_DECISION(CPU_DOWN):
            if (big2_mp_overhead_cpudown) {
                rate =
                    (rate * (100 - bthp_mp_overhead[nr_online_cpus-2])) / 100;
            }

            i_cpu_speed = max(i_cpu_speed, rate + speed_added_to_big2);
            break;

        default:
            break;
        }
    }

	return i_cpu_speed;
}

static bool perf_critical_on (
    int cpu,
    int perf_up_votes
    )
{
    unsigned int nr_running = cpu_nr_running (cpu);
	unsigned int tracked_nr_running = tracked_tasks_nr (cpu);
	unsigned int new_commers = 0;

    if (nr_running > tracked_nr_running)
		new_commers = nr_running - tracked_nr_running;

    /* lets say at least half of tasks vote for performance up */
    nr_running >>= 1;

	/* consider new comer as performance critical */
	new_commers >>= new_comer_as_perf_crit_factor;
	perf_up_votes += new_commers;

    if (perf_up_votes &&
        perf_up_votes > (nr_running + perf_critical_elevator))
        return true;

    return false;
}

static void _adjust__one_core_up (
    struct bthp_params *params
    )
{
    unsigned long total_cc = 0UL, min_tradable_cc = 0UL;
    unsigned long help_ratio = 0;
    unsigned int perfguarded_speed = 0;
    unsigned int offload_speed = 0;
    int perfguarded_best_diff_power = 0;
    unsigned int dest_cpu = best_core_to_turn_up ();

    /* theres no help to plug another core */
    if (!lb_prophet_up (params->cpu, &total_cc, &min_tradable_cc) ||
        dest_cpu >= nr_cpu_ids ||
        !total_cc ||
        !min_tradable_cc ||
        unlikely(total_cc < min_tradable_cc)) /* intermidiate result?! */
    {
        /* total_cc could be 0, since
         * 1) only one running task at this moment
         * 2) all tasks' affinities are pinned to this cpu,
         *    aka not load-balanced
         * 3) ...
         *
         * min_tradable_cc could be 0, since
         * 1) the conditions total_cc has in above
         * 2) cannot transfer any task to another core due to cpu weight load
         * 3) ...
         */
        CPU_DEBUG_PRINTK (CPU_DEBUG_BTHP_LB,
                          " cpu_up is bad trade (%lu / %lu)",
                          min_tradable_cc,
                          total_cc
                          );
        return;
    }

    CPU_DEBUG_PRINTK (CPU_DEBUG_BTHP_LB,
                      " cpu_up: min tradable mips (%lu / %lu)",
                      min_tradable_cc,
                      total_cc
                      );

    /* sooooo slight benefit, seems to be NOT worthy */
    help_ratio = (min_tradable_cc * 100) / total_cc;
    if (!help_ratio && !params->qos.perf_up_votes) {
        return;
    }

    /* basic min. perf requirement w/o considering mp overhead */
    perfguarded_speed = ((params->orig_speed + params->diff_speed) *
                         (100 - help_ratio)) / 100;

    /* take mp overhead into consideration to secure min. perf requirement */
    perfguarded_speed =
        (perfguarded_speed *
         (100 + bthp_mp_overhead[params->active_cpus-1])) / 100;

    /* align to min. bthp-supporting speed */
    perfguarded_speed = perfguarded_speed?: bthp_supported_min_speed();

    /* pick max of both of guarded speed and offloaded speed */
    offload_speed =
        ((params->orig_speed + params->diff_speed) * help_ratio) / 100;
    offload_speed =
        (offload_speed *
         (100 + bthp_mp_overhead[params->active_cpus-1])) / 100;
    perfguarded_speed = max (perfguarded_speed, offload_speed);

    /* never goes below Big Two,
     * otherwise the performance of other cores MIGHT be
     * downgraded due to bthp decision
	 *
     * we don't consider the speed transfered from Big Two
     * and other cores to new online core,
	 * since the final decision MUST be made as quickly as possible
     */
    perfguarded_speed = big_two_mp_adjustment (params->cpu,
                                               perfguarded_speed,
                                               BTHP_DECISION(CPU_UP),
                                               0,
                                               params->cpu,
                                               params->active_cpus);

    /* align to min. freq */
    if (likely(params->qos.min_freq) &&
        perfguarded_speed < params->qos.min_freq) {
        perfguarded_speed = params->qos.min_freq;
    }

    /* never beyond the boundary of max speed */
    if (likely(params->qos.max_freq) &&
        perfguarded_speed > params->qos.max_freq) {
        perfguarded_speed = params->qos.max_freq;
    }

    /* align to chip-supporting freq. level */
    perfguarded_speed =
        _cpu_target_freq (perfguarded_speed, CPUFREQ_RELATION_L);

    if (!perfguarded_speed) {
        return;
    }

    /* to resolve the critical performance ASAP w/o concerns of power */
    if (params->qos.perf_critical)
    {
        perfguarded_speed =
            max(perfguarded_speed, params->orig_speed+params->diff_speed);
        perfguarded_best_diff_power =
            (int)(p2pconv (perfguarded_speed, params->active_cpus+1) -
                  params->orig_power);

        params->best_diff_power =
            valid_power_diff (perfguarded_best_diff_power)?
            perfguarded_best_diff_power:
            valid_max_power()-params->orig_power;

        params->best_target_speed = perfguarded_speed;
        params->best_hotplug_decision = BTHP_DECISION (CPU_UP);
        params->dest_core = dest_cpu;

        return;
    }

    perfguarded_best_diff_power =
        (int)(p2pconv (perfguarded_speed, params->active_cpus+1) -
              params->orig_power);

    /* both of BTHP and CP decisions rely on valid power diff. */
    if (!valid_power_diff (perfguarded_best_diff_power)) {
        return;
    }

    /* to resolve performance low, pick up power-saved && perf-best speed */
    if (params->qos.perf_up_votes > 0 &&
        !better_perf (perfguarded_speed,
                      params->active_cpus + 1,
                      params->best_target_speed,
                      (params->best_hotplug_decision ==
                       BTHP_DECISION (CPU_DOWN)?
                       params->active_cpus - 1:
                       params->active_cpus)
                      ))
    {
        return;
    }

    /* power-saved and performance-secured */
    if (perfguarded_best_diff_power < (params->qos.perf_up_votes > 0?
                                       params->diff_power:
                                       params->best_diff_power))
    {
        params->best_diff_power = perfguarded_best_diff_power;
        params->best_target_speed = perfguarded_speed;
        params->best_hotplug_decision = BTHP_DECISION (CPU_UP);
        params->dest_core = dest_cpu;
    }
}

static unsigned int gov_target_cpu_speed (
    int cpu
    )
{
    /* governor of each cpu makes its own decision */
    return target_cpu_speed[cpu];
}

static void _adjust__one_core_down (
    struct bthp_params *params
    )
{
    unsigned long total_cc = 0UL, max_added_cc = 0UL;
    unsigned long transferred_ratio = 0;
    unsigned int transferred_speed = 0;
    unsigned int perfguarded_speed = 0;
    int perfguarded_best_diff_power = 0;
    unsigned int dest_cpu = best_core_to_turn_down ();

    /* theres no help to unplug the slowest core */
    if (dest_cpu >= nr_cpu_ids ||
        params->cpu == dest_cpu ||
        !lb_prophet_down (params->cpu, dest_cpu, &total_cc, &max_added_cc) ||
        unlikely(total_cc < max_added_cc) || /* intermidiate result?! */
        (cpu_nr_running (dest_cpu) > 0 && !total_cc)) /* stale?! */

    {
        CPU_DEBUG_PRINTK (CPU_DEBUG_BTHP_LB,
                          " cpu_down is bad trade (%lu / %lu)",
                          max_added_cc,
                          total_cc
                          );
        return;
    }

    CPU_DEBUG_PRINTK (CPU_DEBUG_BTHP_LB,
                      " cpu_down: max painful mips (%lu / %lu)",
                      max_added_cc,
                      total_cc
                      );

    /* come out the transffered speed (in kHZ) from
     * the slowest core to be shut down
     */
    if (total_cc && max_added_cc) {
        transferred_ratio = (max_added_cc * 100) / total_cc;
    }
    transferred_speed =
        (gov_target_cpu_speed (dest_cpu) *
         (100 - bthp_mp_overhead[params->active_cpus-2])) / 100;

    /* re-evaluate the target speed which benefits from mp overhead and
     * accommodate the transferred speed
    */
    perfguarded_speed =
        ((params->orig_speed + params->diff_speed) *
         (100 - bthp_mp_overhead[params->active_cpus-2])) / 100;
    perfguarded_speed += (transferred_speed * transferred_ratio) / 100;

    /* never goes below Big Two */
    perfguarded_speed =
        big_two_mp_adjustment (params->cpu,
                               perfguarded_speed,
                               BTHP_DECISION(CPU_DOWN),
                               (transferred_speed *
                                (100-transferred_ratio)) / 100,
                               dest_cpu,
                               params->active_cpus);

    /* align to min. freq */
    if (likely(params->qos.min_freq) &&
        perfguarded_speed < params->qos.min_freq) {
        perfguarded_speed = params->qos.min_freq;
    }

    /* Prohibit over the boundary of max speed
     *
     * For cpu_down, the worst case is the leftover,
     * ((transferred_speed * (100-transferred_ratio))),
     * is transfered to Big Two core,
     * it MIGHT seriously downgrade perf. of Big Two core since
	 * the chip-supporting max cpu speed is LIMITED!!
     * Thus, if Big Two core demands the speed over the boundary of max speed,
     * we have to turn it down to refuse the BTHP decision of cpu_down.
     */
    if (likely(params->qos.max_freq) &&
        perfguarded_speed > params->qos.max_freq) {
        return;
    }

    /* align to chip-supporting freq. level */
    perfguarded_speed =
        _cpu_target_freq (perfguarded_speed, CPUFREQ_RELATION_L);

    /* never turn cpu down in the period of performance critical */
    if (!perfguarded_speed || params->qos.perf_critical) {
        return;
    }

    perfguarded_best_diff_power =
        (int)(p2pconv (perfguarded_speed, params->active_cpus-1) -
              params->orig_power);

    /* both of BTHP and CP decisions rely on valid power diff. */
    if (!valid_power_diff (perfguarded_best_diff_power)) {
        return;
    }

    /* to resolve performance low, pick up power-saved && perf-best speed */
    if (params->qos.perf_up_votes > 0 &&
        !better_perf (perfguarded_speed,
                      params->active_cpus - 1,
                      params->best_target_speed,
                      (params->best_hotplug_decision ==
                       BTHP_DECISION (CPU_UP)?
                       params->active_cpus + 1:
                       params->active_cpus)
                      ))
    {
        return;
    }

    /* power-saved and performance-secured */
    if (perfguarded_best_diff_power < (params->qos.perf_up_votes > 0?
                                       params->diff_power:
                                       params->best_diff_power))
    {
        params->best_diff_power = perfguarded_best_diff_power;
        params->best_target_speed = perfguarded_speed;
        params->best_hotplug_decision = BTHP_DECISION (CPU_DOWN);
        params->dest_core = dest_cpu;
    }
}

static bool resolve_perf_low_by_oc (
    struct bthp_params *params,
    unsigned int *oc_speed
    )
{
    cputime64_t now = ktime_to_us (ktime_get());

    /* reverse big hammer to reduce oc interval */
    if (params->cpu == bthp_kicker.oc.cpu && !params->qos.perf_up_votes) {
        bthp_kicker.oc.big_hammer >>= 1;

        if (!bthp_kicker.oc.big_hammer) {
            bthp_kicker.oc.big_hammer = 1;
            bthp_kicker.oc.cpu = __NO_CPU_KICKING__;
        }

        return false;
    }

    if (params->qos.perf_critical &&
        params->diff_speed == 0 &&
        params->orig_speed == params->qos.max_freq)
    {
        /* boost to max frequency despites of limitations of edp, ... */
        *oc_speed = per_cpu(bthp_cpu, params->cpu).policy_qos.max_freq;

        if (*oc_speed > params->orig_speed) {
            /* reset out-of-date oc kicker */
            if (bthp_kicker.oc.cpu != __NO_CPU_KICKING__ &&
                (!cpu_online (bthp_kicker.oc.cpu) ||
                 !get_perf_votes (bthp_kicker.oc.cpu)))
            {
                bthp_kicker.oc.cpu = __NO_CPU_KICKING__;
                bthp_kicker.oc.big_hammer = 1;
            }

            if (bthp_kicker.oc.big_hammer > oc_big_hammer_limit) {
                bthp_kicker.oc.big_hammer = oc_big_hammer_limit;
            }

            if (time_after64 (now,
                              bthp_kicker.oc.last_oc_time +
                              bthp_kicker.oc.base_clk *
                              bthp_kicker.oc.big_hammer))
            {
                bthp_kicker.oc.cpu = params->cpu;
                bthp_kicker.oc.last_oc_time = now;
                bthp_kicker.oc.big_hammer <<= 1;

                return true;
            }
        }
    }

    return false;
}

static unsigned int iboost_floor_freq = 0UL;
static cputime64_t iboost_floor_time = 0ULL;

void bthp_set_floor_cap (
   unsigned int floor_freq,
   cputime64_t floor_time
   )
{
    iboost_floor_freq = floor_freq;
    iboost_floor_time = floor_time;
}
EXPORT_SYMBOL(bthp_set_floor_cap);

static unsigned int cpu_get_min_speed (
    int cpu
    )
{
    cputime64_t now = ktime_to_ns (ktime_get ());
    unsigned int input_boost_freq = 0UL;

    if (time_before_eq64 (now, iboost_floor_time))
        input_boost_freq = get_scaled_freq (iboost_floor_freq);

    /* we just overwrite scaling_min_freq to cap minimum speed for all cpus */
    return max (per_cpu(bthp_cpu, 0).policy_qos.min_freq,
                input_boost_freq
                );
}

static unsigned int cpu_get_max_speed (
    int cpu
    )
{
    /* we just overwrite scaling_max_freq to cap maximum speed for all cpus */
    return per_cpu(bthp_cpu, 0).policy_qos.max_freq;
}

static unsigned int perf_votes_scaling (
   struct bthp_params *params
   )
{
    unsigned int base_freq = params->orig_speed + params->diff_speed;
    unsigned int scaled_freq = base_freq;
    int target_num_factors = params->qos.perf_up_votes;

    for (; target_num_factors > 0; target_num_factors--) {
        scaled_freq =
            (base_freq * (100 + target_num_factors * perfvote_factor)) / 100;
        if (scaled_freq <= params->qos.max_freq) {
            scaled_freq = _cpu_target_freq (scaled_freq, CPUFREQ_RELATION_L);
            if (!scaled_freq)
                return base_freq;

            params->best_diff_power =
                p2pconv(scaled_freq, params->active_cpus) -
                params->orig_power;
            params->best_target_speed = scaled_freq;

            break;
        }
    }

    return scaled_freq;
}

static unsigned int _do_trade_bargain (
    unsigned int prev_speed,
    unsigned int next_speed,
    int prefer_up_or_down
    )
{
    struct bthp_params params;
    unsigned int oc_speed = next_speed;
    unsigned int scaled_speed = next_speed;
    unsigned int bthp_supported_min_freq = bthp_supported_min_speed ();
    unsigned int this_cpu_nr_running;
    unsigned int stub_debounce_interval = 0UL;
    unsigned int min_bargainable_interval =
        min (*bthp_kicker.up_debounce_time, *bthp_kicker.down_debounce_time);
    unsigned int relax_interval =
        max (*bthp_kicker.up_debounce_time, *bthp_kicker.down_debounce_time);

    params.cpu = smp_processor_id ();
    params.active_cpus = num_online_cpus ();

	/* NOT necessary to do bargain;
	 * instead, catch up with required numbers of cpus ASAP
	 */
    if (params.active_cpus < pm_qos_request (PM_QOS_MIN_ONLINE_CPUS)) {
        if (!bthp_cpu_num_catchup ())
            CPU_DEBUG_PRINTK (CPU_DEBUG_BTHP,
                              " cannot bring up # of cpus required"
                              );

        bthp_kicker.last_hotplug_time = bthp_kicker.last_kick_time;
        atomic_cmpxchg (&bthp_kicker.kicking, 1, 0);
        return next_speed;
    }

    /* Too early to bargain */
    if (!can_do_bthp () ||
        (likely(bthp_kicker.last_hotplug_time) &&
         time_before64 (bthp_kicker.last_kick_time,
                        bthp_kicker.last_hotplug_time +
                        min_bargainable_interval)))
    {
        params.best_target_speed = next_speed;
        goto exit_bargain;
    }

    /* prepare parameters first for cpu_up/cpu_down bargain */
    params.orig_speed = prev_speed;
    params.diff_speed = next_speed-prev_speed;
    params.orig_power =
        p2pconv (prev_speed, params.active_cpus);
    params.diff_power =
        p2pconv (next_speed, params.active_cpus)-
        params.orig_power;

    /* initiate best trade to current governor decision,
     * that is, ONLY DVFS but not Hotplug
     */
    params.best_diff_power = params.diff_power;
    params.best_target_speed = next_speed;
    params.best_hotplug_decision = BTHP_DECISION (DO_NOTHING);
    params.dest_core = NR_CPUS;

    /* trade MUST be legal, limited under policies of pm_qos, cpufreq, ... */
    params.qos.min_freq = cpu_get_min_speed (params.cpu);
    params.qos.max_freq =
        get_scaled_freq (cpu_get_max_speed (params.cpu));
    params.qos.min_cpus = pm_qos_request (PM_QOS_MIN_ONLINE_CPUS)? :1;
    params.qos.max_cpus = pm_qos_request (PM_QOS_MAX_ONLINE_CPUS)? :NR_CPUS;

    /* IF
     * perf_up_votes > nr_running/2 + perf_critical_elevator,
     * performance is critical
     *
     * ELSE IF
     * perf_up_votes <= (nr_running/2 + perf_critical_elevator) && >= 1,
     * performance is low
     *
     * ELSE
     * performance is acceptable
     */
    this_cpu_nr_running = cpu_nr_running (params.cpu);
    params.qos.perf_up_votes = get_perf_votes (params.cpu);
    params.qos.perf_critical = perf_critical_on (params.cpu,
                                                 params.qos.perf_up_votes);

    /* align with all sorts of caps */
    if (params.qos.min_freq > params.qos.max_freq) {
        params.qos.min_freq = params.qos.max_freq;
    }

    /* NOT in the speed range BTHP could deal with */
    if (!valid_bargain_speed (prev_speed) ||
        !valid_bargain_speed (next_speed) ||
        unlikely(!valid_power_value (params.orig_power)) ||
        !valid_power_diff (params.diff_power)
        )
    {
        goto nothing_bargain;
    }

    /* align min_freq to BTHP-supporting minimum frequency
     *
     * this will help on plgging one another core
     * at quite low frequency rather than do nothing, turns out
     * we can bring target frequency down a little bit to
     * save power
     */
    if (bthp_supported_min_freq <= params.qos.max_freq &&
        bthp_supported_min_freq > params.qos.min_freq)
    {
        params.qos.min_freq = bthp_supported_min_freq;
    }

    if ((prefer_up_or_down == BTHP_DECISION (ANY) ||
         prefer_up_or_down == BTHP_DECISION (CPU_UP)) &&
         params.active_cpus < NR_CPUS &&
         params.active_cpus < params.qos.max_cpus &&
         time_after64 (bthp_kicker.last_kick_time,
                       bthp_kicker.last_hotplug_time +
                      *bthp_kicker.up_debounce_time))
    {
        _adjust__one_core_up (&params);
    }

    if ((prefer_up_or_down == BTHP_DECISION (ANY) ||
         prefer_up_or_down == BTHP_DECISION (CPU_DOWN)) &&
         params.active_cpus <= NR_CPUS &&
         params.active_cpus > params.qos.min_cpus &&
         time_after64 (bthp_kicker.last_kick_time,
                       bthp_kicker.last_hotplug_time +
                      *bthp_kicker.down_debounce_time))
    {
        _adjust__one_core_down (&params);
    }

    /* relax, for debounc'in next BTHP */
    if (bthp_relax &&
        time_after64 (bthp_kicker.last_kick_time,
                      bthp_kicker.last_hotplug_time + relax_interval))
    {
        bthp_kicker.last_hotplug_time = bthp_kicker.last_kick_time;
    }

    /* plug or unplug one of G cpus by best-trade decision */
    if (params.best_hotplug_decision != BTHP_DECISION (DO_NOTHING)) {
        if (!bthp_do_hotplug (params.best_hotplug_decision,
                              params.dest_core,
                              &bthp_kicker.last_hotplug_time,
                              &stub_debounce_interval,
                              params.best_target_speed,
                              (unsigned int *)&target_cpu_speed[params.cpu]
                             ))
        {
            /* restore any changes made on target speed */
            params.best_target_speed = next_speed;
            bthp_kicker.last_hotplug_time = ktime_to_ns (ktime_get ());
            goto exit_bargain;
        }
    }

    /* The final chance we have to releive the performance low by
     * scaling the target speed UP by task perf. votes
     */
    if (params.best_hotplug_decision == BTHP_DECISION (DO_NOTHING) &&
        perfvote_factor > 0 &&
        params.qos.perf_up_votes)
    {
        scaled_speed = perf_votes_scaling (&params);

        /* if scale result lge G->LP boundary, SHOULD stay at G cluster
         * rather than switch to LP, since the governor, at next round,
         * has high probabilities to raise CPU speed up.
         * Then, we would probably suffer from the overhead of
         * switching from LP to G again
         */
        if (scaled_speed >= g2lp_bottom_freq()) {
            if (scaled_speed > target_cpu_speed[params.cpu]) {
                /* governor decision is boosted as well */
                target_cpu_speed[params.cpu] = scaled_speed;
            }

        /* Else, ignore the scaled result */
        } else {
            params.best_target_speed = scaled_speed = next_speed;
        }
    }

    if (get_cpu_debug() & CPU_DEBUG_BTHP) {
        unsigned int final_power = valid_max_power ();
        int final_benefit =
            params.diff_power - (final_power - params.orig_power);

        if (params.best_hotplug_decision != BTHP_DECISION (DO_NOTHING)) {
            final_benefit = params.diff_power - params.best_diff_power;

        } else if (scaled_speed != next_speed) {
            if (valid_power_diff(params.best_diff_power))
                final_benefit = params.diff_power - params.best_diff_power;

        } else {
            final_benefit = 0;
        }

        pr_bthp_info ("cpu#%d,%d/%u,%d: (%u kHZ, %u kHZ) -> [%d+(%d)]"\
                      "(%u kHZ, improved %d mA)\n",
                      params.cpu,
                      params.qos.perf_up_votes,
                      this_cpu_nr_running,
                      params.qos.perf_critical,
                      prev_speed,
                      next_speed,
                      params.active_cpus,
                      params.best_hotplug_decision,
                      params.best_target_speed,
                      final_benefit
                      );
    }

nothing_bargain:
    if (prefer_up_or_down == BTHP_DECISION (ANY) &&
        params.best_hotplug_decision == BTHP_DECISION (DO_NOTHING) &&
        scaled_speed == next_speed)
    {
        /* do oc if allowed and in need */
        if (likely(!oc_disabled) &&
            resolve_perf_low_by_oc (&params, &oc_speed))
        {
            pr_bthp_info ("cpu%d: oc boost (%u -> %u)\n",
                          params.cpu,
                          next_speed,
                          oc_speed
                          );

            params.best_target_speed = oc_speed;

        /*
         * lt G-cluster valid frequency range, only need to keep at most
         * one core
         */
        } else if (params.active_cpus > 1 &&
                   next_speed <= g2lp_bottom_freq())
        {
            int target_down_core = best_core_to_turn_down ();

            if (target_down_core < nr_cpu_ids &&
                !params.qos.perf_critical &&
                !bthp_do_hotplug (BTHP_DECISION (CPU_DOWN),
                                  target_down_core,
                                  &bthp_kicker.last_hotplug_time,
                                  &stub_debounce_interval,
                                  next_speed,
                                  (unsigned int *)&target_cpu_speed[params.cpu]
                                 ))
            {
                /* restore any changes made on target speed */
                params.best_target_speed = next_speed;
            }

        /* Awesome, no performance low and demand speed is quite
         * lower than bottom speed of G-cluster.
         *
         * Switch from G -> LP
         */
        } else if (params.active_cpus == 1 &&
                   /* dont fall back to LP since someone is asking for
                    * AT LEAST 1 cpu is kept online for performance sake
                    *
                    * fall back to slow-speed LP cluster or
                    * switch between G and LP clusters back and forth
                    * would somewhat downgrade his/her performance requirement
                    */
                   //!pm_qos_request (PM_QOS_MIN_ONLINE_CPUS) &&
                   next_speed <= g2lp_bottom_freq())
        {
            if (!bthp_do_hotplug (BTHP_DECISION (CPU_DOWN),
                                  0,
                                  &bthp_kicker.last_hotplug_time,
                                  &stub_debounce_interval,
                                  next_speed,
                                  (unsigned int *)&target_cpu_speed[params.cpu]
                                 ))
            {
                /* restore any changes made on target speed */
                params.best_target_speed = next_speed;
            }
        }
    }

exit_bargain:
    atomic_cmpxchg (&bthp_kicker.kicking, 1, 0);

    return params.best_target_speed;
}

void bthp_cpuup_standalone (
    unsigned int freq
    )
{
    if (!mutex_trylock (&tegra_cpu_lock))
        return;

    if (!atomic_cmpxchg (&bthp_kicker.kicking, 0, 1)) {
        bthp_kicker.kick_cpu = smp_processor_id();
        bthp_kicker.last_kick_time = ktime_to_ns (ktime_get ());

        /* make trade to gain at-least-equal perf. & better power
         *  -- only (cpu_up and do_nothing) are considered
         */
        _do_trade_bargain (freq, freq, BTHP_DECISION (CPU_UP));
    }

    mutex_unlock (&tegra_cpu_lock);
}
EXPORT_SYMBOL (bthp_cpuup_standalone);

unsigned long bthp_cpu_highest_speed (void) {
	unsigned long policy_max = ULONG_MAX;
	unsigned long rate = 0;
	int i;
	int cpu = smp_processor_id();
	unsigned long ref_jiffies =
		min (nsecs_to_jiffies (*bthp_kicker.down_debounce_time),
			 nsecs_to_jiffies (*bthp_kicker.up_debounce_time)
			 );

	for_each_online_cpu(i) {
		/* MUST ignore those cores have
		 * being zzzZZZ beyond min BTHP hotplug time
		 */
        if (i == cpu ||
			!idle_cpu (i) ||
			jiffies < (per_cpu (last_freq_update_jiffies, i) + ref_jiffies))
		{
			if (force_policy_max)
				policy_max = min (policy_max, policy_max_speed[i]);

			rate = max (rate, target_cpu_speed[i]);
        }
	}

	rate = min (rate, policy_max);
	return rate;
}

unsigned int bthp_get_slowest_cpu_n (void) {
	unsigned int cpu = nr_cpu_ids;
	unsigned long rate = ULONG_MAX;
	int i;
	unsigned long ref_jiffies =
		min (nsecs_to_jiffies (*bthp_kicker.down_debounce_time),
			 nsecs_to_jiffies (*bthp_kicker.up_debounce_time)
			 );

	for_each_online_cpu(i) {
        if (i > 0) {
			/* ALWAYS find the long idle first */
            if (idle_cpu (i) &&
				jiffies > (per_cpu (last_freq_update_jiffies, i) +
						   ref_jiffies))
			{
				cpu = i;
				break;

			} else if (rate > target_cpu_speed[i]) {
				cpu = i;
				rate = target_cpu_speed[i];
			}
        }
	}

	return cpu;
}
#endif

int tegra_cpu_set_speed_cap(unsigned int *speed_cap)
{
	int ret = 0;
    unsigned int new_speed = tegra_cpu_highest_speed();
    
    unsigned int curr_speed = tegra_getspeed(0);
    
#if defined(CONFIG_BEST_TRADE_HOTPLUG)
    unsigned int speed_wocap = 0UL;
    unsigned int speed_before_bthp = 0UL;
    unsigned int min_speed = cpu_get_min_speed (0);
    int cpu = smp_processor_id();
  
    bool forced_kick = false;

    if (new_speed < min_speed) {
        new_speed = min_speed;
        #if CPU_FREQ_DEBUG
		pr_info("DBG_NS(02): %d kHz\n", new_speed);
#endif

        /* all cpus' frequency requests are < min_speed
         * let cpu0 be the kicker
         */
        if (!cpu)
            forced_kick = true;
    }

    speed_wocap = new_speed;
#endif

	// dont allow changes while in early suspend boost mode
	if (in_earlysuspend)
		return ret;

	if (is_suspended)
		return -EBUSY;

	new_speed = get_scaled_freq(new_speed);
	#if CPU_FREQ_DEBUG
		pr_info("DBG_NS(03): %d kHz\n", new_speed);
#endif

#if defined(CONFIG_BEST_TRADE_HOTPLUG)
    /* do a best trade for power/performance,
     * and come out target speed
     */
    if (likely(is_bthp_en ())) {
        /* bthp doesn't care lp DVFS since
         * no trade can be made on lp cluster
         */
        if (!is_lp_cluster()) {
            cputime64_t now = ktime_to_ns (ktime_get ());

            /* cpu0 MUST take the kicking job,
             * when all cores' demanding perf. lowers than
             * valid G-CPU freq. range.
             * cpu0 is the only one could bring other cores down
             */
            if (!cpu &&
                num_online_cpus () > 1 &&
                new_speed < g2lp_bottom_freq ())
            {
                forced_kick = true;
            }

            /* only who claims the highest frequency can make final deal */
            if ((target_cpu_speed[cpu] >= speed_wocap || forced_kick) &&
                !on_plugging())
            {
                if (!atomic_cmpxchg (&bthp_kicker.kicking, 0, 1)) {
                    bthp_kicker.kick_cpu = cpu;
                    bthp_kicker.last_kick_time = now;

                    speed_before_bthp = new_speed;

                    /* a bargained trade will bring us a
                     * better target speed concerning of perf/power
                     *
                     * Don't drive speed DOWN until core plug/unplug gets done
                     * the overhead of hotplug has to be well-considered
                     */
                    new_speed = _do_trade_bargain (curr_speed,
                                                   new_speed,
                                                   BTHP_DECISION (ANY)
                                                   );
                                                   #if CPU_FREQ_DEBUG
		pr_info("DBG_NS(04): %d kHz\n", new_speed);
#endif

                    if (new_speed < speed_before_bthp)
                    {
                        new_speed = speed_before_bthp;
                        #if CPU_FREQ_DEBUG
		pr_info("DBG_NS(05): %d kHz\n", new_speed);
#endif
                    }
                }
            }

            if (speed_cap)
                *speed_cap = new_speed;

            /* catch up with the highest speed ASAP */
            return (curr_speed == new_speed?
                    0:
                    tegra_update_cpu_speed (new_speed));
        }
    }
#endif
	if (speed_cap)
		*speed_cap = new_speed;

	if (curr_speed == new_speed)
		return 0;

#if CPU_FREQ_DEBUG			
	pr_info("tegra_cpu_set_speed_cap: curr_speed %d new_speed %d\n", curr_speed, new_speed);
#endif

	ret = tegra_update_cpu_speed(new_speed);
	tegra_auto_hotplug_governor(new_speed, false);
	return ret;
}

int tegra_suspended_target(unsigned int target_freq)
{
	unsigned int new_speed = target_freq;

	if (!is_suspended)
		return -EBUSY;

	/* apply only "hard" caps */
	new_speed = tegra_throttle_governor_speed(new_speed);
	#if CPU_FREQ_DEBUG
		pr_info("DBG_NS(06): %d kHz\n", new_speed);
#endif
	new_speed = edp_governor_speed(new_speed);
	#if CPU_FREQ_DEBUG
		pr_info("DBG_NS(07): %d kHz\n", new_speed);
#endif

	return tegra_update_cpu_speed(new_speed);
}

int tegra_input_boost (int cpu, unsigned int target_freq)
{
    int ret = 0;
    unsigned int curfreq = 0, scaling_max_limit = 0;

    curfreq = tegra_getspeed(0);

    /* get global caped limit */
    target_freq = get_scaled_freq(target_freq);

    /* get any per cpu defined limit cause input_boost
     might not be validated against policy->max */
    scaling_max_limit = get_cpu_freq_limit(cpu);

    /* apply any scaling max limits */
    if (scaling_max_limit < target_freq)
        target_freq = scaling_max_limit;

    /* dont need to boost cpu at this moment */
    if (!curfreq || curfreq >= target_freq) {
        return -EINVAL;
    }

#ifdef CONFIG_TEGRA_CPUQUIET
	if (target_freq > T3_LP_MAX_FREQ && is_lp_cluster())
		// disable LP mode asap
		// must be outside tegra_cpu_lock mutex!
		tegra_cpuquiet_force_gmode();
#endif

    mutex_lock(&tegra_cpu_lock);
    
#if CPU_FREQ_DEBUG
	pr_info("tegra_input_boost: cpu=%d curfreq =%d -> target_freq=%d\n", cpu, curfreq, target_freq);
#endif

    target_cpu_speed[cpu] = target_freq;

    /* will auto. round-rate */
    ret = tegra_update_cpu_speed(target_freq);

    mutex_unlock(&tegra_cpu_lock);

    return ret;
}
EXPORT_SYMBOL (tegra_input_boost);

static int tegra_target(struct cpufreq_policy *policy,
		       unsigned int target_freq,
		       unsigned int relation)
{
	int idx;
	unsigned int freq;
	int ret = 0;

	ret = cpufreq_frequency_table_target(policy, freq_table, target_freq,
		relation, &idx);
	if (ret)
		return ret;

	freq = freq_table[idx].frequency;
	
#ifdef CONFIG_TEGRA_CPUQUIET
	if (target_freq > T3_LP_MAX_FREQ && is_lp_cluster())
		// disable LP mode asap
		// must be outside tegra_cpu_lock mutex!
		tegra_cpuquiet_force_gmode();
#endif

	mutex_lock(&tegra_cpu_lock);

#if CPU_FREQ_DEBUG
	pr_info("tegra_target: freq=%d\n", freq);
#endif

	target_cpu_speed[policy->cpu] = freq;
	ret = tegra_cpu_set_speed_cap(NULL);

	mutex_unlock(&tegra_cpu_lock);

	return ret;
}

#ifdef CONFIG_HAS_EARLYSUSPEND
/* put early_suspend/late_resume handlers here for the display in order
 * to keep the code out of the display driver, keeping it closer to upstream
 */
struct early_suspend tegra_cpufreq_early_suspender;
struct early_suspend tegra_cpufreq_performance_early_suspender;
static struct pm_qos_request_list cap_cpu_freq_req;
static struct pm_qos_request_list cap_cpu_num_req;
static struct pm_qos_request_list boost_cpu_freq_req;
#endif

static int tegra_pm_notify(struct notifier_block *nb, unsigned long event,
	void *dummy)
{
	mutex_lock(&tegra_cpu_lock);
	if (event == PM_SUSPEND_PREPARE) {
		unsigned int freq;
		is_suspended = true;
		freq=freq_table[suspend_index].frequency;
#if CPU_FREQ_DEBUG
		pr_info("tegra_pm_notify: suspend: setting frequency to %d kHz\n", freq);
#endif
		tegra_update_cpu_speed(freq);
		tegra_auto_hotplug_governor(freq, true);
	} else if (event == PM_POST_SUSPEND) {
		unsigned int freq;
		is_suspended = false;
		tegra_cpu_edp_init(true);
		
		tegra_cpu_set_speed_cap(&freq);
#if CPU_FREQ_DEBUG
		pr_info("tegra_pm_notify: resume: restoring frequency to %d kHz\n", freq);
#endif
	}
	mutex_unlock(&tegra_cpu_lock);
	return NOTIFY_OK;
}

static struct notifier_block tegra_cpu_pm_notifier = {
	.notifier_call = tegra_pm_notify,
};

static int tegra_cpu_init(struct cpufreq_policy *policy)
{
	if (policy->cpu >= CONFIG_NR_CPUS)
		return -EINVAL;

	cpu_clk = clk_get_sys(NULL, "cpu");
	cpu_g_clk = clk_get_sys(NULL, "cpu_g");
	if (IS_ERR(cpu_clk))
		return PTR_ERR(cpu_clk);

	emc_clk = clk_get_sys("cpu", "emc");
	if (IS_ERR(emc_clk)) {
		clk_put(cpu_clk);
		return PTR_ERR(emc_clk);
	}

	clk_enable(emc_clk);
	clk_enable(cpu_clk);

	cpufreq_frequency_table_cpuinfo(policy, freq_table);
	cpufreq_frequency_table_get_attr(freq_table, policy->cpu);
	policy->cur = tegra_getspeed(policy->cpu);
	target_cpu_speed[policy->cpu] = policy->cur;

	/* FIXME: what's the actual transition time? */
	policy->cpuinfo.transition_latency = 50 * 1000;

	policy->shared_type = CPUFREQ_SHARED_TYPE_ALL;
	cpumask_copy(policy->related_cpus, cpu_possible_mask);

	if (policy->cpu == 0) {
		policy->max = get_cpu_freq_limit(policy->cpu);
		policy->min = get_cpu_freq_limit_min(policy->cpu);
		register_pm_notifier(&tegra_cpu_pm_notifier);
#if CPU_FREQ_DEBUG
		pr_info("tegra_cpu_init: restored cpu[%d]'s freq max=%u min=%u\n", policy->cpu, policy->max, policy->min);
#endif
	}

    /* restore saved cpu frequency */
    if (policy->cpu > 0) {
		policy->max = get_cpu_freq_limit(policy->cpu);
		policy->min = get_cpu_freq_limit_min(policy->cpu);
		tegra_update_cpu_speed(policy->max);

#if CPU_FREQ_DEBUG
		pr_info("tegra_cpu_init: restored cpu[%d]'s freq max=%u min=%u\n", policy->cpu, policy->max, policy->min);
#endif
	}

	return 0;
}

static int tegra_cpu_exit(struct cpufreq_policy *policy)
{
	cpufreq_frequency_table_cpuinfo(policy, freq_table);
	clk_disable(emc_clk);
	clk_put(emc_clk);
	clk_put(cpu_clk);
	return 0;
}

static int tegra_cpufreq_policy_notifier(
	struct notifier_block *nb, unsigned long event, void *data)
{
	int i, ret;
	struct cpufreq_policy *policy = data;

	if (event == CPUFREQ_NOTIFY) {
		ret = cpufreq_frequency_table_target(policy, freq_table,
			policy->max, CPUFREQ_RELATION_H, &i);
		policy_max_speed[policy->cpu] =
			ret ? policy->max : freq_table[i].frequency;
	}
	return NOTIFY_OK;
}

static struct notifier_block tegra_cpufreq_policy_nb = {
	.notifier_call = tegra_cpufreq_policy_notifier,
};

static struct freq_attr *tegra_cpufreq_attr[] = {
	&cpufreq_freq_attr_scaling_available_freqs,
#ifdef CONFIG_TEGRA_THERMAL_THROTTLE
	&throttle,
#endif
	NULL,
};

static struct cpufreq_driver tegra_cpufreq_driver = {
	.verify		= tegra_verify_speed,
	.target		= tegra_target,
	.get		= tegra_getspeed,
	.init		= tegra_cpu_init,
	.exit		= tegra_cpu_exit,
	.name		= "tegra",
	.attr		= tegra_cpufreq_attr,
};

#ifdef CONFIG_HAS_EARLYSUSPEND

static void tegra_cpufreq_early_suspend(struct early_suspend *h)
{
	// this is the last suspend handler
	pr_info("tegra_cpufreq_early_suspend: clean cpu freq boost\n");
	in_earlysuspend = false;
	pm_qos_update_request(&boost_cpu_freq_req, (s32)PM_QOS_CPU_FREQ_MIN_DEFAULT_VALUE);
	
	pr_info("tegra_cpufreq_early_suspend: cap cpu freq to %d\n", suspend_cap_freq);
	pm_qos_update_request(&cap_cpu_freq_req, (s32)suspend_cap_freq);
	
	if (suspend_cap_freq > T3_LP_MAX_FREQ) {
		pr_info("tegra_cpufreq_early_suspend: cap max cpu to %d\n", suspend_cap_cpu_num);
		pm_qos_update_request(&cap_cpu_num_req, (s32)suspend_cap_cpu_num);
	}
#if 0
	unsigned int scaling_min_freq;
	// TODO: if scaling_min_freq != 51000 is set this will 
	// disable deep sleep now
	// how can we overrule cpufreq?
	scaling_min_freq = get_cpu_freq_limit_min(0);
	if (scaling_min_freq != T3_CPU_MIN_FREQ) {
		pr_info("tegra_delayed_suspend_work: scaling_min_freq %d\n", scaling_min_freq);
	}
#endif
}

static void tegra_cpufreq_late_resume(struct early_suspend *h)
{
#ifdef CONFIG_TEGRA_CPUQUIET
	tegra_cpuquiet_force_gmode();
#endif	
	pr_info("tegra_cpufreq_late_resume: clean cpu freq cap\n");
	pm_qos_update_request(&cap_cpu_freq_req, (s32)PM_QOS_CPU_FREQ_MAX_DEFAULT_VALUE);

	if (suspend_cap_freq > T3_LP_MAX_FREQ) {
		pr_info("tegra_cpufreq_late_resume: clean max cpu cap\n");
		pm_qos_update_request(&cap_cpu_num_req, (s32)PM_QOS_MAX_ONLINE_CPUS_DEFAULT_VALUE);
	}

	// boost at the beginning of the resume
	pr_info("tegra_cpufreq_late_resume: boost cpu freq\n");
	tegra_update_cpu_speed(T3_CPU_FREQ_BOOST);
	// now disable all speed changes until finished
	in_earlysuspend = true;
	pm_qos_update_request(&boost_cpu_freq_req, (s32)T3_CPU_FREQ_BOOST);
}

static void tegra_cpufreq_performance_early_suspend(struct early_suspend *h)
{
	// this is the first suspend handler
	pr_info("tegra_cpufreq_performance_early_suspend: boost cpu freq\n");
	tegra_update_cpu_speed(T3_CPU_FREQ_BOOST);
	// now disable all speed changes until finished
	in_earlysuspend = true;
	pm_qos_update_request(&boost_cpu_freq_req, (s32)T3_CPU_FREQ_BOOST);	
}

static void tegra_cpufreq_performance_late_resume(struct early_suspend *h)
{
	// this is the last resume handler
	pr_info("tegra_cpufreq_performance_late_resume: clean cpu freq boost\n");
	in_earlysuspend = false;
	pm_qos_update_request(&boost_cpu_freq_req, (s32)PM_QOS_CPU_FREQ_MIN_DEFAULT_VALUE);
}

#endif
static void ril_suspend_resume_worker(struct work_struct *w)
{
#ifdef CONFIG_TEGRA_CPUQUIET
	// disable LP mode asap
	tegra_cpuquiet_force_gmode();
#endif

	pr_info("ril_suspend_resume_worker: clean cpu cap by RIL\n");
	pm_qos_update_request(&cap_cpu_freq_req, (s32)PM_QOS_CPU_FREQ_MAX_DEFAULT_VALUE);

	pr_info("ril_suspend_resume_worker: boost cpu freq by RIL\n");
	pm_qos_update_request(&boost_cpu_freq_req, (s32)tegra_get_suspend_boost_freq());
	tegra_update_cpu_speed(tegra_get_suspend_boost_freq());
}

static int __init tegra_cpufreq_init(void)
{
	int ret = 0;

	struct tegra_cpufreq_table_data *table_data =
		tegra_cpufreq_table_get();
	if (IS_ERR_OR_NULL(table_data))
		return -EINVAL;

	suspend_index = table_data->suspend_index;

	ret = tegra_throttle_init(&tegra_cpu_lock);
	if (ret)
		return ret;

	ret = tegra_auto_hotplug_init(&tegra_cpu_lock);
	if (ret)
		return ret;

	freq_table = table_data->freq_table;
	tegra_cpu_edp_init(false);

	INIT_WORK(&ril_suspend_resume_work, ril_suspend_resume_worker);
	
#ifdef CONFIG_HAS_EARLYSUSPEND
	pm_qos_add_request(&cap_cpu_freq_req, PM_QOS_CPU_FREQ_MAX, (s32)PM_QOS_CPU_FREQ_MAX_DEFAULT_VALUE);
	pm_qos_add_request(&cap_cpu_num_req, PM_QOS_MAX_ONLINE_CPUS, (s32)PM_QOS_MAX_ONLINE_CPUS_DEFAULT_VALUE);
	pm_qos_add_request(&boost_cpu_freq_req, PM_QOS_CPU_FREQ_MIN, (s32)PM_QOS_CPU_FREQ_MIN_DEFAULT_VALUE);		
	
	// will cap frequency on screen off
	tegra_cpufreq_early_suspender.suspend = tegra_cpufreq_early_suspend;
	tegra_cpufreq_early_suspender.resume = tegra_cpufreq_late_resume;
	tegra_cpufreq_early_suspender.level = EARLY_SUSPEND_LEVEL_DISABLE_FB + 100;
	register_early_suspend(&tegra_cpufreq_early_suspender);

	// to get max boost for the complete suspend and resume time
	tegra_cpufreq_performance_early_suspender.suspend = tegra_cpufreq_performance_early_suspend;
	tegra_cpufreq_performance_early_suspender.resume = tegra_cpufreq_performance_late_resume;
	tegra_cpufreq_performance_early_suspender.level = 0;
	register_early_suspend(&tegra_cpufreq_performance_early_suspender);
#endif


	ret = cpufreq_register_notifier(
		&tegra_cpufreq_policy_nb, CPUFREQ_POLICY_NOTIFIER);
	if (ret)
		return ret;

	return cpufreq_register_driver(&tegra_cpufreq_driver);
}

static void __exit tegra_cpufreq_exit(void)
{
	tegra_throttle_exit();
	tegra_cpu_edp_exit();
	tegra_auto_hotplug_exit();
#ifdef CONFIG_HAS_EARLYSUSPEND
	pm_qos_remove_request(&cap_cpu_freq_req);
	pm_qos_remove_request(&cap_cpu_num_req);
	pm_qos_remove_request(&boost_cpu_freq_req);
			
	unregister_early_suspend(&tegra_cpufreq_performance_early_suspender);
	unregister_early_suspend(&tegra_cpufreq_early_suspender);
#endif
	cpufreq_unregister_driver(&tegra_cpufreq_driver);
	cpufreq_unregister_notifier(
		&tegra_cpufreq_policy_nb, CPUFREQ_POLICY_NOTIFIER);
}


MODULE_AUTHOR("Colin Cross <ccross@android.com>");
MODULE_DESCRIPTION("cpufreq driver for Nvidia Tegra2");
MODULE_LICENSE("GPL");
module_init(tegra_cpufreq_init);
module_exit(tegra_cpufreq_exit);
