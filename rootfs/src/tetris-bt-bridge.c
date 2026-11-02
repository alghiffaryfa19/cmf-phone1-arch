// SPDX-License-Identifier: GPL-2.0
/*
 * tetris-bt-bridge: expose the MTK connac2 Bluetooth (/dev/stpbt, Android HAL interface)
 * to BlueZ as a normal hci device through /dev/vhci.
 *
 * Opening /dev/stpbt powers BT on and downloads the firmware in the kernel; after that
 * it speaks plain H:4 (type byte + HCI packet) in both directions. /dev/vhci takes and
 * returns exactly one H:4 packet per write/read, while /dev/stpbt reads are a byte
 * stream, so the stpbt -> vhci direction is re-framed here.
 *
 * Optional: argv[1] = file with the 6-byte BD address (stock: nvdata APCFG/APRDEB/BT_Addr);
 * it is set with the MTK vendor command 0xfc1a right after HCI_Reset completes.
 *
 * Controller quirk: it advertises Synchronization Train support (LMP features page 2) and
 * Read Synchronization Train Parameters (0x0c77) but rejects that command, which aborts
 * the kernel's hci0 setup. Both are hidden from BlueZ.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define HCI_COMMAND_PKT	0x01
#define HCI_ACLDATA_PKT	0x02
#define HCI_SCODATA_PKT	0x03
#define HCI_EVENT_PKT	0x04
#define HCI_ISODATA_PKT	0x05
#define HCI_VENDOR_PKT	0xff
#define HCI_PRIMARY	0x00

static volatile sig_atomic_t stop;
static uint8_t bdaddr[6];
static int have_bdaddr;

static void on_signal(int sig) { (void)sig; stop = 1; }

static int write_all(int fd, const uint8_t *p, size_t n)
{
	while (n) {
		ssize_t w = write(fd, p, n);

		if (w < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		p += w;
		n -= (size_t)w;
	}
	return 0;
}

/* Total length of the H:4 packet at buf (incl. type byte), 0 if the header is incomplete. */
static size_t h4_len(const uint8_t *b, size_t have)
{
	switch (b[0]) {
	case HCI_EVENT_PKT:
		return have >= 3 ? 3u + b[2] : 0;
	case HCI_ACLDATA_PKT:
		return have >= 5 ? 5u + (b[3] | b[4] << 8) : 0;
	case HCI_SCODATA_PKT:
		return have >= 4 ? 4u + b[3] : 0;
	case HCI_ISODATA_PKT:
		return have >= 5 ? 5u + ((b[3] | b[4] << 8) & 0x3fff) : 0;
	default:
		return SIZE_MAX;	/* unknown type: resync */
	}
}

/* Command Complete event: opcode, or 0 if ev isn't one. */
static unsigned int cc_opcode(const uint8_t *ev, size_t len)
{
	if (len < 7 || ev[0] != HCI_EVENT_PKT || ev[1] != 0x0e)
		return 0;
	return ev[4] | ev[5] << 8;
}

/* Fix up controller replies before BlueZ sees them; returns 0 to drop the packet. */
static int filter_event(uint8_t *ev, size_t len)
{
	switch (cc_opcode(ev, len)) {
	case 0xfc1a:	/* our own set-BD-address command */
		return 0;
	case 0x1002:	/* Read Local Supported Commands: status @6, commands[64] @7 */
		if (len >= 7 + 64 && ev[6] == 0x00)
			ev[7 + 22] &= (uint8_t)~0x04;	/* Read Sync Train Params */
		break;
	case 0x1004:	/* Read Local Extended Features: status @6, page @7, max @8, features @9 */
		if (len >= 9 + 8 && ev[6] == 0x00 && ev[7] == 2)
			ev[9] &= (uint8_t)~0x04;	/* LMP_SYNC_TRAIN */
		break;
	}
	return 1;
}

/* Command Complete for HCI_Reset (opcode 0x0c03) -> program the BD address. */
static void maybe_set_bdaddr(int stpbt, const uint8_t *ev, size_t len)
{
	uint8_t cmd[4 + 6] = { HCI_COMMAND_PKT, 0x1a, 0xfc, 6 };

	if (!have_bdaddr || len < 7 || ev[0] != HCI_EVENT_PKT || ev[1] != 0x0e ||
	    ev[4] != 0x03 || ev[5] != 0x0c || ev[6] != 0x00)
		return;
	memcpy(cmd + 4, bdaddr, 6);
	if (write_all(stpbt, cmd, sizeof(cmd)) < 0)
		perror("set bdaddr");
}

