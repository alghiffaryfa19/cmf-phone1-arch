// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Fingerprint for the CMF Phone 1: a client of the Android fingerprint HAL
 * (android.hardware.biometrics.fingerprint AIDL, Nothing/Goodix HAL with its trusted app
 * in the Trustonic TEE, all in the Halium container), over binder via libbinder_ndk and
 * libhybris.
 *
 *   tetris-fp enroll       enroll a finger (touch the sensor until it reports done)
 *   tetris-fp verify       wait for a finger; exit 0 if it matches an enrolled one
 *   tetris-fp list         list enrolled fingers
 *   tetris-fp clear        remove all enrolled fingers
 *   tetris-fp props        sensor type and position
 *
 * STATUS (2026-11-03): work in progress. Talks to the HAL (gatekeeper token accepted,
 * enrollment starts), but the sensor is optical under the display: it needs the touch
 * (onPointerDown/onUiReady) and a bright spot drawn at its position (540,2109 r93), which
 * on Android SystemUI does. Started by hand: mcDriverDaemon, tee@1.1, gatekeeper and the
 * fingerprint HAL in the container, /dev/goodix_fp chowned to system there.
 *
 * Enrolling needs a HardwareAuthToken from the gatekeeper (also in the TEE), as Android
 * gets it when you confirm your PIN. We enroll an internal random secret with the
 * gatekeeper once (kept root-only in /var/lib/tetris-fp) and verify it to mint tokens.
 *
 * The AIDL interfaces are stable and public, so the parcels are written by hand:
 * transaction code = 1 + index of the method.
 */
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <hybris/common/dlfcn.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define STATE_DIR "/var/lib/tetris-fp"
#define FP_SERVICE "android.hardware.biometrics.fingerprint.IFingerprint/default"
#define GK_SERVICE "android.hardware.gatekeeper.IGatekeeper/default"

/* IFingerprint */
enum { FP_GET_SENSOR_PROPS = 1, FP_CREATE_SESSION = 2 };
/* ISession */
enum { S_GENERATE_CHALLENGE = 1, S_REVOKE_CHALLENGE = 2, S_ENROLL = 3, S_AUTHENTICATE = 4,
       S_ENUMERATE = 6, S_REMOVE = 7, S_CLOSE = 11 };
/* ISessionCallback */
enum { CB_CHALLENGE = 1, CB_ACQUIRED = 3, CB_ERROR = 4, CB_ENROLL_PROGRESS = 5,
       CB_AUTH_OK = 6, CB_AUTH_FAILED = 7, CB_LOCKOUT_TIMED = 8, CB_LOCKOUT_PERMANENT = 9,
       CB_ENUMERATED = 12, CB_REMOVED = 13, CB_SESSION_CLOSED = 16 };
/* IGatekeeper */
enum { GK_ENROLL = 3, GK_VERIFY = 4 };
#define GET_INTERFACE_VERSION 0x00fffffe
#define GET_INTERFACE_HASH    0x00fffffd

typedef struct AIBinder AIBinder;
typedef struct AIBinder_Class AIBinder_Class;
typedef struct AParcel AParcel;
typedef struct AStatus AStatus;
typedef int32_t binder_status_t;
typedef bool (*array_alloc)(void *data, int32_t length, int8_t **buffer);
typedef bool (*i32_array_alloc)(void *data, int32_t length, int32_t **buffer);

static struct {
	void (*startThreadPool)(void);
	AIBinder *(*waitForService)(const char *);
	AIBinder_Class *(*classDefine)(const char *, void *(*)(void *), void (*)(void *),
				       binder_status_t (*)(AIBinder *, uint32_t, const AParcel *, AParcel *));
	AIBinder *(*binderNew)(const AIBinder_Class *, void *);
	bool (*associateClass)(AIBinder *, const AIBinder_Class *);
	void (*markVintfStability)(AIBinder *);
	binder_status_t (*prepareTransaction)(AIBinder *, AParcel **);
	binder_status_t (*transact)(AIBinder *, uint32_t, AParcel **, AParcel **, uint32_t);
	void (*parcelDelete)(AParcel *);
	binder_status_t (*writeInt32)(AParcel *, int32_t);
	binder_status_t (*writeInt64)(AParcel *, int64_t);
	binder_status_t (*writeStrongBinder)(AParcel *, AIBinder *);
	binder_status_t (*writeString)(AParcel *, const char *, int32_t);
	binder_status_t (*writeByteArray)(AParcel *, const int8_t *, int32_t);
	binder_status_t (*writeInt32Array)(AParcel *, const int32_t *, int32_t);
	binder_status_t (*readInt32)(const AParcel *, int32_t *);
	binder_status_t (*readInt64)(const AParcel *, int64_t *);
	binder_status_t (*readStrongBinder)(const AParcel *, AIBinder **);
	binder_status_t (*readByteArray)(const AParcel *, void *, array_alloc);
	binder_status_t (*readInt32Array)(const AParcel *, void *, i32_array_alloc);
	int32_t (*getDataPosition)(const AParcel *);
	binder_status_t (*setDataPosition)(const AParcel *, int32_t);
	binder_status_t (*readStatusHeader)(const AParcel *, AStatus **);
	binder_status_t (*writeStatusHeader)(AParcel *, const AStatus *);
	AStatus *(*statusOk)(void);
	bool (*statusIsOk)(const AStatus *);
	const char *(*statusDescription)(const AStatus *);
	void (*statusDelete)(AStatus *);
} b;

