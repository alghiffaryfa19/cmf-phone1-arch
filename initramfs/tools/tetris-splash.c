// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Boot splash: Tux (the kernel's logo_linux_clut224.ppm), scaled up and centred on the
 * panel, drawn through a DRM dumb buffer. Started by the initramfs once the display
 * driver is up and kept running (the picture lives as long as the process holds the
 * framebuffer) until tetris-splash-stop.service (and tetris-android-prepare) kill it,
 * before the composer HAL or phoc need DRM master. As a failsafe it exits by itself
 * after 60 s: a composer that can't get DRM master spins at RT priority.
 *
 * Freestanding (no libc): the initramfs has only a static busybox, so this does its own
 * system calls. Build: see scripts/build-debug-boot.sh.
 *
 *   tetris-splash <image.ppm>     (ASCII "P3" PPM, max value 255)
 */
#include <stddef.h>
#include <stdint.h>

/* --- system calls (aarch64) ------------------------------------------------ */

#define SYS_ioctl   29
#define SYS_openat  56
#define SYS_close   57
#define SYS_read    63
#define SYS_ppoll   73
#define SYS_exit    93
#define SYS_mmap   222
#define AT_FDCWD  (-100)
#define O_RDWR      02
#define O_CLOEXEC   02000000

static long sys6(long n, long a, long b, long c, long d, long e, long f)
{
	register long x8 __asm__("x8") = n;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	register long x3 __asm__("x3") = d;
	register long x4 __asm__("x4") = e;
	register long x5 __asm__("x5") = f;

	__asm__ volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
			 : "memory");
	return x0;
}

#define sys(n, a, b, c) sys6(n, (long)(a), (long)(b), (long)(c), 0, 0, 0)

static __attribute__((noreturn)) void die(void)
{
	for (;;)
		sys(SYS_exit, 1, 0, 0);
}

/* --- DRM (include/uapi/drm/drm_mode.h) ---------------------------------------- */

struct drm_mode_modeinfo {
	uint32_t clock;
	uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
	uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
	uint32_t vrefresh, flags, type;
	char name[32];
};

struct drm_mode_card_res {
	uint64_t fb_id_ptr, crtc_id_ptr, connector_id_ptr, encoder_id_ptr;
	uint32_t count_fbs, count_crtcs, count_connectors, count_encoders;
	uint32_t min_width, max_width, min_height, max_height;
};

struct drm_mode_get_connector {
	uint64_t encoders_ptr, modes_ptr, props_ptr, prop_values_ptr;
	uint32_t count_modes, count_props, count_encoders;
	uint32_t encoder_id, connector_id, connector_type, connector_type_id;
	uint32_t connection, mm_width, mm_height, subpixel, pad;
};

struct drm_mode_get_encoder {
	uint32_t encoder_id, encoder_type, crtc_id, possible_crtcs, possible_clones;
};

struct drm_mode_crtc {
	uint64_t set_connectors_ptr;
	uint32_t count_connectors, crtc_id, fb_id, x, y, gamma_size, mode_valid;
	struct drm_mode_modeinfo mode;
};

struct drm_mode_create_dumb {
	uint32_t height, width, bpp, flags, handle, pitch;
	uint64_t size;
};

struct drm_mode_fb_cmd {
	uint32_t fb_id, width, height, pitch, bpp, depth, handle;
};

struct drm_mode_map_dumb {
	uint32_t handle, pad;
	uint64_t offset;
};

#define DRM_IOWR(nr, type) ((3u << 30) | ((uint32_t)sizeof(type) << 16) | ('d' << 8) | (nr))
#define DRM_IOCTL_MODE_GETRESOURCES DRM_IOWR(0xA0, struct drm_mode_card_res)
#define DRM_IOCTL_MODE_SETCRTC      DRM_IOWR(0xA2, struct drm_mode_crtc)
#define DRM_IOCTL_MODE_GETENCODER   DRM_IOWR(0xA6, struct drm_mode_get_encoder)
#define DRM_IOCTL_MODE_GETCONNECTOR DRM_IOWR(0xA7, struct drm_mode_get_connector)
#define DRM_IOCTL_MODE_ADDFB        DRM_IOWR(0xAE, struct drm_mode_fb_cmd)
#define DRM_IOCTL_MODE_CREATE_DUMB  DRM_IOWR(0xB2, struct drm_mode_create_dumb)
#define DRM_IOCTL_MODE_MAP_DUMB     DRM_IOWR(0xB3, struct drm_mode_map_dumb)

/* --- image -------------------------------------------------------------------- */

static unsigned char file[256 * 1024];
static uint32_t pixels[80 * 80];
static int img_w, img_h;

static const unsigned char *skip(const unsigned char *p, const unsigned char *end)
{
	while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t' || *p == '#')) {
		if (*p == '#')
			while (p < end && *p != '\n')
				p++;
		else
			p++;
	}
	return p;
}

static const unsigned char *number(const unsigned char *p, const unsigned char *end, int *v)
{
	p = skip(p, end);
	*v = 0;
	while (p < end && *p >= '0' && *p <= '9')
		*v = *v * 10 + (*p++ - '0');
	return p;
}

