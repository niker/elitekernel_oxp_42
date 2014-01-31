/*
 * arch/arm/mach-tegra/cpuquiet.c
 *
 * Cpuquiet driver for Tegra3 CPUs
 *
 * Copyright (c) 2012 NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/sched.h>
#include <linux/cpufreq.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/cpu.h>
#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/pm_qos_params.h>
#include <linux/cpuquiet.h>
#include <linux/earlysuspend.h>

#include "pm.h"
#include "cpu-tegra.h"
#include "clock.h"
#include "tegra_pmqos.h"

#define CPUQUIET_DEBUG_VERBOSE 0

extern unsigned int best_core_to_turn_up(void);

#define INITIAL_STATE		TEGRA_CPQ_IDLE
#define LP_UP_DELAY_MS_DEF			80
#define LP_DOWN_DELAY_MS_DEF		800

static struct mutex *tegra3_cpu_lock;
static struct workqueue_struct *cpuquiet_wq;
static struct delayed_work cpuquiet_work;
static struct work_struct minmax_work;
static struct work_struct cpu_core_state_work;
static struct kobject *tegra_auto_sysfs_kobject;

static bool is_suspended = false;
static bool no_lp;
static bool enable;
static unsigned int lp_up_delay = LP_UP_DELAY_MS_DEF;
static unsigned int lp_down_delay = LP_DOWN_DELAY_MS_DEF;
static unsigned int idle_top_freq;
static bool manual_hotplug = false;
static unsigned int cpusallowed = 0;
// core 0 is always active
unsigned int cpu_core_state[3] = {0, 0, 0};

#ifdef CONFIG_HAS_EARLYSUSPEND
struct early_suspend tegra_cpuquiet_early_suspender;
#endif
static bool screen_on = true;

static struct clk *cpu_clk;
static struct clk *cpu_g_clk;
static struct clk *cpu_lp_clk;

static cputime64_t lp_on_time;
static unsigned int min_cpus = 1;
static unsigned int max_cpus = CONFIG_NR_CPUS;

static DEFINE_MUTEX(hotplug_lock);

int tegra_cpuquiet_force_gmode(void);

#define CPUQUIET_TAG                       "[CPUQUIET]: "

static bool log_hotplugging = false;
#define hotplug_info(msg...) do { \
	if (log_hotplugging) pr_info("[CPUQUIET]: " msg); \
	} while (0)

enum {
	TEGRA_CPQ_DISABLED = 0,
	TEGRA_CPQ_IDLE,
	TEGRA_CPQ_SWITCH_TO_LP,
	TEGRA_CPQ_SWITCH_TO_G,
};

static inline unsigned int num_cpu_check(unsigned int num)
{
	if (num > CONFIG_NR_CPUS)
		return CONFIG_NR_CPUS;
	if (num < 1)
		return 1;
	return num;
}

unsigned inline int tegra_cpq_max_cpus(void)
{
	unsigned int max_cpus_qos = pm_qos_request(PM_QOS_MAX_ONLINE_CPUS);	
	unsigned int num = min(max_cpus_qos, max_cpus);
	return num_cpu_check(num);
}

unsigned inline int tegra_cpq_min_cpus(void)
{
	unsigned int min_cpus_qos = pm_qos_request(PM_QOS_MIN_ONLINE_CPUS);
	unsigned int num = max(min_cpus_qos, min_cpus);
	return num_cpu_check(num);
}

static inline bool lp_possible(void)
{
	return !is_lp_cluster() && !no_lp && !(tegra_cpq_min_cpus() >= 2) && num_online_cpus() == 1;
}

static inline int switch_clk_to_gmode(void)
{
	/* if needed set rate to max of LP mode to make sure G mode switch is ok */
	if (clk_get_rate(cpu_clk) < idle_top_freq * 1000)
		clk_set_rate(cpu_clk, idle_top_freq * 1000);
	return clk_set_parent(cpu_clk, cpu_g_clk);
}

