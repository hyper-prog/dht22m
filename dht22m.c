/*
 * Copyright 2025 Péter Deák (hyper80@gmail.com)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 */

#include <linux/bug.h>
#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/time64.h>
#include <linux/types.h>
#include <linux/uaccess.h>

#define DHT22M_DEVICE_NAME "dht22m"
#define DHT22M_MODULE_NAME "dht22m"

/* Maximum number of dht22 sersor handled by this module */
#define DHT22M_MAX_DEVICES 8

#define DHT22M_WAIT_MILLISECOND_AFTER_READ	2100
#define DHT22M_CHARDEV_BUFFSIZE 32

#define DHT22M_STATES_ZEROCONF		0
#define DHT22M_STATES_CONFIGURED	1
#define DHT22M_STATES_GPIOERROR		2
#define DHT22M_STATES_IRQERROR		3

static DEFINE_MUTEX(gpio_config_mutex);

static int sensor_states[DHT22M_MAX_DEVICES] = {0}; /* DHT22M_STATES_ZEROCONF */
static char chardev_created[DHT22M_MAX_DEVICES] = {0};
static int gpio_pins[DHT22M_MAX_DEVICES] = {0};
static int sensor_irqs[DHT22M_MAX_DEVICES] = {0};

static int num_gpios = 0;

static dev_t dht22m_dev;

static struct cdev dht22m_cdevs[DHT22M_MAX_DEVICES];
static struct class *dht22m_class;

static int create_devices(void);
static void remove_devices(void);
static int sensor_start_read(int sensor_index);
static int sensor_decode_pulses(void);
static int sensor_parse_bytes(void);

#define DTH22M_READSTATE_COLLECT	0
#define DTH22M_READSTATE_OK		1
#define DTH22M_READSTATE_CHKSUMERR	2
#define DTH22M_READSTATE_OTHERR		3
#define DTH22M_READSTATE_TOOSOON	4
#define DTH22M_READSTATE_NEXT		5
/*
 * struct dht22_state - All relevant sensor state.
 * We use only one sensor state struct for all sensor.
 * If a read or calculation in progress the readstate holds
 * DTH22M_READSTATE_COLLECT, so "ReaderBudy" returned.
 * Only accept new read if readstate == DTH22M_READSTATE_NEXT
 * @num_edges: Number of detected edges during a sensor read.
 * @timestamps: Timestamps of detected edges.
 * @bytes: Decoded transmitted data from a sensor read.
 * @read_timestamp: Timestamps of latest sensor read.
 * @temperature: Most recently read temperature (times ten).
 * @humidity: Most recently read humidity percentage (times ten).
 */
struct dht22_state {
	int gpio;
	int readstate;
	int num_edges;
	/*
	 * timestamps[0] contains the start of the sensor read sequence.
	 * The sensor initialization sequence generates two time stamps.
	 * We then record 5*8 timestamps to get data for five bytes.
	 */
	ktime_t timestamps[1 + 2 + 5*8];
	u8 bytes[5];

	ktime_t read_timestamp;
	bool negative;
	int temperature;
	int humidity;
};

/*
 * sensor_state may only be accessed when holding sensor_lock.
 */
static struct dht22_state sensor_state;
static DEFINE_SPINLOCK(sensor_lock);  /* Protects sensor_state. */

/*
 * s_handle_edge() - process interrupt due to falling edge on GPIO pin.
 * @irq: Then IRQ number. Unused.
 * @dev_id: The device identifier. Unused.
 *
 * Records the timestamp of a falling edge (high to low) on the DHT22
 * sensor pin. Prior to the read sequence, sensor_state.timestamps[0]
 * has already been set to the current timestamp and sensor_state.num_edges
 * has been set to 1.
 *
 * During a sensor read, there are in total 42 falling edges: two during
 * the setup phase and then one for each transmitted bit of information.
 * The transmission of each bit starts with the signal going low for 50 µs.
 * Then signal goes high for 22-30 µs when 0 is transmitted; signal goes high
 * for 68-75 µs when 1 is transmitted.
 *
 * Return: IRQ_HANDLED
 */
