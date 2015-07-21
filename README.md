# unit_perf

## Introduction
It is a performance measurement tool as the perf supplement.

As we know, the perf is one good tool to find the performance bottleneck. But sometimes it is not good enough to profile our own codes especially the codes is only one part of whole system. In this case, we may don't care about the cost of the whole system. As a result, the unwanted results from other parts of system may occupies the most of top list, even we could not get the result we care really.  

Now you cold use this unit_perf to get the performance of the codes you specify the aid codes clearly. Maybe they are hooks, functions, even some lines of codes.  

And it provides some other help macros to debug issues. For example, you could use UP_PID_INFO_LOG to pring the log when current pid equals the specific pid by /proc/unit_perf/monitor_pid  

And you could get the result or reset it by proc

## Usage  
###When you want to find the bottleneck in you codes, you could use the monitor point to check it.    
1. Use up_add_monitor to add the monitor point name;    
ATTENSION: It should be invoked in process/thread context. Because it will allocate memory with GFP_KERNEL    
2. Invoke the up_start_monitor when reach the monitor point;    
3. Invoke the up_end_monitor after the monitor point;    
ATTENTION: The monitor name is the index of unit perf, so you should keep it consistent.  
4. Check the result:    
cat /proc/unit_perf/top_list;  
5. Reset the result if necessary  
cat /proc/unit_perf/reset_result  
6. Use up_remove_monitor to remove the monitor point name.  
7. Use /proc/unit_perf/monitor_pid to set the monitor pid which could filter the logs.  


NOTE: You could use UP_AUTO_START_FUNC_MONITOR and UP_AUTO_END_FUNC_MONITOR to avoid fill the function name by yourself.

###When you want to check the performance of one function, you could use up_func_once to get it.  
1. Create one function whose signature is like up_test_func (Defined in unit_perf.h);  
2. Invoke the up_func_once(_preempt/bh/irq) according to your requirement.  
3. Check the result by dmesg  

### Example
Assume you want to check the performance of __nf_conntrack_alloc.  
1. Invoke the up_add_monitor("__nf_conntrack_alloc") in nf_conntrack_init_net;  
2. Invoke the up_start_monitor("__nf_conntrack_alloc") at the entry of __nf_conntrack_alloc;  
3. Invoke the up_end_monitor("__nf_conntrack_alloc") at the exit of __nf_conntrack_alloc;  
4. Invoke the up_remove_monitor("__nf_conntrack_alloc") in nf_conntrack_cleanup_net;  

Note: Actuall you use any name as the monitor name when check the performance of __nf_conntrack_alloc.  

## How to integrate it into kernel
### As a dynamic module
It is very easy. Just make it and insmod it.
But it means the kernel core codes could not use the unit_perf unless you modify the kernel codes.
In this case, you only use the unit_perf with another dynamic module  

### As the kernel core 
1. Because current unit_perf uses the x86 insturction "rdtscll", so I put the unit_perf.c into the arch/x86/unit_perf/, and put the unit_perf.h into the include/linux/  
2. Modify the arch/x86/Kbuild  
	
	+obj-$(CONFIG_UNIT_PERF) += unit_perf/
3. Modify the arch/x86/Kconfig.debug
	
	+config UNIT_PERF
	+       bool "Unit performance profile"  
	+       default n  
	+       ---help---  
	+         Enable the unit perf funciton. You could specify the codes which you want to monitor.  
4. Add one Makefile in arch/x86/unit_perf/  
	
	+  
	+obj-$(CONFIG_UNIT_PERF) := unit_perf.o  
	+  
Now you could use the unit_perf everywhere.













