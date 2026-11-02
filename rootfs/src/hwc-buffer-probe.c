// SPDX-License-Identifier: GPL-2.0
/*
 * hwc-buffer-probe: prototype of the wlroots hwcomposer path planned for phoc.
 *
 *   gralloc buffers (eglHybrisCreateNativeBuffer)
 *     -> GLES renders into them through EGLImage(EGL_NATIVE_BUFFER_ANDROID) + FBO
 *     -> hwc2 client target -> validate/present on the panel
 *
 * This is exactly what a wlroots allocator (buffers), the gles2 renderer (render
 * target) and an hwcomposer backend (output commit) have to do, without window
 * surfaces. Runs for argv[1] seconds (default 5) showing a colour cycle.
 * Needs the Halium container with gralloc + composer HALs, and phoc stopped.
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <hybris/hwc2/hwc2_compatibility_layer.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define NBUF 3
#define HAL_PIXEL_FORMAT_RGBA_8888 1
#define USAGE_HW_TEXTURE 0x100
#define USAGE_HW_RENDER 0x200
#define USAGE_HW_COMPOSER 0x800
#ifndef EGL_NATIVE_BUFFER_ANDROID
#define EGL_NATIVE_BUFFER_ANDROID 0x3140
#endif

typedef EGLBoolean (*PFN_CREATE_NB)(EGLint w, EGLint h, EGLint usage, EGLint format,
				    EGLint *stride, EGLClientBuffer *buffer);

static hwc2_compat_device_t *dev;

static void on_vsync(HWC2EventListener *l, int32_t seq, hwc2_display_t d, int64_t ts) {}
static void on_hotplug(HWC2EventListener *l, int32_t seq, hwc2_display_t d, bool conn, bool primary)
{
	printf("hotplug display %llu connected=%d primary=%d\n", (unsigned long long)d, conn, primary);
	hwc2_compat_device_on_hotplug(dev, d, conn);
}
static void on_refresh(HWC2EventListener *l, int32_t seq, hwc2_display_t d) {}

static HWC2EventListener listener = {
	.on_vsync_received = on_vsync,
	.on_hotplug_received = on_hotplug,
	.on_refresh_received = on_refresh,
};

int main(int argc, char **argv)
{
	double secs = argc > 1 ? atof(argv[1]) : 5;
	hwc2_compat_display_t *disp = NULL;

	dev = hwc2_compat_device_new(false);
	hwc2_compat_device_register_callback(dev, &listener, 0);
	for (int i = 0; i < 50 && !(disp = hwc2_compat_device_get_display_by_id(dev, 0)); i++)
		usleep(100000);
	if (!disp) {
		fprintf(stderr, "no hwc2 display 0\n");
		return 1;
	}
	hwc2_compat_display_set_power_mode(disp, HWC2_POWER_MODE_ON);
	HWC2DisplayConfig *cfg = hwc2_compat_display_get_active_config(disp);
	int W = cfg->width, H = cfg->height;
	printf("display %dx%d, vsync period %lld ns\n", W, H, (long long)cfg->vsyncPeriod);

	hwc2_compat_layer_t *layer = hwc2_compat_display_create_layer(disp);
	hwc2_compat_layer_set_composition_type(layer, HWC2_COMPOSITION_CLIENT);
	hwc2_compat_layer_set_blend_mode(layer, HWC2_BLEND_MODE_NONE);
	hwc2_compat_layer_set_source_crop(layer, 0.0f, 0.0f, W, H);
	hwc2_compat_layer_set_display_frame(layer, 0, 0, W, H);
	hwc2_compat_layer_set_visible_region(layer, 0, 0, W, H);

	/* EGL without any window surface: surfaceless context */
	EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	EGLint n;
	EGLConfig c;
	const EGLint ca[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
			      EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
	const EGLint xa[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	eglInitialize(d, NULL, NULL);
	eglChooseConfig(d, ca, &c, 1, &n);
	EGLContext x = eglCreateContext(d, c, EGL_NO_CONTEXT, xa);
	if (!eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, x)) {
		fprintf(stderr, "surfaceless make current failed: 0x%x\n", eglGetError());
		return 1;
	}
	printf("GL_RENDERER %s\n", glGetString(GL_RENDERER));

	PFN_CREATE_NB create_nb = (PFN_CREATE_NB)eglGetProcAddress("eglHybrisCreateNativeBuffer");
	PFNEGLCREATEIMAGEKHRPROC create_image = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
	PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC rb_storage =
		(PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC)eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES");
	if (!create_nb || !create_image || !rb_storage) {
		fprintf(stderr, "missing entry points %p %p %p\n", create_nb, create_image, rb_storage);
		return 1;
	}

	EGLClientBuffer buf[NBUF];
	GLuint fbo[NBUF], rb[NBUF];
	for (int i = 0; i < NBUF; i++) {
		EGLint stride;
		if (!create_nb(W, H, USAGE_HW_TEXTURE | USAGE_HW_RENDER | USAGE_HW_COMPOSER,
			       HAL_PIXEL_FORMAT_RGBA_8888, &stride, &buf[i])) {
			fprintf(stderr, "gralloc alloc %d failed\n", i);
			return 1;
		}
		const EGLint ia[] = { EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE };
		EGLImageKHR img = create_image(d, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, buf[i], ia);
		if (img == EGL_NO_IMAGE_KHR) {
			fprintf(stderr, "eglCreateImageKHR %d failed: 0x%x\n", i, eglGetError());
			return 1;
		}
		glGenRenderbuffers(1, &rb[i]);
		glBindRenderbuffer(GL_RENDERBUFFER, rb[i]);
		rb_storage(GL_RENDERBUFFER, img);
		glGenFramebuffers(1, &fbo[i]);
		glBindFramebuffer(GL_FRAMEBUFFER, fbo[i]);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb[i]);
		GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
		printf("buffer %d: stride %d, fbo %s\n", i, stride, st == GL_FRAMEBUFFER_COMPLETE ? "complete" : "INCOMPLETE");
		if (st != GL_FRAMEBUFFER_COMPLETE)
			return 1;
	}

	struct timespec t0, t;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	int frames = 0, errors = 0;
	for (;; frames++) {
		clock_gettime(CLOCK_MONOTONIC, &t);
		double el = (t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) / 1e9;
		if (el > secs)
			break;
		int i = frames % NBUF;
		glBindFramebuffer(GL_FRAMEBUFFER, fbo[i]);
		glViewport(0, 0, W, H);
		glClearColor(0.5 + 0.5 * sin(el * 2), 0.5 + 0.5 * sin(el * 2 + 2.1), 0.5 + 0.5 * sin(el * 2 + 4.2), 1);
		glClear(GL_COLOR_BUFFER_BIT);
		glScissor(W / 4, H / 4 + (int)(H / 4 * sin(el * 3)), W / 2, H / 8);
		glEnable(GL_SCISSOR_TEST);
		glClearColor(1, 1, 1, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		glDisable(GL_SCISSOR_TEST);
		glFinish();	/* acquire fence -1: rendering is done */

		uint32_t ntypes, nreq;
		hwc2_error_t e = hwc2_compat_display_validate(disp, &ntypes, &nreq);
		if (e != HWC2_ERROR_NONE && e != HWC2_ERROR_HAS_CHANGES) { errors++; continue; }
		if (ntypes || nreq)
			hwc2_compat_display_accept_changes(disp);
		hwc2_compat_display_set_client_target(disp, i, (struct ANativeWindowBuffer *)buf[i], -1,
						      HAL_DATASPACE_UNKNOWN);
		int32_t present_fence = -1;
		if (hwc2_compat_display_present(disp, &present_fence) != HWC2_ERROR_NONE)
			errors++;
		hwc2_compat_out_fences_t *fences;
		if (hwc2_compat_display_get_release_fences(disp, &fences) == HWC2_ERROR_NONE)
			hwc2_compat_out_fences_destroy(fences);
		if (present_fence >= 0)
			close(present_fence);
	}
	printf("%d frames in %.1f s (%.1f fps), %d errors\n", frames, secs, frames / secs, errors);
	return errors ? 1 : 0;
}
