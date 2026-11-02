/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * MTK data call without the Android RIL, as found on the CMF Phone 1:
 *   AT+EAPNACT=1,"<apn>","IPV4V6",0         activate; the PDN id comes in a URC
 *   AT+EPDN=<id>,"addr"                     +EPDN: <id>, "addr", 1, "<ipv4>"
 *   AT+CGCONTRDP=<id>                       DNS servers (the local address is empty)
 *   AT+EAPNACT=0,<id>,0                     deactivate
 * The modem's interface 101 is ccmni1. Never send AT+CGDATA="M-CCMNI" here: it hangs
 * the AT channel until the next reboot.
 *
 * IPv4 only for now; the modem also assigns IPv6 ("addr", 5).
 */
#include <config.h>
#include <stdio.h>
#include <string.h>

#define _LIBMM_INSIDE_MM
#include <libmm-glib.h>

#include "mm-base-modem-at.h"
#include "mm-broadband-bearer-tetris.h"
#include "mm-log-object.h"
#include "mm-modem-helpers.h"

#define MAX_PDN_ID 3

struct _MMBroadbandBearerTetris {
    MMBroadbandBearer parent;
    gint pdn_id;
};

G_DEFINE_TYPE (MMBroadbandBearerTetris, mm_broadband_bearer_tetris, MM_TYPE_BROADBAND_BEARER)

typedef struct {
    MMBaseModem *modem;
    MMPortSerialAt *primary;
    MMPort *data;
    gint pdn_id;
    gchar *address;
} ConnectContext;

static void
connect_context_free (ConnectContext *ctx)
{
    g_free (ctx->address);
    g_object_unref (ctx->data);
    g_object_unref (ctx->primary);
    g_object_unref (ctx->modem);
    g_free (ctx);
}

static MMBearerConnectResult *
connect_3gpp_finish (MMBroadbandBearer *self,
                     GAsyncResult *res,
                     GError **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

static void query_pdn_address (GTask *task);

static void
dns_ready (MMBaseModem *modem,
           GAsyncResult *res,
           GTask *task)
{
    MMBroadbandBearerTetris *self = g_task_get_source_object (task);
    ConnectContext *ctx = g_task_get_task_data (task);
    const gchar *response;
    g_autoptr(GRegex) regex = NULL;
    g_autoptr(GMatchInfo) match = NULL;
    g_autoptr(MMBearerIpConfig) config = NULL;
    g_autoptr(GPtrArray) dns = g_ptr_array_new_with_free_func (g_free);

    /* Quoted dotted quads with exactly 4 parts: the IPv6 row has 16 */
    response = mm_base_modem_at_command_full_finish (modem, res, NULL);
    regex = g_regex_new ("\"(\\d+\\.\\d+\\.\\d+\\.\\d+)\"", 0, 0, NULL);
    if (response && g_regex_match (regex, response, 0, &match)) {
        while (g_match_info_matches (match) && dns->len < 2) {
            g_ptr_array_add (dns, g_match_info_fetch (match, 1));
            g_match_info_next (match, NULL);
        }
    }
    g_ptr_array_add (dns, NULL);

    config = mm_bearer_ip_config_new ();
    mm_bearer_ip_config_set_method (config, MM_BEARER_IP_METHOD_STATIC);
    mm_bearer_ip_config_set_address (config, ctx->address);
    mm_bearer_ip_config_set_prefix (config, 32);
    mm_bearer_ip_config_set_dns (config, (const gchar **)dns->pdata);

    self->pdn_id = ctx->pdn_id;
    mm_obj_info (self, "PDN %d active on %s", ctx->pdn_id, mm_port_get_device (ctx->data));
    g_task_return_pointer (task,
                           mm_bearer_connect_result_new (ctx->data, config, NULL),
                           (GDestroyNotify)mm_bearer_connect_result_unref);
    g_object_unref (task);
}

static void
address_ready (MMBaseModem *modem,
               GAsyncResult *res,
               GTask *task)
{
    ConnectContext *ctx = g_task_get_task_data (task);
    const gchar *response;
    g_autoptr(GRegex) regex = NULL;
    g_autoptr(GMatchInfo) match = NULL;
    g_autofree gchar *command = NULL;

    /* An unused PDN id answers with an error: try the next one */
    response = mm_base_modem_at_command_full_finish (modem, res, NULL);
    regex = g_regex_new ("\"addr\",\\s*1,\\s*\"(\\d+\\.\\d+\\.\\d+\\.\\d+)\"", 0, 0, NULL);
    if (!response || !g_regex_match (regex, response, 0, &match)) {
        ctx->pdn_id++;
        query_pdn_address (task);
        return;
    }

    ctx->address = g_match_info_fetch (match, 1);
    command = g_strdup_printf ("+CGCONTRDP=%d", ctx->pdn_id);
    mm_base_modem_at_command_full (modem, MM_IFACE_PORT_AT (ctx->primary), command,
                                   5, FALSE, FALSE, g_task_get_cancellable (task),
                                   (GAsyncReadyCallback)dns_ready, task);
}

static void
query_pdn_address (GTask *task)
{
    ConnectContext *ctx = g_task_get_task_data (task);
    g_autofree gchar *command = NULL;

    if (ctx->pdn_id > MAX_PDN_ID) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                 "No active PDN with an IPv4 address");
        g_object_unref (task);
        return;
    }
    command = g_strdup_printf ("+EPDN=%d,\"addr\"", ctx->pdn_id);
    mm_base_modem_at_command_full (ctx->modem, MM_IFACE_PORT_AT (ctx->primary), command,
                                   5, FALSE, FALSE, g_task_get_cancellable (task),
                                   (GAsyncReadyCallback)address_ready, task);
}

