/* drivers/misc/lowmemorykiller.c
 *
 * The lowmemorykiller driver lets user-space specify a set of memory thresholds
 * where processes with a range of oom_adj values will get killed. Specify the
 * minimum oom_adj values in /sys/module/lowmemorykiller/parameters/adj and the
 * number of free pages in /sys/module/lowmemorykiller/parameters/minfree. Both
 * files take a comma separated list of numbers in ascending order.
 *
 * For example, write "0,8" to /sys/module/lowmemorykiller/parameters/adj and
 * "1024,4096" to /sys/module/lowmemorykiller/parameters/minfree to kill processes
 * with a oom_adj value of 8 or higher when the free memory drops below 4096 pages
 * and kill processes with a oom_adj value of 0 or higher when the free memory
 * drops below 1024 pages.
 *
 * The driver considers memory used for caches to be free, but if a large
 * percentage of the cached memory is locked this can be very inaccurate
 * and processes may not get killed until the normal oom killer is triggered.
 *
 * Copyright (C) 2007-2008 Google, Inc.
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

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/oom.h>
#include <linux/sched.h>
#include <linux/notifier.h>
#include <linux/slab.h>

#define LOWMEM_ADJ_SLOTS 12
static uint32_t lowmem_debug_level = 2;
static int lowmem_adj[LOWMEM_ADJ_SLOTS] = {
	0,
	1,
	6,
	12,
};
static int lowmem_adj_size = 4;
static size_t lowmem_minfree[LOWMEM_ADJ_SLOTS] = {
	3 * 512,	/* 6MB */
	2 * 1024,	/* 8MB */
	4 * 1024,	/* 16MB */
	16 * 1024,	/* 64MB */
};
static int lowmem_minfree_size = 4;

static uint32_t lowmem_multiplier = 36;
static int lowmem_oldmethod = 0;

static struct task_struct *lowmem_deathpending;
static unsigned long lowmem_deathpending_timeout;

DEFINE_SPINLOCK(lowmem_lock);

#define PAGESZ_KB (PAGE_SIZE / 1024)

#define lowmem_print(level, x...)			\
	do {						\
		if (lowmem_debug_level >= (level))	\
			printk(x);			\
	} while (0)

static int
task_notify_func(struct notifier_block *self, unsigned long val, void *data);

static struct notifier_block task_nb = {
	.notifier_call	= task_notify_func,
};

static int
task_notify_func(struct notifier_block *self, unsigned long val, void *data)
{
	struct task_struct *task = data;

	if (task == lowmem_deathpending)
		lowmem_deathpending = NULL;

	return NOTIFY_OK;
}