static irqreturn_t s_handle_edge(int irq, void *dev_id)
{
	int *gpio_num = (int *)dev_id;
	const ktime_t now = ktime_get();
	unsigned long flags;

	spin_lock_irqsave(&sensor_lock, flags);
	if (sensor_state.readstate != DTH22M_READSTATE_COLLECT)
		goto irq_handled;
	if (sensor_state.gpio != *gpio_num)
		goto irq_handled;
	if (sensor_state.num_edges <= 0)
		goto irq_handled;
	/* Start storing timestamps after the long start pulse happened. */
	if (sensor_state.num_edges == 1) {
		s64 width = ktime_to_us(now - sensor_state.timestamps[0]);
		if (width < 500)
			goto irq_handled;
	}
	if (sensor_state.num_edges < sizeof sensor_state.timestamps) {
		sensor_state.timestamps[sensor_state.num_edges++] = now;
	}
 irq_handled:
	spin_unlock_irqrestore(&sensor_lock, flags);
	return IRQ_HANDLED;
}

/*
 * sensor_start_read() - reading data from the DHT22 sensor.
 * @sensor_index: Index of the sensor in gpio_pins, sensor_state, sensor_irqs arrays.
 *
 * The protocol for starting a sensor read is to first pull the GPIO pin
 * low for at least 1ms (we pull it low for 1.5ms) and then pull the pin
 * high and wait for the sensor to respond with an 80µs low pulse followed
 * by an 80µs high pulse. After that initial response, the sensor sends 40
 * pulses whose widths encode the actual sensor data. The pulses are
 * recorded by our interrupt handler; we simply wait for 10ms so that the
 * read cycle finishes and then process the data in sensor_state. The
 * pulses are collected by an interrupt on falling edge on the GPIO pin.
 *
 * Return: 0 on success; -EBUSY or -EIO on error.
 */
static int sensor_start_read(int sensor_index)
{
	const ktime_t now = ktime_get();
	s64 timestamp_diff;
	unsigned long flags;

	mutex_lock(&gpio_config_mutex);
	spin_lock_irqsave(&sensor_lock, flags);

	if (sensor_state.readstate != DTH22M_READSTATE_NEXT) {
		spin_unlock_irqrestore(&sensor_lock, flags);
		mutex_unlock(&gpio_config_mutex);
		return -EBUSY;
	}

	if (sensor_states[sensor_index] != DHT22M_STATES_CONFIGURED) {
		sensor_state.readstate = DTH22M_READSTATE_OTHERR;
		spin_unlock_irqrestore(&sensor_lock, flags);
		mutex_unlock(&gpio_config_mutex);
		return -EIO;
	}

	timestamp_diff = ktime_to_ms(now - sensor_state.read_timestamp);
	if (sensor_state.gpio == gpio_pins[sensor_index] &&
	    timestamp_diff < DHT22M_WAIT_MILLISECOND_AFTER_READ) {
		sensor_state.readstate = DTH22M_READSTATE_TOOSOON;
		spin_unlock_irqrestore(&sensor_lock, flags);
		mutex_unlock(&gpio_config_mutex);
		return -EBUSY;
	}

	sensor_state.gpio = gpio_pins[sensor_index];
	sensor_state.readstate = DTH22M_READSTATE_COLLECT;
	sensor_state.negative = false;
	sensor_state.temperature = 0;
	sensor_state.humidity = 0;
	sensor_state.timestamps[0] = now;
	sensor_state.num_edges = 1;
	spin_unlock_irqrestore(&sensor_lock, flags);

	/* We send the 1500 µs low signal to start the reading process */
	if (gpio_direction_output(sensor_state.gpio, 0)) {
		printk(KERN_ALERT DHT22M_MODULE_NAME
		       ": gpio_direction_output failed\n");
		goto start_seq_error;
	}
	udelay(1500);
	gpio_set_value(sensor_state.gpio, 1);

	/* End of active send, start collecting data */
	if (gpio_direction_input(sensor_state.gpio)) {
		printk(KERN_ALERT DHT22M_MODULE_NAME
		       ": gpio_direction_input failed\n");
		goto start_seq_error;
	}
	mutex_unlock(&gpio_config_mutex);
	return 0;

start_seq_error:
	spin_lock_irqsave(&sensor_lock, flags);
	sensor_state.readstate = DTH22M_READSTATE_OTHERR;
	spin_unlock_irqrestore(&sensor_lock, flags);
	mutex_unlock(&gpio_config_mutex);
	return -EIO;
}

