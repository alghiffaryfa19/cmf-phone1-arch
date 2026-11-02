// SPDX-License-Identifier: GPL-2.0-or-later
#include <stdio.h>
#include <dlfcn.h>
#include <hybris/common/dlfcn.h>
#include "ndkcam.h"

#define SYM(lib, name) do { \
		*(void **)&n->name = hybris_dlsym(lib, #name); \
		if (!n->name) { \
			fprintf(stderr, "ndkcam: missing %s\n", #name); \
			return false; \
		} \
	} while (0)

bool ndkcam_load(struct ndkcam *n)
{
	void *cam = hybris_dlopen("libcamera2ndk.so", RTLD_NOW);
	void *media = hybris_dlopen("libmediandk.so", RTLD_NOW);

	void *binder = hybris_dlopen("libbinder_ndk.so", RTLD_NOW);
	void (*start_thread_pool)(void);

	if (!cam || !media || !binder) {
		fprintf(stderr, "ndkcam: %s\n", hybris_dlerror());
		return false;
	}
	/* Device state, capture results and buffer queue callbacks arrive as binder calls */
	*(void **)&start_thread_pool = hybris_dlsym(binder, "ABinderProcess_startThreadPool");
	if (!start_thread_pool) {
		fprintf(stderr, "ndkcam: missing ABinderProcess_startThreadPool\n");
		return false;
	}
	start_thread_pool();
	SYM(cam, ACameraManager_create);
	SYM(cam, ACameraManager_getCameraIdList);
	SYM(cam, ACameraManager_deleteCameraIdList);
	SYM(cam, ACameraManager_getCameraCharacteristics);
	SYM(cam, ACameraManager_openCamera);
	SYM(cam, ACameraMetadata_getConstEntry);
	SYM(cam, ACameraMetadata_free);
	SYM(cam, ACameraDevice_close);
	SYM(cam, ACameraDevice_createCaptureRequest);
	SYM(cam, ACameraDevice_createCaptureSession);
	SYM(cam, ACaptureSessionOutputContainer_create);
	SYM(cam, ACaptureSessionOutputContainer_free);
	SYM(cam, ACaptureSessionOutputContainer_add);
	SYM(cam, ACaptureSessionOutput_create);
	SYM(cam, ACaptureSessionOutput_free);
	SYM(cam, ACameraOutputTarget_create);
	SYM(cam, ACameraOutputTarget_free);
	SYM(cam, ACaptureRequest_addTarget);
	SYM(cam, ACaptureRequest_setEntry_u8);
	SYM(cam, ACaptureRequest_setEntry_i32);
	SYM(cam, ACaptureRequest_free);
	SYM(cam, ACameraCaptureSession_setRepeatingRequest);
	SYM(cam, ACameraCaptureSession_stopRepeating);
	SYM(cam, ACameraCaptureSession_capture);
	SYM(cam, ACameraCaptureSession_close);
	SYM(media, AImageReader_new);
	SYM(media, AImageReader_delete);
	SYM(media, AImageReader_getWindow);
	SYM(media, AImageReader_setImageListener);
	SYM(media, AImageReader_acquireLatestImage);
	SYM(media, AImageReader_acquireNextImage);
	SYM(media, AImage_delete);
	SYM(media, AImage_getWidth);
	SYM(media, AImage_getHeight);
	SYM(media, AImage_getPlaneData);
	SYM(media, AImage_getPlaneRowStride);
	SYM(media, AImage_getPlanePixelStride);
	return true;
}
