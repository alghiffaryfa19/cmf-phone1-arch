/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MM_BROADBAND_BEARER_TETRIS_H
#define MM_BROADBAND_BEARER_TETRIS_H

#include "mm-broadband-bearer.h"
#include "mm-broadband-modem.h"

#define MM_TYPE_BROADBAND_BEARER_TETRIS (mm_broadband_bearer_tetris_get_type ())
G_DECLARE_FINAL_TYPE (MMBroadbandBearerTetris, mm_broadband_bearer_tetris, MM, BROADBAND_BEARER_TETRIS, MMBroadbandBearer)

void mm_broadband_bearer_tetris_new (MMBroadbandModem *modem,
                                     MMBearerProperties *config,
                                     GCancellable *cancellable,
                                     GAsyncReadyCallback callback,
                                     gpointer user_data);
MMBaseBearer *mm_broadband_bearer_tetris_new_finish (GAsyncResult *res,
                                                     GError **error);
#endif