/*
 * sensor_decode_pulses() - decode pulse widths and validate checksum.
 *
 * May only be called when holding sensor_lock.
 *
 * Translates pulse widths into bit values; stores the result in sensor_state.
 * Validates the checksum.
 *
 * Return: 0 on success; -EIO on input error.
 */
static int sensor_decode_pulses(void)
{
	int i;
	u8 sum;
	/*
	 * The last falling edge which is the end of the start sequence occurs
	 * at index 2 in the array of timestamps. Each falling edge after that
	 * defines a pulse which encodes one bit.
	 */
	BUILD_BUG_ON(sizeof sensor_state.timestamps < 5*8+3);
	BUILD_BUG_ON(sizeof sensor_state.bytes < 5);
	if (sensor_state.num_edges < 5*8+3) {
		sensor_state.readstate = DTH22M_READSTATE_OTHERR;
		return -EIO;
	}
	memset(sensor_state.bytes, 0, sizeof sensor_state.bytes);
	for (i = 0; i < 5*8; i++) {
		const ktime_t this = sensor_state.timestamps[i+3];
		const ktime_t last = sensor_state.timestamps[i+2];
		const s64 width = ktime_to_us(this - last);
		/*
		 * Since we zeroed out the bytes array before the loop,
		 * we only have to update bytes when we read a 1 (which
		 * is encoded as a long pulse) from the sensor. We use 101µs
		 * as the boundary between reading a 0 and reading a 1.
		 * According to the data sheet: (Aosong AM2302)
		 *                             Min   Typ   Max  (µs)
		 *  Signal "0", "1" low time   48    50    55
		 *  Signal "0" high time       22    26    30
		 *  Signal "1" high time       68    70    75
		 * Derived: longest "0" period is 85, the shortest "1" period is 116
		 * the middle between two values is ~ 101 µs
		 */
		if (width > 101) {
			sensor_state.bytes[i / 8] |= 1 << (7 - (i & 7));
		}
	}
	sum = (sensor_state.bytes[0] + sensor_state.bytes[1] +
	       sensor_state.bytes[2] + sensor_state.bytes[3]);
	if (sum != sensor_state.bytes[4]) {
		sensor_state.readstate = DTH22M_READSTATE_CHKSUMERR;
		return 0;
	}
	sensor_state.readstate = DTH22M_READSTATE_OK;
	return 0;
}

/*
 * sensor_parse_bytes() - parsing 4 byte data read from the DHT22 sensor.
 *
 * Check that the right number of bits have been read and that the checksum is
 * correct. If those checks pass, update read_timestamp, humidity and
 * temperature fields in sensor_state with the newly read data.
 *
 * Return: 0 on success; -EIO on error.
 */
