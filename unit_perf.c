#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <net/net_namespace.h>
#include <net/netns/generic.h>
#include <linux/spinlock.h>
#include <linux/jhash.h>
#include <linux/sort.h>


MODULE_LICENSE("GPL");
MODULE_AUTHOR("Feng Gao <gfree.wind@gmail.com>");
MODULE_DESCRIPTION("unit_perf: Used to profile the specific codes");
MODULE_ALIAS("Unit Perf");

/**********************************************************************************************/
#define UNIT_PERF_DIR_NAME				"unit_perf"
struct proc_dir_entry *unit_perf_dir = NULL;
#define UNIT_PERF_TOP_LIST				"top_list"
struct proc_dir_entry *unit_perf_top_proc = NULL;
#define UNIT_PERF_RESET_RESULT			"reset_result"
struct proc_dir_entry *unit_perf_reset_proc = NULL;

struct cpu_cost_stats {
	unsigned long long start;
	unsigned long long cost;
};

#define UNIT_PERF_MONITOR_NAME_SIZE		(32)
struct monitor_stats {
	struct list_head next;
	char name[UNIT_PERF_MONITOR_NAME_SIZE];
	struct cpu_cost_stats __percpu *cost_stats;
};

struct monitor_result {
	char name[UNIT_PERF_MONITOR_NAME_SIZE];
	unsigned long long cost;
};

#define UNIT_PERF_SLOT_CNT				(128)
#define UNIT_PERF_SLOT_MASK				(UNIT_PERF_SLOT_CNT-1)

struct unit_perf_monitor {
	struct list_head list[UNIT_PERF_SLOT_CNT];
	spinlock_t 		 list_lock[UNIT_PERF_SLOT_CNT];
	u32  monitor_cnt;
};

struct unit_perf_monitor *g_up_monitor;
typedef void (*up_test_func) (void *);

/**********************************************************************************************/
static void insert_up_monitor_point(struct unit_perf_monitor *monitor, const char *name);
static void remove_up_monitor_point(struct unit_perf_monitor *monitor, const char *name);

static struct cpu_cost_stats * get_monitor_pointer_stats(struct unit_perf_monitor *monitor, const char *name);
static struct unit_perf_monitor * unit_perf_monitor_alloc(void);
static void unit_perf_monitor_free(struct unit_perf_monitor *monitor);

/**********************************************************************************************/

int up_add_monitor_point(const char *name)
{
	struct unit_perf_monitor *monitor;

	rcu_read_lock();
	monitor = rcu_dereference(g_up_monitor);
	if (monitor) {
		insert_up_monitor_point(monitor, name);
	}
	
	rcu_read_unlock();

	return 0;
}
EXPORT_SYMBOL(up_add_monitor_point);

void up_remove_monitor_point(const char *name)
{
	struct unit_perf_monitor *monitor;

	rcu_read_lock();
	monitor = rcu_dereference(g_up_monitor);
	if (monitor) {
		remove_up_monitor_point(monitor, name);
	}
	
	rcu_read_unlock();
}
EXPORT_SYMBOL(up_remove_monitor_point);

void up_start_monitor(const char *name)
{
	struct unit_perf_monitor *monitor;

	rcu_read_lock();
	monitor = rcu_dereference(g_up_monitor);
	if (monitor) {
		struct cpu_cost_stats *cost_stats = NULL;

		cost_stats = get_monitor_pointer_stats(monitor, name);
		
		if (cost_stats) {
			rdtscll(cost_stats->start);
		}
	}
	rcu_read_unlock();
}
EXPORT_SYMBOL(up_start_monitor);

void up_end_monitor(const char *name)
{
	struct unit_perf_monitor *monitor;
	unsigned long long end_time;

	rdtscll(end_time);	

	rcu_read_lock();
	monitor = rcu_dereference(g_up_monitor);
	if (monitor) {
		struct cpu_cost_stats *cost_stats = NULL;

		cost_stats = get_monitor_pointer_stats(monitor, name);
		/* Check the cost_stats->start to avoid there is one replace monitor during start and end */
		if (cost_stats && cost_stats->start) {
			cost_stats->cost += (end_time-cost_stats->start);
		}
	}
	rcu_read_unlock();
}
EXPORT_SYMBOL(up_end_monitor);

