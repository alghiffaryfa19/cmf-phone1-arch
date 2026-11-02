// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Kamera: small GTK4 camera app for the CMF Phone 1. Talks to the Android camera HAL
 * in the Halium container through the NDK Camera2 API (ndkcam.c, libhybris).
 * Preview frames arrive as YUV_420_888 on a binder thread, are converted to RGB at
 * half resolution and rotated upright there, then shown as a GdkMemoryTexture.
 * Photos are JPEGs from the HAL, saved to the XDG pictures directory.
 *
 * The second back camera is a monochrome IR depth sensor. Its "IR" mode stretches the
 * contrast (1st..99th percentile) and can map brightness to false colours, which makes
 * the IR look-through effect on thin plastics visible; its photos are saved as PNG from
 * the full-resolution stream with the same processing.
 */
#include <gtk/gtk.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>
#include "ndkcam.h"

#define APP_ID "eu.tetris.Camera"

struct cam_info {
	char id[8];
	const char *label;
	int orientation;      /* sensor orientation, degrees clockwise */
	bool front;
	bool ir;               /* monochrome IR sensor: processed luma only */
	int pw, ph;           /* preview stream size */
	int jw, jh;           /* still size */
};

static struct ndkcam n;
static ACameraManager *mgr;
static struct cam_info cams[3];
static int ncams, cur;

/* Camera session state; only touched on the main thread except the readers' callbacks */
static ACameraDevice *dev;
static ACameraCaptureSession *sess;
static ACaptureSessionOutputContainer *outs;
static ACaptureSessionOutput *prev_out, *jpeg_out;
static ACameraOutputTarget *prev_t, *jpeg_t;
static ACaptureRequest *prev_req, *still_req;
static AImageReader *prev_r, *jpeg_r;
static atomic_bool frame_queued, ir_snapshot;
static atomic_int ir_mode;            /* 0 contrast, 1 false colour */

static GtkWidget *picture, *shutter, *switcher, *status, *mode_button;

/* --- frames ------------------------------------------------------------------ */

struct frame {
	int w, h;
	GBytes *rgb;
};

static char *photo_path(const char *ext);

static gboolean show_frame(gpointer data)
{
	struct frame *f = data;
	GdkTexture *tex = gdk_memory_texture_new(f->w, f->h, GDK_MEMORY_R8G8B8, f->rgb, f->w * 3);

	gtk_picture_set_paintable(GTK_PICTURE(picture), GDK_PAINTABLE(tex));
	g_object_unref(tex);
	g_bytes_unref(f->rgb);
	g_free(f);
	frame_queued = false;
	return G_SOURCE_REMOVE;
}

static inline uint8_t clamp8(int v)
{
	return v < 0 ? 0 : v > 255 ? 255 : v;
}

/* YUV_420_888 -> RGB at half size, rotated by `rot` degrees clockwise, mirrored for
 * the front camera so the preview behaves like a mirror. */
static struct frame *convert(AImage *img, int rot, bool mirror)
{
	uint8_t *y, *u, *v;
	int ylen, ulen, vlen, ys, us, ups;
	int32_t w, h;
	struct frame *f;
	uint8_t *out;
	int ow, oh;

	n.AImage_getWidth(img, &w);
	n.AImage_getHeight(img, &h);
	n.AImage_getPlaneData(img, 0, &y, &ylen);
	n.AImage_getPlaneData(img, 1, &u, &ulen);
	n.AImage_getPlaneData(img, 2, &v, &vlen);
	n.AImage_getPlaneRowStride(img, 0, &ys);
	n.AImage_getPlaneRowStride(img, 1, &us);
	n.AImage_getPlanePixelStride(img, 1, &ups);

