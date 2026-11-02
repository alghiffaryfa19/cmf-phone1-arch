// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Force-feedback input device for feedbackd, backed by the haptic driver's timed
 * vibrator (/sys/class/leds/vibrator_nt, driver haptic_rt: duration in ms, then
 * activate; the driver only accepts Nothing's magic activate value 101 and stops
 * on its own timer).
 * feedbackd only drives force-feedback input devices; this creates one via uinput
 * (rumble, plus periodic/constant like the vibrators feedbackd knows) and plays each
 * uploaded effect for its replay length; strength is ignored.
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define VIB "/sys/class/leds/vibrator_nt/"
#define MAX_EFFECTS 16

static unsigned int lengths[MAX_EFFECTS];

static void sysfs_write(const char *attr, unsigned int val)
{
	char path[128], buf[16];
	int fd, len;

	snprintf(path, sizeof(path), VIB "%s", attr);
	len = snprintf(buf, sizeof(buf), "%u", val);
	fd = open(path, O_WRONLY);
	if (fd < 0 || write(fd, buf, len) != len)
		fprintf(stderr, "tetris-vibra: %s: %s\n", path, strerror(errno));
	if (fd >= 0)
		close(fd);
}

#define ACTIVATE 101

/* ms = 0 stops: there is no stop command, so restart with a 1 ms run */
static void vibrate(unsigned int ms)
{
	sysfs_write("duration", ms ? ms : 1);
	sysfs_write("activate", ACTIVATE);
}

int main(void)
{
	struct uinput_setup setup = { .id = { .bustype = BUS_VIRTUAL }, .ff_effects_max = MAX_EFFECTS };
	struct input_event ev;
	int fd;

	fd = open("/dev/uinput", O_RDWR);
	if (fd < 0) {
		perror("tetris-vibra: /dev/uinput");
		return 1;
	}
	strcpy(setup.name, "tetris-vibra");
	if (ioctl(fd, UI_SET_EVBIT, EV_FF) || ioctl(fd, UI_SET_FFBIT, FF_RUMBLE) ||
	    ioctl(fd, UI_SET_FFBIT, FF_PERIODIC) || ioctl(fd, UI_SET_FFBIT, FF_SINE) ||
	    ioctl(fd, UI_SET_FFBIT, FF_CONSTANT) || ioctl(fd, UI_SET_FFBIT, FF_GAIN) ||
	    ioctl(fd, UI_DEV_SETUP, &setup) || ioctl(fd, UI_DEV_CREATE)) {
		perror("tetris-vibra: uinput setup");
		return 1;
	}

	while (read(fd, &ev, sizeof(ev)) == sizeof(ev)) {
		if (ev.type == EV_UINPUT && ev.code == UI_FF_UPLOAD) {
			struct uinput_ff_upload up = { .request_id = ev.value };

			if (ioctl(fd, UI_BEGIN_FF_UPLOAD, &up))
				continue;
			if (up.effect.id >= 0 && up.effect.id < MAX_EFFECTS)
				lengths[up.effect.id] = up.effect.replay.length;
			else
				up.retval = -EINVAL;
			ioctl(fd, UI_END_FF_UPLOAD, &up);
		} else if (ev.type == EV_UINPUT && ev.code == UI_FF_ERASE) {
			struct uinput_ff_erase er = { .request_id = ev.value };

			if (ioctl(fd, UI_BEGIN_FF_ERASE, &er))
				continue;
			if (er.effect_id < MAX_EFFECTS)
				lengths[er.effect_id] = 0;
			ioctl(fd, UI_END_FF_ERASE, &er);
		} else if (ev.type == EV_FF && ev.code < MAX_EFFECTS) {
			/* value = repeat count to start, 0 to stop; a length of 0 means "until
			 * stopped", which the timed vibrator caps at 10 s */
			vibrate(ev.value ? (lengths[ev.code] ? lengths[ev.code] : 10000) : 0);
		}
	}
	perror("tetris-vibra: read");
	return 1;
}
