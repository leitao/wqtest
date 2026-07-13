# SPDX-License-Identifier: GPL-2.0
#
# Kbuild for the out-of-tree workqueue self-test modules.  Consumed by the
# kernel build system when invoked as `make -C $(KDIR) M=$(PWD) modules`.

ccflags-y += -I$(src)

obj-m += wqt_01_basic.o
obj-m += wqt_02_ordered.o
obj-m += wqt_03_max_active.o
obj-m += wqt_04_delayed.o
obj-m += wqt_05_cancel.o
obj-m += wqt_06_mem_reclaim.o
obj-m += wqt_07_queue_on_cpu.o
obj-m += wqt_08_flags_matrix.o
obj-m += wqt_09_torture.o
obj-m += wqt_10_perf.o
