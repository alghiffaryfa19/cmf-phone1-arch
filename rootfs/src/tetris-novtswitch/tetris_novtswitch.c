// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * No VT switch around suspend. The kernel moves to a text console on suspend unless some
 * driver says it doesn't need that (pm_vt_switch_required); without fbdev nobody does.
 * logind then sees the session leave and come back, and phoc turns the display on at
 * every resume, so a wakeup by WLAN or the modem would light the screen.
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm.h>

static struct platform_device *pdev;

static int __init novt_init(void)
{
	pdev = platform_device_register_simple("tetris_novtswitch", -1, NULL, 0);
	if (IS_ERR(pdev))
		return PTR_ERR(pdev);
	pm_vt_switch_required(&pdev->dev, false);
	return 0;
}

static void __exit novt_exit(void)
{
	pm_vt_switch_unregister(&pdev->dev);
	platform_device_unregister(pdev);
}

module_init(novt_init);
module_exit(novt_exit);
MODULE_DESCRIPTION("Tell the PM core that no VT switch is needed around suspend");
MODULE_LICENSE("GPL");
