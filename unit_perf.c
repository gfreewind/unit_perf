#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <net/net_namespace.h>
#include <net/netns/generic.h>
#include <linux/spinlock.h>
#include <linux/jhash.h>
#include <linux/sort.h>
#include <asm/div64.h>
#include <asm/uaccess.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Feng Gao <gfree.wind@gmail.com>");
MODULE_DESCRIPTION("unit_perf: Used to profile the specific codes");
MODULE_ALIAS("Unit Perf");

/*********************************  Platform Selection ********************************************/
#define UNIT_PERF_X86

/**********************************************************************************************/
#define TEST_UNIT_PERF

#define UNIT_PERF_DIR_NAME				"unit_perf"
struct proc_dir_entry *unit_perf_dir = NULL;
#define UNIT_PERF_TOP_LIST				"top_list"
struct proc_dir_entry *unit_perf_top_proc = NULL;
#define UNIT_PERF_RESET_RESULT				"reset_result"
struct proc_dir_entry *unit_perf_reset_proc = NULL;
#define UNIT_PERF_MONITOR_PID				"monitor_pid"
struct proc_dir_entry *unit_perf_mpid_proc = NULL;
unsigned long g_up_monitor_pid __read_mostly = 0;

struct cpu_cost_stats {
	unsigned long long start;
	unsigned long long cost;
	unsigned long long overflow;
	unsigned long long call_times;
};

#define UNIT_PERF_MONITOR_NAME_SIZE		(32)
struct monitor_stats {
	char name[UNIT_PERF_MONITOR_NAME_SIZE];
	struct cpu_cost_stats __percpu *cost_stats;
};

struct monitor_result {
	char name[UNIT_PERF_MONITOR_NAME_SIZE];
	unsigned long long overflow;
	unsigned long long cost;
	unsigned long long call_times;
	unsigned long long average;
};

#define UNIT_PERF_SLOT_CNT				(128)

struct unit_perf_monitor {
	struct monitor_stats monitors[UNIT_PERF_SLOT_CNT];
	u32  monitor_cnt;
	spinlock_t lock;
};

struct unit_perf_monitor *g_up_monitor;
typedef void (*up_test_func) (void *);

/**********************************************************************************************/
static int insert_monitor(struct unit_perf_monitor *monitor, const char *name);
static void remove_monitor(struct unit_perf_monitor *monitor, int md);

static struct unit_perf_monitor * unit_perf_monitor_alloc(void);
static void unit_perf_monitor_free(struct unit_perf_monitor *monitor);


#ifdef UNIT_PERF_X86
#define UP_GET_CPU_CYCLES(x)					rdtscll((x))	
#endif

/**********************************************************************************************/
int up_add_monitor(const char *name)
{
	struct unit_perf_monitor *monitor;
	int ret = -1;

	rcu_read_lock();
	monitor = rcu_dereference(g_up_monitor);
	if (monitor) {
		ret = insert_monitor(monitor, name);
	}
	
	rcu_read_unlock();

	return ret;
}
EXPORT_SYMBOL(up_add_monitor);

void up_remove_monitor(int md)
{
	struct unit_perf_monitor *monitor;

	rcu_read_lock();
	monitor = rcu_dereference(g_up_monitor);
	if (monitor) {
		remove_monitor(monitor, md);
	}
	rcu_read_unlock();
}
EXPORT_SYMBOL(up_remove_monitor);

void up_start_monitor(int md)
{
	struct unit_perf_monitor *monitor;

	if (unlikely(md >= UNIT_PERF_SLOT_CNT)) {
		return;
	}

	rcu_read_lock();
	monitor = rcu_dereference(g_up_monitor);
	if (likely(monitor)) {
		struct cpu_cost_stats *cost_stats = per_cpu_ptr(monitor->monitors[md].cost_stats,
			smp_processor_id());
		UP_GET_CPU_CYCLES(cost_stats->start);
	}	
	rcu_read_unlock();
}
EXPORT_SYMBOL(up_start_monitor);