static int sensor_parse_bytes(void)
{
	unsigned long flags;
	spin_lock_irqsave(&sensor_lock, flags);
	sensor_decode_pulses();
	if (sensor_state.readstate != DTH22M_READSTATE_OK)
		goto end_parse_bytes;
	sensor_state.read_timestamp = sensor_state.timestamps[sensor_state.num_edges - 1];
	sensor_state.negative = false;
	sensor_state.humidity = (sensor_state.bytes[0] * 256 + sensor_state.bytes[1]);
	sensor_state.temperature = ((sensor_state.bytes[2] & 0x7F) * 256 +
				   sensor_state.bytes[3]);
	if (sensor_state.bytes[2] & 0x80) {
		sensor_state.negative = true;
	}
end_parse_bytes:
	spin_unlock_irqrestore(&sensor_lock, flags);
	return 0;
}

/*
 * configure_gpios() - Configure the gpios according to the gpio_pins array
 *
 * Configure GPIOs from the gpio_pins array
 * Check gpio value, set pin, request IRQs
 * If a configuration success, the appropriate value of sensor_states
 * set to DHT22M_STATES_CONFIGURED. Other values means errors and
 * block the read from sensor.
 */
static int configure_gpios(void)
{
	int i;

	printk(KERN_INFO DHT22M_MODULE_NAME ": configure sensors gpios\n");
	for (i = 0; i < num_gpios; ++i) {
		if (!gpio_is_valid(gpio_pins[i])) {
			printk(KERN_ALERT DHT22M_MODULE_NAME ": invalid GPIO pin\n");
			sensor_states[i] = DHT22M_STATES_GPIOERROR;
			continue;
		}

		if (gpio_request(gpio_pins[i], DHT22M_MODULE_NAME) < 0) {
			printk(KERN_ALERT DHT22M_MODULE_NAME ": gpio_request failed\n");
			sensor_states[i] = DHT22M_STATES_GPIOERROR;
			continue;
		}

		if (gpio_direction_input(gpio_pins[i])) {
			printk(KERN_ALERT DHT22M_MODULE_NAME
				": initial gpio_direction_input failed\n");
			gpio_free(gpio_pins[i]);
			sensor_states[i] = DHT22M_STATES_GPIOERROR;
			continue;
		}

		if ((sensor_irqs[i] = gpio_to_irq(gpio_pins[i])) < 0) {
			printk(KERN_ALERT DHT22M_MODULE_NAME ": gpio_to_irq failed\n");
			gpio_free(gpio_pins[i]);
			sensor_states[i] = DHT22M_STATES_IRQERROR;
			continue;
		}

		if (request_irq(sensor_irqs[i], s_handle_edge,
				IRQF_TRIGGER_FALLING,DHT22M_MODULE_NAME,
				&gpio_pins[i]) < 0) {
			printk(KERN_ALERT DHT22M_MODULE_NAME ": request_irq failed\n");
			gpio_free(gpio_pins[i]);
			sensor_states[i] = DHT22M_STATES_IRQERROR;
			continue;
		}

		sensor_states[i] = DHT22M_STATES_CONFIGURED;
		printk(KERN_INFO DHT22M_MODULE_NAME ": GPIO %d configured (IRQ %d)\n",
		       gpio_pins[i], sensor_irqs[i]);
	}
	return 0;
}

/*
 * free_gpios() - Free all configured GPIOs and IRQs
 */
static void free_gpios(void)
{
	int i;

	printk(KERN_INFO DHT22M_MODULE_NAME ": Free IRQ and GPIOs\n");
	for (i = 0; i < DHT22M_MAX_DEVICES; ++i) {
		if (sensor_states[i] == DHT22M_STATES_CONFIGURED) {
			/*
			printk(KERN_INFO DHT22M_MODULE_NAME
			       ": Free IRQ %d and GPIO %d\n",sensor_irqs[i],gpio_pins[i]);
			*/
			free_irq(sensor_irqs[i], &gpio_pins[i]);
			gpio_free(gpio_pins[i]);
			sensor_states[i] = DHT22M_STATES_ZEROCONF;
		}
	}
}

/*
 * dht22m_gpios_store() - Sysfs write handler, gpio config function
 *
 * Receices a string from the userspace through sysfs which contains the gpio numbers.
 */