struct bytes {
	int8_t *data;
	int32_t len;
};

/* HardwareAuthToken (android.hardware.security.keymint) */
struct hat {
	int64_t challenge, user_id, authenticator_id;
	int32_t authenticator_type;
	int64_t timestamp_ms;
	struct bytes mac;
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static struct {
	bool challenge_done, done;
	int64_t challenge;
	int result;             /* 0 ok, 1 no match, <0 error */
	int remaining;
} st;

static void signal_done(int result)
{
	pthread_mutex_lock(&lock);
	st.done = true;
	st.result = result;
	pthread_cond_broadcast(&cond);
	pthread_mutex_unlock(&lock);
}

/* --- parcel helpers --------------------------------------------------------- */

static bool alloc_bytes(void *data, int32_t length, int8_t **buffer)
{
	struct bytes *out = data;

	out->len = length;
	if (length <= 0) {
		out->data = NULL;
		*buffer = NULL;
		return true;
	}
	out->data = malloc(length);
	*buffer = out->data;
	return out->data != NULL;
}

struct i32s {
	int32_t *data;
	int32_t len;
};

static bool alloc_i32s(void *data, int32_t length, int32_t **buffer)
{
	struct i32s *out = data;

	out->len = length;
	if (length <= 0) {
		out->data = NULL;
		*buffer = NULL;
		return true;
	}
	out->data = calloc(length, sizeof(int32_t));
	*buffer = out->data;
	return out->data != NULL;
}

/* Parcelables go as int32 1 (non-null), int32 size (incl. itself), fields */
static int32_t parcelable_begin(AParcel *p)
{
	int32_t start;

	b.writeInt32(p, 1);
	start = b.getDataPosition(p);
	b.writeInt32(p, 0);
	return start;
}

static void parcelable_end(AParcel *p, int32_t start)
{
	int32_t end = b.getDataPosition(p);

	b.setDataPosition(p, start);
	b.writeInt32(p, end - start);
	b.setDataPosition(p, end);
}

/* returns the end position of the parcelable, or -1 if null/bad */
static int32_t parcelable_read_begin(const AParcel *p)
{
	int32_t nonnull = 0, start, size = 0;

	if (b.readInt32(p, &nonnull) || !nonnull)
		return -1;
	start = b.getDataPosition(p);
	if (b.readInt32(p, &size) || size < 4)
		return -1;
	return start + size;
}

static void write_hat(AParcel *p, const struct hat *h)
{
	int32_t s = parcelable_begin(p), t;

	b.writeInt64(p, h->challenge);
	b.writeInt64(p, h->user_id);
	b.writeInt64(p, h->authenticator_id);
	b.writeInt32(p, h->authenticator_type);
	t = parcelable_begin(p);
	b.writeInt64(p, h->timestamp_ms);
	parcelable_end(p, t);
	b.writeByteArray(p, h->mac.data, h->mac.len);
	parcelable_end(p, s);
}

static int read_hat(const AParcel *p, struct hat *h)
{
	int32_t end = parcelable_read_begin(p), tend;

	if (end < 0)
		return -1;
	b.readInt64(p, &h->challenge);
	b.readInt64(p, &h->user_id);
	b.readInt64(p, &h->authenticator_id);
	b.readInt32(p, &h->authenticator_type);
	tend = parcelable_read_begin(p);
	if (tend < 0)
		return -1;
	b.readInt64(p, &h->timestamp_ms);
	b.setDataPosition(p, tend);
	b.readByteArray(p, &h->mac, alloc_bytes);
	b.setDataPosition(p, end);
	return 0;
}

/* --- binder calls -------------------------------------------------------------- */

typedef void (*writer)(AParcel *in, void *arg);
typedef int (*reader)(const AParcel *out, void *arg);

static int call(AIBinder *target, uint32_t code, const char *what, writer w, void *warg,
		reader r, void *rarg)
{
	AParcel *in = NULL, *out = NULL;
	AStatus *status = NULL;
	binder_status_t ret;
	int res = -1;

	ret = b.prepareTransaction(target, &in);
	if (!ret && w)
		w(in, warg);
	if (!ret)
		ret = b.transact(target, code, &in, &out, 0);
	if (!ret)
		ret = b.readStatusHeader(out, &status);
	if (ret)
		fprintf(stderr, "tetris-fp: %s: binder error %d\n", what, ret);
	else if (!b.statusIsOk(status))
		fprintf(stderr, "tetris-fp: %s: %s\n", what, b.statusDescription(status));
	else
		res = r ? r(out, rarg) : 0;
	if (status)
		b.statusDelete(status);
	if (out)
		b.parcelDelete(out);
	return res;
}

static void w_i64(AParcel *in, void *arg) { b.writeInt64(in, *(int64_t *)arg); }
static void w_hat(AParcel *in, void *arg) { write_hat(in, arg); }

static int r_binder(const AParcel *out, void *arg)
{
	return b.readStrongBinder(out, arg) ? -1 : 0;
}

/* --- ISessionCallback ------------------------------------------------------------ */

/* ids from the last onEnrollmentsEnumerated, for "clear" */
static struct i32s enrolled;

static const char *acquired_name(int32_t info)
{
	static const char *const names[] = {
		"unknown", "good", "partial", "insufficient", "sensor dirty", "too slow",
		"too fast", "vendor", "start", "too dark", "too bright", "immobile",
		"retrying capture", "lift finger", "touch",
	};

	return info >= 0 && info < (int32_t)(sizeof(names) / sizeof(*names)) ? names[info] : "?";
}

static binder_status_t cb_transact(AIBinder *binder, uint32_t code, const AParcel *in,
				   AParcel *out)
{
	AStatus *ok = b.statusOk();
	int32_t a = 0, c = 0;

	switch (code) {
	case CB_CHALLENGE:
		pthread_mutex_lock(&lock);
		b.readInt64(in, &st.challenge);
		st.challenge_done = true;
		pthread_cond_broadcast(&cond);
		pthread_mutex_unlock(&lock);
		break;
	case CB_ACQUIRED:
		b.readInt32(in, &a);
		b.readInt32(in, &c);
		if (a != 1 && a != 8)
			printf("acquired: %s (%d/%d)\n", acquired_name(a), a, c);
		break;
	case CB_ERROR:
		b.readInt32(in, &a);
		b.readInt32(in, &c);
		printf("error %d (vendor %d)\n", a, c);
		signal_done(-a ? -a : -1);
		break;
	case CB_ENROLL_PROGRESS:
		b.readInt32(in, &a);
		b.readInt32(in, &c);
		printf("enroll: finger %d, %d touches left\n", a, c);
		if (c == 0)
			signal_done(0);
		break;
	case CB_AUTH_OK:
		b.readInt32(in, &a);
		printf("match: finger %d\n", a);
		signal_done(0);
		break;
	case CB_AUTH_FAILED:
		printf("no match\n");
		signal_done(1);
		break;
	case CB_LOCKOUT_TIMED: {
		int64_t ms = 0;

		b.readInt64(in, &ms);
		printf("locked out for %lld ms\n", (long long)ms);
		signal_done(-100);
		break;
	}
	case CB_LOCKOUT_PERMANENT:
		printf("locked out permanently\n");
		signal_done(-101);
		break;
	case CB_ENUMERATED:
	case CB_REMOVED: {
		struct i32s ids = { 0 };

		b.readInt32Array(in, &ids, alloc_i32s);
		printf(code == CB_ENUMERATED ? "enrolled:" : "removed:");
		for (int i = 0; i < ids.len; i++)
			printf(" %d", ids.data[i]);
		printf("%s\n", ids.len > 0 ? "" : " none");
		if (code == CB_ENUMERATED) {
			free(enrolled.data);
			enrolled = ids;
		} else {
			free(ids.data);
		}
		signal_done(0);
		break;
	}
	default:
		break;
	}
	b.writeStatusHeader(out, ok);
	if (code == GET_INTERFACE_VERSION)
		b.writeInt32(out, 3);
	else if (code == GET_INTERFACE_HASH)
		b.writeString(out, "notfrozen", 9);
	b.statusDelete(ok);
	return 0;
}

static void *noop_create(void *args) { return args; }
static void noop_destroy(void *data) { }
static binder_status_t noop_transact(AIBinder *bd, uint32_t c, const AParcel *i, AParcel *o)
{
	return -1;
}

struct create_args {
	int32_t sensor, user;
	AIBinder *cb;
};

static void w_create(AParcel *in, void *arg)
{
	struct create_args *a = arg;

	b.writeInt32(in, a->sensor);
	b.writeInt32(in, a->user);
	b.writeStrongBinder(in, a->cb);
}

static void w_ids(AParcel *in, void *arg)
{
	struct i32s *ids = arg;

	b.writeInt32Array(in, ids->data, ids->len);
}

/* waits for the result of an operation; timeout in seconds */
static int wait_done(int timeout)
{
	struct timespec ts;
	int ret = 0;

	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += timeout;
	pthread_mutex_lock(&lock);
	while (!st.done && ret != ETIMEDOUT)
		ret = pthread_cond_timedwait(&cond, &lock, &ts);
	ret = st.done ? st.result : -ETIMEDOUT;
	st.done = false;
	pthread_mutex_unlock(&lock);
	return ret;
}

/* --- gatekeeper: mint a HardwareAuthToken for a challenge ------------------------------ */

struct gk_enroll_args {
	struct bytes password;
};

static void w_gk_enroll(AParcel *in, void *arg)
{
	static const int8_t none[1];
	struct gk_enroll_args *a = arg;

	b.writeInt32(in, 0);                    /* uid */

	/* currentPasswordHandle, currentPassword: empty, not null (non-nullable arrays) */
	b.writeByteArray(in, none, 0);
	b.writeByteArray(in, none, 0);
	b.writeByteArray(in, a->password.data, a->password.len);
}

static int r_gk_enroll(const AParcel *out, void *arg)
{
	struct bytes *handle = arg;
	int32_t end = parcelable_read_begin(out), status = -1, timeout = 0;
	int64_t sid = 0;

	if (end < 0)
		return -1;
	b.readInt32(out, &status);
	b.readInt32(out, &timeout);
	b.readInt64(out, &sid);
	b.readByteArray(out, handle, alloc_bytes);
	if (status != 0)
		fprintf(stderr, "tetris-fp: gatekeeper enroll status %d\n", status);
	return status == 0 && handle->len > 0 ? 0 : -1;
}

struct gk_verify_args {
	int64_t challenge;
	struct bytes handle, password;
};

static void w_gk_verify(AParcel *in, void *arg)
{
	struct gk_verify_args *a = arg;

	b.writeInt32(in, 0);
	b.writeInt64(in, a->challenge);
	b.writeByteArray(in, a->handle.data, a->handle.len);
	b.writeByteArray(in, a->password.data, a->password.len);
}

static int r_gk_verify(const AParcel *out, void *arg)
{
	int32_t end = parcelable_read_begin(out), status = -1, timeout = 0;

	if (end < 0)
		return -1;
	b.readInt32(out, &status);
	b.readInt32(out, &timeout);
	if (status != 0 && status != 1) {
		fprintf(stderr, "tetris-fp: gatekeeper verify status %d\n", status);
		return -1;
	}
	return read_hat(out, arg);
}

static int read_file(const char *path, struct bytes *out)
{
	struct stat s;
	int fd = open(path, O_RDONLY | O_CLOEXEC);

	if (fd < 0 || fstat(fd, &s) || s.st_size <= 0) {
		if (fd >= 0)
			close(fd);
		return -1;
	}
	out->len = s.st_size;
	out->data = malloc(out->len);
	if (read(fd, out->data, out->len) != out->len) {
		close(fd);
		return -1;
	}
	close(fd);
	return 0;
}

static int write_file(const char *path, const struct bytes *in)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);

	if (fd < 0 || write(fd, in->data, in->len) != in->len) {
		perror(path);
		if (fd >= 0)
			close(fd);
		return -1;
	}
	return close(fd);
}

