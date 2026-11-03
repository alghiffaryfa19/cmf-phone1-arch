// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * GPS for the CMF Phone 1: a client of the Android GNSS HAL (android.hardware.gnss AIDL,
 * MediaTek's android.hardware.gnss-service.mediatek over mnld, both in the Halium
 * container), talking binder through libbinder_ndk via libhybris. It starts positioning
 * with NMEA output and serves the NMEA sentences on a Unix socket, which geoclue reads
 * (network-nmea source, nmea-socket=). The receiver only runs while someone is connected.
 *
 * The AIDL interfaces are stable and public (hardware/interfaces/gnss/aidl), so the
 * parcels are written by hand: transaction code = 1 + index of the method.
 *   IGnss:         setCallback 1, start 14, stop 15, setPositionMode 20, startNmea 25
 *   IGnssCallback: gnssNmeaCb(long timestamp, String nmea) 5
 */
#include <dlfcn.h>
#include <errno.h>
#include <hybris/common/dlfcn.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define SOCKET_PATH  "/run/tetris-gnss/nmea.sock"
#define GNSS_SERVICE "android.hardware.gnss.IGnss/default"
#define MAX_CLIENTS  8

enum { IGNSS_SET_CALLBACK = 1, IGNSS_START = 14, IGNSS_STOP = 15,
       IGNSS_SET_POSITION_MODE = 20, IGNSS_START_NMEA = 25 };
enum { CB_NMEA = 5 };
#define FIRST_CALL_TRANSACTION 1
#define GET_INTERFACE_VERSION  0x00fffffe
#define GET_INTERFACE_HASH     0x00fffffd

typedef struct AIBinder AIBinder;
typedef struct AIBinder_Class AIBinder_Class;
typedef struct AParcel AParcel;
typedef struct AStatus AStatus;
typedef int32_t binder_status_t;
typedef bool (*string_allocator)(void *data, int32_t length, char **buffer);

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
	binder_status_t (*writeBool)(AParcel *, bool);
	binder_status_t (*writeStrongBinder)(AParcel *, AIBinder *);
	binder_status_t (*writeString)(AParcel *, const char *, int32_t);
	binder_status_t (*readInt64)(const AParcel *, int64_t *);
	binder_status_t (*readString)(const AParcel *, void *, string_allocator);
	int32_t (*getDataPosition)(const AParcel *);
	binder_status_t (*setDataPosition)(const AParcel *, int32_t);
	binder_status_t (*readStatusHeader)(const AParcel *, AStatus **);
	binder_status_t (*writeStatusHeader)(AParcel *, const AStatus *);
	AStatus *(*statusOk)(void);
	bool (*statusIsOk)(const AStatus *);
	const char *(*statusDescription)(const AStatus *);
	void (*statusDelete)(AStatus *);
} b;

static int clients[MAX_CLIENTS], nclients;
static pthread_mutex_t clients_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t clients_changed = PTHREAD_COND_INITIALIZER;
static AIBinder *gnss;

/* --- NMEA socket ------------------------------------------------------------ */

static void broadcast(const char *line, size_t len)
{
	pthread_mutex_lock(&clients_lock);
	for (int i = 0; i < MAX_CLIENTS; i++)
		if (clients[i] > 0 && send(clients[i], line, len, MSG_NOSIGNAL | MSG_DONTWAIT) < 0 &&
		    errno != EAGAIN) {
			close(clients[i]);
			clients[i] = 0;
			if (--nclients == 0)
				pthread_cond_signal(&clients_changed);
		}
	pthread_mutex_unlock(&clients_lock);
}

static void *accept_loop(void *arg)
{
	int srv = (int)(intptr_t)arg;

	for (;;) {
		int fd = accept(srv, NULL, NULL);

		if (fd < 0)
			continue;
		pthread_mutex_lock(&clients_lock);
		for (int i = 0; i < MAX_CLIENTS; i++)
			if (clients[i] <= 0) {
				clients[i] = fd;
				fd = -1;
				if (nclients++ == 0)
					pthread_cond_signal(&clients_changed);
				break;
			}
		pthread_mutex_unlock(&clients_lock);
		if (fd >= 0)
			close(fd);
	}
	return NULL;
}

