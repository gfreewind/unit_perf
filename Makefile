obj-m := unit_perf.o

KDIR := /lib/modules/`uname -r`/build

modules:
	make -C $(KDIR) M=$(PWD) modules

clean:
	@rm -rf *.ko *.o *.mod.o *.mod.c .symvers .tmp_versions  modules.order  Module.symvers
