// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Camera2 NDK probe: lists all cameras, and with an id takes one JPEG
 * (preview stream for ~1.5 s so AE settles, then a still capture).
 *   tetris-cam2probe [id [out.jpg]]
 */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "ndkcam.h"

static struct ndkcam n;
static atomic_int preview_frames, jpeg_done;
static const char *out_path = "/tmp/cam2.jpg";

static void dev_disconnected(void *c, ACameraDevice *d) { fprintf(stderr, "disconnected\n"); }
static void dev_error(void *c, ACameraDevice *d, int e) { fprintf(stderr, "device error %d\n", e); }
static void sess_nop(void *c, ACameraCaptureSession *s) { }

static void on_preview(void *ctx, AImageReader *r)
{
	AImage *img;

	if (n.AImageReader_acquireLatestImage(r, &img) == ACAMERA_OK) {
		preview_frames++;
		n.AImage_delete(img);
	}
}

static void on_jpeg(void *ctx, AImageReader *r)
{
	AImage *img;
	uint8_t *data;
	int len;
	FILE *f;

	if (n.AImageReader_acquireNextImage(r, &img) != ACAMERA_OK)
		return;
	n.AImage_getPlaneData(img, 0, &data, &len);
	f = fopen(out_path, "wb");
	if (f) {
		fwrite(data, 1, len, f);
		fclose(f);
		printf("wrote %s (%d bytes)\n", out_path, len);
	}
	n.AImage_delete(img);
	jpeg_done = 1;
}

/* Largest size of a format in the stream configurations (output streams only) */
static void largest(const ACameraMetadata *m, int fmt, int max_w, int *w, int *h)
{
	ACameraMetadata_const_entry e;

	*w = *h = 0;
	if (n.ACameraMetadata_getConstEntry(m, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, &e))
		return;
	for (uint32_t i = 0; i + 3 < e.count; i += 4) {
		const int32_t *c = e.data.i32 + i;

		if (c[0] == fmt && !c[3] && c[1] <= max_w && c[1] * c[2] > *w * *h) {
			*w = c[1];
			*h = c[2];
		}
	}
}

static void list(ACameraManager *mgr)
{
	ACameraIdList *ids;

	if (n.ACameraManager_getCameraIdList(mgr, &ids)) {
		fprintf(stderr, "getCameraIdList failed\n");
		return;
	}
	for (int i = 0; i < ids->numCameras; i++) {
		ACameraMetadata *m;
		ACameraMetadata_const_entry e;
		int facing = -1, orient = -1, logical = 0, jw, jh;

		if (n.ACameraManager_getCameraCharacteristics(mgr, ids->cameraIds[i], &m))
			continue;
		if (!n.ACameraMetadata_getConstEntry(m, ACAMERA_LENS_FACING, &e))
			facing = e.data.u8[0];
		if (!n.ACameraMetadata_getConstEntry(m, ACAMERA_SENSOR_ORIENTATION, &e))
			orient = e.data.i32[0];
		if (!n.ACameraMetadata_getConstEntry(m, ACAMERA_REQUEST_AVAILABLE_CAPABILITIES, &e))
			for (uint32_t k = 0; k < e.count; k++)
				logical |= e.data.u8[k] == ACAMERA_CAPABILITY_LOGICAL_MULTI_CAMERA;
		largest(m, AIMAGE_FORMAT_JPEG, 100000, &jw, &jh);
		printf("camera %s: facing %s, orientation %d, jpeg %dx%d%s\n", ids->cameraIds[i],
		       facing == 0 ? "front" : facing == 1 ? "back" : "?", orient, jw, jh,
		       logical ? ", logical" : "");
		n.ACameraMetadata_free(m);
	}
	n.ACameraManager_deleteCameraIdList(ids);
}