	w /= 2;
	h /= 2;
	ow = rot % 180 ? h : w;
	oh = rot % 180 ? w : h;
	out = g_malloc(ow * oh * 3);
	for (int r = 0; r < h; r++) {
		const uint8_t *yr = y + 2 * r * ys;
		const uint8_t *ur = u + r * us, *vr = v + r * us;

		for (int c = 0; c < w; c++) {
			int Y = yr[2 * c] - 16, U = ur[c * ups] - 128, V = vr[c * ups] - 128;
			int x, yy;
			uint8_t *p;

			switch (rot) {
			case 90:  x = h - 1 - r; yy = c; break;
			case 180: x = w - 1 - c; yy = h - 1 - r; break;
			case 270: x = r; yy = w - 1 - c; break;
			default:  x = c; yy = r; break;
			}
			if (mirror)
				x = ow - 1 - x;
			p = out + (yy * ow + x) * 3;
			Y = 298 * (Y < 0 ? 0 : Y);
			p[0] = clamp8((Y + 409 * V + 128) >> 8);
			p[1] = clamp8((Y - 100 * U - 208 * V + 128) >> 8);
			p[2] = clamp8((Y + 516 * U + 128) >> 8);
		}
	}
	f = g_new(struct frame, 1);
	f->w = ow;
	f->h = oh;
	f->rgb = g_bytes_new_take(out, ow * oh * 3);
	return f;
}

/* Black -> purple -> red -> orange -> yellow -> white, like thermal/IR viewers */
static const uint8_t *false_colour_lut(void)
{
	static uint8_t lut[256][3];
	static const uint8_t stops[][3] = {
		{ 0, 0, 0 }, { 80, 0, 120 }, { 200, 20, 60 }, { 255, 120, 0 },
		{ 255, 220, 40 }, { 255, 255, 255 },
	};
	static gsize done;

	if (g_once_init_enter(&done)) {
		for (int i = 0; i < 256; i++) {
			float t = i / 255.0f * (G_N_ELEMENTS(stops) - 1);
			int k = MIN((int)t, (int)G_N_ELEMENTS(stops) - 2);
			float f = t - k;

			for (int ch = 0; ch < 3; ch++)
				lut[i][ch] = stops[k][ch] + f * (stops[k + 1][ch] - stops[k][ch]);
		}
		g_once_init_leave(&done, 1);
	}
	return &lut[0][0];
}

/* IR sensor: luma only, contrast stretched between the 1st and 99th percentile,
 * optionally false-coloured; `step` 1 = full resolution, 2 = half */
static struct frame *convert_ir(AImage *img, int rot, int step, int mode)
{
	uint8_t *y, map[256];
	int ylen, ys, hist[256] = { 0 }, total = 0, lo = 0, hi = 255, acc;
	int32_t w, h;
	const uint8_t *lut = false_colour_lut();
	struct frame *f;
	uint8_t *out;
	int ow, oh;

	n.AImage_getWidth(img, &w);
	n.AImage_getHeight(img, &h);
	n.AImage_getPlaneData(img, 0, &y, &ylen);
	n.AImage_getPlaneRowStride(img, 0, &ys);

	for (int r = 0; r < h; r += 4)
		for (int c = 0; c < w; c += 4, total++)
			hist[y[r * ys + c]]++;
	for (acc = 0; lo < 255 && (acc += hist[lo]) < total / 100; lo++)
		;
	for (acc = 0; hi > lo + 1 && (acc += hist[hi]) < total / 100; hi--)
		;
	for (int i = 0; i < 256; i++)
		map[i] = i <= lo ? 0 : i >= hi ? 255 : (i - lo) * 255 / (hi - lo);

	w /= step;
	h /= step;
	ow = rot % 180 ? h : w;
	oh = rot % 180 ? w : h;
	out = g_malloc(ow * oh * 3);
	for (int r = 0; r < h; r++) {
		const uint8_t *yr = y + step * r * ys;

		for (int c = 0; c < w; c++) {
			uint8_t v = map[yr[step * c]], *p;
			int x, yy;

			switch (rot) {
			case 90:  x = h - 1 - r; yy = c; break;
			case 180: x = w - 1 - c; yy = h - 1 - r; break;
			case 270: x = r; yy = w - 1 - c; break;
			default:  x = c; yy = r; break;
			}
			p = out + (yy * ow + x) * 3;
			if (mode)
				memcpy(p, lut + v * 3, 3);
			else
				p[0] = p[1] = p[2] = v;
		}
	}
	f = g_new(struct frame, 1);
	f->w = ow;
	f->h = oh;
	f->rgb = g_bytes_new_take(out, ow * oh * 3);
	return f;
}

