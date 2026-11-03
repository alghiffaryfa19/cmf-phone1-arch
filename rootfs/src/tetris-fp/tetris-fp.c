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
 *   tetris-fp unlockd      fingerprint unlock for the Phosh lock screen (tetris-fpd.service):
 *                          while the session is locked and the display on, show the
 *                          sensor spot and scan; unlock through logind on a match
 *
 * STATUS (2026-11-03): enroll and verify work (spot from tetris-fp-overlay, HBM via the
 * patched panel driver, HAL outside the container's netns: tetris-fp.service). Unlock
 * (unlockd, tetris-fpd.service) is not enabled: if the touch controller's fingerprint
 * mode gets set while the display is off (the HAL does that after enumerating or
 * cancelling), touch input stays dead after the display comes back on.
 */
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <linux/input.h>
#include <fcntl.h>
#include <hybris/common/dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
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
       S_ENUMERATE = 6, S_REMOVE = 7, S_CLOSE = 11, S_POINTER_DOWN = 12, S_POINTER_UP = 13,
       S_UI_READY = 14 };
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
	binder_status_t (*writeFloat)(AParcel *, float);
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

static int last_result = -ETIMEDOUT;

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
	last_result = ret;
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

struct sensor {
	int32_t id, type, x, y, radius;
};

/* --- touch: the optical sensor under the display -------------------------------------
 * The HAL needs to know when a finger is on the sensor (onPointerDown) and when the
 * spot that lights it is shown (onUiReady); on Android SystemUI does both. We read the
 * touchscreen directly (libinput in phoc doesn't grab it); tetris-fp-overlay draws the
 * spot. */

struct touch_args {
	AIBinder *session;
	struct sensor *sensor;
};

struct pointer {
	int32_t id, x, y;
	float minor, major;
};

static void w_pointer_down(AParcel *in, void *arg)
{
	struct pointer *p = arg;

	b.writeInt32(in, p->id);
	b.writeInt32(in, p->x);
	b.writeInt32(in, p->y);
	b.writeFloat(in, p->minor);
	b.writeFloat(in, p->major);
}

static void w_i32(AParcel *in, void *arg) { b.writeInt32(in, *(int32_t *)arg); }

/* The Goodix driver tells the HAL "UI ready" (and the HAL only then captures) once the
 * panel driver has both high brightness and the UI status set; SystemUI does that on
 * Android when it has drawn the spot. */
static void panel_feature(const char *name, int on)
{
	char path[64];
	int fd;

	snprintf(path, sizeof(path), "/sys/panel_feature/%s", name);
	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0 || write(fd, on ? "1" : "0", 1) != 1)
		fprintf(stderr, "tetris-fp: %s: %s\n", path, strerror(errno));
	if (fd >= 0)
		close(fd);
}

static int open_touchscreen(void)
{
	DIR *d = opendir("/dev/input");
	struct dirent *e;
	int fd = -1;

	while (d && (e = readdir(d)) && fd < 0) {
		char path[300], name[64] = "";

		if (strncmp(e->d_name, "event", 5))
			continue;
		snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
		fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd >= 0 && (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0 ||
				strcmp(name, "fts_ts"))) {
			close(fd);
			fd = -1;
		}
	}
	if (d)
		closedir(d);
	return fd;
}

static void *touch_thread(void *arg)
{
	struct touch_args *t = arg;
	struct pointer p = { .id = 0, .minor = 1, .major = 1 };
	int32_t x = 0, y = 0, major = 0, slot = 0;
	bool touching = false, down = false;
	struct input_event ev;
	int fd = open_touchscreen();

	if (fd < 0) {
		fprintf(stderr, "tetris-fp: no touchscreen\n");
		return NULL;
	}
	while (read(fd, &ev, sizeof(ev)) == sizeof(ev)) {
		if (ev.type == EV_ABS && ev.code == ABS_MT_SLOT)
			slot = ev.value;
		else if (ev.type == EV_ABS && slot == 0 && ev.code == ABS_MT_POSITION_X)
			x = ev.value;
		else if (ev.type == EV_ABS && slot == 0 && ev.code == ABS_MT_POSITION_Y)
			y = ev.value;
		else if (ev.type == EV_ABS && slot == 0 && ev.code == ABS_MT_TOUCH_MAJOR)
			major = ev.value;
		else if (ev.type == EV_KEY && ev.code == BTN_TOUCH)
			touching = ev.value;
		else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
			int64_t dx = x - t->sensor->x, dy = y - t->sensor->y;
			int64_t r = t->sensor->radius;
			bool on = touching && dx * dx + dy * dy <= r * r;

			if (on && !down) {
				p.x = x;
				p.y = y;
				p.major = p.minor = major > 0 ? major : 1;
				call(t->session, S_POINTER_DOWN, "onPointerDown", w_pointer_down,
				     &p, NULL, NULL);
				panel_feature("hbm_mode", 1);
				panel_feature("ui_status", 1);
				call(t->session, S_UI_READY, "onUiReady", NULL, NULL, NULL, NULL);
			} else if (!on && down) {
				call(t->session, S_POINTER_UP, "onPointerUp", w_i32, &p.id, NULL,
				     NULL);
				panel_feature("ui_status", 0);
				panel_feature("hbm_mode", 0);
			}
			down = on;
		}
	}
	return NULL;
}

