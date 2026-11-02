// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Sensor proxy for the CMF Phone 1: serves iio-sensor-proxy's D-Bus API
 * (net.hadess.SensorProxy, used by Phosh and GNOME for rotation, automatic brightness
 * and proximity blanking) from MediaTek's sensor hub, /dev/hf_manager. The sensors
 * sit behind the SCP coprocessor, so there is no IIO device for iio-sensor-proxy.
 * A sensor is only enabled while some D-Bus client has claimed it.
 */
#include <fcntl.h>
#include <errno.h>
#include <gio/gio.h>
#include <glib-unix.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "hf_abi.h"

#define BUS_NAME "net.hadess.SensorProxy"
#define OBJ_PATH "/net/hadess/SensorProxy"

enum { ACCEL, LIGHT, PROX, NSENSORS };

static const struct {
	int hf_type;
	const char *claim, *release, *has;
	int64_t delay_ns;
} sensor_defs[NSENSORS] = {
	[ACCEL] = { HF_ACCEL, "ClaimAccelerometer", "ReleaseAccelerometer", "HasAccelerometer", 100000000 },
	[LIGHT] = { HF_LIGHT, "ClaimLight", "ReleaseLight", "HasAmbientLight", 200000000 },
	[PROX]  = { HF_PROXIMITY, "ClaimProximity", "ReleaseProximity", "HasProximity", 200000000 },
};

static struct {
	bool present;
	float gain;
	GHashTable *clients;   /* unique bus name -> name watch id */
} sensors[NSENSORS];

static GDBusConnection *bus;
static int hf_fd = -1;
static const char *orientation = "undefined", *tilt = "vertical";
static double light_level;
static gboolean prox_near;

static const char introspection[] =
	"<node>"
	" <interface name='net.hadess.SensorProxy'>"
	"  <method name='ClaimAccelerometer'/><method name='ReleaseAccelerometer'/>"
	"  <method name='ClaimLight'/><method name='ReleaseLight'/>"
	"  <method name='ClaimProximity'/><method name='ReleaseProximity'/>"
	"  <property name='HasAccelerometer' type='b' access='read'/>"
	"  <property name='AccelerometerOrientation' type='s' access='read'/>"
	"  <property name='AccelerometerTilt' type='s' access='read'/>"
	"  <property name='HasAmbientLight' type='b' access='read'/>"
	"  <property name='LightLevelUnit' type='s' access='read'/>"
	"  <property name='LightLevel' type='d' access='read'/>"
	"  <property name='HasProximity' type='b' access='read'/>"
	"  <property name='ProximityNear' type='b' access='read'/>"
	" </interface>"
	"</node>";

/* No magnetometer is exposed by the hub; clients still look for the Compass object */
static const char compass_introspection[] =
	"<node>"
	" <interface name='net.hadess.SensorProxy.Compass'>"
	"  <method name='ClaimCompass'/><method name='ReleaseCompass'/>"
	"  <property name='HasCompass' type='b' access='read'/>"
	"  <property name='CompassHeading' type='d' access='read'/>"
	" </interface>"
	"</node>";

/* --- sensor hub ----------------------------------------------------------- */

static void hf_enable(int s, bool on)
{
	struct hf_manager_cmd cmd = {
		.sensor_type = sensor_defs[s].hf_type,
		.action = on ? HF_MANAGER_SENSOR_ENABLE : HF_MANAGER_SENSOR_DISABLE,
		.length = sizeof(struct hf_manager_batch),
	};
	struct hf_manager_batch b = { .delay = sensor_defs[s].delay_ns };

	memcpy(cmd.data, &b, sizeof(b));
	/* write() returns the driver's result: 0 on success. The light and proximity
	 * sensors report -EINVAL (no batching on on-change sensors) but run anyway. */
	if (write(hf_fd, &cmd, sizeof(cmd)) < 0 && s == ACCEL)
		g_warning("enable sensor %d: %s", sensor_defs[s].hf_type, g_strerror(errno));
}

