/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>

#define _LIBMM_INSIDE_MM
#include <libmm-glib.h>

#include "mm-base-modem.h"
#include "mm-base-modem-at.h"
#include "mm-iface-modem.h"
#include "mm-iface-modem-messaging.h"
#include "mm-log-object.h"
#include "mm-broadband-modem-tetris.h"
#include "mm-broadband-bearer-tetris.h"

struct _MMBroadbandModemTetris {
    MMBroadbandModem parent;
};

static void iface_modem_init (MMIfaceModemInterface *iface);
static void iface_modem_messaging_init (MMIfaceModemMessagingInterface *iface);

static MMIfaceModemMessagingInterface *iface_modem_messaging_parent;

G_DEFINE_TYPE_EXTENDED (MMBroadbandModemTetris, mm_broadband_modem_tetris, MM_TYPE_BROADBAND_MODEM, 0,
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM, iface_modem_init)
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM_MESSAGING, iface_modem_messaging_init))

static MMBaseBearer *
modem_create_bearer_finish (MMIfaceModem *self,
                            GAsyncResult *res,
                            GError **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

static void
bearer_new_ready (GObject *source,
                  GAsyncResult *res,
                  GTask *task)
{
    GError *error = NULL;
    MMBaseBearer *bearer;

    bearer = mm_broadband_bearer_tetris_new_finish (res, &error);
    if (!bearer)
        g_task_return_error (task, error);
    else
        g_task_return_pointer (task, bearer, g_object_unref);
    g_object_unref (task);
}

static void
modem_create_bearer (MMIfaceModem *self,
                     MMBearerProperties *properties,
                     GAsyncReadyCallback callback,
                     gpointer user_data)
{
    mm_broadband_bearer_tetris_new (MM_BROADBAND_MODEM (self),
                                    properties,
                                    NULL,
                                    (GAsyncReadyCallback)bearer_new_ready,
                                    g_task_new (self, NULL, callback, user_data));
}

/*****************************************************************************/
/* Supported SMS storages: right after the PIN the modem answers +CPMS=? with
 * +CME ERROR: 100 until its SIM SMS init is done, and MM never asks again, so
 * messaging would stay without storages. Retry for a while. */

#define CPMS_RETRIES        20
#define CPMS_RETRY_SECONDS  3

typedef struct {
    GArray *mem1;
    GArray *mem2;
    GArray *mem3;
} StoragesResult;

static void
storages_result_free (StoragesResult *result)
{
    g_clear_pointer (&result->mem1, g_array_unref);
    g_clear_pointer (&result->mem2, g_array_unref);
    g_clear_pointer (&result->mem3, g_array_unref);
    g_free (result);
}

static gboolean
messaging_load_supported_storages_finish (MMIfaceModemMessaging *self,
                                          GAsyncResult *res,
                                          GArray **mem1,
                                          GArray **mem2,
                                          GArray **mem3,
                                          GError **error)
{
    StoragesResult *result;

    result = g_task_propagate_pointer (G_TASK (res), error);
    if (!result)
        return FALSE;
    *mem1 = g_steal_pointer (&result->mem1);
    *mem2 = g_steal_pointer (&result->mem2);
    *mem3 = g_steal_pointer (&result->mem3);
    storages_result_free (result);
    return TRUE;
}

static void parent_load_supported_storages (GTask *task);

static gboolean
retry_load_supported_storages (GTask *task)
{
    parent_load_supported_storages (task);
    return G_SOURCE_REMOVE;
}

static void
parent_load_supported_storages_ready (MMIfaceModemMessaging *self,
                                      GAsyncResult *res,
                                      GTask *task)
{
    GError *error = NULL;
    StoragesResult *result;
    guint attempt;

    result = g_new0 (StoragesResult, 1);
    if (iface_modem_messaging_parent->load_supported_storages_finish (self, res,
                                                                      &result->mem1,
                                                                      &result->mem2,
                                                                      &result->mem3,
                                                                      &error)) {
        g_task_return_pointer (task, result, (GDestroyNotify)storages_result_free);
        g_object_unref (task);
        return;
    }
    storages_result_free (result);

    attempt = GPOINTER_TO_UINT (g_task_get_task_data (task)) + 1;
    if (attempt >= CPMS_RETRIES) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    mm_obj_dbg (self, "couldn't load supported SMS storages (%s), retrying in %us",
                error->message, CPMS_RETRY_SECONDS);
    g_error_free (error);
    g_task_set_task_data (task, GUINT_TO_POINTER (attempt), NULL);
    g_timeout_add_seconds (CPMS_RETRY_SECONDS, (GSourceFunc)retry_load_supported_storages, task);
}

static void
parent_load_supported_storages (GTask *task)
{
    iface_modem_messaging_parent->load_supported_storages (
        MM_IFACE_MODEM_MESSAGING (g_task_get_source_object (task)),
        (GAsyncReadyCallback)parent_load_supported_storages_ready,
        task);
}

static void
messaging_load_supported_storages (MMIfaceModemMessaging *self,
                                   GAsyncReadyCallback callback,
                                   gpointer user_data)
{
    parent_load_supported_storages (g_task_new (self, NULL, callback, user_data));
}

/*****************************************************************************/
/* Default SMS storage: the modem answers +CPMS? with spaces after the commas,
 * which MM's parser rejects, so MM never learns the current mem1 and refuses
 * to set a default storage. Set all three memories directly instead. */

static gboolean
messaging_set_default_storage_finish (MMIfaceModemMessaging *self,
                                      GAsyncResult *res,
                                      GError **error)
{
    return !!mm_base_modem_at_command_finish (MM_BASE_MODEM (self), res, error);
}

static void
messaging_set_default_storage (MMIfaceModemMessaging *self,
                               MMSmsStorage storage,
                               GAsyncReadyCallback callback,
                               gpointer user_data)
{
    g_autofree gchar *mem = NULL;
    g_autofree gchar *cmd = NULL;

    mem = g_ascii_strup (mm_sms_storage_get_string (storage), -1);
    cmd = g_strdup_printf ("+CPMS=\"%s\",\"%s\",\"%s\"", mem, mem, mem);
    mm_base_modem_at_command (MM_BASE_MODEM (self), cmd, 3, FALSE, callback, user_data);
}

/*****************************************************************************/

MMBroadbandModemTetris *
mm_broadband_modem_tetris_new (const gchar *device,
                               const gchar *physdev,
                               const gchar **drivers,
                               const gchar *plugin,
                               guint16 vendor_id,
                               guint16 product_id)
{
    return g_object_new (MM_TYPE_BROADBAND_MODEM_TETRIS,
                         MM_BASE_MODEM_DEVICE, device,
                         MM_BASE_MODEM_PHYSDEV, physdev,
                         MM_BASE_MODEM_DRIVERS, drivers,
                         MM_BASE_MODEM_PLUGIN, plugin,
                         MM_BASE_MODEM_VENDOR_ID, vendor_id,
                         MM_BASE_MODEM_PRODUCT_ID, product_id,
                         MM_BASE_MODEM_DATA_NET_SUPPORTED, TRUE,
                         MM_BASE_MODEM_DATA_TTY_SUPPORTED, FALSE,
                         NULL);
}

static void
mm_broadband_modem_tetris_init (MMBroadbandModemTetris *self)
{
}

static void
iface_modem_init (MMIfaceModemInterface *iface)
{
    iface->create_bearer = modem_create_bearer;
    iface->create_bearer_finish = modem_create_bearer_finish;
}

static void
iface_modem_messaging_init (MMIfaceModemMessagingInterface *iface)
{
    iface_modem_messaging_parent = g_type_interface_peek_parent (iface);

    iface->load_supported_storages = messaging_load_supported_storages;
    iface->load_supported_storages_finish = messaging_load_supported_storages_finish;
    iface->set_default_storage = messaging_set_default_storage;
    iface->set_default_storage_finish = messaging_set_default_storage_finish;
}

static void
mm_broadband_modem_tetris_class_init (MMBroadbandModemTetrisClass *klass)
{
}
