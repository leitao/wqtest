# SPDX-License-Identifier: GPL-2.0
#
# Out-of-tree build for the workqueue self-test suite.
#
#   make                # build all wqt_*.ko against $(KDIR)
#   make clean          # remove build artifacts
#   make run            # build, then boot virtme-ng and run the suite
#
# Override the kernel tree / arch on the command line, e.g.:
#   make KDIR=/path/to/linux ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-

KDIR          ?= /home/leit/Devel/linux-next
ARCH          ?= x86_64
CROSS_COMPILE ?=

PWD := $(CURDIR)

all: modules

modules:
	$(MAKE) -C $(KDIR) M=$(PWD) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) ARCH=$(ARCH) clean
	$(RM) results.tap

run: modules
	KDIR=$(KDIR) ARCH=$(ARCH) ./run.sh

.PHONY: all modules clean run