static int mint_token(int64_t challenge, struct hat *hat)
{
	struct gk_verify_args v = { .challenge = challenge };
	AIBinder *gk = b.waitForService(GK_SERVICE);

	if (!gk) {
		fprintf(stderr, "tetris-fp: no %s\n", GK_SERVICE);
		return -1;
	}
	b.associateClass(gk, b.classDefine("android.hardware.gatekeeper.IGatekeeper",
					     noop_create, noop_destroy, noop_transact));
	mkdir(STATE_DIR, 0700);
	if (read_file(STATE_DIR "/gk-secret", &v.password) ||
	    read_file(STATE_DIR "/gk-handle", &v.handle)) {
		struct gk_enroll_args e = { .password = { malloc(32), 32 } };

		if (getrandom(e.password.data, 32, 0) != 32)
			return -1;
		if (call(gk, GK_ENROLL, "gatekeeper enroll", w_gk_enroll, &e, r_gk_enroll,
			 &v.handle) || write_file(STATE_DIR "/gk-secret", &e.password) ||
		    write_file(STATE_DIR "/gk-handle", &v.handle))
			return -1;
		v.password = e.password;
	}
	return call(gk, GK_VERIFY, "gatekeeper verify", w_gk_verify, &v, r_gk_verify, hat);
}