int main(int argc, char **argv)
{
	ACameraDevice_StateCallbacks dev_cb = { NULL, dev_disconnected, dev_error };
	ACameraCaptureSession_stateCallbacks sess_cb = { NULL, sess_nop, sess_nop, sess_nop };
	AImageReader_ImageListener prev_l = { NULL, on_preview }, jpeg_l = { NULL, on_jpeg };
	ACaptureSessionOutputContainer *outs;
	ACaptureSessionOutput *prev_out, *jpeg_out;
	ACameraOutputTarget *prev_t, *jpeg_t;
	ACaptureRequest *prev_req, *still_req;
	ACameraCaptureSession *sess;
	ACameraManager *mgr;
	ACameraMetadata *m;
	ACameraDevice *dev;
	AImageReader *prev_r, *jpeg_r;
	ANativeWindow *prev_w, *jpeg_w;
	int pw, ph, jw, jh, rc;
	uint8_t af = ACAMERA_CONTROL_AF_MODE_CONTINUOUS_PICTURE, q = 95;

	setvbuf(stdout, NULL, _IOLBF, 0);
	if (!ndkcam_load(&n))
		return 1;
	mgr = n.ACameraManager_create();
	list(mgr);
	if (argc < 2)
		return 0;
	if (argc > 2)
		out_path = argv[2];

	if (n.ACameraManager_getCameraCharacteristics(mgr, argv[1], &m)) {
		fprintf(stderr, "no camera %s\n", argv[1]);
		return 1;
	}
	largest(m, AIMAGE_FORMAT_YUV_420_888, 1920, &pw, &ph);
	largest(m, AIMAGE_FORMAT_JPEG, 100000, &jw, &jh);
	n.ACameraMetadata_free(m);
	printf("preview %dx%d, still %dx%d\n", pw, ph, jw, jh);

	if ((rc = n.ACameraManager_openCamera(mgr, argv[1], &dev_cb, &dev))) {
		fprintf(stderr, "openCamera %s: %d\n", argv[1], rc);
		return 1;
	}
	n.AImageReader_new(pw, ph, AIMAGE_FORMAT_YUV_420_888, 4, &prev_r);
	n.AImageReader_new(jw, jh, AIMAGE_FORMAT_JPEG, 2, &jpeg_r);
	n.AImageReader_setImageListener(prev_r, &prev_l);
	n.AImageReader_setImageListener(jpeg_r, &jpeg_l);
	n.AImageReader_getWindow(prev_r, &prev_w);
	n.AImageReader_getWindow(jpeg_r, &jpeg_w);

	n.ACaptureSessionOutputContainer_create(&outs);
	n.ACaptureSessionOutput_create(prev_w, &prev_out);
	n.ACaptureSessionOutput_create(jpeg_w, &jpeg_out);
	n.ACaptureSessionOutputContainer_add(outs, prev_out);
	n.ACaptureSessionOutputContainer_add(outs, jpeg_out);
	if ((rc = n.ACameraDevice_createCaptureSession(dev, outs, &sess_cb, &sess))) {
		fprintf(stderr, "createCaptureSession: %d\n", rc);
		return 1;
	}
	n.ACameraOutputTarget_create(prev_w, &prev_t);
	n.ACameraOutputTarget_create(jpeg_w, &jpeg_t);

	n.ACameraDevice_createCaptureRequest(dev, TEMPLATE_PREVIEW, &prev_req);
	n.ACaptureRequest_setEntry_u8(prev_req, ACAMERA_CONTROL_AF_MODE, 1, &af);
	n.ACaptureRequest_addTarget(prev_req, prev_t);
	if ((rc = n.ACameraCaptureSession_setRepeatingRequest(sess, NULL, 1, &prev_req, NULL))) {
		fprintf(stderr, "setRepeatingRequest: %d\n", rc);
		return 1;
	}
	usleep(1500000);
	printf("%d preview frames in 1.5 s\n", preview_frames);

	n.ACameraDevice_createCaptureRequest(dev, TEMPLATE_STILL_CAPTURE, &still_req);
	n.ACaptureRequest_setEntry_u8(still_req, ACAMERA_CONTROL_AF_MODE, 1, &af);
	n.ACaptureRequest_setEntry_u8(still_req, ACAMERA_JPEG_QUALITY, 1, &q);
	n.ACaptureRequest_addTarget(still_req, jpeg_t);
	if ((rc = n.ACameraCaptureSession_capture(sess, NULL, 1, &still_req, NULL)))
		fprintf(stderr, "capture: %d\n", rc);
	for (int i = 0; i < 100 && !jpeg_done; i++)
		usleep(100000);
	if (!jpeg_done)
		fprintf(stderr, "no JPEG after 10 s\n");

	n.ACameraCaptureSession_stopRepeating(sess);
	n.ACameraCaptureSession_close(sess);
	n.ACameraDevice_close(dev);
	return jpeg_done ? 0 : 1;
}
