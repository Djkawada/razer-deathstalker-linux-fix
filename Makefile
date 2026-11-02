obj-m += razer_deathstalker_fix.o

KERNELRELEASE ?= $(shell uname -r)
KDIR ?= /usr/lib/modules/$(KERNELRELEASE)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