static ssize_t dht22m_gpios_store(struct class *class,
				  struct class_attribute *attr,
				  const char *buf, size_t count)
{
	int full_length;
	char localbuf[32];
	char *runner;
	int i,gpiovalue;
	char is_change;

	int new_gpio_pins[DHT22M_MAX_DEVICES] = {0};
	int new_num_gpios = 0;

	strscpy(localbuf, buf, sizeof(localbuf));
	full_length = strlen(localbuf);
	runner = localbuf;
	for(i = 0; i < full_length && new_num_gpios < DHT22M_MAX_DEVICES; ++i) {
		if (localbuf[i] == ' ' || localbuf[i] == ';' || localbuf[i] == ',') {
			localbuf[i] = '\0';
			if(sscanf(runner, "%d", &gpiovalue) == 1) {
				new_gpio_pins[new_num_gpios] = gpiovalue;
				++new_num_gpios;
				/* If we have more characters until the string end */
				if (i + 1 < full_length) {
					/* runner will points the next char
					 * after the converted separator
					 */
					runner = localbuf + i + 1;
					continue;
				} else {
					break; /* It was the last number */
				}
			} else {
				break; /* Some bad data found */
			}
		}
		if (i + 1 == full_length) {
			if(sscanf(runner, "%d", &gpiovalue) == 1) {
				new_gpio_pins[new_num_gpios] = gpiovalue;
				++new_num_gpios;
				break; /* It was the last number */
			}
		}
	}

	/* Only for debugging
	for (i = 0; i < new_num_gpios; ++i)
		printk(KERN_INFO DHT22M_MODULE_NAME
		       ": About to set gpio %d to on pos %d\n",new_gpio_pins[i],i);
	*/

	mutex_lock(&gpio_config_mutex);
	is_change = 0;
	for(i = 0; i < DHT22M_MAX_DEVICES; ++i)
		if (gpio_pins[i] != new_gpio_pins[i]) {
			is_change = 1;
			break;
		}
	if(is_change) {
		free_gpios();
		remove_devices();

		num_gpios = new_num_gpios;
		for(i = 0; i < DHT22M_MAX_DEVICES; ++i)
			gpio_pins[i] = new_gpio_pins[i];

		configure_gpios();
		create_devices();
		printk(KERN_INFO DHT22M_MODULE_NAME ": Conf req - %d GPIOs set. \n", num_gpios);
	} else {
		printk(KERN_INFO DHT22M_MODULE_NAME ": Conf req - GPIOs unchanged. \n");
	}
	mutex_unlock(&gpio_config_mutex);
	return count;
}

/*
 * dht22m_gpios_show() -  Sysfs read handler: print the current configured gpios
 */
static ssize_t dht22m_gpios_show(struct class *class,
				 struct class_attribute *attr, char *buf)
{
	int i;
	int len = 0;

	mutex_lock(&gpio_config_mutex);
	for (i = 0; i < num_gpios; ++i) {
		len += sprintf(buf + len, "%d ", gpio_pins[i]);
	}
	mutex_unlock(&gpio_config_mutex);
	len += sprintf(buf + len, "\n");
	return len;
}

/* Sysfs attribute for gpio set file "gpiolist" */
static struct class_attribute dht22m_class_attr =
	__ATTR(gpiolist, 0664, dht22m_gpios_show, dht22m_gpios_store);

/*
 * chardevice_open() - Characted device open handler
 *
 * This function starts the sensor reading process.
 * According to the minor number we query which
 * chardev is accessed -> which sensor needs to be read.
 */