static gboolean save_ir_photo(gpointer data)
{
	struct frame *f = data;
	GdkTexture *tex = gdk_memory_texture_new(f->w, f->h, GDK_MEMORY_R8G8B8, f->rgb, f->w * 3);
	char *path = photo_path(".png");
	char *msg;

	if (gdk_texture_save_to_png(tex, path))
		msg = g_strdup_printf("Gespeichert: %s", strrchr(path, '/') + 1);
	else
		msg = g_strdup("Fehler beim Speichern");
	gtk_label_set_text(GTK_LABEL(status), msg);
	gtk_widget_set_sensitive(shutter, TRUE);
	g_free(msg);
	g_free(path);
	g_object_unref(tex);
	g_bytes_unref(f->rgb);
	g_free(f);
	return G_SOURCE_REMOVE;
}

static void on_preview(void *ctx, AImageReader *r)
{
	struct cam_info *c = ctx;
	AImage *img;

	if (n.AImageReader_acquireLatestImage(r, &img) != ACAMERA_OK)
		return;
	if (c->ir) {
		if (atomic_exchange(&ir_snapshot, false))
			g_idle_add(save_ir_photo, convert_ir(img, c->orientation, 1, ir_mode));
		if (!atomic_exchange(&frame_queued, true))
			g_idle_add(show_frame, convert_ir(img, c->orientation, 2, ir_mode));
		n.AImage_delete(img);
		return;
	}
	/* Drop frames while the UI hasn't shown the previous one */
	if (!atomic_exchange(&frame_queued, true)) {
		/* Front sensor is mounted at 270: rotating by 270 and mirroring gives the
		 * upright selfie view */
		g_idle_add(show_frame, convert(img, c->orientation, c->front));
	}
	n.AImage_delete(img);
}

/* --- photos ------------------------------------------------------------------ */

struct photo {
	GBytes *jpeg;
	char *path;
};

static gboolean photo_saved(gpointer data)
{
	struct photo *p = data;
	GError *err = NULL;
	char *msg;

	if (g_file_set_contents(p->path, g_bytes_get_data(p->jpeg, NULL),
				g_bytes_get_size(p->jpeg), &err))
		msg = g_strdup_printf("Gespeichert: %s", strrchr(p->path, '/') + 1);
	else
		msg = g_strdup_printf("Fehler: %s", err->message);
	gtk_label_set_text(GTK_LABEL(status), msg);
	gtk_widget_set_sensitive(shutter, TRUE);
	g_clear_error(&err);
	g_free(msg);
	g_bytes_unref(p->jpeg);
	g_free(p->path);
	g_free(p);
	return G_SOURCE_REMOVE;
}

static char *photo_path(const char *ext)
{
	const char *dir = g_get_user_special_dir(G_USER_DIRECTORY_PICTURES);
	char name[64];
	time_t t = time(NULL);

	strftime(name, sizeof(name), "IMG_%Y%m%d_%H%M%S", localtime(&t));
	g_strlcat(name, ext, sizeof(name));
	if (!dir)
		dir = g_get_home_dir();
	g_mkdir_with_parents(dir, 0755);
	return g_build_filename(dir, name, NULL);
}

static void on_jpeg(void *ctx, AImageReader *r)
{
	struct photo *p;
	AImage *img;
	uint8_t *data;
	int len;

	if (n.AImageReader_acquireNextImage(r, &img) != ACAMERA_OK)
		return;
	n.AImage_getPlaneData(img, 0, &data, &len);
	p = g_new(struct photo, 1);
	p->jpeg = g_bytes_new(data, len);
	p->path = photo_path(".jpg");
	n.AImage_delete(img);
	g_idle_add(photo_saved, p);
}