void up_func_once(const char *name, up_test_func cb, void *data)
{
	unsigned long long start, end;

	rdtscll(start);
	cb(data);
	rdtscll(end);

	printk(KERN_INFO "%s costs %llu cycles", name, end-start);
}
EXPORT_SYMBOL(up_func_once);

void up_func_once_preempt(const char *name, up_test_func cb, void *data)
{
	preempt_disable();
	up_func_once(name, cb, data);
	preempt_enable();
}
EXPORT_SYMBOL(up_func_once_preempt);

void up_func_once_bh(const char *name, up_test_func cb, void *data)
{
	local_bh_disable();
	up_func_once(name, cb, data);
	local_bh_enable();
}
EXPORT_SYMBOL(up_func_once_bh);

void up_func_once_irq(const char *name, up_test_func cb, void *data)
{
	local_irq_disable();
	up_func_once(name, cb, data);
	local_irq_enable();
}
EXPORT_SYMBOL(up_func_once_irq);

static u32 get_point_index(const char *name)
{
#define UNIT_PERF_HASH_SEED		(*((u32*)"uphs"))

	u32 value = jhash(name, strlen(name), UNIT_PERF_HASH_SEED);

	return (value&UNIT_PERF_SLOT_MASK);
}

/*
Should be protected by rcu_read_lock
*/
static struct cpu_cost_stats * get_monitor_pointer_stats(struct unit_perf_monitor *monitor, const char *name)
{	
	struct monitor_stats *pos;
	bool find = false;
	u32 index = get_point_index(name);	

	list_for_each_entry_rcu(pos, monitor->list+index, next) {
		if (0 == strcmp(pos->name, name)) {
			find = true;
			break;
		}
	}
	if (find) {
		return per_cpu_ptr(pos->cost_stats, smp_processor_id());
	} else {
		printk(KERN_ERR "Fail to find the monitor point(%s)", name);
		return NULL;
	}
}

static void insert_up_monitor_point(struct unit_perf_monitor *monitor, const char *name)
{
	struct monitor_stats *pos;
	bool find = false;
	u32 index = get_point_index(name);	
	
	spin_lock(monitor->list_lock+index);
	
	list_for_each_entry_rcu(pos, monitor->list+index, next) {
		if (0 == strcmp(pos->name, name)) {
			find = true;
			break;
		}
	}
	
	if (find) {
		printk(KERN_ERR "There is one duplicated monitor point already");
	} else {
		pos = kmalloc(sizeof(*pos), GFP_ATOMIC);
		if (pos) {
			memset(pos, 0, sizeof(*pos));
			INIT_LIST_HEAD(&pos->next);
			strncpy(pos->name, name, sizeof(pos->name)-1);
			pos->cost_stats = alloc_percpu(struct cpu_cost_stats);
			if (pos->cost_stats) {
				list_add_rcu(&pos->next, monitor->list+index);
				++monitor->monitor_cnt;
				printk(KERN_INFO "Add the new monitor point(%s)", name);
			} else {
				printk(KERN_ERR "Fail to allocate cpu stats for %s", name);
			}
		} else {
			printk(KERN_ERR "Fail to allocate new monitor point");
		}
	}
	
	spin_unlock(monitor->list_lock+index);
}

/*
Should protected by rcu lock
*/
static void remove_up_monitor_point(struct unit_perf_monitor *monitor, const char *name)
{
	struct monitor_stats *pos;
	bool find = false;
	u32 index = get_point_index(name);	
		
	list_for_each_entry_rcu(pos, monitor->list+index, next) {
		if (0 == strcmp(pos->name, name)) {
			find = true;
			break;
		}
	}
	
	if (find) {
		spin_lock(monitor->list_lock+index);
		list_del_rcu(&pos->next);
		monitor->monitor_cnt--;
		spin_unlock(monitor->list_lock+index);

		synchronize_rcu();
		if (pos->cost_stats) {
			free_percpu(pos->cost_stats);
		}
		kfree(pos);
		
	} else {
		printk(KERN_INFO "There is no %s monitor point", name);
	}
}