static inline int switch_clk_to_lpmode(void)
{
	/* this is expected to fail if the current freq is to high
	 for LP mode - but we never want to force LP mode */
	return clk_set_parent(cpu_clk, cpu_lp_clk);
}

static inline void show_status(const char* extra, cputime64_t on_time, int cpu)
{
	if(on_time)
		hotplug_info("%s Mask=[%d.%d%d%d%d]|lp_on_time=%llu\n",
    		extra, is_lp_cluster(), ((is_lp_cluster() == 1) ? 0 : cpu_online(0)),
        	cpu_online(1), cpu_online(2), cpu_online(3), on_time);
	else		
		if(cpu>0)
			hotplug_info("%s %d Mask=[%d.%d%d%d%d]\n",
    			extra, cpu, is_lp_cluster(), ((is_lp_cluster() == 1) ? 0 : cpu_online(0)),
        		cpu_online(1), cpu_online(2), cpu_online(3));

		else
			hotplug_info("%s Mask=[%d.%d%d%d%d]\n",
    			extra, is_lp_cluster(), ((is_lp_cluster() == 1) ? 0 : cpu_online(0)),
        		cpu_online(1), cpu_online(2), cpu_online(3));
}

static int cpq_state;

static int update_core_config(unsigned int cpunumber, bool up)
{
	int ret = -EINVAL;
	unsigned int nr_cpus = num_online_cpus();
	int max_cpus = tegra_cpq_max_cpus();
	int min_cpus = tegra_cpq_min_cpus();

#if CPUQUIET_DEBUG_VERBOSE
	pr_info(CPUQUIET_TAG "%s\n", __func__);
#endif
				
	if (cpq_state == TEGRA_CPQ_DISABLED || cpunumber >= nr_cpu_ids)
		return ret;

	/* sync with tegra_cpuquiet_work_func 
	 else if we are currently switching to LP and an up
	 comes we can end up with more then 1 core up and
	 governor stopped and !lp mode */
    if (!mutex_trylock (&hotplug_lock)){
#if CPUQUIET_DEBUG_VERBOSE
		pr_info(CPUQUIET_TAG "%s failed to get hotplug_lock\n", __func__);
#endif
        return -EBUSY;
	}
			
	if (up) { 
		/* disable hotplugging based force g-mode on screen off */
		if(is_lp_cluster()){
			if (screen_on){
				show_status("LP -> off - hotplug", 1, -1);
				tegra_cpuquiet_force_gmode();
			} else {
				mutex_unlock(&hotplug_lock);
				return -EBUSY;
			}
		}
		if (nr_cpus < max_cpus){
			show_status("UP", 0, cpunumber);
			ret = cpu_up(cpunumber);
		}
	} else {
		if (nr_cpus > 1 && nr_cpus > min_cpus){
			show_status("DOWN", 0, cpunumber);
			ret = cpu_down(cpunumber);
		}
	}

	mutex_unlock(&hotplug_lock);
			
	return ret;
}

static int tegra_quiesence_cpu(unsigned int cpunumber)
{
	return update_core_config(cpunumber, false);
}

static int tegra_wake_cpu(unsigned int cpunumber)
{
	return update_core_config(cpunumber, true);
}

static struct cpuquiet_driver tegra_cpuquiet_driver = {
	.name                   = "tegra",
	.quiesence_cpu          = tegra_quiesence_cpu,
	.wake_cpu               = tegra_wake_cpu,
};

