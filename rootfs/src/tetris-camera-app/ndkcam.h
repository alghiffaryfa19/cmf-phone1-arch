// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Android NDK Camera2 + AImageReader, loaded from the Halium system image through
 * libhybris (hybris_dlopen). Only the parts of the stable NDK C API we use, declared
 * here because the NDK headers aren't on the phone.
 */
#ifndef TETRIS_NDKCAM_H
#define TETRIS_NDKCAM_H

#include <stdbool.h>
#include <stdint.h>

typedef struct ACameraManager ACameraManager;
typedef struct ACameraDevice ACameraDevice;
typedef struct ACameraMetadata ACameraMetadata;
typedef struct ACaptureRequest ACaptureRequest;
typedef struct ACameraCaptureSession ACameraCaptureSession;
typedef struct ACaptureSessionOutput ACaptureSessionOutput;
typedef struct ACaptureSessionOutputContainer ACaptureSessionOutputContainer;
typedef struct ACameraOutputTarget ACameraOutputTarget;
typedef struct AImageReader AImageReader;
typedef struct AImage AImage;
typedef struct ANativeWindow ANativeWindow;

typedef struct {
	int numCameras;
	const char **cameraIds;
} ACameraIdList;

typedef struct {
	uint32_t tag;
	uint8_t type;
	uint32_t count;
	union {
		const uint8_t *u8;
		const int32_t *i32;
		const float *f;
		const int64_t *i64;
		const double *d;
		const void *r;
	} data;
} ACameraMetadata_const_entry;

typedef struct {
	void *context;
	void (*onDisconnected)(void *context, ACameraDevice *device);
	void (*onError)(void *context, ACameraDevice *device, int error);
} ACameraDevice_StateCallbacks;

typedef struct {
	void *context;
	void (*onClosed)(void *context, ACameraCaptureSession *session);
	void (*onReady)(void *context, ACameraCaptureSession *session);
	void (*onActive)(void *context, ACameraCaptureSession *session);
} ACameraCaptureSession_stateCallbacks;

typedef struct {
	void *context;
	void (*onImageAvailable)(void *context, AImageReader *reader);
} AImageReader_ImageListener;

#define ACAMERA_OK                        0
#define TEMPLATE_PREVIEW                  1
#define TEMPLATE_STILL_CAPTURE            2
#define AIMAGE_FORMAT_YUV_420_888         0x23
#define AIMAGE_FORMAT_JPEG                0x100
#define ACAMERA_LENS_FACING               0x80005   /* u8: 0 front, 1 back, 2 external */
#define ACAMERA_SENSOR_ORIENTATION        0xE000E   /* i32 degrees */
#define ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS 0xD000A /* i32 (fmt, w, h, input) */
#define ACAMERA_REQUEST_AVAILABLE_CAPABILITIES 0xC000C /* u8 */
#define ACAMERA_CAPABILITY_LOGICAL_MULTI_CAMERA 11
#define ACAMERA_JPEG_ORIENTATION          0x70003   /* i32 */
#define ACAMERA_JPEG_QUALITY              0x70004   /* u8 */
#define ACAMERA_CONTROL_AF_MODE           0x10007   /* u8 */
#define ACAMERA_CONTROL_AF_MODE_CONTINUOUS_PICTURE 4

struct ndkcam {
	ACameraManager *(*ACameraManager_create)(void);
	int (*ACameraManager_getCameraIdList)(ACameraManager *, ACameraIdList **);
	void (*ACameraManager_deleteCameraIdList)(ACameraIdList *);
	int (*ACameraManager_getCameraCharacteristics)(ACameraManager *, const char *, ACameraMetadata **);
	int (*ACameraManager_openCamera)(ACameraManager *, const char *, ACameraDevice_StateCallbacks *, ACameraDevice **);
	int (*ACameraMetadata_getConstEntry)(const ACameraMetadata *, uint32_t, ACameraMetadata_const_entry *);
	void (*ACameraMetadata_free)(ACameraMetadata *);
	int (*ACameraDevice_close)(ACameraDevice *);
	int (*ACameraDevice_createCaptureRequest)(const ACameraDevice *, int, ACaptureRequest **);
	int (*ACameraDevice_createCaptureSession)(ACameraDevice *, const ACaptureSessionOutputContainer *, const ACameraCaptureSession_stateCallbacks *, ACameraCaptureSession **);
	int (*ACaptureSessionOutputContainer_create)(ACaptureSessionOutputContainer **);
	void (*ACaptureSessionOutputContainer_free)(ACaptureSessionOutputContainer *);
	int (*ACaptureSessionOutputContainer_add)(ACaptureSessionOutputContainer *, const ACaptureSessionOutput *);
	int (*ACaptureSessionOutput_create)(ANativeWindow *, ACaptureSessionOutput **);
	void (*ACaptureSessionOutput_free)(ACaptureSessionOutput *);
	int (*ACameraOutputTarget_create)(ANativeWindow *, ACameraOutputTarget **);
	void (*ACameraOutputTarget_free)(ACameraOutputTarget *);
	int (*ACaptureRequest_addTarget)(ACaptureRequest *, const ACameraOutputTarget *);
	int (*ACaptureRequest_setEntry_u8)(ACaptureRequest *, uint32_t, uint32_t, const uint8_t *);
	int (*ACaptureRequest_setEntry_i32)(ACaptureRequest *, uint32_t, uint32_t, const int32_t *);
	void (*ACaptureRequest_free)(ACaptureRequest *);
	int (*ACameraCaptureSession_setRepeatingRequest)(ACameraCaptureSession *, void *, int, ACaptureRequest **, int *);
	int (*ACameraCaptureSession_stopRepeating)(ACameraCaptureSession *);
	int (*ACameraCaptureSession_capture)(ACameraCaptureSession *, void *, int, ACaptureRequest **, int *);
	void (*ACameraCaptureSession_close)(ACameraCaptureSession *);

	int (*AImageReader_new)(int32_t, int32_t, int32_t, int32_t, AImageReader **);
	void (*AImageReader_delete)(AImageReader *);
	int (*AImageReader_getWindow)(AImageReader *, ANativeWindow **);
	int (*AImageReader_setImageListener)(AImageReader *, AImageReader_ImageListener *);
	int (*AImageReader_acquireLatestImage)(AImageReader *, AImage **);
	int (*AImageReader_acquireNextImage)(AImageReader *, AImage **);
	void (*AImage_delete)(AImage *);
	int (*AImage_getWidth)(const AImage *, int32_t *);
	int (*AImage_getHeight)(const AImage *, int32_t *);
	int (*AImage_getPlaneData)(const AImage *, int, uint8_t **, int *);
	int (*AImage_getPlaneRowStride)(const AImage *, int, int32_t *);
	int (*AImage_getPlanePixelStride)(const AImage *, int, int32_t *);
};

/* Loads both libraries; prints the missing symbol and returns false on failure. */
bool ndkcam_load(struct ndkcam *n);

#endif