static void probe_sensors(void)
{
	for (int s = 0; s < NSENSORS; s++) {
		struct ioctl_packet p = { .sensor_type = sensor_defs[s].hf_type };
		struct sensor_info info;

		sensors[s].clients = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
		if (ioctl(hf_fd, HF_MANAGER_REQUEST_REGISTER_STATUS, &p) || !p.status)
			continue;
		p.sensor_type = sensor_defs[s].hf_type;
		sensors[s].gain = 1;
		if (!ioctl(hf_fd, HF_MANAGER_REQUEST_SENSOR_INFO, &p)) {
			memcpy(&info, p.byte, sizeof(info));
			if (info.gain)
				sensors[s].gain = info.gain;
			g_message("sensor %d: %.16s, gain %u", sensor_defs[s].hf_type, info.name, info.gain);
		}
		sensors[s].present = true;
	}
}

static void emit_changed(const char *prop, GVariant *value)
{
	GVariantBuilder changed;

	g_variant_builder_init(&changed, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&changed, "{sv}", prop, value);
	g_dbus_connection_emit_signal(bus, NULL, OBJ_PATH, "org.freedesktop.DBus.Properties",
				      "PropertiesChanged",
				      g_variant_new("(sa{sv}as)", BUS_NAME, &changed, NULL), NULL);
}

/* Upright portrait reads +g on y (Android axes: x right, y up, z out of the screen).
 * A side counts as up when its axis clearly dominates and the phone isn't lying flat. */
static void update_orientation(float x, float y, float z)
{
	const char *o = orientation;
	float g = sqrtf(x * x + y * y + z * z);

	const char *t;
	float pitch;

	if (g < 4)
		return;   /* free fall */
	/* Tilt like iio-sensor-proxy: screen facing up/down, or leaning back/forward */
	pitch = asinf(fmaxf(-1, fminf(1, z / g))) * 180 / M_PI;
	t = pitch > 75 ? "face-up" : pitch < -75 ? "face-down" : pitch > 30 ? "tilted-up" :
	    pitch < -30 ? "tilted-down" : "vertical";
	if (t != tilt) {
		tilt = t;
		emit_changed("AccelerometerTilt", g_variant_new_string(t));
	}
	if (hypotf(x, y) < 0.5f * g)
		return;   /* flat on the table: keep the last orientation */
	if (fabsf(y) > 1.5f * fabsf(x))
		o = y > 0 ? "normal" : "bottom-up";
	else if (fabsf(x) > 1.5f * fabsf(y))
		o = x > 0 ? "right-up" : "left-up";
	if (o != orientation) {
		orientation = o;
		emit_changed("AccelerometerOrientation", g_variant_new_string(o));
	}
}

static gboolean on_hf_event(gint fd, GIOCondition cond, gpointer data)
{
	struct hf_manager_event ev[32];
	ssize_t n = read(fd, ev, sizeof(ev));

	for (int i = 0; i < n / (ssize_t)sizeof(ev[0]); i++) {
		if (ev[i].action != DATA_ACTION)
			continue;
		switch (ev[i].sensor_type) {
		case HF_ACCEL: {
			float k = sensors[ACCEL].gain;

			update_orientation(ev[i].word[0] / k, ev[i].word[1] / k, ev[i].word[2] / k);
			break;
		}
		case HF_LIGHT: {
			double lux = ev[i].word[0] / sensors[LIGHT].gain;

			if (lux != light_level) {
				light_level = lux;
				emit_changed("LightLevel", g_variant_new_double(lux));
			}
			break;
		}
		case HF_PROXIMITY: {
			/* distance in cm: 0 near, 5 (max range) far */
			gboolean near = ev[i].word[0] < 1;

			if (near != prox_near) {
				prox_near = near;
				emit_changed("ProximityNear", g_variant_new_boolean(near));
			}
			break;
		}
		}
	}
	return G_SOURCE_CONTINUE;
}

/* --- claims ----------------------------------------------------------------- */

static void release(int s, const char *client)
{
	gpointer watch;

	if (!g_hash_table_lookup_extended(sensors[s].clients, client, NULL, &watch))
		return;
	g_bus_unwatch_name(GPOINTER_TO_UINT(watch));
	g_hash_table_remove(sensors[s].clients, client);
	if (!g_hash_table_size(sensors[s].clients))
		hf_enable(s, false);
}

static void client_vanished(GDBusConnection *c, const char *name, gpointer data)
{
	release(GPOINTER_TO_INT(data), name);
}