static void tegra_cpuquiet_work_func(struct work_struct *work)
{
	cputime64_t on_time = 0;

#if CPUQUIET_DEBUG_VERBOSE
	pr_info(CPUQUIET_TAG "%s\n", __func__);
#endif

	if (!mutex_trylock (&hotplug_lock)){
#if CPUQUIET_DEBUG_VERBOSE
		pr_info(CPUQUIET_TAG "%s failed to get hotplug_lock\n", __func__);
#endif
		return;
	}

	mutex_lock(tegra3_cpu_lock);
	
	switch(cpq_state) {
		case TEGRA_CPQ_DISABLED:
		case TEGRA_CPQ_IDLE:
			break;
		case TEGRA_CPQ_SWITCH_TO_G:
			if (is_lp_cluster()) {
				if (!switch_clk_to_gmode()) {
					on_time = ktime_to_ms(ktime_get()) - lp_on_time;
					show_status("LP -> off", on_time, -1);
					/*catch-up with governor target speed */
					tegra_cpu_set_speed_cap(NULL);
				} else
					pr_err(CPUQUIET_TAG "tegra_cpuquiet_work_func - switch_clk_to_gmode failed\n");				
			}
#if CPUQUIET_DEBUG_VERBOSE
			else
				pr_info(CPUQUIET_TAG "skipping queued TEGRA_CPQ_SWITCH_TO_G - cond failed\n");
#endif
			break;
		case TEGRA_CPQ_SWITCH_TO_LP:
			if (lp_possible()) {
				if (!switch_clk_to_lpmode()) {
					show_status("LP -> on", 0, -1);
					/*catch-up with governor target speed*/
					tegra_cpu_set_speed_cap(NULL);
					lp_on_time = ktime_to_ms(ktime_get());
				}
#if CPUQUIET_DEBUG_VERBOSE
				else
					pr_info(CPUQUIET_TAG "skipping queued TEGRA_CPQ_SWITCH_TO_LP - switch_clk_to_lpmode failed\n");
#endif
			}
#if CPUQUIET_DEBUG_VERBOSE
			else
				pr_info(CPUQUIET_TAG "skipping queued TEGRA_CPQ_SWITCH_TO_LP - cond failed\n");
#endif			
			break;
		default:
			pr_err(CPUQUIET_TAG "%s: invalid tegra hotplug state %d\n",
		       __func__, cpq_state);
	}
	
	mutex_unlock(tegra3_cpu_lock);

	mutex_unlock(&hotplug_lock);
}

static void min_max_constraints_workfunc(struct work_struct *work)
{
	int count = -1;
	bool up = false;
	unsigned int cpu;

	int nr_cpus = num_online_cpus();
	int max_cpus = tegra_cpq_max_cpus();
	int min_cpus = tegra_cpq_min_cpus();
	
	if (cpq_state == TEGRA_CPQ_DISABLED)
		return;

	if (is_lp_cluster())
		return;

	if (nr_cpus < min_cpus) {
		up = true;
		count = min_cpus - nr_cpus;
	} else if (nr_cpus > max_cpus && max_cpus >= min_cpus) {
		count = nr_cpus - max_cpus;
	}

	for (;count > 0; count--) {
		if (up) {
			cpu = best_core_to_turn_up();
			if (cpu < nr_cpu_ids){
				show_status("UP", 0, cpu);
				cpu_up(cpu);
			}
			else
				break;
		} else {
			cpu = cpumask_next(0, cpu_online_mask);
			if (cpu < nr_cpu_ids){
				show_status("DOWN", 0, cpu);
				cpu_down(cpu);
			}
			else
				break;
		}
	}
}

static void min_cpus_change(void)
{
	bool g_cluster = false;
    cputime64_t on_time = 0;
	
	if (cpq_state == TEGRA_CPQ_DISABLED)
		return;

	mutex_lock(tegra3_cpu_lock);

	if ((tegra_cpq_min_cpus() >= 2) && is_lp_cluster()) {
		if (switch_clk_to_gmode()){
			pr_err(CPUQUIET_TAG "min_cpus_change - switch_clk_to_gmode failed\n");
			mutex_unlock(tegra3_cpu_lock);
			return;
		}
		
		on_time = ktime_to_ms(ktime_get()) - lp_on_time;
		show_status("LP -> off - min_cpus_change", on_time, -1);

		g_cluster = true;
	}

	tegra_cpu_set_speed_cap(NULL);
	mutex_unlock(tegra3_cpu_lock);

	schedule_work(&minmax_work);
}