static struct unit_perf_monitor * unit_perf_monitor_alloc(void)
{
	struct unit_perf_monitor *monitor;
	u32 i;

	monitor = kmalloc(sizeof(*monitor), GFP_KERNEL);
	if (!monitor) {
		return NULL;
	}
	memset(monitor, 0, sizeof(*monitor));

	for (i = 0; i < UNIT_PERF_SLOT_CNT; ++i) {
		INIT_LIST_HEAD(monitor->list+i);
		spin_lock_init(monitor->list_lock+i);
	}

	return monitor;
}

static void unit_perf_monitor_free(struct unit_perf_monitor *monitor)
{
	if (monitor) {
		struct monitor_stats *pos, *next;
		u32 i;
		
		for (i = 0; i < UNIT_PERF_SLOT_CNT; ++i) {
			list_for_each_entry_safe(pos, next, monitor->list, next) {
				list_del(&pos->next);
				if (pos->cost_stats) {
					free_percpu(pos->cost_stats);
				}
				kfree(pos);
			}
		}
	}
	
}


static void *up_generic_seq_start(struct seq_file *s, loff_t *pos)
{
	return 0 == *pos ? pos : NULL;
}

static void *up_generic_seq_next(struct seq_file *s, void *v, loff_t *pos)
{
	return NULL;
}

static void up_generic_seq_stop(struct seq_file *s, void *v)
{
}

static void get_total_cpu_stats(struct monitor_result *result, struct monitor_stats *stats)
{
	struct cpu_cost_stats *cost_stats;
	u32 cpu;

	strcpy(result->name, stats->name);

	for_each_online_cpu(cpu) {
		cost_stats = per_cpu_ptr(stats->cost_stats, cpu);
		result->cost += cost_stats->cost;
	}
}

static int monitor_result_reverse_cmp(const void *a, const void *b)
{
	const struct monitor_result *s1 = a;
	const struct monitor_result *s2 = b;

	if (s1->cost < s2->cost) {
		return 1;
	} else if (s1->cost > s2->cost) {
		return -1;
	} else {
		return 0;
	}	
}

static void monitor_result_swap(void *a, void *b, int size)
{	
	struct monitor_result *s1 = a;
	struct monitor_result *s2 = b;
	struct monitor_result tmp = *s1;

	*s1 = *s2;
	*s2 = tmp;
}

static int up_top_seq_show(struct seq_file *s, void *v)
{
	struct unit_perf_monitor *monitor;

	seq_printf(s, "Top List: \n");

	rcu_read_lock();
	monitor = rcu_dereference(g_up_monitor);
	if (monitor && monitor->monitor_cnt) {
		u32 result_cnt = monitor->monitor_cnt;
		struct monitor_result *result = kmalloc(sizeof(*result)*result_cnt, GFP_KERNEL);
		
		printk(KERN_INFO "There are %u monitor point", result_cnt);
		seq_printf(s, "There are %u monitor points\n", result_cnt);
		
		if (result) {
			u32 i;			
			u32 copy = 0;
			
			memset(result, 0, sizeof(*result)*result_cnt);

			for (i = 0; i < UNIT_PERF_SLOT_CNT; ++i) {
				struct monitor_stats *pos;

				list_for_each_entry_rcu(pos, monitor->list+i, next) {					
					if (copy == result_cnt) {
						goto sort_show;
					}
					get_total_cpu_stats(result+copy, pos);
					++copy;
				}
			}

sort_show:
			sort(result, result_cnt, sizeof(*result), 
				monitor_result_reverse_cmp,
				monitor_result_swap);

			for (i = 0; i < result_cnt; ++i) {
				seq_printf(s, "%s %llu\n", result[i].name, result[i].cost);
			}
			
		} else {
			printk(KERN_ERR "Fail to allocate result memory");
		}
	} else {
		seq_printf(s, "No monitor point\n");
	}

	rcu_read_unlock();

	return 0;
}

