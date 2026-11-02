/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MM_BROADBAND_MODEM_TETRIS_H
#define MM_BROADBAND_MODEM_TETRIS_H

#include "mm-broadband-modem.h"

#define MM_TYPE_BROADBAND_MODEM_TETRIS (mm_broadband_modem_tetris_get_type ())
G_DECLARE_FINAL_TYPE (MMBroadbandModemTetris, mm_broadband_modem_tetris, MM, BROADBAND_MODEM_TETRIS, MMBroadbandModem)

MMBroadbandModemTetris *mm_broadband_modem_tetris_new (const gchar *device,
                                                       const gchar *physdev,
                                                       const gchar **drivers,
                                                       const gchar *plugin,
                                                       guint16 vendor_id,
                                                       guint16 product_id);
#endif