static int min_cpus_notify(struct notifier_block *nb, unsigned long n, void *p)
{
	pr_info(CPUQUIET_TAG "PM QoS PM_QOS_MIN_ONLINE_CPUS %lu\n", n);

	if (n < 1 || n > CONFIG_NR_CPUS)
		return NOTIFY_OK;

	if (manual_hotplug){
		return NOTIFY_OK;
	}

	min_cpus_change();

	return NOTIFY_OK;
}

static void max_cpus_change(void)
{	
	if (cpq_state == TEGRA_CPQ_DISABLED)
		return;

	if (tegra_cpq_max_cpus() < num_online_cpus())
		schedule_work(&minmax_work);
}

static int max_cpus_notify(struct notifier_block *nb, unsigned long n, void *p)
{
	pr_info(CPUQUIET_TAG "PM QoS PM_QOS_MAX_ONLINE_CPUS %lu\n", n);

	if (n < 1)
		return NOTIFY_OK;

	if (manual_hotplug){
		return NOTIFY_OK;
	}

	max_cpus_change();

	return NOTIFY_OK;
}

int tegra_cpuquiet_force_gmode(void)
{
    cputime64_t on_time = 0;

	if (no_lp)
		return -EBUSY;
		
	if (!is_g_cluster_present())
		return -EBUSY;

	if (cpq_state == TEGRA_CPQ_DISABLED)
		return -EBUSY;

	if (is_lp_cluster()) {
		mutex_lock(tegra3_cpu_lock);

		if (switch_clk_to_gmode()) {
			pr_err(CPUQUIET_TAG "tegra_cpuquiet_force_gmode - switch_clk_to_gmode failed\n");
    		mutex_unlock(tegra3_cpu_lock);
    		return -EBUSY;
		}
		
		on_time = ktime_to_ms(ktime_get()) - lp_on_time;
		show_status("LP -> off - force", on_time, -1);

    	mutex_unlock(tegra3_cpu_lock);
	}
	
	return 0;
}

int tegra_cpuquiet_force_gmode_locked(void)
{
    cputime64_t on_time = 0;

	if (no_lp)
		return -EBUSY;
		
	if (!is_g_cluster_present())
		return -EBUSY;

	if (cpq_state == TEGRA_CPQ_DISABLED)
		return -EBUSY;

	if (is_lp_cluster()) {
		if (switch_clk_to_gmode()) {
			pr_err(CPUQUIET_TAG "tegra_cpuquiet_force_gmode - switch_clk_to_gmode failed\n");
    		return -EBUSY;
		}
		
		on_time = ktime_to_ms(ktime_get()) - lp_on_time;
		show_status("LP -> off - force", on_time, -1);
	}
	return 0;
}

void tegra_cpuquiet_set_no_lp(bool value)
{
	if (value)
		tegra_cpuquiet_force_gmode();

	no_lp = value;
}

void tegra_auto_hotplug_governor(unsigned int cpu_freq, bool suspend)
{
	if (!is_g_cluster_present())
		return;

	if (cpq_state == TEGRA_CPQ_DISABLED)
		return;

	cpq_state = TEGRA_CPQ_IDLE;
	is_suspended = suspend;
	
	if (suspend) {
		return;
	}

	if (is_lp_cluster() && 
			(cpu_freq > idle_top_freq || no_lp)) {
       	cpq_state = TEGRA_CPQ_SWITCH_TO_G;
		queue_delayed_work(cpuquiet_wq, &cpuquiet_work, msecs_to_jiffies(lp_up_delay));
	} else if (cpu_freq <= idle_top_freq && lp_possible()) {
		cpq_state = TEGRA_CPQ_SWITCH_TO_LP;
		if (queue_delayed_work(cpuquiet_wq, &cpuquiet_work, msecs_to_jiffies(lp_down_delay)))
#if CPUQUIET_DEBUG_VERBOSE
        	pr_info(CPUQUIET_TAG "qeued TEGRA_CPQ_SWITCH_TO_LP\n");		
#else
			;
#endif
	}
}