/* The touch controller's fingerprint mode (/proc/touchpanel/fod_mode: 0 off, 1 on,
 * 2 "unlock", for unlocking with the screen off). The HAL sets 1 for a scan and 2 after
 * enumerating or cancelling, but with the mode on, the touch driver doesn't come back
 * from the display going off: no touches until a reboot. So: 1 only while we scan with
 * the display on, 0 otherwise. */
static void fod_mode(int mode)
{
	char c = '0' + mode;
	int fd = open("/proc/touchpanel/fod_mode", O_WRONLY | O_CLOEXEC);

	if (fd < 0 || write(fd, &c, 1) != 1)
		fprintf(stderr, "tetris-fp: fod_mode: %s\n", strerror(errno));
	if (fd >= 0)
		close(fd);
}

/* Leave the panel and touch controller as we found them, also when killed (tetris-fpd
 * stops us when the session gets unlocked by PIN); cancelling tells the HAL to stop
 * scanning and switch the touch controller's fingerprint mode off */
static AIBinder *g_session, *g_cancel;
static bool g_scanned;

static void set_cancel(AIBinder *cancel)
{
	if (cancel)
		b.associateClass(cancel, b.classDefine(
			"android.hardware.biometrics.common.ICancellationSignal", noop_create,
			noop_destroy, noop_transact));
	g_cancel = cancel;
	g_scanned = true;
}

static bool st_finished(void)
{
	/* wait_done() consumed st.done; a result other than a timeout ends the operation */
	return last_result != -ETIMEDOUT;
}

static void cleanup(void)
{
	if (g_cancel)
		call(g_cancel, 1, "cancel", NULL, NULL, NULL, NULL);
	if (g_scanned) {
		panel_feature("ui_status", 0);
		panel_feature("hbm_mode", 0);
	}
	if (g_session)
		call(g_session, S_CLOSE, "close", NULL, NULL, NULL, NULL);
	fod_mode(0);
}

static void *signal_thread(void *arg)
{
	sigset_t *set = arg;
	int sig;

	sigwait(set, &sig);
	cleanup();
	_exit(3);
}

/* --- unlockd ----------------------------------------------------------------------- */

static char lock_session[32], lock_user[64];

/* the graphical user session: id and user name */
static bool find_session(void)
{
	FILE *f = popen("loginctl list-sessions --no-legend 2>/dev/null", "r");
	char line[256], id[32], user[64], seat[32];
	unsigned uid;
	bool found = false;

	/* SESSION UID USER SEAT ...: the one on seat0 (not ssh logins) */
	while (f && fgets(line, sizeof(line), f))
		if (sscanf(line, "%31s %u %63s %31s", id, &uid, user, seat) == 4 && uid >= 1000 &&
		    !strcmp(seat, "seat0")) {
			strcpy(lock_session, id);
			strcpy(lock_user, user);
			found = true;
			break;
		}
	if (f)
		pclose(f);
	return found;
}

static bool session_locked(void)
{
	char cmd[128], buf[16] = "";
	FILE *f;

	snprintf(cmd, sizeof(cmd), "loginctl show-session %s -p LockedHint --value 2>/dev/null",
		 lock_session);
	f = popen(cmd, "r");
	if (f) {
		if (!fgets(buf, sizeof(buf), f))
			buf[0] = 0;
		pclose(f);
	}
	return !strncmp(buf, "yes", 3);
}

/* the display driver holds this wakelock while the panel is on */
static bool display_on(void)
{
	FILE *f = fopen("/sys/kernel/debug/wakeup_sources", "r");
	char line[256], name[64];
	long v[5];
	bool on = false;

	while (f && fgets(line, sizeof(line), f))
		if (sscanf(line, "%63s %ld %ld %ld %ld %ld", name, &v[0], &v[1], &v[2], &v[3],
			   &v[4]) == 6 && !strcmp(name, "disp_crtc0_wakelock")) {
			on = v[4] > 0;
			break;
		}
	if (f)
		fclose(f);
	return on;
}