static int load_ppm(const char *path)
{
	long fd = sys6(SYS_openat, AT_FDCWD, (long)path, O_CLOEXEC, 0, 0, 0), n, len = 0;
	const unsigned char *p, *end;
	int maxval;

	if (fd < 0)
		return -1;
	while ((n = sys(SYS_read, fd, file + len, sizeof(file) - len)) > 0)
		len += n;
	sys(SYS_close, fd, 0, 0);
	if (len < 2 || file[0] != 'P' || file[1] != '3')
		return -1;
	end = file + len;
	p = number(file + 2, end, &img_w);
	p = number(p, end, &img_h);
	p = number(p, end, &maxval);
	if (img_w * img_h > (int)(sizeof(pixels) / sizeof(pixels[0])) || maxval != 255)
		return -1;
	for (int i = 0; i < img_w * img_h; i++) {
		int r, g, b;

		p = number(p, end, &r);
		p = number(p, end, &g);
		p = number(p, end, &b);
		pixels[i] = (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
	}
	return 0;
}

/* --- main ----------------------------------------------------------------------- */

static int run(int argc, char **argv)
{
	uint32_t crtcs[8], connectors[8], encoders[16], conn_enc[8];
	struct drm_mode_modeinfo modes[32];
	struct drm_mode_card_res res = { 0 };
	struct drm_mode_get_connector conn = { 0 };
	struct drm_mode_get_encoder enc = { 0 };
	struct drm_mode_create_dumb dumb = { 0 };
	struct drm_mode_fb_cmd fb = { 0 };
	struct drm_mode_map_dumb map = { 0 };
	struct drm_mode_crtc crtc = { 0 };
	uint32_t *buf, connector_id = 0;
	long fd;
	int scale, x0, y0;

	if (argc < 2 || load_ppm(argv[1]))
		return 1;
	fd = sys6(SYS_openat, AT_FDCWD, (long)"/dev/dri/card0", O_RDWR | O_CLOEXEC, 0, 0, 0);
	if (fd < 0)
		return 1;

	res.crtc_id_ptr = (uint64_t)crtcs;
	res.connector_id_ptr = (uint64_t)connectors;
	res.encoder_id_ptr = (uint64_t)encoders;
	res.count_crtcs = 8;
	res.count_connectors = 8;
	res.count_encoders = 16;
	if (sys(SYS_ioctl, fd, DRM_IOCTL_MODE_GETRESOURCES, &res) || !res.count_connectors)
		return 1;

	/* The first connected connector (the DSI panel) and its preferred (first) mode */
	for (uint32_t i = 0; i < res.count_connectors && i < 8 && !connector_id; i++) {
		struct drm_mode_get_connector c = { .connector_id = connectors[i] };

		c.modes_ptr = (uint64_t)modes;
		c.count_modes = 32;
		c.encoders_ptr = (uint64_t)conn_enc;
		c.count_encoders = 8;
		if (!sys(SYS_ioctl, fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) && c.connection == 1 &&
		    c.count_modes) {
			conn = c;
			connector_id = connectors[i];
		}
	}
	if (!connector_id)
		return 1;

	enc.encoder_id = conn.encoder_id ? conn.encoder_id : conn_enc[0];
	if (sys(SYS_ioctl, fd, DRM_IOCTL_MODE_GETENCODER, &enc))
		return 1;
	if (!enc.crtc_id) {
		for (uint32_t i = 0; i < res.count_crtcs && i < 8; i++)
			if (enc.possible_crtcs & (1u << i)) {
				enc.crtc_id = crtcs[i];
				break;
			}
	}

	dumb.width = modes[0].hdisplay;
	dumb.height = modes[0].vdisplay;
	dumb.bpp = 32;
	if (sys(SYS_ioctl, fd, DRM_IOCTL_MODE_CREATE_DUMB, &dumb))
		return 1;
	fb.width = dumb.width;
	fb.height = dumb.height;
	fb.pitch = dumb.pitch;
	fb.bpp = 32;
	fb.depth = 24;
	fb.handle = dumb.handle;
	if (sys(SYS_ioctl, fd, DRM_IOCTL_MODE_ADDFB, &fb))
		return 1;
	map.handle = dumb.handle;
	if (sys(SYS_ioctl, fd, DRM_IOCTL_MODE_MAP_DUMB, &map))
		return 1;
	buf = (uint32_t *)sys6(SYS_mmap, 0, dumb.size, 3 /* RW */, 1 /* SHARED */, fd, map.offset);
	if ((long)buf < 0 && (long)buf > -4096)
		return 1;

	/* Black, with Tux at 6/10 of the panel width, a bit above the middle */
	scale = dumb.width * 6 / 10 / img_w;
	if (scale < 1)
		scale = 1;
	x0 = (dumb.width - img_w * scale) / 2;
	y0 = (dumb.height - img_h * scale) * 2 / 5;
	for (uint32_t y = 0; y < dumb.height; y++) {
		uint32_t *row = (uint32_t *)((char *)buf + y * dumb.pitch);
		int iy = ((int)y - y0) / scale;

		for (uint32_t x = 0; x < dumb.width; x++) {
			int ix = ((int)x - x0) / scale;

			row[x] = (int)y >= y0 && iy < img_h && (int)x >= x0 && ix < img_w ?
				 pixels[iy * img_w + ix] : 0;
		}
	}

	crtc.crtc_id = enc.crtc_id;
	crtc.fb_id = fb.fb_id;
	crtc.set_connectors_ptr = (uint64_t)&connector_id;
	crtc.count_connectors = 1;
	crtc.mode = modes[0];
	crtc.mode_valid = 1;
	if (sys(SYS_ioctl, fd, DRM_IOCTL_MODE_SETCRTC, &crtc))
		return 1;

	/* Keep the framebuffer (and the picture) until we're killed, at most 60 s */
	for (int i = 0; i < 60; i++) {
		struct { long sec, nsec; } one_second = { 1, 0 };

		sys6(SYS_ppoll, 0, 0, (long)&one_second, 0, 0, 0);
	}
	return 0;
}

void __attribute__((noreturn, used)) cstart(long *sp)
{
	sys(SYS_exit, run((int)sp[0], (char **)(sp + 1)), 0, 0);
	die();
}

__asm__(".global _start\n_start:\n\tmov x0, sp\n\tb cstart\n");