static struct notifier_block min_cpus_notifier = {
	.notifier_call = min_cpus_notify,
};

static struct notifier_block max_cpus_notifier = {
	.notifier_call = max_cpus_notify,
};

static void enable_callback(struct cpuquiet_attribute *attr)
{
	int disabled = -1;

	mutex_lock(tegra3_cpu_lock);

	if (!enable && cpq_state != TEGRA_CPQ_DISABLED) {
		disabled = 1;
		cpq_state = TEGRA_CPQ_DISABLED;
	} else if (enable && cpq_state == TEGRA_CPQ_DISABLED) {
		disabled = 0;
		cpq_state = TEGRA_CPQ_IDLE;
		tegra_cpu_set_speed_cap(NULL);
	}

	mutex_unlock(tegra3_cpu_lock);

	if (disabled == -1)
		return;

	pr_info(CPUQUIET_TAG "enable=%d\n", enable);				
	
	if (disabled == 1) {
		cancel_delayed_work_sync(&cpuquiet_work);
		pr_info(CPUQUIET_TAG "enable_callback: clusterswitch disabled\n");
		cpuquiet_device_busy();
	} else if (!disabled) {
		pr_info(CPUQUIET_TAG "enable_callback: clusterswitch enabled\n");
		cpuquiet_device_free();
	}
}

static ssize_t show_min_cpus(struct cpuquiet_attribute *cattr, char *buf)
{
	char *out = buf;
		
	out += sprintf(out, "%d\n", min_cpus);

	return out - buf;
}

static ssize_t store_min_cpus(struct cpuquiet_attribute *cattr,
					const char *buf, size_t count)
{
	int ret;
	unsigned int n;
	
	ret = sscanf(buf, "%d", &n);

	if ((ret != 1) || n < 1 || n > CONFIG_NR_CPUS)
		return -EINVAL;

	if (manual_hotplug)
		return -EBUSY;
	
	min_cpus = n;
	min_cpus_change();

	pr_info(CPUQUIET_TAG "min_cpus=%d\n", min_cpus);				
	return count;
}

static ssize_t show_max_cpus(struct cpuquiet_attribute *cattr, char *buf)
{
	char *out = buf;
		
	out += sprintf(out, "%d\n", max_cpus);

	return out - buf;
}

static ssize_t store_max_cpus(struct cpuquiet_attribute *cattr,
					const char *buf, size_t count)
{
	int ret;
	unsigned int n;
	
	ret = sscanf(buf, "%d", &n);

	if ((ret != 1) || n < 1 || n > CONFIG_NR_CPUS)
		return -EINVAL;

	if (manual_hotplug)
		return -EBUSY;

	max_cpus = n;	
	max_cpus_change();

	pr_info(CPUQUIET_TAG "max_cpus=%d\n", max_cpus);			
	return count;
}

static ssize_t show_no_lp(struct cpuquiet_attribute *cattr, char *buf)
{
	char *out = buf;
	
	out += sprintf(out, "%d\n", no_lp);

	return out - buf;
}

static ssize_t store_no_lp(struct cpuquiet_attribute *cattr,
					const char *buf, size_t count)
{
	int ret;
	unsigned int n;

	ret = sscanf(buf, "%d", &n);

	if ((ret != 1) || n < 0 || n > 1)
		return -EINVAL;

	if (no_lp == n)
		return count;
	
	if (n)
		tegra_cpuquiet_force_gmode();

	no_lp = n;	

	pr_info(CPUQUIET_TAG "no_lp=%d\n", no_lp);	
	return count;
}

static unsigned int tegra_cpuquiet_get_manual_hotplug(void)
{
	return manual_hotplug;
}

