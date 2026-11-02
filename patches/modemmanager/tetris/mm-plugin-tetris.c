/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * CMF Phone 1 (MT6878) modem. AT through the tetris_mdtty tty pair, data on the
 * ccmni network interfaces, activated with MTK's own AT commands (see the bearer).
 */
#include <config.h>
#include <gmodule.h>

#define _LIBMM_INSIDE_MM
#include <libmm-glib.h>

#include "mm-log.h"
#include "mm-plugin-common.h"
#include "mm-broadband-modem-tetris.h"

#define MM_TYPE_PLUGIN_TETRIS mm_plugin_tetris_get_type ()
MM_DEFINE_PLUGIN (TETRIS, tetris, Tetris)

static MMBaseModem *
create_modem (MMPlugin *self,
              const gchar *uid,
              const gchar *physdev,
              const gchar **drivers,
              guint16 vendor,
              guint16 product,
              guint16 subsystem_vendor,
              guint16 subsystem_device,
              GList *probes,
              GError **error)
{
    return MM_BASE_MODEM (mm_broadband_modem_tetris_new (uid, physdev, drivers,
                                                         mm_plugin_get_name (self),
                                                         vendor, product));
}

MM_PLUGIN_NAMED_CREATOR_SCOPE MMPlugin *
mm_plugin_create_tetris (void)
{
    static const gchar *subsystems[] = { "tty", "net", NULL };
    static const gchar *drivers[] = { "tetris_mdtty", NULL };

    return MM_PLUGIN (
        g_object_new (MM_TYPE_PLUGIN_TETRIS,
                      MM_PLUGIN_NAME,               MM_MODULE_NAME,
                      MM_PLUGIN_ALLOWED_SUBSYSTEMS, subsystems,
                      MM_PLUGIN_ALLOWED_DRIVERS,    drivers,
                      MM_PLUGIN_ALLOWED_AT,         TRUE,
                      NULL));
}

static void
mm_plugin_tetris_init (MMPluginTetris *self)
{
}

static void
mm_plugin_tetris_class_init (MMPluginTetrisClass *klass)
{
    MM_PLUGIN_CLASS (klass)->create_modem = create_modem;
}