static int open_socket(void)
{
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	pthread_t t;
	int fd;

	mkdir("/run/tetris-gnss", 0755);
	unlink(SOCKET_PATH);
	strcpy(addr.sun_path, SOCKET_PATH);
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0 || bind(fd, (struct sockaddr *)&addr, sizeof(addr)) || listen(fd, 4)) {
		perror("tetris-gnss: " SOCKET_PATH);
		return -1;
	}
	chmod(SOCKET_PATH, 0666);
	pthread_create(&t, NULL, accept_loop, (void *)(intptr_t)fd);
	return 0;
}

/* --- IGnssCallback ------------------------------------------------------------- */

static bool alloc_string(void *data, int32_t length, char **buffer)
{
	char **out = data;

	if (length < 0) {          /* null string */
		*buffer = NULL;
		return true;
	}
	*out = malloc(length);
	*buffer = *out;
	return *out != NULL;
}

static void *cb_create(void *args) { return args; }
static void cb_destroy(void *data) { }

static binder_status_t cb_transact(AIBinder *binder, uint32_t code, const AParcel *in,
				   AParcel *out)
{
	AStatus *ok = b.statusOk();

	if (code == FIRST_CALL_TRANSACTION + CB_NMEA - 1) {
		int64_t ts;
		char *nmea = NULL;

		if (!b.readInt64(in, &ts) && !b.readString(in, &nmea, alloc_string) && nmea) {
			size_t len = strlen(nmea);

			if (getenv("TETRIS_GNSS_DEBUG")) {
				fputs(nmea, stdout);
				if (len && nmea[len - 1] != '\n')
					fputc('\n', stdout);
			}
			broadcast(nmea, len);
			if (len && nmea[len - 1] != '\n')
				broadcast("\r\n", 2);
		}
		free(nmea);
	}
	b.writeStatusHeader(out, ok);
	if (code == GET_INTERFACE_VERSION)
		b.writeInt32(out, 3);
	else if (code == GET_INTERFACE_HASH)
		b.writeString(out, "notfrozen", 9);
	b.statusDelete(ok);
	return 0;
}

/* --- IGnss calls ----------------------------------------------------------------- */

static void *gnss_create(void *args) { return args; }
static binder_status_t gnss_transact(AIBinder *bd, uint32_t c, const AParcel *i, AParcel *o)
{
	return -1;
}

typedef void (*writer)(AParcel *in, void *arg);

static int call(AIBinder *gnss, uint32_t code, const char *what, writer w, void *arg)
{
	AParcel *in = NULL, *out = NULL;
	AStatus *status = NULL;
	binder_status_t ret;

	ret = b.prepareTransaction(gnss, &in);
	if (!ret && w)
		w(in, arg);
	if (!ret)
		ret = b.transact(gnss, code, &in, &out, 0);
	if (!ret)
		ret = b.readStatusHeader(out, &status);
	if (ret)
		printf("tetris-gnss: %s: binder error %d\n", what, ret);
	else if (!b.statusIsOk(status))
		printf("tetris-gnss: %s: %s\n", what, b.statusDescription(status));
	else
		printf("tetris-gnss: %s ok\n", what);
	if (status)
		b.statusDelete(status);
	if (out)
		b.parcelDelete(out);
	return ret;
}

static void write_binder(AParcel *in, void *arg) { b.writeStrongBinder(in, arg); }

