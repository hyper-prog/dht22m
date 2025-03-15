![DHT22M logo](https://raw.githubusercontent.com/hyper-prog/dht22m/master/images/dht22m_logo.png)

Raspberry Pi kernel module for DHT22 (AM2302) sensors
============================================================

_The most reliable dht22 sensor reader_

This project implements a Raspberry Pi kernel module for the DHT22 temperature
and humidity sensor. The goal of the driver is to read multiple sensors so that
the GPIOs can be dynamically set without reloading the kernel module.
The sensor data can easely read from `/dev/dht22mX` character devices.
This kernel module uses hardware interrupts to collect sensor data,
which results in much more reliable results (fewer crc errors) than
user-mode readers that constantly monitor the status of the gpio.

Introduction & Backgrounds
----------------------------

I was created a project which handle multiple DHT22 sensors.
My code collected statistics on the number of successful and failed readings,
and I found that some cases approximately 20 percent of the readings were erroneous.
It is very big amount of error!

Initially, I attempted to improve the user-space reading code,
but the DHT22 sensor transmits data using so very short pulses
(20, 50, and 70 microseconds) which makes very hard to read without loosing a bit.
Accurately capturing these signals with conventional GPIO polling proved to be unreliable.
Due to the operation system task switching, interrupts,
and system load, the number of failed readings remained high.

To achieve more precise timing and improve reading accuracy,
I turned to kernel module development. By handling GPIO signal processing at the kernel level,
the module ensures much finer timing control,
significantly reducing read errors and enhancing overall reliability.

Configure sensor GPIOs to read data
-------------------------------------

You can send your dht22m sensor GPIOs numbers to as sysfs file: `/sys/class/dht22m/gpiolist`
The GPIO numbers are separated by spaces. After If success the `/dev/dht22mX`
devices will be created in same order as the GPIOs listed in the sysfs file.

    echo "2 3 22" > /sys/class/dht22m/gpiolist

You can always check the GPIOs settings of your dht22 devices

    cat /sys/class/dht22m/gpiolist
    2 3 22

    ls /dev/dht22*
    /dev/dht22m0 /dev/dht22m1 /dev/dht22m2

_Note: The kernel module is written in such a way that if a configuration request
is received matches the running configuration, recognizes the matching and does nothing.
So the user programs can safely send the necessary settings at startup, it will not cause kernel overhead._

Read the values
----------------

You can read temperature and humidity values by simple read from `/dev/dht22mX` device files.

    cat /dev/dht22m*
    Ok;19.7;38.2
    Ok;20.1;42.1
    Ok;11.7;26.0

The device files could send the following strings

| Received string             | Possible events / meaning                                       |
| --------------------------- | --------------------------------------------------------------- |
| `Ok;TEMPERATURE;HUMIDITY`   | Successful read, with the read values.                          |
| `ChecksumError`             | Checksum error during the read. Wait 2 sec and read again!      |
| `ReadTooSoon`               | Read the sensor too quickly. Read later!                        |
| `IOError`                   | GPIO/Sensor/Other error                                         |
| `NotRead`                   | Data not read                                                   |

Pre-requisites to build the kernel module
-----------------------------------------

To build the kernel module, you need to first install the following
packages:

    sudo apt install build-essential raspberrypi-kernel-headers

On some [raspbian](https://www.raspberrypi.com/software/) versions, the sources for the running kernel
may not be installed from a package.
In this case, check the https://github.com/RPi-Distro/rpi-source tool
which downloads the current running raspbery pi kernel for you.
If you did so, run the following commands to configure soruces as your running kernel:

    cd /lib/modules/$(uname -r)/build
    zcat /proc/config.gz > .config
    make olddefconfig
    make modules_prepare

Building and install the kernel module
--------------------------------------

To compile enter the dht22m directory and build & install the module

    make
    make install

Load the module by:

    modprobe dht22m

If you would like to make the dht22m module loads after reboot run:

    echo "dht22m" > /etc/modules-load.d/dht22m.conf

Wiring of DHT22 & Hardware connections
--------------------------------------

The hardware connection of the dht22 sensor is the same as the method described in user mode reading,
which can be found here https://github.com/hyper-prog/DHT22-Raspberry-Pi

References
-----------

To learn more about Linux kernel
modules in general, see [The Linux Kernel Module Programming
Guide](https://sysprog21.github.io/lkmpg/). To learn more about the DHT22
sensor, see the [data sheet](http://www.electrodragon.com/w/AM2302).

Author
-------
- Written by Péter Deák (C) hyper80@gmail.com, License GPLv2
- Thanks to the Google and Lars Engebretsen for sensor handling tips and codes!
- The author wrote this project entirely as a hobby. Any help is welcome!

------

[![paypal](https://raw.githubusercontent.com/hyper-prog/dht22m/master/images/tipjar.png)](https://www.paypal.com/donate/?business=EM2E9A6BZBK64&no_recurring=0&currency_code=USD)