static void take_photo(GtkButton *b, gpointer data)
{
	if (!sess)
		return;
	gtk_widget_set_sensitive(shutter, FALSE);
	gtk_label_set_text(GTK_LABEL(status), "Foto …");
	if (cams[cur].ir) {
		/* the next full-resolution preview frame, processed like the preview */
		ir_snapshot = true;
		return;
	}
	if (n.ACameraCaptureSession_capture(sess, NULL, 1, &still_req, NULL)) {
		gtk_label_set_text(GTK_LABEL(status), "Aufnahme fehlgeschlagen");
		gtk_widget_set_sensitive(shutter, TRUE);
	}
}

/* --- camera session -------------------------------------------------------------- */

static void dev_disconnected(void *c, ACameraDevice *d) { g_warning("camera disconnected"); }
static void dev_error(void *c, ACameraDevice *d, int e) { g_warning("camera error %d", e); }
static void sess_nop(void *c, ACameraCaptureSession *s) { }

static ACameraDevice_StateCallbacks dev_cb = { NULL, dev_disconnected, dev_error };
static ACameraCaptureSession_stateCallbacks sess_cb = { NULL, sess_nop, sess_nop, sess_nop };
static AImageReader_ImageListener prev_l = { NULL, on_preview }, jpeg_l = { NULL, on_jpeg };

static void close_camera(void)
{
	if (sess) {
		n.ACameraCaptureSession_stopRepeating(sess);
		n.ACameraCaptureSession_close(sess);
		sess = NULL;
	}
	if (dev) {
		n.ACameraDevice_close(dev);
		dev = NULL;
	}
	g_clear_pointer(&prev_req, n.ACaptureRequest_free);
	g_clear_pointer(&still_req, n.ACaptureRequest_free);
	g_clear_pointer(&prev_t, n.ACameraOutputTarget_free);
	g_clear_pointer(&jpeg_t, n.ACameraOutputTarget_free);
	g_clear_pointer(&outs, n.ACaptureSessionOutputContainer_free);
	g_clear_pointer(&prev_out, n.ACaptureSessionOutput_free);
	g_clear_pointer(&jpeg_out, n.ACaptureSessionOutput_free);
	g_clear_pointer(&prev_r, n.AImageReader_delete);
	g_clear_pointer(&jpeg_r, n.AImageReader_delete);
}

static bool open_camera(struct cam_info *c)
{
	ANativeWindow *pw, *jw;
	uint8_t af = ACAMERA_CONTROL_AF_MODE_CONTINUOUS_PICTURE, q = 95;
	int32_t orient = c->orientation;

	if (n.ACameraManager_openCamera(mgr, c->id, &dev_cb, &dev))
		return false;
	n.AImageReader_new(c->pw, c->ph, AIMAGE_FORMAT_YUV_420_888, 4, &prev_r);
	n.AImageReader_new(c->jw, c->jh, AIMAGE_FORMAT_JPEG, 2, &jpeg_r);
	prev_l.context = c;
	n.AImageReader_setImageListener(prev_r, &prev_l);
	n.AImageReader_setImageListener(jpeg_r, &jpeg_l);
	n.AImageReader_getWindow(prev_r, &pw);
	n.AImageReader_getWindow(jpeg_r, &jw);

	n.ACaptureSessionOutputContainer_create(&outs);
	n.ACaptureSessionOutput_create(pw, &prev_out);
	n.ACaptureSessionOutput_create(jw, &jpeg_out);
	n.ACaptureSessionOutputContainer_add(outs, prev_out);
	n.ACaptureSessionOutputContainer_add(outs, jpeg_out);
	if (n.ACameraDevice_createCaptureSession(dev, outs, &sess_cb, &sess))
		return false;
	n.ACameraOutputTarget_create(pw, &prev_t);
	n.ACameraOutputTarget_create(jw, &jpeg_t);

	n.ACameraDevice_createCaptureRequest(dev, TEMPLATE_PREVIEW, &prev_req);
	n.ACaptureRequest_setEntry_u8(prev_req, ACAMERA_CONTROL_AF_MODE, 1, &af);
	n.ACaptureRequest_addTarget(prev_req, prev_t);

	/* The phone is held upright: the picture needs the sensor's own rotation */
	n.ACameraDevice_createCaptureRequest(dev, TEMPLATE_STILL_CAPTURE, &still_req);
	n.ACaptureRequest_setEntry_u8(still_req, ACAMERA_CONTROL_AF_MODE, 1, &af);
	n.ACaptureRequest_setEntry_u8(still_req, ACAMERA_JPEG_QUALITY, 1, &q);
	n.ACaptureRequest_setEntry_i32(still_req, ACAMERA_JPEG_ORIENTATION, 1, &orient);
	n.ACaptureRequest_addTarget(still_req, jpeg_t);

	return n.ACameraCaptureSession_setRepeatingRequest(sess, NULL, 1, &prev_req, NULL) == 0;
}