static void
activate_ready (MMBaseModem *modem,
                GAsyncResult *res,
                GTask *task)
{
    GError *error = NULL;

    /* Not fatal: the PDN may still be up from a previous connection, which the
     * address query finds out */
    if (!mm_base_modem_at_command_full_finish (modem, res, &error)) {
        mm_obj_dbg (g_task_get_source_object (task), "activation: %s", error->message);
        g_error_free (error);
    }
    query_pdn_address (task);
}

static void
connect_3gpp (MMBroadbandBearer *self,
              MMBroadbandModem *modem,
              MMPortSerialAt *primary,
              MMPortSerialAt *secondary,
              GCancellable *cancellable,
              GAsyncReadyCallback callback,
              gpointer user_data)
{
    ConnectContext *ctx;
    GTask *task;
    MMPort *data;
    const gchar *apn;
    g_autofree gchar *quoted = NULL;
    g_autofree gchar *command = NULL;

    data = mm_base_modem_peek_best_data_port (MM_BASE_MODEM (modem), MM_PORT_TYPE_NET);
    if (!data) {
        g_task_report_new_error (self, callback, user_data, connect_3gpp,
                                 MM_CORE_ERROR, MM_CORE_ERROR_NOT_FOUND,
                                 "No net port available");
        return;
    }

    ctx = g_new0 (ConnectContext, 1);
    ctx->modem = MM_BASE_MODEM (g_object_ref (modem));
    ctx->primary = g_object_ref (primary);
    ctx->data = g_object_ref (data);

    task = g_task_new (self, cancellable, callback, user_data);
    g_task_set_task_data (task, ctx, (GDestroyNotify)connect_context_free);

    apn = mm_bearer_properties_get_apn (mm_base_bearer_peek_config (MM_BASE_BEARER (self)));
    quoted = mm_at_quote_string (apn ? apn : "");
    command = g_strdup_printf ("+EAPNACT=1,%s,\"IPV4V6\",0", quoted);
    mm_base_modem_at_command_full (ctx->modem, MM_IFACE_PORT_AT (primary), command,
                                   60, FALSE, FALSE, cancellable,
                                   (GAsyncReadyCallback)activate_ready, task);
}

static gboolean
disconnect_3gpp_finish (MMBroadbandBearer *self,
                        GAsyncResult *res,
                        GError **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
deactivate_ready (MMBaseModem *modem,
                  GAsyncResult *res,
                  GTask *task)
{
    GError *error = NULL;

    if (!mm_base_modem_at_command_full_finish (modem, res, &error)) {
        mm_obj_warn (g_task_get_source_object (task), "deactivation: %s", error->message);
        g_error_free (error);
    }
    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
disconnect_3gpp (MMBroadbandBearer *_self,
                 MMBroadbandModem *modem,
                 MMPortSerialAt *primary,
                 MMPortSerialAt *secondary,
                 MMPort *data,
                 guint cid,
                 GAsyncReadyCallback callback,
                 gpointer user_data)
{
    MMBroadbandBearerTetris *self = MM_BROADBAND_BEARER_TETRIS (_self);
    GTask *task = g_task_new (self, NULL, callback, user_data);
    g_autofree gchar *command = g_strdup_printf ("+EAPNACT=0,%d,0", self->pdn_id);

    mm_base_modem_at_command_full (MM_BASE_MODEM (modem), MM_IFACE_PORT_AT (primary), command,
                                   30, FALSE, FALSE, NULL,
                                   (GAsyncReadyCallback)deactivate_ready, task);
}

MMBaseBearer *
mm_broadband_bearer_tetris_new_finish (GAsyncResult *res,
                                       GError **error)
{
    g_autoptr(GObject) source = g_async_result_get_source_object (res);
    GObject *bearer = g_async_initable_new_finish (G_ASYNC_INITABLE (source), res, error);

    if (!bearer)
        return NULL;
    mm_base_bearer_export (MM_BASE_BEARER (bearer));
    return MM_BASE_BEARER (bearer);
}

void
mm_broadband_bearer_tetris_new (MMBroadbandModem *modem,
                                MMBearerProperties *config,
                                GCancellable *cancellable,
                                GAsyncReadyCallback callback,
                                gpointer user_data)
{
    g_async_initable_new_async (MM_TYPE_BROADBAND_BEARER_TETRIS,
                                G_PRIORITY_DEFAULT,
                                cancellable,
                                callback,
                                user_data,
                                MM_BASE_BEARER_MODEM, modem,
                                MM_BASE_BEARER_CONFIG, config,
                                NULL);
}

static void
mm_broadband_bearer_tetris_init (MMBroadbandBearerTetris *self)
{
}

static void
mm_broadband_bearer_tetris_class_init (MMBroadbandBearerTetrisClass *klass)
{
    MMBaseBearerClass *base_bearer_class = MM_BASE_BEARER_CLASS (klass);
    MMBroadbandBearerClass *broadband_bearer_class = MM_BROADBAND_BEARER_CLASS (klass);

    /* Our PDN ids aren't CIDs: the generic +CGACT? based status checks don't apply */
    base_bearer_class->load_connection_status = NULL;
    base_bearer_class->load_connection_status_finish = NULL;
#if defined WITH_SUSPEND_RESUME
    base_bearer_class->reload_connection_status = NULL;
    base_bearer_class->reload_connection_status_finish = NULL;
#endif
    broadband_bearer_class->connect_3gpp = connect_3gpp;
    broadband_bearer_class->connect_3gpp_finish = connect_3gpp_finish;
    broadband_bearer_class->disconnect_3gpp = disconnect_3gpp;
    broadband_bearer_class->disconnect_3gpp_finish = disconnect_3gpp_finish;
}
