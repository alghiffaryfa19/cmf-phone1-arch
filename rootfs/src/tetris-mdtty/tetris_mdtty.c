// SPDX-License-Identifier: GPL-2.0
/*
 * tetris_mdtty: a connected pair of real ttys for the CMF Phone 1 modem.
 *
 * The modem's AT channel /dev/ttyC0 is a plain CCCI char device (no tty, subsystem
 * ccci_node), which ModemManager can't use. ptys don't help either: their slaves
 * have no sysfs device, so udev can't tag them for ModemManager. This creates
 * /dev/ttyMD0 (for ModemManager) and /dev/ttyMD1 (for tetris-mdtty-bridge, which
 * copies between it and /dev/ttyC0); what's written to one is read from the other.
 * The ttys hang off a platform device with a bound driver because ModemManager
 * refuses devices without a kernel driver name.
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/tty.h>
#include <linux/tty_driver.h>
#include <linux/tty_flip.h>

#define MDTTY_PORTS 2

static struct tty_driver *mdtty_driver;
static struct tty_port mdtty_port[MDTTY_PORTS];

static const struct tty_port_operations mdtty_port_ops;

static struct platform_device *mdtty_pdev;

static int mdtty_probe(struct platform_device *pdev)
{
	return 0;
}

static struct platform_driver mdtty_platform_driver = {
	.probe = mdtty_probe,
	.driver = { .name = "tetris_mdtty" },
};

static int mdtty_open(struct tty_struct *tty, struct file *filp)
{
	return tty_port_open(&mdtty_port[tty->index], tty, filp);
}

static void mdtty_close(struct tty_struct *tty, struct file *filp)
{
	tty_port_close(&mdtty_port[tty->index], tty, filp);
}

static void mdtty_hangup(struct tty_struct *tty)
{
	tty_port_hangup(&mdtty_port[tty->index]);
}

static int mdtty_write(struct tty_struct *tty, const unsigned char *buf, int count)
{
	struct tty_port *peer = &mdtty_port[!tty->index];
	int done;

	/* Nobody on the other side: drop, like a line with no one listening */
	if (!tty_port_initialized(peer))
		return count;
	done = tty_insert_flip_string(peer, buf, count);
	if (done)
		tty_flip_buffer_push(peer);
	return done;
}

static unsigned int mdtty_write_room(struct tty_struct *tty)
{
	struct tty_port *peer = &mdtty_port[!tty->index];

	if (!tty_port_initialized(peer))
		return 4096;
	return tty_buffer_space_avail(peer);
}

static const struct tty_operations mdtty_ops = {
	.open = mdtty_open,
	.close = mdtty_close,
	.hangup = mdtty_hangup,
	.write = mdtty_write,
	.write_room = mdtty_write_room,
};

static void mdtty_destroy_ports(void)
{
	int i;

	for (i = 0; i < MDTTY_PORTS; i++)
		tty_port_destroy(&mdtty_port[i]);
}

static int __init mdtty_init(void)
{
	struct device *dev;
	int i, ret;

	ret = platform_driver_register(&mdtty_platform_driver);
	if (ret)
		return ret;
	mdtty_pdev = platform_device_register_simple("tetris_mdtty", -1, NULL, 0);
	if (IS_ERR(mdtty_pdev)) {
		ret = PTR_ERR(mdtty_pdev);
		goto err_driver;
	}

	mdtty_driver = tty_alloc_driver(MDTTY_PORTS, TTY_DRIVER_RESET_TERMIOS |
					TTY_DRIVER_REAL_RAW | TTY_DRIVER_DYNAMIC_DEV);
	if (IS_ERR(mdtty_driver)) {
		ret = PTR_ERR(mdtty_driver);
		goto err_pdev;
	}

	mdtty_driver->driver_name = "tetris_mdtty";
	mdtty_driver->name = "ttyMD";
	mdtty_driver->type = TTY_DRIVER_TYPE_SERIAL;
	mdtty_driver->subtype = SERIAL_TYPE_NORMAL;
	mdtty_driver->init_termios = tty_std_termios;
	/*
	 * Raw from the first open: with the default ECHO, every modem response gets
	 * sent back to the modem as a command, a feedback loop that floods the AT channel.
	 */
	mdtty_driver->init_termios.c_iflag = 0;
	mdtty_driver->init_termios.c_oflag = 0;
	mdtty_driver->init_termios.c_lflag = 0;
	mdtty_driver->init_termios.c_cflag = B115200 | CS8 | CREAD | HUPCL | CLOCAL;
	tty_set_operations(mdtty_driver, &mdtty_ops);

	for (i = 0; i < MDTTY_PORTS; i++) {
		tty_port_init(&mdtty_port[i]);
		mdtty_port[i].ops = &mdtty_port_ops;
	}

	ret = tty_register_driver(mdtty_driver);
	if (ret)
		goto err_ports;

	for (i = 0; i < MDTTY_PORTS; i++) {
		dev = tty_port_register_device(&mdtty_port[i], mdtty_driver, i, &mdtty_pdev->dev);
		if (IS_ERR(dev)) {
			ret = PTR_ERR(dev);
			goto err_devices;
		}
	}
	return 0;

err_devices:
	while (--i >= 0)
		tty_unregister_device(mdtty_driver, i);
	tty_unregister_driver(mdtty_driver);
err_ports:
	mdtty_destroy_ports();
	tty_driver_kref_put(mdtty_driver);
err_pdev:
	platform_device_unregister(mdtty_pdev);
err_driver:
	platform_driver_unregister(&mdtty_platform_driver);
	return ret;
}

static void __exit mdtty_exit(void)
{
	int i;

	for (i = 0; i < MDTTY_PORTS; i++)
		tty_unregister_device(mdtty_driver, i);
	tty_unregister_driver(mdtty_driver);
	mdtty_destroy_ports();
	tty_driver_kref_put(mdtty_driver);
	platform_device_unregister(mdtty_pdev);
	platform_driver_unregister(&mdtty_platform_driver);
}

module_init(mdtty_init);
module_exit(mdtty_exit);
MODULE_DESCRIPTION("Connected tty pair for the CMF Phone 1 modem AT channel");
MODULE_LICENSE("GPL");
