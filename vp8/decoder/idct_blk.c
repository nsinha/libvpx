/*
 *  Copyright (c) 2010 The WebM project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "vpx_config.h"
#include "vp8/common/idct.h"
#include "dequantize.h"

void vp8_dequant_dc_idct_add_c(short *input, short *dq,
                               unsigned char *dest, int stride,
                               int Dc);
void vp8_dequant_idct_add_c(short *input, short *dq,
                            unsigned char *dest, int stride);
void vp8_dc_only_idct_add_c(short input_dc, unsigned char * pred,
                            int pred_stride, unsigned char *dst_ptr,
                            int dst_stride);

void vp8_dequant_dc_idct_add_y_block_c
            (short *q, short *dq,
             unsigned char *dst, int stride, char *eobs, short *dc)
{
    int i, j;

    for (i = 0; i < 4; i++)
    {
        for (j = 0; j < 4; j++)
        {
            if (*eobs++ > 1)
                vp8_dequant_dc_idct_add_c (q, dq, dst, stride, dc[0]);
            else
                vp8_dc_only_idct_add_c (dc[0], dst, stride, dst, stride);

            q   += 16;
            dst += 4;
            dc  ++;
        }

        dst += 4*stride - 16;
    }
}

void vp8_dequant_idct_add_y_block_c
            (short *q, short *dq,
             unsigned char *dst, int stride, char *eobs)
{
    int i, j;

    for (i = 0; i < 4; i++)
    {
        for (j = 0; j < 4; j++)
        {
            if (*eobs++ > 1)
                vp8_dequant_idct_add_c (q, dq, dst, stride);
            else
            {
                vp8_dc_only_idct_add_c (q[0]*dq[0], dst, stride, dst, stride);
                ((int *)q)[0] = 0;
            }

            q   += 16;
            dst += 4;
        }

        dst += 4*stride - 16;
    }
}

void vp8_dequant_idct_add_uv_block_c
            (short *q, short *dq,
             unsigned char *dstu, unsigned char *dstv, int stride, char *eobs)
{
    int i, j;

    for (i = 0; i < 2; i++)
    {
        for (j = 0; j < 2; j++)
        {
            if (*eobs++ > 1)
                vp8_dequant_idct_add_c (q, dq, dstu, stride);
            else
            {
                vp8_dc_only_idct_add_c (q[0]*dq[0], dstu, stride, dstu, stride);
                ((int *)q)[0] = 0;
            }

            q    += 16;
            dstu += 4;
        }

        dstu += 4*stride - 8;
    }

    for (i = 0; i < 2; i++)
    {
        for (j = 0; j < 2; j++)
        {
            if (*eobs++ > 1)
                vp8_dequant_idct_add_c (q, dq, dstv, stride);
            else
            {
                vp8_dc_only_idct_add_c (q[0]*dq[0], dstv, stride, dstv, stride);
                ((int *)q)[0] = 0;
            }

            q    += 16;
            dstv += 4;
        }

        dstv += 4*stride - 8;
    }
}