static int lowmem_shrink(struct shrinker *s, int nr_to_scan, gfp_t gfp_mask)
{
	struct task_struct *p;
	struct task_struct *selected = NULL;
	int rem = 0;
	int tasksize;
	int i, banner = 0;
	int min_adj = OOM_ADJUST_MAX + 1;
	int selected_tasksize = 0;
	int selected_oom_adj;
	int array_size = ARRAY_SIZE(lowmem_adj);
	int other_free = global_page_state(NR_FREE_PAGES);
	int other_file = global_page_state(NR_FILE_PAGES) -
						global_page_state(NR_SHMEM);
	unsigned lowmem_delta = (1048576 * 1024) / PAGE_SIZE; /* 1GB system RAM */
	int *ooms_seen = kzalloc(sizeof(int) * 20, GFP_ATOMIC);

	/*
	 * If we already have a death outstanding, then
	 * bail out right away; indicating to vmscan
	 * that we have nothing further to offer on
	 * this pass.
	 *
	 */
	if (lowmem_deathpending &&
	    time_before_eq(jiffies, lowmem_deathpending_timeout))
		return 0;

	if (lowmem_adj_size < array_size)
		array_size = lowmem_adj_size;
	if (lowmem_minfree_size < array_size)
		array_size = lowmem_minfree_size;
	for (i = 0; i < array_size; i++) {
		if (other_free < lowmem_minfree[i] &&
		    other_file < lowmem_minfree[i]) {
			min_adj = lowmem_adj[i];
			break;
		}
	}
	if (nr_to_scan > 0)
		lowmem_print(3, "lowmem_shrink %d, mask %X, ofree %d ofile %d, min_adj %d\n",
			     nr_to_scan, gfp_mask, other_free, other_file,
			     min_adj);
	rem = global_page_state(NR_ACTIVE_ANON) +
		global_page_state(NR_ACTIVE_FILE) +
		global_page_state(NR_INACTIVE_ANON) +
		global_page_state(NR_INACTIVE_FILE);
	if (nr_to_scan <= 0 || min_adj == OOM_ADJUST_MAX + 1) {
		lowmem_print(5, "lowmem_shrink %d, %x, return %d\n",
			     nr_to_scan, gfp_mask, rem);
		return rem;
	}
	selected_oom_adj = min_adj;

	spin_lock(&lowmem_lock);
	for_each_process(p) {
		struct mm_struct *mm;
		struct signal_struct *sig;
		int oom_adj;

		task_lock(p);
		mm = p->mm;
		sig = p->signal;
		if (!mm || !sig) {
			task_unlock(p);
			continue;
		}
		oom_adj = sig->oom_adj;

		lowmem_print(5, "oom_adj for pid %d: %d\n", p->pid, oom_adj);
		if (ooms_seen && (oom_adj > -1))
			ooms_seen[oom_adj]++;

		if (oom_adj < min_adj) {
			task_unlock(p);
			continue;
		}
		tasksize = get_mm_rss(mm);
		task_unlock(p);
		if (tasksize <= 0)
			continue;
		if (selected) {
			if (oom_adj < selected_oom_adj)
				continue;

			if (lowmem_oldmethod) {
				if (oom_adj == selected_oom_adj &&
				    tasksize <= selected_tasksize)
					continue;
			} else {
				unsigned delta = abs((nr_to_scan * lowmem_multiplier) - tasksize);
				lowmem_print(3, "%s: l_delta %u delta %u nr_to_scan * mult %u tasksize %u oom_adj %d\n",
					__func__, lowmem_delta, delta,
					nr_to_scan * lowmem_multiplier, tasksize, oom_adj);
				if ((oom_adj == selected_oom_adj) && (delta > lowmem_delta))
					continue;
				if (delta <= lowmem_delta)
					lowmem_delta = delta;
			}
		}
		selected = p;
		selected_tasksize = tasksize;
		selected_oom_adj = oom_adj;
		if (!banner++) {
			int i;
			lowmem_print(2, "NTS:%7luK MA:%3d MFs:",
				nr_to_scan * PAGESZ_KB, min_adj);
			for (i = 0; i < max(lowmem_minfree_size, lowmem_adj_size); i++) {
				lowmem_print(2, "%3d:%6luK",
				i < lowmem_adj_size ? lowmem_adj[i] : -1,
				i < lowmem_minfree_size ? lowmem_minfree[i] * PAGESZ_KB : 0);
			}
			lowmem_print(2, "\n");
		};
		lowmem_print(2, "select %d (%s), adj %d, size %d (%luK), to kill\n",
			     p->pid, p->comm, oom_adj, tasksize, tasksize * PAGESZ_KB);
	}
	if (selected) {
		lowmem_print(1, "send sigkill to %d (%s), adj %d, size %d (%luK)\n",
			     selected->pid, selected->comm,
			     selected_oom_adj, selected_tasksize,
			     selected_tasksize * PAGESZ_KB);
		lowmem_deathpending = selected;
		lowmem_deathpending_timeout = jiffies + HZ;
		force_sig(SIGKILL, selected);
		rem -= selected_tasksize;
	}
	lowmem_print(4, "lowmem_shrink %d, %x, return %d\n",
		     nr_to_scan, gfp_mask, rem);
	if (ooms_seen) {
		int i;
		lowmem_print(3, "ooms seen: ");
		for (i = 0; i < 20; i++)
			if (ooms_seen[i])
				lowmem_print(3, "%2d:%-2d ",
				  i, ooms_seen[i]);
		lowmem_print(3, "\n");
	};
	kfree(ooms_seen);
	spin_unlock(&lowmem_lock);
	return rem;
}

static struct shrinker lowmem_shrinker = {
	.shrink = lowmem_shrink,
	.seeks = DEFAULT_SEEKS * 16
};

static int __init lowmem_init(void)
{
	task_free_register(&task_nb);
	register_shrinker(&lowmem_shrinker);
	return 0;
}

static void __exit lowmem_exit(void)
{
	unregister_shrinker(&lowmem_shrinker);
	task_free_unregister(&task_nb);
}

module_param_named(cost, lowmem_shrinker.seeks, int, S_IRUGO | S_IWUSR);
module_param_array_named(adj, lowmem_adj, int, &lowmem_adj_size,
			 S_IRUGO | S_IWUSR);
module_param_array_named(minfree, lowmem_minfree, uint, &lowmem_minfree_size,
			 S_IRUGO | S_IWUSR);
module_param_named(debug_level, lowmem_debug_level, uint, S_IRUGO | S_IWUSR);
module_param_named(multiplier, lowmem_multiplier, uint, S_IRUGO | S_IWUSR);
module_param_named(old_method, lowmem_oldmethod, int, S_IRUGO| S_IWUSR);

module_init(lowmem_init);
module_exit(lowmem_exit);

MODULE_LICENSE("GPL");