static void set_manual_hotplug(unsigned int mode)
{
	if (manual_hotplug == mode)
		return;
     
	manual_hotplug = mode;	

	pr_info(CPUQUIET_TAG "manual_hotplug=%d\n", manual_hotplug);
		
	// stop governor
	if (manual_hotplug) {
		cancel_delayed_work_sync(&cpuquiet_work);
		cpuquiet_device_busy();
		schedule_work(&cpu_core_state_work);
	} else {
		cpuquiet_device_free();
	}	    
}

static ssize_t show_manual_hotplug(struct cpuquiet_attribute *cattr, char *buf)
{
	char *out = buf;
		
	out += sprintf(out, "%d\n", manual_hotplug);

	return out - buf;
}

static ssize_t store_manual_hotplug(struct cpuquiet_attribute *cattr,
					const char *buf, size_t count)
{
	int ret;
	unsigned int n;
		
	ret = sscanf(buf, "%d", &n);

	if ((ret != 1) || n < 0 || n > 1)
		return -EINVAL;

	set_manual_hotplug(n);
	return count;
}

static void cpu_core_state_workfunc(struct work_struct *work)
{
	int i = 0;
	int cpu = 0;

	for (i = 0; i < 3; i++){
		cpu = i + 1;
		if (cpu_core_state[i] == 0 && cpu_online(cpu)){
			show_status("DOWN", 0, cpu);
			cpu_down(cpu);
		} else if (cpu_core_state[i] == 1 && !cpu_online(cpu)){
			if (is_lp_cluster())
				tegra_cpuquiet_force_gmode();
			
			show_status("UP", 0, cpu);
			cpu_up(cpu);
		}
	}
}

static void set_cpu_core_state(unsigned int new_cpu_core_state_user[3])
{
	cpu_core_state[0]=new_cpu_core_state_user[0];
	cpu_core_state[1]=new_cpu_core_state_user[1];
	cpu_core_state[2]=new_cpu_core_state_user[2];

	if (manual_hotplug)
		schedule_work(&cpu_core_state_work);

	pr_info(CPUQUIET_TAG "cpu_core_state=%u %u %u\n", cpu_core_state[0], cpu_core_state[1], cpu_core_state[2]);
}

static ssize_t show_cpu_core_state(struct cpuquiet_attribute *cattr, char *buf)
{
	char *out = buf;
		
	out += sprintf(out, "%u %u %u\n", cpu_core_state[0], cpu_core_state[1], cpu_core_state[2]);

	return out - buf;
}

static ssize_t store_cpu_core_state(struct cpuquiet_attribute *cattr,
					const char *buf, size_t count)
{
	int ret;
	unsigned int cpu_core_state_user[3] = {0, 0, 0};
	int i = 0;

	ret = sscanf(buf, "%u %u %u", &cpu_core_state_user[0], &cpu_core_state_user[1],
		&cpu_core_state_user[2]);

	if (ret < 3)
		return -EINVAL;

	for (i = 0; i < 3; i++){
		if (cpu_core_state_user[i] < 0 || cpu_core_state_user[i] > 1)
			return -EINVAL;
	}

	set_cpu_core_state(cpu_core_state_user);
		    
	return count;
}

static ssize_t show_log_hotplugging(struct cpuquiet_attribute *cattr, char *buf)
{
	char *out = buf;
		
	out += sprintf(out, "%d\n", log_hotplugging);

	return out - buf;
}

static ssize_t store_log_hotplugging(struct cpuquiet_attribute *cattr,
					const char *buf, size_t count)
{
	int ret;
	unsigned int n;
		
	ret = sscanf(buf, "%d", &n);

	if ((ret != 1) || n < 0 || n > 1)
		return -EINVAL;

	log_hotplugging = n;	
	return count;
}

