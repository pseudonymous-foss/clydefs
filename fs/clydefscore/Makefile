EMBUNIT_DIR := ../../embunit
EMBUNIT_SRC := $(EMBUNIT_DIR)/embUnit
EMBUNIT_SAMPLES := $(EMBUNIT_DIR)/samples

ccflags-y += -I$(src)/$(EMBUNIT_DIR)
ccflags-y += -I$(src)/$(EMBUNIT_DIR)/wrap
ccflags-y += -DNO_STDIO_PRINTF



obj-$(CONFIG_CLYDEFS_CORE_FS) := clydefscore.o

clydefscore-y += core.o blinktree.o stack.o treeinterface.o
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