/* --- main ------------------------------------------------------------------------- */

#define SYM(field, name) do { \
		*(void **)&b.field = hybris_dlsym(lib, name); \
		if (!b.field) { \
			fprintf(stderr, "tetris-fp: missing %s\n", name); \
			return 1; \
		} \
	} while (0)

struct sensor {
	int32_t id, type, x, y, radius;
};

/* SensorProps[]: count, then SensorProps { CommonProps commonProps { sensorId, ... },
 * FingerprintSensorType sensorType, SensorLocation[] sensorLocations { displayId, x, y,
 * radius, ... }, ... } */
static int read_sensor(const AParcel *out, void *arg)
{
	struct sensor *s = arg;
	int32_t count = 0, common_end, loc_end;

	if (b.readInt32(out, &count) || count < 1 || parcelable_read_begin(out) < 0)
		return -1;
	common_end = parcelable_read_begin(out);
	if (common_end < 0 || b.readInt32(out, &s->id))
		return -1;
	b.setDataPosition(out, common_end);
	b.readInt32(out, &s->type);
	if (!b.readInt32(out, &count) && count > 0 && (loc_end = parcelable_read_begin(out)) > 0) {
		int32_t display;

		b.readInt32(out, &display);
		b.readInt32(out, &s->x);
		b.readInt32(out, &s->y);
		b.readInt32(out, &s->radius);
	}
	return 0;
}