static void claim(int s, const char *client)
{
	guint watch;

	if (g_hash_table_contains(sensors[s].clients, client))
		return;
	watch = g_bus_watch_name_on_connection(bus, client, G_BUS_NAME_WATCHER_FLAGS_NONE,
					       NULL, client_vanished, GINT_TO_POINTER(s), NULL);
	g_hash_table_insert(sensors[s].clients, g_strdup(client), GUINT_TO_POINTER(watch));
	if (g_hash_table_size(sensors[s].clients) == 1)
		hf_enable(s, true);
}

static void method_call(GDBusConnection *c, const char *sender, const char *path,
			const char *iface, const char *method, GVariant *params,
			GDBusMethodInvocation *inv, gpointer data)
{
	for (int s = 0; s < NSENSORS; s++) {
		if (!strcmp(method, sensor_defs[s].claim) || !strcmp(method, sensor_defs[s].release)) {
			if (!sensors[s].present) {
				g_dbus_method_invocation_return_dbus_error(inv, BUS_NAME ".Error",
									   "No such sensor");
				return;
			}
			if (!strcmp(method, sensor_defs[s].claim))
				claim(s, sender);
			else
				release(s, sender);
			g_dbus_method_invocation_return_value(inv, NULL);
			return;
		}
	}
	g_dbus_method_invocation_return_dbus_error(inv, "org.freedesktop.DBus.Error.UnknownMethod",
						   method);
}

static GVariant *get_property(GDBusConnection *c, const char *sender, const char *path,
			      const char *iface, const char *prop, GError **err, gpointer data)
{
	for (int s = 0; s < NSENSORS; s++)
		if (!strcmp(prop, sensor_defs[s].has))
			return g_variant_new_boolean(sensors[s].present);
	if (!strcmp(prop, "AccelerometerOrientation"))
		return g_variant_new_string(orientation);
	if (!strcmp(prop, "AccelerometerTilt"))
		return g_variant_new_string(tilt);
	if (!strcmp(prop, "LightLevelUnit"))
		return g_variant_new_string("lux");
	if (!strcmp(prop, "LightLevel"))
		return g_variant_new_double(light_level);
	if (!strcmp(prop, "ProximityNear"))
		return g_variant_new_boolean(prox_near);
	return NULL;
}

static const GDBusInterfaceVTable vtable = { method_call, get_property, NULL };

static void compass_call(GDBusConnection *c, const char *sender, const char *path,
			 const char *iface, const char *method, GVariant *params,
			 GDBusMethodInvocation *inv, gpointer data)
{
	g_dbus_method_invocation_return_dbus_error(inv, BUS_NAME ".Error", "No compass");
}

static GVariant *compass_property(GDBusConnection *c, const char *sender, const char *path,
				  const char *iface, const char *prop, GError **err, gpointer data)
{
	return !strcmp(prop, "HasCompass") ? g_variant_new_boolean(FALSE) : g_variant_new_double(-1);
}

static const GDBusInterfaceVTable compass_vtable = { compass_call, compass_property, NULL };

static void on_bus(GDBusConnection *c, const char *name, gpointer data)
{
	GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(introspection, NULL);

	bus = c;
	g_dbus_connection_register_object(c, OBJ_PATH, info->interfaces[0], &vtable,
					  NULL, NULL, NULL);
	g_dbus_node_info_unref(info);
	info = g_dbus_node_info_new_for_xml(compass_introspection, NULL);
	g_dbus_connection_register_object(c, OBJ_PATH "/Compass", info->interfaces[0],
					  &compass_vtable, NULL, NULL, NULL);
	g_dbus_node_info_unref(info);
}

static void name_lost(GDBusConnection *c, const char *name, gpointer data)
{
	g_printerr("tetris-sensors: lost the bus name %s\n", name);
	exit(1);
}

int main(void)
{
	GMainLoop *loop = g_main_loop_new(NULL, FALSE);

	hf_fd = open("/dev/hf_manager", O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (hf_fd < 0) {
		g_printerr("tetris-sensors: /dev/hf_manager: %s\n", g_strerror(errno));
		return 1;
	}
	probe_sensors();
	g_unix_fd_add(hf_fd, G_IO_IN, on_hf_event, NULL);
	g_bus_own_name(G_BUS_TYPE_SYSTEM, BUS_NAME, G_BUS_NAME_OWNER_FLAGS_NONE,
		       on_bus, NULL, name_lost, NULL, NULL);
	g_main_loop_run(loop);
	return 0;
}
