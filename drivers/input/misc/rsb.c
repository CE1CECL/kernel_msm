/*
 * Copyright (C) 2016 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_gpio.h>
#include <linux/of_irq.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/regulator/consumer.h>

#define NUM_WRITE_RETRIES	5

#define RSB_MAGIC_PID		0x30

/* MOTION Register addresses */
#define MOTION		0x02
#define DELTA_X		0x03
#define DELTA_Y		0x04
#define MOTION_BITMASK	0x80

struct rsb_spi_comms {
	int (*open)(void *);
	void (*close)(void *);
	int (*write)(void *, uint8_t, uint8_t);
	int (*read)(void *, uint8_t *, uint8_t);
	int (*write_read)(void *, uint8_t, uint8_t);

	struct spi_device *spi_device;
	uint8_t tx_buf;
	uint8_t rx_buf;
};

struct rsb_drv_data {
	struct spi_device *device;
	struct rsb_spi_comms comms;
	struct dentry *dent;
	struct task_struct *poll_thread;
	struct input_dev *in_dev;
	int cs;
	struct regulator *vld_reg; /* power supply voltage v3.3 */
	struct regulator *vdd_reg; /* power supply voltage for IO v1.8 */
};

static struct spi_device_id rsb_spi_id[] = {
	{"rsb", 0},
	{},
};

/*
 * We iterate over this array, writing all the registers to the addresses
 * NOTE: Please make sure the #define values are kept in lock step with the
 * size of the array. If not, the RSB will *not* be initialized properly!
 */
#define INIT_WRITES_FIRST_BATCH_INDEX	5
#define INIT_WRITES_SECOND_BATCH_INDEX	34

static uint8_t init_writes[][2] = {
	{0x05, 0xA0 }, /* OPERATION_MODE */
	{0x0D, 0x05 }, /* RES_X */
	{0x0E, 0x0A }, /* RES_Y */
	{0x19, 0x04 }, /* ORIENTATION */
	{0x2B, 0x6D },
	{0x5C, 0xD7 }, /* LD_SRC */
	{0x09, 0x22 }, /* WRITE_PROTECT */
	{0x2A, 0x03 },
	{0x30, 0x4C },
	{0x33, 0x90 },
	{0x36, 0xCC },
	{0x37, 0x51 },
	{0x38, 0x01 },
	{0x3A, 0x7A },
	{0x40, 0x38 },
	{0x41, 0x33 },
	{0x42, 0x4F },
	{0x43, 0x83 },
	{0x44, 0x4F },
	{0x45, 0x80 },
	{0x46, 0x23 },
	{0x47, 0x49 },
	{0x48, 0xC3 },
	{0x49, 0x49 },
	{0x4A, 0xC0 },
	{0x52, 0x00 },
	{0x61, 0x80 },
	{0x62, 0x51 },
	{0x67, 0x53 },
	{0x68, 0x13 },
	{0x6C, 0x10 },
	{0x6F, 0xF6 },
	{0x71, 0x28 },
	{0x72, 0x28 },
	{0x79, 0x08 },
};