static int load_bdaddr(const char *path)
{
	int fd = open(path, O_RDONLY);
	uint8_t raw[6];
	ssize_t n;

	if (fd < 0)
		return -1;
	n = read(fd, raw, sizeof(raw));
	close(fd);
	if (n != sizeof(raw))
		return -1;
	/* nvdata stores it in display order (3c:b0:..), HCI wants little endian */
	for (int i = 0; i < 6; i++)
		bdaddr[i] = raw[5 - i];
	for (int i = 0; i < 6; i++)
		if (bdaddr[i] != 0x00 && bdaddr[i] != 0xff)
			return 0;
	return -1;	/* all zero / unprogrammed */
}

int main(int argc, char **argv)
{
	static uint8_t sbuf[65536 + 8];
	static uint8_t vbuf[65536 + 8];
	size_t sfill = 0;
	uint8_t create[2] = { HCI_VENDOR_PKT, HCI_PRIMARY };
	struct pollfd pfd[2];
	int stpbt, vhci;

	if (argc > 1 && load_bdaddr(argv[1]) == 0) {
		have_bdaddr = 1;
		fprintf(stderr, "BD address from %s: %02X:%02X:%02X:%02X:%02X:%02X\n",
			argv[1], bdaddr[5], bdaddr[4], bdaddr[3], bdaddr[2], bdaddr[1], bdaddr[0]);
	}

	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);

	stpbt = open("/dev/stpbt", O_RDWR | O_NOCTTY);
	if (stpbt < 0) {
		perror("/dev/stpbt");
		return 1;
	}
	vhci = open("/dev/vhci", O_RDWR);
	if (vhci < 0) {
		perror("/dev/vhci");
		return 1;
	}
	if (write_all(vhci, create, sizeof(create)) < 0) {
		perror("vhci create");
		return 1;
	}
	fprintf(stderr, "bridging /dev/stpbt <-> /dev/vhci\n");

	pfd[0] = (struct pollfd){ .fd = stpbt, .events = POLLIN };
	pfd[1] = (struct pollfd){ .fd = vhci, .events = POLLIN };

	while (!stop) {
		if (poll(pfd, 2, -1) < 0) {
			if (errno == EINTR)
				continue;
			perror("poll");
			break;
		}

		/* BlueZ -> controller: one packet per vhci read; vendor pkt is vhci's own reply */
		if (pfd[1].revents & POLLIN) {
			ssize_t n = read(vhci, vbuf, sizeof(vbuf));

			if (n <= 0)
				break;
			if (vbuf[0] != HCI_VENDOR_PKT && write_all(stpbt, vbuf, (size_t)n) < 0) {
				perror("write stpbt");
				break;
			}
		}

		/* controller -> BlueZ: byte stream, re-frame into H:4 packets */
		if (pfd[0].revents & POLLIN) {
			ssize_t n = read(stpbt, sbuf + sfill, sizeof(sbuf) - sfill);
			size_t off = 0;

			if (n <= 0) {
				if (n < 0 && errno == EINTR)
					continue;
				perror("read stpbt");
				break;
			}
			sfill += (size_t)n;
			while (off < sfill) {
				size_t len = h4_len(sbuf + off, sfill - off);

				if (len == SIZE_MAX) {
					fprintf(stderr, "dropping unknown packet type 0x%02x\n", sbuf[off]);
					off++;
					continue;
				}
				if (!len || sfill - off < len)
					break;
				if (filter_event(sbuf + off, len) &&
				    write_all(vhci, sbuf + off, len) < 0) {
					perror("write vhci");
					stop = 1;
					break;
				}
				maybe_set_bdaddr(stpbt, sbuf + off, len);
				off += len;
			}
			memmove(sbuf, sbuf + off, sfill - off);
			sfill -= off;
		}

		if ((pfd[0].revents | pfd[1].revents) & (POLLERR | POLLHUP))
			break;
	}

	close(vhci);
	close(stpbt);	/* powers BT off */
	return 0;
}