static int up_reset_seq_show(struct seq_file *s, void *v)
{
	struct unit_perf_monitor *monitor;

	seq_printf(s, "Reset the stats of monitor stats\n");

	monitor = unit_perf_monitor_alloc();
	if (monitor) {
		struct unit_perf_monitor *old_monitor;

		rcu_read_lock();
		old_monitor = rcu_dereference(g_up_monitor);
		if (old_monitor) {
			u32 i;
			for (i = 0; i < UNIT_PERF_SLOT_CNT; ++i) {
				struct monitor_stats *pos;

				list_for_each_entry_rcu(pos, old_monitor->list+i, next) {
					insert_up_monitor_point(monitor, pos->name);
				}
			}				
		}
		rcu_read_unlock();

		//Replace the old one
		rcu_assign_pointer(g_up_monitor, monitor);
		synchronize_rcu();

		unit_perf_monitor_free(old_monitor);
	} else {
		seq_printf(s, "Fail to allocate monitor");
	}

	return 0;
}


static const struct seq_operations up_top_seq_ops = {
    .start = up_generic_seq_start,
    .next  = up_generic_seq_next,
    .stop  = up_generic_seq_stop,
    .show  = up_top_seq_show
};

static const struct seq_operations up_reset_seq_ops = {
    .start = up_generic_seq_start,
    .next  = up_generic_seq_next,
    .stop  = up_generic_seq_stop,
    .show  = up_reset_seq_show
};

static int up_top_proc_open(struct inode *inode, struct file *file)
{
    return seq_open(file, &up_top_seq_ops);
}

static int up_reset_proc_open(struct inode *inode, struct file *file)
{
    return seq_open(file, &up_reset_seq_ops);
}

static const struct file_operations up_top_proc_fops = {
    .owner   = THIS_MODULE,
    .open    = up_top_proc_open,
    .read    = seq_read,
    .llseek  = seq_lseek,
    .release = seq_release
};

static const struct file_operations up_reset_proc_fops = {
    .owner   = THIS_MODULE,
    .open    = up_reset_proc_open,
    .read    = seq_read,
    .llseek  = seq_lseek,
    .release = seq_release
};

static int __init unit_perf_init(void)
{
	int ret = -ENOENT;

	printk(KERN_INFO "unit_perf init");

	unit_perf_dir = proc_mkdir(UNIT_PERF_DIR_NAME, NULL);
	if (!unit_perf_dir) {
		printk(KERN_ERR "Fail to create unit_perf proc dir");
		goto err1;
	}
	unit_perf_top_proc = proc_create_data(UNIT_PERF_TOP_LIST, 0400, unit_perf_dir,
		&up_top_proc_fops, NULL);
	if (!unit_perf_top_proc) {
		printk(KERN_ERR "Fail to craete the unit_perf top file");
		goto err2;
	}
	unit_perf_reset_proc = proc_create_data(UNIT_PERF_RESET_RESULT, 0400, unit_perf_dir,
		&up_reset_proc_fops, NULL);
	if (!unit_perf_reset_proc) {
		printk(KERN_ERR "Fail to create the unit_perf reset file");
		goto err3;
	}
	g_up_monitor = unit_perf_monitor_alloc();
	if (!g_up_monitor) {
		ret = -ENOMEM;
		printk(KERN_ERR "Fail to init unit_perf monitor");
		goto err4;
	}

	printk(KERN_INFO "Unit Perf is OK now");
	return 0;

err4:
	remove_proc_entry(UNIT_PERF_RESET_RESULT, unit_perf_dir);
err3:
	remove_proc_entry(UNIT_PERF_TOP_LIST, unit_perf_dir);
err2:
	remove_proc_entry(UNIT_PERF_DIR_NAME, NULL);
err1:
	return ret;
}

static void __exit unit_perf_exit(void)
{	
	struct unit_perf_monitor *monitor = g_up_monitor;
	rcu_assign_pointer(g_up_monitor, NULL);

	synchronize_rcu();

	unit_perf_monitor_free(monitor);

	remove_proc_entry(UNIT_PERF_RESET_RESULT, unit_perf_dir);
	remove_proc_entry(UNIT_PERF_TOP_LIST, unit_perf_dir);
	remove_proc_entry(UNIT_PERF_DIR_NAME, NULL);
	printk(KERN_INFO "unit_perf exit");
}


module_init(unit_perf_init);
module_exit(unit_perf_exit);

