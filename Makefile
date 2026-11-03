obj-m += razer_deathstalker_fix.o

KERNELRELEASE ?= $(shell uname -r)
KDIR ?= /usr/lib/modules/$(KERNELRELEASE)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