static void select_camera(int i)
{
	char *msg;

	close_camera();
	cur = i;
	if (open_camera(&cams[i])) {
		msg = g_strdup(cams[i].label);
	} else {
		close_camera();
		msg = g_strdup_printf("%s: lässt sich nicht öffnen", cams[i].label);
	}
	gtk_label_set_text(GTK_LABEL(status), msg);
	gtk_widget_set_sensitive(shutter, sess != NULL);
	gtk_widget_set_visible(mode_button, cams[i].ir);
	g_free(msg);
}

static void toggle_ir_mode(GtkButton *b, gpointer data)
{
	ir_mode = !ir_mode;
	gtk_button_set_label(b, ir_mode ? "Falschfarben" : "Kontrast");
}

static void switch_camera(GtkButton *b, gpointer data)
{
	select_camera((cur + 1) % ncams);
}

/* Largest output size of a format, optionally limited in width and to an aspect ratio */
static void largest(const ACameraMetadata *m, int fmt, int max_w, int aw, int ah, int *w, int *h)
{
	ACameraMetadata_const_entry e;

	*w = *h = 0;
	if (n.ACameraMetadata_getConstEntry(m, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, &e))
		return;
	for (uint32_t i = 0; i + 3 < e.count; i += 4) {
		const int32_t *c = e.data.i32 + i;

		if (c[0] != fmt || c[3] || c[1] > max_w || c[1] * c[2] <= *w * *h)
			continue;
		if (aw && c[1] * ah != c[2] * aw)
			continue;
		*w = c[1];
		*h = c[2];
	}
}

/* The physical cameras: main and secondary back, front. Id 3 is the logical main+depth
 * camera, which only adds the bokeh pipeline. */
static void find_cameras(void)
{
	static const struct { const char *id, *label; } want[] = {
		{ "0", "Hauptkamera" }, { "2", "IR-Tiefensensor" }, { "1", "Frontkamera" },
	};

	for (unsigned i = 0; i < G_N_ELEMENTS(want); i++) {
		struct cam_info *c = &cams[ncams];
		ACameraMetadata *m;
		ACameraMetadata_const_entry e;

		if (n.ACameraManager_getCameraCharacteristics(mgr, want[i].id, &m))
			continue;
		g_strlcpy(c->id, want[i].id, sizeof(c->id));
		c->label = want[i].label;
		c->ir = !strcmp(c->id, "2");
		if (!n.ACameraMetadata_getConstEntry(m, ACAMERA_SENSOR_ORIENTATION, &e))
			c->orientation = e.data.i32[0];
		if (!n.ACameraMetadata_getConstEntry(m, ACAMERA_LENS_FACING, &e))
			c->front = e.data.u8[0] == 0;
		largest(m, AIMAGE_FORMAT_JPEG, 100000, 0, 0, &c->jw, &c->jh);
		/* Preview in the still's aspect ratio, at most 1440 wide (shown at half); the IR
		 * sensor streams at full size, its photos come from that stream */
		largest(m, AIMAGE_FORMAT_YUV_420_888, c->ir ? 100000 : 1440, c->jw, c->jh, &c->pw, &c->ph);
		if (!c->pw)
			largest(m, AIMAGE_FORMAT_YUV_420_888, 1440, 0, 0, &c->pw, &c->ph);
		n.ACameraMetadata_free(m);
		if (c->jw && c->pw)
			ncams++;
	}
}