static int get_test_read(void *data, u64 *val)
{
	struct rsb_drv_data *rsb_data = data;
	uint8_t rx_data;

	dev_info(&rsb_data->device->dev, "Writing to debugfs\n");

	/* Read the sensorPID */
	if (rsb_data->comms.read(data, &rx_data, 0x00) == 0)
		dev_info(&rsb_data->device->dev, "PID is %x\n",
			rx_data);
	else
		dev_err(&rsb_data->device->dev, "read error\n");

	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(test_read_fops, get_test_read, NULL, "%llu\n");

/*
 * SPI device protocol to read a register.
 * Write a 8 bit address value, and read an 8 bit value back.
 */
static int rsb_spi_read(void *data, uint8_t *rx, uint8_t addr)
{

	/* TODO(pmalani): Do we need a timeout check?? */
	struct rsb_drv_data *rsb_data = data;

	rsb_data->comms.tx_buf = (0 << 7) | addr;

	gpio_set_value(rsb_data->cs, 0);
	spi_write(rsb_data->device, &(rsb_data->comms.tx_buf), 1);
	spi_read(rsb_data->device, &(rsb_data->comms.rx_buf), 1);
	gpio_set_value(rsb_data->cs, 1);

	memcpy(rx, &(rsb_data->comms.rx_buf), sizeof(*rx));
	return 0;
}

/*
 * SPI Protocol to write a byte to an address.
 * Returns 0 on success, -1 otherwise.
 */
static int rsb_spi_write(void *data, uint8_t tx_val, uint8_t addr)
{
	struct rsb_drv_data *rsb_data = data;
	uint8_t tx_buf[2] = {0};

	tx_buf[0] = (1 << 7) | addr;
	tx_buf[1] = tx_val;

	gpio_set_value(rsb_data->cs, 0);
	if (spi_write(rsb_data->device, tx_buf, 2) == 0) {
		gpio_set_value(rsb_data->cs, 1);
		return 0;
	}

	dev_warn(&rsb_data->device->dev,
		"Write %x to addr %x failed\n", tx_val, addr);
	return -EIO;
}

/*
 * SPI Protocol to write a byte to an address.
 * Also reads back the address to make sure it's written correctly.
 * Returns 0 on success, -1 otherwise.
 */
static int rsb_spi_write_read(void *data, uint8_t tx_val, uint8_t addr)
{
	struct rsb_drv_data *rsb_data = data;
	uint8_t num_retries = NUM_WRITE_RETRIES;
	uint8_t read_val;

	do {
		rsb_data->comms.write(rsb_data, tx_val, addr);
		rsb_data->comms.read(rsb_data, &read_val, addr);
		if (read_val == tx_val) {
			dev_info(&rsb_data->device->dev,
				"Addr %x: Wrote %x got back %x\n",
				addr, tx_val, read_val);
			return 0;
		}
	} while (num_retries-- > 0);

	dev_warn(&rsb_data->device->dev,
		"Write_read %x to addr %x failed\n", tx_val, addr);
	return -EIO;
}

static int rsb_spi_open(void *data)
{
	struct rsb_drv_data *rsb_data = data;
	int ret;

	/* TODO(pmalani): How much of this is prepopulated from DT? */
	rsb_data->device->max_speed_hz = 2000000;
	rsb_data->device->mode = SPI_MODE_0;
	rsb_data->device->bits_per_word = 8;
	ret = spi_setup(rsb_data->device);
	if (!ret) {
		dev_info(&rsb_data->device->dev,
			"SPI device set up successfully!\n");
		/* Toggle CS low for 1ms at power up. */
		gpio_set_value(rsb_data->cs, 0);
		udelay(1000);
		gpio_set_value(rsb_data->cs, 1);
	}

	return ret;
}

static void rsb_spi_close(void *data)
{
	struct rsb_drv_data *rsb_data = data;

	gpio_set_value(rsb_data->cs, 1);
}

/*
 * Initialize SPI related communications callbacks and buffers
 */
void rsb_spi_comms_init(struct rsb_drv_data *rsb_data)
{
	struct rsb_spi_comms *comms = &(rsb_data->comms);

	comms->open = rsb_spi_open;
	comms->close = rsb_spi_close;
	comms->write = rsb_spi_write;
	comms->read = rsb_spi_read;
	comms->write_read = rsb_spi_write_read;
}

static int rsb_create_debugfs(struct rsb_drv_data *rsb_data)
{
	struct dentry *file;

	rsb_data->dent = debugfs_create_dir("rsb", NULL);
	if (IS_ERR(rsb_data->dent)) {
		dev_err(&rsb_data->device->dev,
			"rsb driver couldn't create debugfs dir\n");
		return -EFAULT;
	}

	file = debugfs_create_file("test_read", 0644, rsb_data->dent,
			(void *)rsb_data, &test_read_fops);
	if (IS_ERR(file)) {
		dev_err(&rsb_data->device->dev,
			"debugfs create file for test_read failed\n");
		return -EFAULT;
	}
	return 0;
}

#ifdef CONFIG_OF
static int rsb_parse_dt(struct spi_device *spi_dev)
{
	int ret;
	struct device_node *dt = spi_dev->dev.of_node;
	struct rsb_drv_data *rsb_data = spi_get_drvdata(spi_dev);

	ret = rsb_data->cs = of_get_named_gpio(dt, "rsb,spi-cs-gpio", 0);
	dev_info(&spi_dev->dev, "cs GPIO read from DT:%u\n",
			rsb_data->cs);

	return 0;
}
#else
static int rsb_parse_dt(struct spi_device *spi_dev)
{
	dev_err(&spi_dev->dev,
		"Kernel not configured with DT support\n");
	return -EINVAL;
}
#endif

/*
 * Sequence of start up writes mandated by the RSB data sheet.
 */
static int rsb_init_sequence(struct rsb_drv_data *rsb_data)
{
	uint8_t read_val;
	int i;
	struct rsb_spi_comms *comms = &rsb_data->comms;

	/* Read the SensorPID to ensure the SPI Link is valid */
	if (comms->read(rsb_data, &read_val, 0x00) == 0) {
		if (read_val != RSB_MAGIC_PID) {
			dev_err(&rsb_data->device->dev,
				"Couldn't read SPI Magic PID,"
				 "value read: %u\n", read_val);
			return -EIO;
		}
	}

	if (comms->write(rsb_data, 0x00, 0x7F) != 0)
		return -EIO;

	if (comms->write_read(rsb_data, 0x5A, 0x09) != 0)
		return -EIO;

	for (i = 0; i <= INIT_WRITES_FIRST_BATCH_INDEX; i++) {
		if (comms->write_read(rsb_data, init_writes[i][1],
			init_writes[i][0]) != 0) {
			return -EIO;
		}
	}

	if (comms->write(rsb_data, 0x01, 0x7F) != 0)
		return -EIO;

	for (; i <= INIT_WRITES_SECOND_BATCH_INDEX; i++) {
		if (comms->write_read(rsb_data, init_writes[i][1],
			init_writes[i][0]) != 0) {
			return -EIO;
		}
	}

	if (comms->write(rsb_data, 0x00, 0x7F) != 0)
		return -EIO;

	if (comms->write_read(rsb_data, 0x00, 0x09) != 0)
		return -EIO;

	dev_info(&rsb_data->device->dev, "Rsb init success\n");
	return 0;
}

static irqreturn_t rsb_handler(int irq, void *dev_id)
{
	int8_t delta_x, delta_y;
	struct rsb_drv_data *rsb_data = dev_id;
	uint8_t rx_val;

	rsb_data->comms.read(rsb_data, &rx_val, MOTION);
	while (rx_val & MOTION_BITMASK) {
		rsb_data->comms.read(rsb_data, &rx_val, DELTA_X);
		delta_x = (int8_t)rx_val;
		rsb_data->comms.read(rsb_data, &rx_val, DELTA_Y);
		delta_y = (int8_t)rx_val;
		if ((delta_x | delta_y) != 0) {
			input_report_rel(rsb_data->in_dev, REL_WHEEL,
					delta_x);
			input_sync(rsb_data->in_dev);
		}
		rsb_data->comms.read(rsb_data, &rx_val, MOTION);
	}

	return IRQ_HANDLED;
}

static int rsb_init_regulator(struct spi_device *spi_dev)
{
	struct rsb_drv_data *rsb_data = spi_get_drvdata(spi_dev);
	int ret = 0;

	rsb_data->vld_reg = devm_regulator_get(&spi_dev->dev,
		"rsb,vld");
	if (IS_ERR(rsb_data->vld_reg)) {
		dev_warn(&spi_dev->dev,
			"regulator: VLD request failed\n");
		ret = (int)rsb_data->vld_reg;
		rsb_data->vld_reg = NULL;
		/*
		 * Surface up the error obtained from the get() call.
		 * If it is EPROBE_DEFER, surface that up to the
		 * probe() caller. If it is anything else, continue with
		 * the probe function execution.
		 */
		return ret;
	}

	rsb_data->vdd_reg = devm_regulator_get(&spi_dev->dev,
		"rsb,vdd");
	if (IS_ERR(rsb_data->vdd_reg)) {
		dev_warn(&spi_dev->dev,
			"regulator: VDD request failed\n");
		ret = (int)rsb_data->vld_reg;
		rsb_data->vdd_reg = NULL;
		return ret;
	}

	if (rsb_data->vld_reg) {
		ret = regulator_enable(rsb_data->vld_reg);
		if (ret) {
			dev_warn(&spi_dev->dev,
				"regulator: VLD enable failed\n");
			return ret;
		}
	}

	if (rsb_data->vdd_reg) {
		ret = regulator_enable(rsb_data->vdd_reg);
		if (ret) {
			dev_warn(&spi_dev->dev,
				"regulator: VDD enable failed\n");
			return ret;
		}
	}
	return 0;
}

static int rsb_probe(struct spi_device *spi)
{
	struct rsb_drv_data *rsb_data;
	int err = 0;

	rsb_data = devm_kzalloc(&spi->dev, sizeof(struct rsb_drv_data),
		GFP_KERNEL);

	if (!rsb_data)
		return -ENOMEM;

	spi_set_drvdata(spi, rsb_data);
	rsb_data->device = spi;
	rsb_spi_comms_init(rsb_data);

	err = rsb_parse_dt(spi);
	if (err)
		return err;

	if (gpio_is_valid(rsb_data->cs)) {
		err = devm_gpio_request(&spi->dev, rsb_data->cs,
			"rsb_spi_cs");
		if (err) {
			dev_err(&spi->dev,
				"spi_cs_gpio:%d request failed\n",
				rsb_data->cs);
			return err;
		} else {
			gpio_direction_output(rsb_data->cs, 1);
		}
	} else {
		dev_err(&spi->dev, "spi_cs_gpio:%d is not valid\n",
			rsb_data->cs);
		return -EINVAL;
	}

	/* Initialize regulators */
	err = rsb_init_regulator(spi);
	if (err)
		return err;

	/* Should the open of the SPI bus be done only once?? */
	rsb_data->comms.open(rsb_data);

	err = rsb_init_sequence(rsb_data);
	if (err)
		return err;

	/* Allocate and register an input device */
	rsb_data->in_dev = devm_input_allocate_device(&spi->dev);
	if (!rsb_data->in_dev) {
		dev_err(&spi->dev, "Couldn't allocate input device\n");
		return -ENOMEM;
	}

	rsb_data->in_dev->evbit[0] = BIT_MASK(EV_REL);
	rsb_data->in_dev->relbit[0] = BIT_MASK(REL_WHEEL);
	rsb_data->in_dev->name = "rsb";

	err = input_register_device(rsb_data->in_dev);
	if (err) {
		dev_err(&spi->dev, "Failed to register rsb device\n");
		return err;
	}

	err = devm_request_threaded_irq(&spi->dev, spi->irq, NULL,
		rsb_handler, IRQF_ONESHOT | IRQF_TRIGGER_FALLING |
		IRQF_TRIGGER_LOW, "rsb_handler", rsb_data);
	if (err) {
		dev_err(&spi->dev,
			"Failed to register irq handler IRQ:%d\n",
			spi->irq);
		return err;
	}

	rsb_create_debugfs(rsb_data);
	return 0;
}

static int rsb_remove(struct spi_device *spi)
{
	struct rsb_drv_data *rsb_data;

	rsb_data = spi_get_drvdata(spi);

	rsb_data->comms.close(rsb_data);
	debugfs_remove_recursive(rsb_data->dent);

	return 0;
}

static struct spi_driver rsb_driver = {
	.driver = {
		.name = "rsb",
		.owner = THIS_MODULE,
	},
	.probe = rsb_probe,
	.remove = rsb_remove,
	.id_table = rsb_spi_id,
};

int __init rsb_init(void)
{
	return spi_register_driver(&rsb_driver);
}

static void __exit rsb_exit(void)
{
	spi_unregister_driver(&rsb_driver);
}


module_init(rsb_init);
module_exit(rsb_exit);

MODULE_AUTHOR("Prashant Malani");
MODULE_LICENSE("GPL");
