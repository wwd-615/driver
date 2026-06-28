obj-m := rpi-dma-user.o
KERNELDIR ?= /home/wwd/raspi4b/rasberry_pi_os/linux-rpi-kernel/linux
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean

install:
	scp rpi-dma-user.ko shaoer@192.168.10.101:/home/shaoer/