/* --- UI ---------------------------------------------------------------------------- */

static void on_close(GtkWindow *w, gpointer data)
{
	close_camera();
}

static void activate(GtkApplication *app, gpointer data)
{
	GtkWidget *win, *box, *bar;

	win = gtk_application_window_new(app);
	gtk_window_set_title(GTK_WINDOW(win), "Kamera");
	gtk_window_set_default_size(GTK_WINDOW(win), 360, 720);
	g_signal_connect(win, "close-request", G_CALLBACK(on_close), NULL);

	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	picture = gtk_picture_new();
	gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_CONTAIN);
	gtk_widget_set_vexpand(picture, TRUE);
	gtk_box_append(GTK_BOX(box), picture);

	status = gtk_label_new("Kamera wird gestartet …");
	gtk_widget_set_margin_top(status, 6);
	gtk_box_append(GTK_BOX(box), status);

	bar = gtk_center_box_new();
	gtk_widget_set_margin_top(bar, 12);
	gtk_widget_set_margin_bottom(bar, 18);
	gtk_widget_set_margin_start(bar, 18);
	gtk_widget_set_margin_end(bar, 18);
	shutter = gtk_button_new_from_icon_name("camera-photo-symbolic");
	gtk_widget_set_size_request(shutter, 88, 88);
	gtk_widget_add_css_class(shutter, "circular");
	gtk_widget_add_css_class(shutter, "suggested-action");
	g_signal_connect(shutter, "clicked", G_CALLBACK(take_photo), NULL);
	switcher = gtk_button_new_from_icon_name("camera-switch-symbolic");
	gtk_widget_set_size_request(switcher, 56, 56);
	gtk_widget_add_css_class(switcher, "circular");
	g_signal_connect(switcher, "clicked", G_CALLBACK(switch_camera), NULL);
	mode_button = gtk_button_new_with_label("Kontrast");
	gtk_widget_set_valign(mode_button, GTK_ALIGN_CENTER);
	gtk_widget_set_visible(mode_button, FALSE);
	g_signal_connect(mode_button, "clicked", G_CALLBACK(toggle_ir_mode), NULL);
	gtk_center_box_set_start_widget(GTK_CENTER_BOX(bar), mode_button);
	gtk_center_box_set_center_widget(GTK_CENTER_BOX(bar), shutter);
	gtk_center_box_set_end_widget(GTK_CENTER_BOX(bar), switcher);
	gtk_box_append(GTK_BOX(box), bar);

	gtk_window_set_child(GTK_WINDOW(win), box);
	gtk_window_present(GTK_WINDOW(win));

	if (!ncams) {
		gtk_label_set_text(GTK_LABEL(status), "Keine Kamera gefunden (läuft tetris-camera?)");
		gtk_widget_set_sensitive(shutter, FALSE);
		gtk_widget_set_sensitive(switcher, FALSE);
		return;
	}
	gtk_widget_set_sensitive(switcher, ncams > 1);
	select_camera(0);
}

int main(int argc, char **argv)
{
	GtkApplication *app;
	int ret;

	if (!ndkcam_load(&n))
		return 1;
	mgr = n.ACameraManager_create();
	find_cameras();

	app = gtk_application_new(APP_ID, G_APPLICATION_DEFAULT_FLAGS);
	g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
	ret = g_application_run(G_APPLICATION(app), argc, argv);
	close_camera();
	g_object_unref(app);
	return ret;
}
