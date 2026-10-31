/*
 * Black Arrow menu video hook (target/i386/tcg/xbl_hle.c) and the GPU code
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef HW_XBOX_XBL_YUV_H
#define HW_XBOX_XBL_YUV_H

/*
 * Bytes in the row the video hook is about to store, while it checks that it
 * can write it (0 the rest of the time). A row that starts or ends a render
 * target and is as wide as it means the video replaces that whole picture.
 */
extern __thread uint32_t xbl_yuv_row_bytes;

#endif