int main(int argc, char **argv)
{
	const char *cmd = argc > 1 ? argv[1] : "";
	AIBinder *fp, *cb, *session = NULL, *cancel = NULL;
	AIBinder_Class *fp_class, *cb_class, *any_class;
	struct sensor sensor = { 0 };
	int ret = 1;
	void *lib;

	if (strcmp(cmd, "enroll") && strcmp(cmd, "verify") && strcmp(cmd, "list") &&
	    strcmp(cmd, "clear") && strcmp(cmd, "props")) {
		fprintf(stderr, "usage: tetris-fp enroll|verify|list|clear|props\n");
		return 2;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	lib = hybris_dlopen("libbinder_ndk.so", RTLD_NOW);
	if (!lib) {
		fprintf(stderr, "tetris-fp: %s\n", hybris_dlerror());
		return 1;
	}
	SYM(startThreadPool, "ABinderProcess_startThreadPool");
	SYM(waitForService, "AServiceManager_waitForService");
	SYM(classDefine, "AIBinder_Class_define");
	SYM(binderNew, "AIBinder_new");
	SYM(associateClass, "AIBinder_associateClass");
	SYM(markVintfStability, "AIBinder_markVintfStability");
	SYM(prepareTransaction, "AIBinder_prepareTransaction");
	SYM(transact, "AIBinder_transact");
	SYM(parcelDelete, "AParcel_delete");
	SYM(writeInt32, "AParcel_writeInt32");
	SYM(writeInt64, "AParcel_writeInt64");
	SYM(writeStrongBinder, "AParcel_writeStrongBinder");
	SYM(writeString, "AParcel_writeString");
	SYM(writeByteArray, "AParcel_writeByteArray");
	SYM(writeInt32Array, "AParcel_writeInt32Array");
	SYM(readInt32, "AParcel_readInt32");
	SYM(readInt64, "AParcel_readInt64");
	SYM(readStrongBinder, "AParcel_readStrongBinder");
	SYM(readByteArray, "AParcel_readByteArray");
	SYM(readInt32Array, "AParcel_readInt32Array");
	SYM(getDataPosition, "AParcel_getDataPosition");
	SYM(setDataPosition, "AParcel_setDataPosition");
	SYM(readStatusHeader, "AParcel_readStatusHeader");
	SYM(writeStatusHeader, "AParcel_writeStatusHeader");
	SYM(statusOk, "AStatus_newOk");
	SYM(statusIsOk, "AStatus_isOk");
	SYM(statusDescription, "AStatus_getDescription");
	SYM(statusDelete, "AStatus_delete");
	b.startThreadPool();

	fp = b.waitForService(FP_SERVICE);
	if (!fp) {
		fprintf(stderr, "tetris-fp: no %s\n", FP_SERVICE);
		return 1;
	}
	fp_class = b.classDefine("android.hardware.biometrics.fingerprint.IFingerprint",
				 noop_create, noop_destroy, noop_transact);
	cb_class = b.classDefine("android.hardware.biometrics.fingerprint.ISessionCallback",
				 noop_create, noop_destroy, cb_transact);
	if (!b.associateClass(fp, fp_class)) {
		fprintf(stderr, "tetris-fp: IFingerprint class mismatch\n");
		return 1;
	}
	if (call(fp, FP_GET_SENSOR_PROPS, "getSensorProps", NULL, NULL, read_sensor, &sensor))
		fprintf(stderr, "tetris-fp: no sensor props, trying sensor 0\n");
	if (!strcmp(cmd, "props")) {
		/* type: 1 rear, 2 under-display ultrasonic, 3 under-display optical, 4 power button */
		printf("sensor %d type %d at %d,%d radius %d\n", sensor.id, sensor.type, sensor.x,
		       sensor.y, sensor.radius);
		return 0;
	}
	cb = b.binderNew(cb_class, NULL);
	/* The vendor HAL only calls binders of VINTF stability; ours would be "system" */
	b.markVintfStability(cb);

	struct create_args cs = { sensor.id, 0, cb };

	if (call(fp, FP_CREATE_SESSION, "createSession", w_create, &cs, r_binder, &session) ||
	    !session)
		return 1;
	any_class = b.classDefine("android.hardware.biometrics.fingerprint.ISession",
				  noop_create, noop_destroy, noop_transact);
	b.associateClass(session, any_class);

	if (!strcmp(cmd, "list") || !strcmp(cmd, "clear")) {
		if (call(session, S_ENUMERATE, "enumerateEnrollments", NULL, NULL, NULL, NULL))
			goto out;
		ret = wait_done(10) ? 1 : 0;
		if (!strcmp(cmd, "clear")) {
			if (!call(session, S_REMOVE, "removeEnrollments", w_ids, &enrolled, NULL, NULL))
				ret = wait_done(10) ? 1 : 0;
		}
	} else if (!strcmp(cmd, "enroll")) {
		struct hat hat = { 0 };
		struct timespec ts;

		if (call(session, S_GENERATE_CHALLENGE, "generateChallenge", NULL, NULL, NULL, NULL))
			goto out;
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_sec += 5;
		pthread_mutex_lock(&lock);
		while (!st.challenge_done && !pthread_cond_timedwait(&cond, &lock, &ts))
			;
		pthread_mutex_unlock(&lock);
		if (!st.challenge_done) {
			fprintf(stderr, "tetris-fp: no challenge\n");
			goto out;
		}
		if (mint_token(st.challenge, &hat))
			goto out;
		printf("touch the sensor (power button) repeatedly\n");
		if (call(session, S_ENROLL, "enroll", w_hat, &hat, r_binder, &cancel))
			goto out;
		ret = wait_done(120) ? 1 : 0;
		call(session, S_REVOKE_CHALLENGE, "revokeChallenge", w_i64, &st.challenge, NULL,
		     NULL);
	} else {
		int64_t op = 0;

		printf("touch the sensor\n");
		if (call(session, S_AUTHENTICATE, "authenticate", w_i64, &op, r_binder, &cancel))
			goto out;
		ret = wait_done(30);
		ret = ret == 0 ? 0 : ret == 1 ? 1 : 2;
	}
out:
	call(session, S_CLOSE, "close", NULL, NULL, NULL, NULL);
	return ret;
}