CPQ_BASIC_ATTRIBUTE(lp_up_delay, 0644, uint);
CPQ_BASIC_ATTRIBUTE(lp_down_delay, 0644, uint);
CPQ_ATTRIBUTE(enable, 0644, bool, enable_callback);
CPQ_ATTRIBUTE_CUSTOM(min_cpus, 0644, show_min_cpus, store_min_cpus);
CPQ_ATTRIBUTE_CUSTOM(max_cpus, 0644, show_max_cpus, store_max_cpus);
CPQ_ATTRIBUTE_CUSTOM(no_lp, 0644, show_no_lp, store_no_lp);
CPQ_ATTRIBUTE_CUSTOM(manual_hotplug, 0644, show_manual_hotplug, store_manual_hotplug);
CPQ_ATTRIBUTE_CUSTOM(cpu_core_state, 0644, show_cpu_core_state, store_cpu_core_state);
CPQ_ATTRIBUTE_CUSTOM(log_hotplugging, 0644, show_log_hotplugging, store_log_hotplugging);

static struct attribute *tegra_auto_attributes[] = {
	&no_lp_attr.attr,
	&lp_up_delay_attr.attr,
	&lp_down_delay_attr.attr,
	&enable_attr.attr,
	&min_cpus_attr.attr,
	&max_cpus_attr.attr,
	&manual_hotplug_attr.attr,
	&cpu_core_state_attr.attr,
	&log_hotplugging_attr.attr,
	NULL,
};

static const struct sysfs_ops tegra_auto_sysfs_ops = {
	.show = cpuquiet_auto_sysfs_show,
	.store = cpuquiet_auto_sysfs_store,
};

static struct kobj_type ktype_sysfs = {
	.sysfs_ops = &tegra_auto_sysfs_ops,
	.default_attrs = tegra_auto_attributes,
};

static int tegra_auto_sysfs(void)
{
	int err;

	tegra_auto_sysfs_kobject = kzalloc(sizeof(*tegra_auto_sysfs_kobject),
					GFP_KERNEL);

	if (!tegra_auto_sysfs_kobject)
		return -ENOMEM;

	err = cpuquiet_kobject_init(tegra_auto_sysfs_kobject, &ktype_sysfs,
				"tegra_cpuquiet");

	if (err)
		kfree(tegra_auto_sysfs_kobject);

	return err;
}


/* cpusallowed interface in /sys/class/misc 
   for CoreManager app */
static ssize_t cpusallowed_status_read(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sprintf(buf,"%u\n", cpusallowed);
}

static ssize_t cpusallowed_status_write(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	unsigned int data;
	unsigned int cpu_core_state_user[3] = {0, 0, 0};
	
	if (sscanf(buf, "%u\n", &data) == 1){
		cpusallowed = data;
		
		if (cpusallowed == 0){
			set_manual_hotplug(0);
			return size;
		}
		
		if (!tegra_cpuquiet_get_manual_hotplug())
			set_manual_hotplug(1);
		
		if (cpusallowed == 2){
			cpu_core_state_user[0] = 0;
			cpu_core_state_user[1] = 0;
			cpu_core_state_user[2] = 1;
		} else if (cpusallowed == 3){
			cpu_core_state_user[0] = 0;
			cpu_core_state_user[1] = 1;
			cpu_core_state_user[2] = 1;
		} else if (cpusallowed == 4){
			cpu_core_state_user[0] = 1;
			cpu_core_state_user[1] = 1;
			cpu_core_state_user[2] = 1;
		}
		set_cpu_core_state(cpu_core_state_user);
	}
	else
		pr_info("%s: input error\n", __FUNCTION__);

	return size;
}

static DEVICE_ATTR(cpusallowed, S_IRUGO | S_IWUGO, cpusallowed_status_read, cpusallowed_status_write);

static struct attribute *cpusallowed_attributes[] = {
	&dev_attr_cpusallowed.attr,
	NULL
};

static struct attribute_group cpusallowed_group = {
	.attrs  = cpusallowed_attributes,
};

static struct miscdevice cpusallowed_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "cpusallowed",
};

#ifdef CONFIG_HAS_EARLYSUSPEND
static void tegra_cpuquiet_early_suspend(struct early_suspend *h)
{
	screen_on = false;
}

static void tegra_cpuquiet_late_resume(struct early_suspend *h)
{
	screen_on = true;
}
#endif