static int chardevice_open(struct inode *inode, struct file *file)
{
	int readstate, hum_int, hum_frac, temp_int, temp_frac;
	unsigned long flags;
	char sign[2];
	char *message;
	int minor = iminor(inode);
	int error;

	//printk(KERN_INFO DHT22M_MODULE_NAME ": Device opened (minor: %d) \n",minor);

	error = sensor_start_read(minor);
	if (error != 0) {
		spin_lock_irqsave(&sensor_lock, flags);
		sensor_state.readstate = DTH22M_READSTATE_NEXT;
		spin_unlock_irqrestore(&sensor_lock, flags);

		message = kmalloc(DHT22M_CHARDEV_BUFFSIZE, GFP_KERNEL);
		if (!message) {
			printk(KERN_ALERT DHT22M_MODULE_NAME
			       ": no memory for chrdev buffer\n");
			return -ENOMEM;
		}

		if (error == -EBUSY) {
			snprintf(message,DHT22M_CHARDEV_BUFFSIZE, "ReaderBusy\n");
		} else {
			snprintf(message,DHT22M_CHARDEV_BUFFSIZE, "IOError\n");
		}

		file->private_data = message;
		return 0;
	}

	msleep(20);  /* Read cycle takes less than 6ms. */
	sensor_parse_bytes();
	sign[0] = '\0';
	sign[1] = '\0';

	/* Read sensor data (protected by sensor_lock) into local variables. */
	spin_lock_irqsave(&sensor_lock, flags);
	readstate = sensor_state.readstate;
	hum_int = sensor_state.humidity / 10;
	hum_frac = sensor_state.humidity % 10;
	temp_int = sensor_state.temperature / 10;
	temp_frac = sensor_state.temperature % 10;
	if (sensor_state.negative)
		sign[0] = '-';
	sensor_state.readstate = DTH22M_READSTATE_NEXT;
	spin_unlock_irqrestore(&sensor_lock, flags);
	/* Sensor lock released. */

	message = kmalloc(DHT22M_CHARDEV_BUFFSIZE, GFP_KERNEL);
	if (!message) {
		printk(KERN_ALERT DHT22M_MODULE_NAME
		       ": no memory for chrdev buffer\n");
		return -ENOMEM;
	}

	if (readstate == DTH22M_READSTATE_OK) {
		snprintf(message,DHT22M_CHARDEV_BUFFSIZE, "Ok;%s%d.%d;%d.%d\n",
			 sign,temp_int, temp_frac,hum_int, hum_frac);
	} else if(readstate == DTH22M_READSTATE_CHKSUMERR) {
		snprintf(message,DHT22M_CHARDEV_BUFFSIZE, "ChecksumError\n");
	} else if(readstate == DTH22M_READSTATE_TOOSOON) {
		snprintf(message,DHT22M_CHARDEV_BUFFSIZE, "ReadTooSoon\n");
	} else if(readstate == DTH22M_READSTATE_COLLECT) {
		snprintf(message,DHT22M_CHARDEV_BUFFSIZE, "NotRead\n");
	} else {
		snprintf(message,DHT22M_CHARDEV_BUFFSIZE, "IOError\n");
	}

	file->private_data = message;
	return 0;
}

/* chardevice_read() - Characted device read handler */
static ssize_t chardevice_read(struct file *file, char __user *user_buf,
			       size_t count, loff_t *ppos)
{
	char *message = file->private_data;
	size_t len;

	if (!message)
		return 0;
	len = strlen(message);
	if (*ppos >= len)
		return 0;
	if (count > len - *ppos)
		count = len - *ppos;
	if (copy_to_user(user_buf, message + *ppos, count))
		return -EFAULT;
	*ppos += count;
	return count;
}

/* chardevice_release() - Characted device release handler */
static int chardevice_release(struct inode *inode, struct file *file)
{
	kfree(file->private_data);
	file->private_data = NULL;
	return 0;
}

/* The "dht22mX" character devices file operations struct */
static struct file_operations dht22m_cdevs_fops = {
	.owner = THIS_MODULE,
	.read = chardevice_read,
	.open = chardevice_open,
	.release = chardevice_release
};

/*
 * create_devices() - Create character devices
 *
 * Create the /dev/dht22mX character devices for every gpios
 * set through sysfs "gpiolist" file.
 */
