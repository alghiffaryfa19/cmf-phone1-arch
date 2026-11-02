// SPDX-License-Identifier: GPL-2.0
/*
 * tetris-mdtty-bridge: copy between the modem's AT channel (/dev/ttyC0) and the
 * tetris_mdtty pair's far end (/dev/ttyMD1), so ModemManager on /dev/ttyMD0 sees
 * the modem. Exits on a modem-side error (modem reset): systemd restarts it.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <termios.h>
#include <unistd.h>

static int copy(int from, int to)
{
	char buf[4096];
	ssize_t n = read(from, buf, sizeof(buf));

	if (n < 0)
		return errno == EAGAIN || errno == EINTR ? 0 : -1;
	for (ssize_t off = 0; off < n;) {
		ssize_t w = write(to, buf + off, n - off);

		if (w < 0) {
			if (errno != EAGAIN && errno != EINTR)
				return -1;
			poll(&(struct pollfd){ .fd = to, .events = POLLOUT }, 1, 100);
			continue;
		}
		off += w;
	}
	return 0;
}

int main(void)
{
	int md = open("/dev/ttyC0", O_RDWR | O_NOCTTY | O_NONBLOCK);
	int tty = open("/dev/ttyMD1", O_RDWR | O_NOCTTY | O_NONBLOCK);
	struct termios t;

	if (md < 0 || tty < 0) {
		perror("open");
		return 1;
	}
	if (tcgetattr(tty, &t) == 0) {
		cfmakeraw(&t);
		tcsetattr(tty, TCSANOW, &t);
	}

	struct pollfd p[2] = { { .fd = md, .events = POLLIN }, { .fd = tty, .events = POLLIN } };

	for (;;) {
		if (poll(p, 2, -1) < 0) {
			if (errno == EINTR)
				continue;
			perror("poll");
			return 1;
		}
		if ((p[0].revents & POLLIN) && copy(md, tty) < 0) {
			perror("modem -> tty");
			return 1;
		}
		if ((p[1].revents & POLLIN) && copy(tty, md) < 0) {
			perror("tty -> modem");
			return 1;
		}
		if ((p[0].revents | p[1].revents) & (POLLERR | POLLNVAL)) {
			fprintf(stderr, "port error\n");
			return 1;
		}
	}
}
