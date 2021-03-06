KMAK_FLAGS := \
	CONFIG_CLYDEFS_CORE_FS=m \
	KDIR=${KDIR}

ifneq ($(KERNELRELEASE),)
	EMBUNIT_DIR := ../../embunit
	EMBUNIT_SRC := $(EMBUNIT_DIR)/embUnit
	EMBUNIT_SAMPLES := $(EMBUNIT_DIR)/samples

	ccflags-y += -I$(src)/$(EMBUNIT_DIR)
	ccflags-y += -I$(src)/$(EMBUNIT_DIR)/wrap
	ccflags-y += -DNO_STDIO_PRINTF
	
	CFLAGS_blinktree.o := -DDEBUG
	obj-$(CONFIG_CLYDEFS_CORE_FS) := clydefscore.o
	
	clydefscore-y += core.o blinktree.o stack.o treeinterface.o blinktreeinterface.o

	#debugging build
	ifneq ($(CONFIG_CLYDEFS_CORE_DEBUG),)
		#test code
		clydefscore-y += test.o stacktests.o blinktreetests.o
		
		#embunit
		clydefscore-y += $(EMBUNIT_SRC)/AssertImpl.o
		clydefscore-y += $(EMBUNIT_SRC)/RepeatedTest.o
		clydefscore-y += $(EMBUNIT_SRC)/stdImpl.o
		clydefscore-y += $(EMBUNIT_SRC)/TestCaller.o
		clydefscore-y += $(EMBUNIT_SRC)/TestCase.o
		clydefscore-y += $(EMBUNIT_SRC)/TestResult.o
		clydefscore-y += $(EMBUNIT_SRC)/TestRunner.o
		clydefscore-y += $(EMBUNIT_SRC)/TestSuite.o
	endif
# out of kernel build
else
	KDIR ?= /lib/modules/$(shell uname -r)/build
	PWD := $(shell pwd)


default: prep
	echo "making clydefscore..."
	$(MAKE) -C "$(KDIR)" M="$(PWD)" $(KMAK_FLAGS) modules

prep:
	@test -r "$(KDIR)/.config" || { \
		echo "error: $(KDIR) sources are not configured." 1>&2; \
		false; \
	}
	@test -r "$(KDIR)/include/asm-generic" || { \
		echo "error: $(KDIR) sources are not prepared." 1>&2; \
		false; \
	}

clean:
	rm -rf *.o *.ko *.mod.c .tmp_versions .*.cmd modules.order
endif
