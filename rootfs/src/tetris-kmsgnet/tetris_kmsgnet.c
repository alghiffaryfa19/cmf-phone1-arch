// SPDX-License-Identifier: GPL-2.0
/*
 * tetris_kmsgnet: debug aid, sends the kernel log by UDP to the PC over USB.
 *
 * A device hanging in system suspend leaves no trace: userspace (journald) is frozen
 * and pstore is only written on a panic. netconsole would need CONFIG_NETPOLL, which
 * changes struct net_device and so every module. This is a plain non-freezable
 * kthread instead, working as long as usb0 does. If a suspend takes longer than
 * hang_secs, it also logs the stacks of the blocked tasks.
 *
 *   PC:    socat -u UDP-RECV:6666 - > log
 *   Phone: modprobe tetris_kmsgnet [ip=172.16.42.2]
 */
#include <linux/delay.h>
#include <linux/inet.h>
#include <linux/kmsg_dump.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/net.h>
#include <linux/sysrq.h>
#include <linux/suspend.h>
#include <net/sock.h>

static char *ip = "172.16.42.2";
module_param(ip, charp, 0444);
static ushort port = 6666;
module_param(port, ushort, 0444);
static uint hang_secs = 20;
module_param(hang_secs, uint, 0644);
static bool backlog;
module_param(backlog, bool, 0444);
MODULE_PARM_DESC(backlog, "Also send the log from before loading (slow, it is paced)");

static struct task_struct *thread;
static struct socket *sock;
static struct sockaddr_in addr;
static unsigned long suspend_since;	/* jiffies, 0 if not suspending */

static int pm_event(struct notifier_block *nb, unsigned long ev, void *unused)
{
	if (ev == PM_SUSPEND_PREPARE)
		WRITE_ONCE(suspend_since, jiffies | 1);
	else if (ev == PM_POST_SUSPEND)
		WRITE_ONCE(suspend_since, 0);
	return NOTIFY_DONE;
}

static struct notifier_block pm_nb = { .notifier_call = pm_event };

static void send_line(const char *buf, size_t len)
{
	struct kvec vec = { .iov_base = (void *)buf, .iov_len = len };
	struct msghdr msg = { .msg_name = &addr, .msg_namelen = sizeof(addr),
			      .msg_flags = MSG_DONTWAIT };

	kernel_sendmsg(sock, &msg, &vec, 1, len);
}

static int kmsgnet_thread(void *unused)
{
	struct kmsg_dump_iter iter;
	static char line[1024];
	bool dumped = false;
	size_t len;

	kmsg_dump_rewind(&iter);
	if (!backlog)
		while (kmsg_dump_get_line(&iter, true, line, sizeof(line), &len))
			;
	while (!kthread_should_stop()) {
		unsigned long since = READ_ONCE(suspend_since);

		/* Pace it: too fast overruns the USB queue and drops lines */
		while (kmsg_dump_get_line(&iter, true, line, sizeof(line), &len)) {
			send_line(line, len);
			usleep_range(500, 1000);
		}

		if (!since) {
			dumped = false;
		} else if (!dumped && time_after(jiffies, since + hang_secs * HZ)) {
			dumped = true;
			pr_info("tetris_kmsgnet: suspend stuck for %us, blocked tasks:\n", hang_secs);
			handle_sysrq('w');
		}
		msleep(100);
	}
	return 0;
}

static int __init kmsgnet_init(void)
{
	int ret;

	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = in_aton(ip);

	ret = sock_create_kern(&init_net, AF_INET, SOCK_DGRAM, IPPROTO_UDP, &sock);
	if (ret)
		return ret;
	register_pm_notifier(&pm_nb);
	thread = kthread_run(kmsgnet_thread, NULL, "tetris_kmsgnet");
	if (IS_ERR(thread)) {
		ret = PTR_ERR(thread);
		unregister_pm_notifier(&pm_nb);
		sock_release(sock);
		return ret;
	}
	return 0;
}

static void __exit kmsgnet_exit(void)
{
	kthread_stop(thread);
	unregister_pm_notifier(&pm_nb);
	sock_release(sock);
}

module_init(kmsgnet_init);
module_exit(kmsgnet_exit);
MODULE_DESCRIPTION("Send the kernel log by UDP, also during system suspend");
MODULE_LICENSE("GPL");
