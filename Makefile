KDIR ?= /lib/modules/$(shell uname -r)/build
PW_VERSION := $(shell cat $(src)/VERSION 2>/dev/null || cat VERSION)
obj-m += pex_window.o
ccflags-y += -Wall -Wextra -Wno-unused-parameter -DPW_VERSION=\"$(PW_VERSION)\"

.PHONY: all clean
all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules
clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