static int create_devices(void)
{
	int i;

	for (i = 0; i < DHT22M_MAX_DEVICES; ++i)
		chardev_created[i] = 0;

	for (i = 0; i < num_gpios && i < DHT22M_MAX_DEVICES; ++i) {
		cdev_init(&dht22m_cdevs[i], &dht22m_cdevs_fops);
		dht22m_cdevs[i].owner = THIS_MODULE;
		if (cdev_add(&dht22m_cdevs[i], MKDEV(MAJOR(dht22m_dev),
			     MINOR(dht22m_dev) + i), 1) < 0) {
			printk(KERN_ERR "Failed to add cdev %d\n", i);
			chardev_created[i] = 0;
			continue;
		} else {
			device_create(dht22m_class, NULL,
				      MKDEV(MAJOR(dht22m_dev), MINOR(dht22m_dev) + i),
				       NULL, "dht22m%d", i);
			chardev_created[i] = 1;
		}
	}
	return 0;
}

/*
 * remove_devices() - Remove all character devices previously created
 */
static void remove_devices(void)
{
	int i;

	for (i = 0; i < DHT22M_MAX_DEVICES; ++i) {
		if (chardev_created[i]) {
			device_destroy(dht22m_class,
				       MKDEV(MAJOR(dht22m_dev), MINOR(dht22m_dev) + i));
			cdev_del(&dht22m_cdevs[i]);
			chardev_created[i] = 0;
		}
	}
}

/* Initialize the DHT22M module as it is loaded. */
int __init dht22m_init(void)
{
	int i;
	int error = 0;
	unsigned long flags;

	num_gpios = 0;
	for (i = 0; i < DHT22M_MAX_DEVICES; ++i)
		sensor_states[i] = DHT22M_STATES_ZEROCONF;

	if ((error = alloc_chrdev_region(&dht22m_dev, 0, DHT22M_MAX_DEVICES,
					 DHT22M_DEVICE_NAME)) < 0) {
		printk(KERN_ALERT DHT22M_MODULE_NAME
		       ": alloc_chrdev_region failed\n");
		goto alloc_chrdev_region_failed;
	}

	dht22m_class = class_create(THIS_MODULE, DHT22M_MODULE_NAME);
	if (IS_ERR(dht22m_class)) {
		printk(KERN_ALERT DHT22M_MODULE_NAME ": class_create failed\n");
		error = PTR_ERR(dht22m_class);
		goto class_create_failed;
	}

	if (class_create_file(dht22m_class, &dht22m_class_attr)) {
		error = -ENOMEM;
		goto class_create_file_failed;
	}

	spin_lock_irqsave(&sensor_lock, flags);
	sensor_state.readstate = DTH22M_READSTATE_NEXT;
	spin_unlock_irqrestore(&sensor_lock, flags);

	printk(KERN_INFO DHT22M_MODULE_NAME ": Init successfully.\n");
	return 0;

class_create_file_failed:
	class_destroy(dht22m_class);
class_create_failed:
	unregister_chrdev_region(dht22m_dev, DHT22M_MAX_DEVICES);
alloc_chrdev_region_failed:
	return error;
}

/* Clean up before the DHT22M module is unloaded. */
void __exit dht22m_cleanup(void)
{
	mutex_lock(&gpio_config_mutex);
	free_gpios();
	remove_devices();
	mutex_unlock(&gpio_config_mutex);

	class_remove_file(dht22m_class, &dht22m_class_attr);
	class_destroy(dht22m_class);
	unregister_chrdev_region(dht22m_dev, DHT22M_MAX_DEVICES);
	printk(KERN_INFO DHT22M_MODULE_NAME ": Module unloaded\n");
}

module_init(dht22m_init);
module_exit(dht22m_cleanup);

MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");
MODULE_AUTHOR("Peter Deak <hyper80@gmail.com>");
MODULE_DESCRIPTION("DHT22 (AM2302) humidity/temperature multi device driver");
