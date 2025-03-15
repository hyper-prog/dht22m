MODULE = dht22m
# Comment/uncomment the following line to disable/enable debugging
#DEBUG = y

ifeq ($(DEBUG),y)
 ccflags-y := -O -g -DDHT22M_DEBUG
endif

obj-m := ${MODULE}.o

module_upload=${MODULE}.ko

compile:
	make -C /lib/modules/$(shell uname -r)/build M=$(shell pwd) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(shell pwd) clean

info: compile
	modinfo ${module_upload}

test: compile
	sudo dmesg -C
	sudo insmod ${module_upload}
	sudo rmmod ${module_upload}
	dmesg

install:
	mkdir -p /lib/modules/$(shell uname -r)/kernel/extra/
	cp -f dht22m.ko /lib/modules/$(shell uname -r)/kernel/extra/
	depmod -a
