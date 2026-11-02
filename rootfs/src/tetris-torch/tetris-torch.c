// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Torch for Phosh: the flash LEDs (lm3644) are only V4L2 flash subdevices, while
 * Phosh's torch looks for an LED class device named "*:torch". This registers
 * "white:torch" through uleds and turns brightness changes into the subdevices'
 * torch mode and intensity.
 */
#include <dirent.h>
#include <fcntl.h>
#include <linux/uleds.h>
#include <linux/v4l2-controls.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define MAX_BRIGHTNESS 255

static int leds[4], nleds;

static void find_leds(void)
{
	DIR *d = opendir("/sys/class/video4linux");
	struct dirent *e;

	while (d && (e = readdir(d)) && nleds < 4) {
		char path[300], name[64] = "";
		FILE *f;

		snprintf(path, sizeof(path), "/sys/class/video4linux/%s/name", e->d_name);
		f = fopen(path, "r");
		if (!f)
			continue;
		if (fgets(name, sizeof(name), f) && !strncmp(name, "lm3644-led", 10)) {
			snprintf(path, sizeof(path), "/dev/%s", e->d_name);
			leds[nleds] = open(path, O_RDWR);
			if (leds[nleds] >= 0)
				nleds++;
		}
		fclose(f);
	}
	if (d)
		closedir(d);
}

static void set_ctrl(int fd, unsigned int id, int value)
{
	struct v4l2_control c = { .id = id, .value = value };

	if (ioctl(fd, VIDIOC_S_CTRL, &c))
		perror("tetris-torch: VIDIOC_S_CTRL");
}

static void set_torch(int brightness)
{
	for (int i = 0; i < nleds; i++) {
		struct v4l2_queryctrl q = { .id = V4L2_CID_FLASH_TORCH_INTENSITY };

		if (brightness && !ioctl(leds[i], VIDIOC_QUERYCTRL, &q)) {
			int v = q.minimum + (long long)(q.maximum - q.minimum) * brightness / MAX_BRIGHTNESS;

			set_ctrl(leds[i], V4L2_CID_FLASH_TORCH_INTENSITY, v - (v - q.minimum) % q.step);
		}
		set_ctrl(leds[i], V4L2_CID_FLASH_LED_MODE,
			 brightness ? V4L2_FLASH_LED_MODE_TORCH : V4L2_FLASH_LED_MODE_NONE);
	}
}

int main(void)
{
	struct uleds_user_dev dev = { .name = "white:torch", .max_brightness = MAX_BRIGHTNESS };
	int fd, brightness;

	find_leds();
	if (!nleds) {
		fprintf(stderr, "tetris-torch: no lm3644 flash subdevice\n");
		return 1;
	}
	fd = open("/dev/uleds", O_RDWR);
	if (fd < 0 || write(fd, &dev, sizeof(dev)) != sizeof(dev)) {
		perror("tetris-torch: /dev/uleds");
		return 1;
	}
	while (read(fd, &brightness, sizeof(brightness)) == sizeof(brightness))
		set_torch(brightness);
	perror("tetris-torch: read");
	return 1;
}