/* PositionModeOptions: standalone, periodic every second */
static void write_position_mode(AParcel *in, void *arg)
{
	int32_t start, end;

	b.writeInt32(in, 1);                  /* non-null parcelable */
	start = b.getDataPosition(in);
	b.writeInt32(in, 0);                  /* size, patched below */
	b.writeInt32(in, 0);                  /* mode STANDALONE */
	b.writeInt32(in, 0);                  /* recurrence PERIODIC */
	b.writeInt32(in, 1000);               /* minIntervalMs */
	b.writeInt32(in, 0);                  /* preferredAccuracyMeters */
	b.writeInt32(in, 0);                  /* preferredTimeMs */
	b.writeBool(in, false);               /* lowPowerMode */
	end = b.getDataPosition(in);
	b.setDataPosition(in, start);
	b.writeInt32(in, end - start);
	b.setDataPosition(in, end);
}

#define SYM(field, name) do { \
		*(void **)&b.field = hybris_dlsym(lib, name); \
		if (!b.field) { \
			fprintf(stderr, "tetris-gnss: missing %s\n", name); \
			return 1; \
		} \
	} while (0)

int main(void)
{
	AIBinder_Class *cb_class, *gnss_class;
	AIBinder *cb;
	void *lib;

	setvbuf(stdout, NULL, _IOLBF, 0);
	lib = hybris_dlopen("libbinder_ndk.so", RTLD_NOW);
	if (!lib) {
		fprintf(stderr, "tetris-gnss: %s\n", hybris_dlerror());
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
	SYM(writeBool, "AParcel_writeBool");
	SYM(writeStrongBinder, "AParcel_writeStrongBinder");
	SYM(writeString, "AParcel_writeString");
	SYM(readInt64, "AParcel_readInt64");
	SYM(readString, "AParcel_readString");
	SYM(getDataPosition, "AParcel_getDataPosition");
	SYM(setDataPosition, "AParcel_setDataPosition");
	SYM(readStatusHeader, "AParcel_readStatusHeader");
	SYM(writeStatusHeader, "AParcel_writeStatusHeader");
	SYM(statusOk, "AStatus_newOk");
	SYM(statusIsOk, "AStatus_isOk");
	SYM(statusDescription, "AStatus_getDescription");
	SYM(statusDelete, "AStatus_delete");

	b.startThreadPool();

	gnss = b.waitForService(GNSS_SERVICE);
	if (!gnss) {
		fprintf(stderr, "tetris-gnss: no %s\n", GNSS_SERVICE);
		return 1;
	}
	gnss_class = b.classDefine("android.hardware.gnss.IGnss", gnss_create, cb_destroy,
				   gnss_transact);
	cb_class = b.classDefine("android.hardware.gnss.IGnssCallback", cb_create, cb_destroy,
				 cb_transact);
	if (!b.associateClass(gnss, gnss_class)) {
		fprintf(stderr, "tetris-gnss: IGnss class mismatch\n");
		return 1;
	}
	cb = b.binderNew(cb_class, NULL);
	/* The vendor HAL only calls binders of VINTF stability; ours would be "system" */
	b.markVintfStability(cb);

	call(gnss, IGNSS_SET_CALLBACK, "setCallback", write_binder, cb);
	call(gnss, IGNSS_START_NMEA, "startNmea", NULL, NULL);
	if (open_socket())
		return 1;

	/* Start and stop from here, not from the binder threads: the HAL may be blocked in
	 * a callback to us while we call it */
	pthread_mutex_lock(&clients_lock);
	for (bool running = false;;) {
		bool want = nclients > 0;

		if (want != running) {
			pthread_mutex_unlock(&clients_lock);
			if (want) {
				call(gnss, IGNSS_SET_POSITION_MODE, "setPositionMode",
				     write_position_mode, NULL);
				call(gnss, IGNSS_START, "start", NULL, NULL);
			} else {
				call(gnss, IGNSS_STOP, "stop", NULL, NULL);
			}
			running = want;
			pthread_mutex_lock(&clients_lock);
			continue;
		}
		pthread_cond_wait(&clients_changed, &clients_lock);
	}
}