void up_end_monitor(int md)
{
	struct unit_perf_monitor *monitor;
	unsigned long long end_time;

	if (unlikely(md >= UNIT_PERF_SLOT_CNT)) {
		return;
	}

	UP_GET_CPU_CYCLES(end_time);	

	rcu_read_lock();
	monitor = rcu_dereference(g_up_monitor);
	if (likely(monitor)) {
		struct cpu_cost_stats *cost_stats = per_cpu_ptr(monitor->monitors[md].cost_stats,
			smp_processor_id());

		if (cost_stats->start) {
			unsigned long long old_cost = cost_stats->cost;
			unsigned long long cost = end_time-cost_stats->start;

			cost_stats->cost += cost;
			cost_stats->start = 0;
			cost_stats->call_times++;

			if (cost_stats->cost < old_cost) {
				//Overflow happens
				cost_stats->overflow++;
			}
		}
	}
	rcu_read_unlock();
}
EXPORT_SYMBOL(up_end_monitor);

void up_func_once(const char *name, up_test_func cb, void *data)
{
	unsigned long long start, end;

	UP_GET_CPU_CYCLES(start);
	cb(data);
	UP_GET_CPU_CYCLES(end);

	printk(KERN_INFO "%s costs %llu cycles\n", name, end-start);
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

static int insert_monitor(struct unit_perf_monitor *monitor, const char *name)
{
	struct cpu_cost_stats *stats;
	u32 cpu, i;

	if (monitor->monitor_cnt >= UNIT_PERF_SLOT_CNT) {
		pr_err("UnitPerf: Slots are full\n");
		return -1;
	}

	spin_lock(&monitor->lock);
	
	for (i = 0; i < UNIT_PERF_SLOT_CNT; ++i) {
		if ('\0' == monitor->monitors[i].name[0]) {
			break;
		}
	}
	if (i == UNIT_PERF_SLOT_CNT) {
		//it is full
		pr_err("UnitPerf: Slots are full\n");
		spin_unlock(&monitor->lock);
		return -1;
	}

	memset(monitor->monitors[i].name, 0, sizeof(monitor->monitors[i].name));
	for_each_online_cpu(cpu) {
		stats = per_cpu_ptr(monitor->monitors[i].cost_stats, cpu);
		memset(stats, 0, sizeof(*stats));
	}
	strncpy(monitor->monitors[i].name, name, sizeof(monitor->monitors[i].name)-1);
	monitor->monitor_cnt++;
	spin_unlock(&monitor->lock);

	return i;
}

/*
Should protected by rcu lock
*/
static void remove_monitor(struct unit_perf_monitor *monitor, int md)
{
	if (-1 == md || md >= UNIT_PERF_SLOT_CNT) {
		pr_err("UnitPerf: Invalid md\n");
		return;
	}

	spin_lock(&monitor->lock);
	if (monitor->monitors[md].name[0] != '\0') {
		monitor->monitors[md].name[0] = '\0';
		monitor->monitor_cnt--;
	} else {
		pr_err("UnitPerf: The monitor is removed already\n");
	}
	spin_unlock(&monitor->lock);	
}

static void unit_perf_monitor_free(struct unit_perf_monitor *monitor)
{
	if (monitor) {
		u32 i;
		
		for (i = 0; i < UNIT_PERF_SLOT_CNT; ++i) {
			free_percpu(monitor->monitors[i].cost_stats);
		}

		kfree(monitor);
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

	spin_lock_init(&monitor->lock);

	for (i = 0; i < UNIT_PERF_SLOT_CNT; ++i) {
		monitor->monitors[i].cost_stats = alloc_percpu(struct cpu_cost_stats);
		if (!monitor->monitors[i].cost_stats) {
			goto error;
		}
	}

	return monitor;

error:
	unit_perf_monitor_free(monitor);
	return NULL;
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
		unsigned long long old_cost = result->cost;

		cost_stats = per_cpu_ptr(stats->cost_stats, cpu);

		result->call_times += cost_stats->call_times;
		result->overflow += cost_stats->overflow;
		result->cost += cost_stats->cost;
		if (result->cost < old_cost) {
			result->overflow++;
		}

		if (result->call_times) {
			result->average = cost_stats->cost;
			do_div(result->average, result->call_times);
		} else {
			result->average = 0;
		}
	}
}

static int monitor_result_reverse_cmp(const void *a, const void *b)
{
	const struct monitor_result *s1 = a;
	const struct monitor_result *s2 = b;

	if (s1->name[0] == '\0') {
		return 1;
	} else if (s2->name [0] == '\0') {
		return -1;
	}

	/* Compare the overflow firstly */
	if (s1->overflow < s2->overflow) {
		return 1;
	} else if (s1->overflow > s2->overflow) {
		return -1;
	} else {		
		if (s1->cost < s2->cost) {
			return 1;
		} else if (s1->cost > s2->cost) {
			return -1;
		} else {
			return 0;
		}	
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

	rcu_read_lock();
	monitor = rcu_dereference(g_up_monitor);
	if (monitor && monitor->monitor_cnt) {
		u32 result_cnt = monitor->monitor_cnt;
		struct monitor_result *result = kmalloc(sizeof(*result)*result_cnt, GFP_KERNEL);
		
		if (result) {
			u32 i;			
			u32 copy = 0;
			
			memset(result, 0, sizeof(*result)*result_cnt);

			for (i = 0; i < UNIT_PERF_SLOT_CNT; ++i) {
				struct monitor_stats *pos;
				pos = monitor->monitors+i;

				if (pos->name[0] != '\0') {					
					get_total_cpu_stats(result+copy, pos);
					++copy;
				}

				if (copy >= result_cnt) {
					break;
				}
			}

			sort(result, result_cnt, sizeof(*result), 
				monitor_result_reverse_cmp,
				monitor_result_swap);

			seq_printf(s, "%-32s    %-10s    %-8s    %-22s    %-22s\n",
				"monitor", "call_times", "overflow", "total_costs", "average_cost");

			for (i = 0; i < result_cnt; ++i) {
				seq_printf(s, "%-32s    %-10llu    %-8llu    %-22llu    %-22llu\n", 
					result[i].name, result[i].call_times,
					result[i].overflow, result[i].cost, result[i].average);
			}
			kfree(result);
		} else {
			printk(KERN_ERR "Fail to allocate result memory\n");
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
			
			*monitor = *old_monitor;
			for (i = 0; i < UNIT_PERF_SLOT_CNT; ++i) {
				struct cpu_cost_stats *stats;
				u32 cpu;
				
				for_each_online_cpu(cpu) {
					stats = per_cpu_ptr(monitor->monitors[i].cost_stats, cpu);
					memset(stats, 0, sizeof(*stats));
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

static int up_show_monitor_pid(char *page, char **start, off_t offset,
	int count, int *eof, void *data)
{
	return snprintf(page, count, "%lu\n", g_up_monitor_pid);
}

static int up_store_monitor_pid(struct file *file, const char *buffer, 
	unsigned long count, void *data)
{
	char buf[32] = {0};
	unsigned long copy_bytes = sizeof(buf)-1;
	char *p = (char *)buf;

	if (copy_bytes > count) {
		copy_bytes = count;
	}

	if (copy_from_user(buf, buffer, copy_bytes)) {
		return count;
	}

	g_up_monitor_pid = simple_strtoul(p, &p, 10);

	printk(KERN_INFO "Unit Perf: The monitor pid is updated to %lu\n", g_up_monitor_pid);

	return copy_bytes;
}

#ifdef TEST_UNIT_PERF
static int g_test1, g_test2, g_test_monitor;
static void test_monitor(void)
{
	g_test1 = up_add_monitor("test1");
	g_test2 = up_add_monitor("test2");
	g_test_monitor = up_add_monitor("test_monitor");
	up_start_monitor(g_test_monitor);
	//UP_AUTO_START_FUNC_MONITOR();
	up_start_monitor(g_test1);
	up_start_monitor(g_test2);
	up_end_monitor(g_test1);
	up_end_monitor(g_test2);
	up_start_monitor(g_test1);
	up_start_monitor(g_test2);
	up_end_monitor(g_test1);
	up_end_monitor(g_test2);
	up_start_monitor(g_test1);
	up_start_monitor(g_test2);
	up_end_monitor(g_test1);
	up_end_monitor(g_test2);
	up_start_monitor(g_test1);
	up_start_monitor(g_test2);
	up_end_monitor(g_test1);
	up_end_monitor(g_test2);
	//UP_AUTO_END_FUNC_MONITOR();
}

static void remove_test_monitor(void)
{
	up_end_monitor(g_test_monitor);
	up_remove_monitor(g_test1);
	up_remove_monitor(g_test2);
	up_remove_monitor(g_test_monitor);
}
#endif


static int __init unit_perf_init(void)
{
	int ret = -ENOENT;

	printk(KERN_INFO "Unit Perf init\n");

	unit_perf_dir = proc_mkdir(UNIT_PERF_DIR_NAME, NULL);
	if (!unit_perf_dir) {
		printk(KERN_ERR "Fail to create unit_perf proc dir\n");
		goto err1;
	}

	unit_perf_top_proc = proc_create_data(UNIT_PERF_TOP_LIST, 0400, unit_perf_dir,
		&up_top_proc_fops, NULL);
	if (!unit_perf_top_proc) {
		printk(KERN_ERR "Fail to craete the unit_perf top file\n");
		goto err2;
	}
	unit_perf_reset_proc = proc_create_data(UNIT_PERF_RESET_RESULT, 0400, unit_perf_dir,
		&up_reset_proc_fops, NULL);
	if (!unit_perf_reset_proc) {
		printk(KERN_ERR "Fail to create the unit_perf reset file\n");
		goto err3;
	}
	unit_perf_mpid_proc = create_proc_entry(UNIT_PERF_MONITOR_PID, S_IFREG | S_IRUGO | S_IWUSR, unit_perf_dir);
	if (!unit_perf_mpid_proc) {
		printk(KERN_ERR "Fail to create the unit_perf monitor_pid file\n");
		goto err4;
	}
	unit_perf_mpid_proc->read_proc = up_show_monitor_pid;
	unit_perf_mpid_proc->write_proc = up_store_monitor_pid;
	unit_perf_mpid_proc->data = NULL;
	
	g_up_monitor = unit_perf_monitor_alloc();
	if (!g_up_monitor) {
		ret = -ENOMEM;
		printk(KERN_ERR "Fail to init unit_perf monitor\n");
		goto err5;
	}

	printk(KERN_INFO "Unit Perf is ready now\n");

#ifdef TEST_UNIT_PERF
	test_monitor();
#endif	
	return 0;

err5:
	remove_proc_entry(UNIT_PERF_MONITOR_PID, unit_perf_dir);
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

#ifdef TEST_UNIT_PERF
	remove_test_monitor();
#endif
	
	rcu_assign_pointer(g_up_monitor, NULL);

	synchronize_rcu();

	unit_perf_monitor_free(monitor);

	remove_proc_entry(UNIT_PERF_MONITOR_PID, unit_perf_dir);
	remove_proc_entry(UNIT_PERF_RESET_RESULT, unit_perf_dir);
	remove_proc_entry(UNIT_PERF_TOP_LIST, unit_perf_dir);
	remove_proc_entry(UNIT_PERF_DIR_NAME, NULL);
	printk(KERN_INFO "Unit Perf exit now\n");
}


module_init(unit_perf_init);
module_exit(unit_perf_exit);

