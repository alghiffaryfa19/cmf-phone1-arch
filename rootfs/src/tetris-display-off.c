// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Server profile: switch the panel off (DPMS off on every connector) and keep DRM master,
 * so nothing turns it on again. Without a compositor the panel would otherwise stay lit
 * with the last picture of the boot splash. Stopping the service (closing the fd) gives
 * the display back.
 *
 *   tetris-display-off [/dev/dri/card0]
 */
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int dpms_off(int fd, uint32_t conn)
{
	struct drm_mode_obj_get_properties op = { .obj_id = conn, .obj_type = DRM_MODE_OBJECT_CONNECTOR };
	uint32_t props[64];
	uint64_t vals[64];

	if (ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &op) < 0)
		return -1;
	if (op.count_props > 64)
		op.count_props = 64;
	op.props_ptr = (uintptr_t)props;
	op.prop_values_ptr = (uintptr_t)vals;
	if (ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &op) < 0)
		return -1;
	for (uint32_t i = 0; i < op.count_props; i++) {
		struct drm_mode_get_property p = { .prop_id = props[i] };

		if (ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &p) < 0 || strcmp(p.name, "DPMS"))
			continue;
		struct drm_mode_obj_set_property sp = {
			.value = DRM_MODE_DPMS_OFF, .prop_id = p.prop_id,
			.obj_id = conn, .obj_type = DRM_MODE_OBJECT_CONNECTOR,
		};
		return ioctl(fd, DRM_IOCTL_MODE_OBJ_SETPROPERTY, &sp);
	}
	errno = ENOENT;
	return -1;
}

int main(int argc, char **argv)
{
	const char *dev = argc > 1 ? argv[1] : "/dev/dri/card0";
	struct drm_mode_card_res res = { 0 };
	uint32_t conns[16];
	int fd = -1, off = 0;

	/* The boot splash may still hold DRM master for a moment */
	for (int i = 0; i < 60; i++) {
		if (fd < 0)
			fd = open(dev, O_RDWR | O_CLOEXEC);
		if (fd >= 0 && ioctl(fd, DRM_IOCTL_SET_MASTER, 0) == 0)
			break;
		sleep(1);
	}
	if (fd < 0 || ioctl(fd, DRM_IOCTL_SET_MASTER, 0) < 0) {
		perror("tetris-display-off: DRM master");
		return 1;
	}
	if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
		perror("tetris-display-off: resources");
		return 1;
	}
	if (res.count_connectors > 16)
		res.count_connectors = 16;
	struct drm_mode_card_res r2 = { .connector_id_ptr = (uintptr_t)conns,
					.count_connectors = res.count_connectors };
	if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &r2) < 0) {
		perror("tetris-display-off: connectors");
		return 1;
	}
	for (uint32_t i = 0; i < r2.count_connectors; i++) {
		if (dpms_off(fd, conns[i]) == 0)
			off++;
		else
			fprintf(stderr, "tetris-display-off: connector %u: %s\n", conns[i], strerror(errno));
	}
	printf("tetris-display-off: %d of %u connectors off\n", off, r2.count_connectors);
	fflush(stdout);
	/* Hold DRM master until stopped */
	for (;;)
		pause();
}