static void overlay(const char *action)
{
	char cmd[192];

	snprintf(cmd, sizeof(cmd),
		 "systemctl --user -M %s@ %s tetris-fp-overlay.service >/dev/null 2>&1",
		 lock_user, action);
	if (system(cmd))
		fprintf(stderr, "tetris-fp: overlay %s failed\n", action);
}

static void unlockd(AIBinder *session)
{
	AIBinder *cancel = NULL;
	bool spent = false;
	int64_t op = 0;

	for (;;) {
		int ret, secs = 0;

		if (!display_on())
			spent = false;
		if (spent || !find_session() || !session_locked() || !display_on()) {
			usleep(500000);
			continue;
		}
		overlay("start");
		cancel = NULL;
		if (call(session, S_AUTHENTICATE, "authenticate", w_i64, &op, r_binder, &cancel)) {
			overlay("stop");
			sleep(2);
			continue;
		}
		set_cancel(cancel);
		fod_mode(1);
		/* scan until a match, an error, or the lock screen/display goes away; a finger
		 * that doesn't match doesn't end the operation */
		/* at most a minute per time the display is on: the touch controller is in
		 * fingerprint mode meanwhile, and should something go wrong, touch is back
		 * to normal soon */
		do {
			ret = wait_done(1);
		} while ((ret == -ETIMEDOUT || ret == 1) && ++secs < 60 && session_locked() &&
			 display_on());
		if (secs >= 60)
			spent = true;
		if (ret == -ETIMEDOUT || ret == 1) {
			call(g_cancel, 1, "cancel", NULL, NULL, NULL, NULL);
			wait_done(2);           /* its onError(CANCELED) */
		}
		g_cancel = NULL;
		fod_mode(0);
		panel_feature("ui_status", 0);
		panel_feature("hbm_mode", 0);
		overlay("stop");
		if (ret == 0) {
			char cmd[96];

			printf("match, unlocking session %s\n", lock_session);
			snprintf(cmd, sizeof(cmd), "loginctl unlock-session %s", lock_session);
			if (system(cmd))
				fprintf(stderr, "tetris-fp: unlock failed\n");
			/* until Phosh has followed */
			for (int i = 0; i < 20 && session_locked(); i++)
				usleep(100000);
		} else if (ret < 0 && ret != -ETIMEDOUT) {
			/* lockout or another error: don't spin */
			sleep(2);
		}
	}
}

/* --- main ------------------------------------------------------------------------- */

#define SYM(field, name) do { \
		*(void **)&b.field = hybris_dlsym(lib, name); \
		if (!b.field) { \
			fprintf(stderr, "tetris-fp: missing %s\n", name); \
			return 1; \
		} \
	} while (0)

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
	    strcmp(cmd, "clear") && strcmp(cmd, "props") && strcmp(cmd, "unlockd")) {
		fprintf(stderr, "usage: tetris-fp enroll|verify|list|clear|props|unlockd\n");
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
	SYM(writeFloat, "AParcel_writeFloat");
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
	/* handle SIGTERM/SIGINT in one thread, after binder is up */
	static sigset_t sigs;
	pthread_t sig_th;

	sigemptyset(&sigs);
	sigaddset(&sigs, SIGTERM);
	sigaddset(&sigs, SIGINT);
	pthread_sigmask(SIG_BLOCK, &sigs, NULL);
	b.startThreadPool();
	pthread_create(&sig_th, NULL, signal_thread, &sigs);

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
	g_session = session;
	if (sensor.type == 2 || sensor.type == 3) {
		static struct touch_args t;
		pthread_t th;

		t.session = session;
		t.sensor = &sensor;
		pthread_create(&th, NULL, touch_thread, &t);
	}

	if (!strcmp(cmd, "unlockd")) {
		unlockd(session);
	} else if (!strcmp(cmd, "list") || !strcmp(cmd, "clear")) {
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
		printf("put your finger on the sensor, lift it, repeat\n");
		if (call(session, S_ENROLL, "enroll", w_hat, &hat, r_binder, &cancel))
			goto out;
		set_cancel(cancel);
		ret = wait_done(120) ? 1 : 0;
		call(session, S_REVOKE_CHALLENGE, "revokeChallenge", w_i64, &st.challenge, NULL,
		     NULL);
	} else {
		int64_t op = 0;

		printf("touch the sensor\n");
		if (call(session, S_AUTHENTICATE, "authenticate", w_i64, &op, r_binder, &cancel))
			goto out;
		set_cancel(cancel);
		ret = wait_done(30);
		ret = ret == 0 ? 0 : ret == 1 ? 1 : 2;
	}
out:
	/* after a result the operation is over: nothing to cancel */
	if (st_finished())
		g_cancel = NULL;
	cleanup();
	return ret;
}
