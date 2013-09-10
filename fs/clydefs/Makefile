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
	
	obj-$(CONFIG_CLYDEFS_FS) := clydefs.o
	
	clydefs-y += init.o super.o file.o inode.o io.o sysfs.o mkfs.o pagecache.o chunk.o

	#debugging build
	ifneq ($(CONFIG_CLYDEFS_DEBUG),)
		#test code
		clydefs-y += test.o testio.o testchunk.o
		
		#embunit
		clydefs-y += $(EMBUNIT_SRC)/AssertImpl.o
		clydefs-y += $(EMBUNIT_SRC)/RepeatedTest.o
		clydefs-y += $(EMBUNIT_SRC)/stdImpl.o
		clydefs-y += $(EMBUNIT_SRC)/TestCaller.o
		clydefs-y += $(EMBUNIT_SRC)/TestCase.o
		clydefs-y += $(EMBUNIT_SRC)/TestResult.o
		clydefs-y += $(EMBUNIT_SRC)/TestRunner.o
		clydefs-y += $(EMBUNIT_SRC)/TestSuite.o
	endif
# out of kernel build
else
	KDIR ?= /lib/modules/$(shell uname -r)/build
	PWD := $(shell pwd)


default: prep
	echo "making clydefs..."
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