int tegra_auto_hotplug_init(struct mutex *cpu_lock)
{
	int err;

	cpu_clk = clk_get_sys(NULL, "cpu");
	cpu_g_clk = clk_get_sys(NULL, "cpu_g");
	cpu_lp_clk = clk_get_sys(NULL, "cpu_lp");

	if (IS_ERR(cpu_clk) || IS_ERR(cpu_g_clk) || IS_ERR(cpu_lp_clk))
		return -ENOENT;

	idle_top_freq = T3_LP_MAX_FREQ;
	pr_info(CPUQUIET_TAG "%s: idle_top_freq = %d\n", __func__, idle_top_freq);
	
	/*
	 * Not bound to the issuer CPU (=> high-priority), has rescue worker
	 * task, single-threaded, freezable.
	 */
	cpuquiet_wq = alloc_workqueue(
		"cpuquiet", WQ_UNBOUND | WQ_RESCUER | WQ_FREEZABLE, 1);

	if (!cpuquiet_wq)
		return -ENOMEM;

	INIT_DELAYED_WORK(&cpuquiet_work, tegra_cpuquiet_work_func);
	INIT_WORK(&minmax_work, min_max_constraints_workfunc);
	INIT_WORK(&cpu_core_state_work, cpu_core_state_workfunc);

	tegra3_cpu_lock = cpu_lock;

	cpq_state = INITIAL_STATE;
	enable = cpq_state == TEGRA_CPQ_DISABLED ? false : true;

	pr_info(CPUQUIET_TAG "%s: initialized: %s\n", __func__,
		(cpq_state == TEGRA_CPQ_DISABLED) ? "disabled" : "enabled");

	if (pm_qos_add_notifier(PM_QOS_MIN_ONLINE_CPUS, &min_cpus_notifier))
		pr_err(CPUQUIET_TAG "%s: Failed to register min cpus PM QoS notifier\n",
			__func__);
	if (pm_qos_add_notifier(PM_QOS_MAX_ONLINE_CPUS, &max_cpus_notifier))
		pr_err(CPUQUIET_TAG "%s: Failed to register max cpus PM QoS notifier\n",
			__func__);

#ifdef CONFIG_HAS_EARLYSUSPEND	
	tegra_cpuquiet_early_suspender.suspend = tegra_cpuquiet_early_suspend;
	tegra_cpuquiet_early_suspender.resume = tegra_cpuquiet_late_resume;
	tegra_cpuquiet_early_suspender.level = EARLY_SUSPEND_LEVEL_DISABLE_FB + 100;
	register_early_suspend(&tegra_cpuquiet_early_suspender);
#endif

	err = cpuquiet_register_driver(&tegra_cpuquiet_driver);
	if (err) {
		destroy_workqueue(cpuquiet_wq);
		return err;
	}

	err = tegra_auto_sysfs();
	if (err)
		goto error;

	// sysfs interface for misc cpusallowed
	err = misc_register(&cpusallowed_device);
	if (err) {
		pr_err(CPUQUIET_TAG "%s: misc_register(%s) fail\n", __func__,
				cpusallowed_device.name);
		goto error;
	}
	if (sysfs_create_group(&cpusallowed_device.this_device->kobj,
				&cpusallowed_group) < 0) {
		pr_err(CPUQUIET_TAG "%s: Failed to create sysfs group for device (%s)!\n",
				__func__, cpusallowed_device.name);
		goto error;
	}

	return err;
	
error:
	cpuquiet_unregister_driver(&tegra_cpuquiet_driver);
	destroy_workqueue(cpuquiet_wq);

	return err;
}

void tegra_auto_hotplug_exit(void)
{
#ifdef CONFIG_HAS_EARLYSUSPEND
	unregister_early_suspend(&tegra_cpuquiet_early_suspender);
#endif

	destroy_workqueue(cpuquiet_wq);
	cpuquiet_unregister_driver(&tegra_cpuquiet_driver);
	kobject_put(tegra_auto_sysfs_kobject);
}
