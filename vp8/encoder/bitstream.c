/*
 *  Copyright (c) 2010 The WebM project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */


#include "vp8/common/header.h"
#include "encodemv.h"
#include "vp8/common/entropymode.h"
#include "vp8/common/findnearmv.h"
#include "mcomp.h"
#include "vp8/common/systemdependent.h"
#include <assert.h>
#include <stdio.h>
#include <limits.h>
#include "vp8/common/pragmas.h"
#include "vpx/vpx_encoder.h"
#include "vpx_mem/vpx_mem.h"
#include "bitstream.h"

#include "defaultcoefcounts.h"

const int vp8cx_base_skip_false_prob[128] =
{
    255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255,
    251, 248, 244, 240, 236, 232, 229, 225,
    221, 217, 213, 208, 204, 199, 194, 190,
    187, 183, 179, 175, 172, 168, 164, 160,
    157, 153, 149, 145, 142, 138, 134, 130,
    127, 124, 120, 117, 114, 110, 107, 104,
    101, 98,  95,  92,  89,  86,  83, 80,
    77,  74,  71,  68,  65,  62,  59, 56,
    53,  50,  47,  44,  41,  38,  35, 32,
    30,  28,  26,  24,  22,  20,  18, 16,
};

#if defined(SECTIONBITS_OUTPUT)
unsigned __int64 Sectionbits[500];
#endif

#ifdef ENTROPY_STATS
int intra_mode_stats[10][10][10];
static unsigned int tree_update_hist [BLOCK_TYPES] [COEF_BANDS] [PREV_COEF_CONTEXTS] [ENTROPY_NODES] [2];
extern unsigned int active_section;
#endif

#ifdef MODE_STATS
int count_mb_seg[4] = { 0, 0, 0, 0 };
#endif


static void update_mode(
    vp8_writer *const w,
    int n,
    vp8_token tok               [/* n */],
    vp8_tree tree,
    vp8_prob Pnew               [/* n-1 */],
    vp8_prob Pcur               [/* n-1 */],
    unsigned int bct            [/* n-1 */] [2],
    const unsigned int num_events[/* n */]
)
{
    unsigned int new_b = 0, old_b = 0;
    int i = 0;

    vp8_tree_probs_from_distribution(
        n--, tok, tree,
        Pnew, bct, num_events,
        256, 1
    );

    do
    {
        new_b += vp8_cost_branch(bct[i], Pnew[i]);
        old_b += vp8_cost_branch(bct[i], Pcur[i]);
    }
    while (++i < n);

    if (new_b + (n << 8) < old_b)
    {
        int i = 0;

        vp8_write_bit(w, 1);

        do
        {
            const vp8_prob p = Pnew[i];

            vp8_write_literal(w, Pcur[i] = p ? p : 1, 8);
        }
        while (++i < n);
    }
    else
        vp8_write_bit(w, 0);
}

static void update_mbintra_mode_probs(VP8_COMP *cpi)
{
    VP8_COMMON *const x = & cpi->common;

    vp8_writer *const w = & cpi->bc;

    {
        vp8_prob Pnew   [VP8_YMODES-1];
        unsigned int bct [VP8_YMODES-1] [2];

        update_mode(
            w, VP8_YMODES, vp8_ymode_encodings, vp8_ymode_tree,
            Pnew, x->fc.ymode_prob, bct, (unsigned int *)cpi->ymode_count
        );
    }
    {
        vp8_prob Pnew   [VP8_UV_MODES-1];
        unsigned int bct [VP8_UV_MODES-1] [2];

        update_mode(
            w, VP8_UV_MODES, vp8_uv_mode_encodings, vp8_uv_mode_tree,
            Pnew, x->fc.uv_mode_prob, bct, (unsigned int *)cpi->uv_mode_count
        );
    }
}

static void write_ymode(vp8_writer *bc, int m, const vp8_prob *p)
{
    vp8_write_token(bc, vp8_ymode_tree, p, vp8_ymode_encodings + m);
}

static void kfwrite_ymode(vp8_writer *bc, int m, const vp8_prob *p)
{
    vp8_write_token(bc, vp8_kf_ymode_tree, p, vp8_kf_ymode_encodings + m);
}

static void write_uv_mode(vp8_writer *bc, int m, const vp8_prob *p)
{
    vp8_write_token(bc, vp8_uv_mode_tree, p, vp8_uv_mode_encodings + m);
}


static void write_bmode(vp8_writer *bc, int m, const vp8_prob *p)
{
    vp8_write_token(bc, vp8_bmode_tree, p, vp8_bmode_encodings + m);
}

static void write_split(vp8_writer *bc, int x)
{
    vp8_write_token(
        bc, vp8_mbsplit_tree, vp8_mbsplit_probs, vp8_mbsplit_encodings + x
    );
}

static void pack_tokens_c(vp8_writer *w, const TOKENEXTRA *p, int xcount)
{
    const TOKENEXTRA *const stop = p + xcount;
    unsigned int split;
    unsigned int shift;
    int count = w->count;
    unsigned int range = w->range;
    unsigned int lowvalue = w->lowvalue;

    while (p < stop)
    {
        const int t = p->Token;
        vp8_token *const a = vp8_coef_encodings + t;
        const vp8_extra_bit_struct *const b = vp8_extra_bits + t;
        int i = 0;
        const unsigned char *pp = p->context_tree;
        int v = a->value;
        int n = a->Len;

        if (p->skip_eob_node)
        {
            n--;
            i = 2;
        }

        do
        {
            const int bb = (v >> --n) & 1;
            split = 1 + (((range - 1) * pp[i>>1]) >> 8);
            i = vp8_coef_tree[i+bb];

            if (bb)
            {
                lowvalue += split;
                range = range - split;
            }
            else
            {
                range = split;
            }

            shift = vp8_norm[range];
            range <<= shift;
            count += shift;

            if (count >= 0)
            {
                int offset = shift - count;

                if ((lowvalue << (offset - 1)) & 0x80000000)
                {
                    int x = w->pos - 1;

                    while (x >= 0 && w->buffer[x] == 0xff)
                    {
                        w->buffer[x] = (unsigned char)0;
                        x--;
                    }

                    w->buffer[x] += 1;
                }

                w->buffer[w->pos++] = (lowvalue >> (24 - offset));
                lowvalue <<= offset;
                shift = count;
                lowvalue &= 0xffffff;
                count -= 8 ;
            }

            lowvalue <<= shift;
        }
        while (n);


        if (b->base_val)
        {
            const int e = p->Extra, L = b->Len;

            if (L)
            {
                const unsigned char *pp = b->prob;
                int v = e >> 1;
                int n = L;              /* number of bits in v, assumed nonzero */
                int i = 0;

                do
                {
                    const int bb = (v >> --n) & 1;
                    split = 1 + (((range - 1) * pp[i>>1]) >> 8);
                    i = b->tree[i+bb];

                    if (bb)
                    {
                        lowvalue += split;
                        range = range - split;
                    }
                    else
                    {
                        range = split;
                    }

                    shift = vp8_norm[range];
                    range <<= shift;
                    count += shift;

                    if (count >= 0)
                    {
                        int offset = shift - count;

                        if ((lowvalue << (offset - 1)) & 0x80000000)
                        {
                            int x = w->pos - 1;

                            while (x >= 0 && w->buffer[x] == 0xff)
                            {
                                w->buffer[x] = (unsigned char)0;
                                x--;
                            }

                            w->buffer[x] += 1;
                        }

                        w->buffer[w->pos++] = (lowvalue >> (24 - offset));
                        lowvalue <<= offset;
                        shift = count;
                        lowvalue &= 0xffffff;
                        count -= 8 ;
                    }

                    lowvalue <<= shift;
                }
                while (n);
            }


            {

                split = (range + 1) >> 1;

                if (e & 1)
                {
                    lowvalue += split;
                    range = range - split;
                }
                else
                {
                    range = split;
                }

                range <<= 1;

                if ((lowvalue & 0x80000000))
                {
                    int x = w->pos - 1;

                    while (x >= 0 && w->buffer[x] == 0xff)
                    {
                        w->buffer[x] = (unsigned char)0;
                        x--;
                    }

                    w->buffer[x] += 1;

                }

                lowvalue  <<= 1;

                if (!++count)
                {
                    count = -8;
                    w->buffer[w->pos++] = (lowvalue >> 24);
                    lowvalue &= 0xffffff;
                }
            }

        }

        ++p;
    }

    w->count = count;
    w->lowvalue = lowvalue;
    w->range = range;

}

static void write_partition_size(unsigned char *cx_data, int size)
{
    signed char csize;

    csize = size & 0xff;
    *cx_data = csize;
    csize = (size >> 8) & 0xff;
    *(cx_data + 1) = csize;
    csize = (size >> 16) & 0xff;
    *(cx_data + 2) = csize;

}

static void pack_tokens_into_partitions_c(VP8_COMP *cpi, unsigned char *cx_data, unsigned char * cx_data_end, int num_part, int *size)
{

    int i;
    unsigned char *ptr = cx_data;
    unsigned char *ptr_end = cx_data_end;
    unsigned int shift;
    vp8_writer *w = &cpi->bc2;
    *size = 3 * (num_part - 1);
    cpi->partition_sz[0] += *size;
    ptr = cx_data + (*size);

    for (i = 0; i < num_part; i++)
    {
        vp8_start_encode(w, ptr, ptr_end);
        {
            unsigned int split;
            int count = w->count;
            unsigned int range = w->range;
            unsigned int lowvalue = w->lowvalue;
            int mb_row;

            for (mb_row = i; mb_row < cpi->common.mb_rows; mb_row += num_part)
            {
                TOKENEXTRA *p    = cpi->tplist[mb_row].start;
                TOKENEXTRA *stop = cpi->tplist[mb_row].stop;

                while (p < stop)
                {
                    const int t = p->Token;
                    vp8_token *const a = vp8_coef_encodings + t;
                    const vp8_extra_bit_struct *const b = vp8_extra_bits + t;
                    int i = 0;
                    const unsigned char *pp = p->context_tree;
                    int v = a->value;
                    int n = a->Len;

                    if (p->skip_eob_node)
                    {
                        n--;
                        i = 2;
                    }

                    do
                    {
                        const int bb = (v >> --n) & 1;
                        split = 1 + (((range - 1) * pp[i>>1]) >> 8);
                        i = vp8_coef_tree[i+bb];

                        if (bb)
                        {
                            lowvalue += split;
                            range = range - split;
                        }
                        else
                        {
                            range = split;
                        }

                        shift = vp8_norm[range];
                        range <<= shift;
                        count += shift;

                        if (count >= 0)
                        {
                            int offset = shift - count;

                            if ((lowvalue << (offset - 1)) & 0x80000000)
                            {
                                int x = w->pos - 1;

                                while (x >= 0 && w->buffer[x] == 0xff)
                                {
                                    w->buffer[x] = (unsigned char)0;
                                    x--;
                                }

                                w->buffer[x] += 1;
                            }

                            validate_buffer(w->buffer + w->pos,
                                            1,
                                            cx_data_end,
                                            &cpi->common.error);

                            w->buffer[w->pos++] = (lowvalue >> (24 - offset));

                            lowvalue <<= offset;
                            shift = count;
                            lowvalue &= 0xffffff;
                            count -= 8 ;
                        }

                        lowvalue <<= shift;
                    }
                    while (n);


                    if (b->base_val)
                    {
                        const int e = p->Extra, L = b->Len;

                        if (L)
                        {
                            const unsigned char *pp = b->prob;
                            int v = e >> 1;
                            int n = L;              /* number of bits in v, assumed nonzero */
                            int i = 0;

                            do
                            {
                                const int bb = (v >> --n) & 1;
                                split = 1 + (((range - 1) * pp[i>>1]) >> 8);
                                i = b->tree[i+bb];

                                if (bb)
                                {
                                    lowvalue += split;
                                    range = range - split;
                                }
                                else
                                {
                                    range = split;
                                }

                                shift = vp8_norm[range];
                                range <<= shift;
                                count += shift;

                                if (count >= 0)
                                {
                                    int offset = shift - count;

                                    if ((lowvalue << (offset - 1)) & 0x80000000)
                                    {
                                        int x = w->pos - 1;

                                        while (x >= 0 && w->buffer[x] == 0xff)
                                        {
                                            w->buffer[x] = (unsigned char)0;
                                            x--;
                                        }

                                        w->buffer[x] += 1;
                                    }

                                    validate_buffer(w->buffer + w->pos,
                                                    1,
                                                    cx_data_end,
                                                    &cpi->common.error);

                                    w->buffer[w->pos++] =
                                        (lowvalue >> (24 - offset));

                                    lowvalue <<= offset;
                                    shift = count;
                                    lowvalue &= 0xffffff;
                                    count -= 8 ;
                                }

                                lowvalue <<= shift;
                            }
                            while (n);
                        }

                        {
                            split = (range + 1) >> 1;

                            if (e & 1)
                            {
                                lowvalue += split;
                                range = range - split;
                            }
                            else
                            {
                                range = split;
                            }

                            range <<= 1;

                            if ((lowvalue & 0x80000000))
                            {
                                int x = w->pos - 1;

                                while (x >= 0 && w->buffer[x] == 0xff)
                                {
                                    w->buffer[x] = (unsigned char)0;
                                    x--;
                                }

                                w->buffer[x] += 1;

                            }

                            lowvalue  <<= 1;

                            if (!++count)
                            {
                                count = -8;
                                validate_buffer(w->buffer + w->pos,
                                                1,
                                                cx_data_end,
                                                &cpi->common.error);

                                w->buffer[w->pos++] = (lowvalue >> 24);

                                lowvalue &= 0xffffff;
                            }
                        }

                    }

                    ++p;
                }
            }

            w->count    = count;
            w->lowvalue = lowvalue;
            w->range    = range;

        }

        vp8_stop_encode(w);
        *size +=   w->pos;

        /* The first partition size is set earlier */
        cpi->partition_sz[i + 1] = w->pos;

        if (i < (num_part - 1))
        {
            write_partition_size(cx_data, w->pos);
            cx_data += 3;
            ptr += w->pos;
        }
    }
}


static void pack_mb_row_tokens_c(VP8_COMP *cpi, vp8_writer *w)
{

    unsigned int split;
    int count = w->count;
    unsigned int range = w->range;
    unsigned int lowvalue = w->lowvalue;
    unsigned int shift;
    int mb_row;

    for (mb_row = 0; mb_row < cpi->common.mb_rows; mb_row++)
    {
        TOKENEXTRA *p    = cpi->tplist[mb_row].start;
        TOKENEXTRA *stop = cpi->tplist[mb_row].stop;

        while (p < stop)
        {
            const int t = p->Token;
            vp8_token *const a = vp8_coef_encodings + t;
            const vp8_extra_bit_struct *const b = vp8_extra_bits + t;
            int i = 0;
            const unsigned char *pp = p->context_tree;
            int v = a->value;
            int n = a->Len;

            if (p->skip_eob_node)
            {
                n--;
                i = 2;
            }

            do
            {
                const int bb = (v >> --n) & 1;
                split = 1 + (((range - 1) * pp[i>>1]) >> 8);
                i = vp8_coef_tree[i+bb];

                if (bb)
                {
                    lowvalue += split;
                    range = range - split;
                }
                else
                {
                    range = split;
                }

                shift = vp8_norm[range];
                range <<= shift;
                count += shift;

                if (count >= 0)
                {
                    int offset = shift - count;

                    if ((lowvalue << (offset - 1)) & 0x80000000)
                    {
                        int x = w->pos - 1;

                        while (x >= 0 && w->buffer[x] == 0xff)
                        {
                            w->buffer[x] = (unsigned char)0;
                            x--;
                        }

                        w->buffer[x] += 1;
                    }

                    w->buffer[w->pos++] = (lowvalue >> (24 - offset));
                    lowvalue <<= offset;
                    shift = count;
                    lowvalue &= 0xffffff;
                    count -= 8 ;
                }

                lowvalue <<= shift;
            }
            while (n);


            if (b->base_val)
            {
                const int e = p->Extra, L = b->Len;

                if (L)
                {
                    const unsigned char *pp = b->prob;
                    int v = e >> 1;
                    int n = L;              /* number of bits in v, assumed nonzero */
                    int i = 0;

                    do
                    {
                        const int bb = (v >> --n) & 1;
                        split = 1 + (((range - 1) * pp[i>>1]) >> 8);
                        i = b->tree[i+bb];

                        if (bb)
                        {
                            lowvalue += split;
                            range = range - split;
                        }
                        else
                        {
                            range = split;
                        }

                        shift = vp8_norm[range];
                        range <<= shift;
                        count += shift;

                        if (count >= 0)
                        {
                            int offset = shift - count;

                            if ((lowvalue << (offset - 1)) & 0x80000000)
                            {
                                int x = w->pos - 1;

                                while (x >= 0 && w->buffer[x] == 0xff)
                                {
                                    w->buffer[x] = (unsigned char)0;
                                    x--;
                                }

                                w->buffer[x] += 1;
                            }

                            w->buffer[w->pos++] = (lowvalue >> (24 - offset));
                            lowvalue <<= offset;
                            shift = count;
                            lowvalue &= 0xffffff;
                            count -= 8 ;
                        }

                        lowvalue <<= shift;
                    }
                    while (n);
                }

                {
                    split = (range + 1) >> 1;

                    if (e & 1)
                    {
                        lowvalue += split;
                        range = range - split;
                    }
                    else
                    {
                        range = split;
                    }

                    range <<= 1;

                    if ((lowvalue & 0x80000000))
                    {
                        int x = w->pos - 1;

                        while (x >= 0 && w->buffer[x] == 0xff)
                        {
                            w->buffer[x] = (unsigned char)0;
                            x--;
                        }

                        w->buffer[x] += 1;

                    }

                    lowvalue  <<= 1;

                    if (!++count)
                    {
                        count = -8;
                        w->buffer[w->pos++] = (lowvalue >> 24);
                        lowvalue &= 0xffffff;
                    }
                }

            }

            ++p;
        }
    }

    w->count = count;
    w->lowvalue = lowvalue;
    w->range = range;

}

static void write_mv_ref
(
    vp8_writer *w, MB_PREDICTION_MODE m, const vp8_prob *p
)
{
#if CONFIG_DEBUG
    assert(NEARESTMV <= m  &&  m <= SPLITMV);
#endif
    vp8_write_token(w, vp8_mv_ref_tree, p,
                    vp8_mv_ref_encoding_array - NEARESTMV + m);
}

static void write_sub_mv_ref
(
    vp8_writer *w, B_PREDICTION_MODE m, const vp8_prob *p
)
{
#if CONFIG_DEBUG
    assert(LEFT4X4 <= m  &&  m <= NEW4X4);
#endif
    vp8_write_token(w, vp8_sub_mv_ref_tree, p,
                    vp8_sub_mv_ref_encoding_array - LEFT4X4 + m);
}

static void write_mv
(
    vp8_writer *w, const MV *mv, const int_mv *ref, const MV_CONTEXT *mvc
)
{
    MV e;
    e.row = mv->row - ref->as_mv.row;
    e.col = mv->col - ref->as_mv.col;

    vp8_encode_motion_vector(w, &e, mvc);
}

static void write_mb_features(vp8_writer *w, const MB_MODE_INFO *mi, const MACROBLOCKD *x)
{
    // Encode the MB segment id.
    if (x->segmentation_enabled && x->update_mb_segmentation_map)
    {
        switch (mi->segment_id)
        {
        case 0:
            vp8_write(w, 0, x->mb_segment_tree_probs[0]);
            vp8_write(w, 0, x->mb_segment_tree_probs[1]);
            break;
        case 1:
            vp8_write(w, 0, x->mb_segment_tree_probs[0]);
            vp8_write(w, 1, x->mb_segment_tree_probs[1]);
            break;
        case 2:
            vp8_write(w, 1, x->mb_segment_tree_probs[0]);
            vp8_write(w, 0, x->mb_segment_tree_probs[2]);
            break;
        case 3:
            vp8_write(w, 1, x->mb_segment_tree_probs[0]);
            vp8_write(w, 1, x->mb_segment_tree_probs[2]);
            break;

            // TRAP.. This should not happen
        default:
            vp8_write(w, 0, x->mb_segment_tree_probs[0]);
            vp8_write(w, 0, x->mb_segment_tree_probs[1]);
            break;
        }
    }
}


static void pack_inter_mode_mvs(VP8_COMP *const cpi)
{
    VP8_COMMON *const pc = & cpi->common;
    vp8_writer *const w = & cpi->bc;
    const MV_CONTEXT *mvc = pc->fc.mvc;

    const int *const rfct = cpi->count_mb_ref_frame_usage;
    const int rf_intra = rfct[INTRA_FRAME];
    const int rf_inter = rfct[LAST_FRAME] + rfct[GOLDEN_FRAME] + rfct[ALTREF_FRAME];

    MODE_INFO *m = pc->mi, *ms;
    const int mis = pc->mode_info_stride;
    int mb_row = -1;

    int prob_last_coded;
    int prob_gf_coded;
    int prob_skip_false = 0;
    ms = pc->mi - 1;

    cpi->mb.partition_info = cpi->mb.pi;

    // Calculate the probabilities to be used to code the reference frame based on actual useage this frame
    if (!(cpi->prob_intra_coded = rf_intra * 255 / (rf_intra + rf_inter)))
        cpi->prob_intra_coded = 1;

    prob_last_coded = rf_inter ? (rfct[LAST_FRAME] * 255) / rf_inter : 128;

    if (!prob_last_coded)
        prob_last_coded = 1;

    prob_gf_coded = (rfct[GOLDEN_FRAME] + rfct[ALTREF_FRAME])
                    ? (rfct[GOLDEN_FRAME] * 255) / (rfct[GOLDEN_FRAME] + rfct[ALTREF_FRAME]) : 128;

    if (!prob_gf_coded)
        prob_gf_coded = 1;


#ifdef ENTROPY_STATS
    active_section = 1;
#endif

    if (pc->mb_no_coeff_skip)
    {
        prob_skip_false = cpi->skip_false_count * 256 / (cpi->skip_false_count + cpi->skip_true_count);

        if (prob_skip_false <= 1)
            prob_skip_false = 1;

        if (prob_skip_false > 255)
            prob_skip_false = 255;

        cpi->prob_skip_false = prob_skip_false;
        vp8_write_literal(w, prob_skip_false, 8);
    }

    vp8_write_literal(w, cpi->prob_intra_coded, 8);
    vp8_write_literal(w, prob_last_coded, 8);
    vp8_write_literal(w, prob_gf_coded, 8);

    update_mbintra_mode_probs(cpi);

    vp8_write_mvprobs(cpi);

    while (++mb_row < pc->mb_rows)
    {
        int mb_col = -1;

        while (++mb_col < pc->mb_cols)
        {
            const MB_MODE_INFO *const mi = & m->mbmi;
            const MV_REFERENCE_FRAME rf = mi->ref_frame;
            const MB_PREDICTION_MODE mode = mi->mode;

            MACROBLOCKD *xd = &cpi->mb.e_mbd;

            // Distance of Mb to the various image edges.
            // These specified to 8th pel as they are always compared to MV values that are in 1/8th pel units
            xd->mb_to_left_edge = -((mb_col * 16) << 3);
            xd->mb_to_right_edge = ((pc->mb_cols - 1 - mb_col) * 16) << 3;
            xd->mb_to_top_edge = -((mb_row * 16)) << 3;
            xd->mb_to_bottom_edge = ((pc->mb_rows - 1 - mb_row) * 16) << 3;

#ifdef ENTROPY_STATS
            active_section = 9;
#endif

            if (cpi->mb.e_mbd.update_mb_segmentation_map)
                write_mb_features(w, mi, &cpi->mb.e_mbd);

            if (pc->mb_no_coeff_skip)
                vp8_encode_bool(w, m->mbmi.mb_skip_coeff, prob_skip_false);

            if (rf == INTRA_FRAME)
            {
                vp8_write(w, 0, cpi->prob_intra_coded);
#ifdef ENTROPY_STATS
                active_section = 6;
#endif
                write_ymode(w, mode, pc->fc.ymode_prob);

                if (mode == B_PRED)
                {
                    int j = 0;

                    do
                        write_bmode(w, m->bmi[j].as_mode, pc->fc.bmode_prob);
                    while (++j < 16);
                }

                write_uv_mode(w, mi->uv_mode, pc->fc.uv_mode_prob);
            }
            else    /* inter coded */
            {
                int_mv best_mv;
                vp8_prob mv_ref_p [VP8_MVREFS-1];

                vp8_write(w, 1, cpi->prob_intra_coded);

                if (rf == LAST_FRAME)
                    vp8_write(w, 0, prob_last_coded);
                else
                {
                    vp8_write(w, 1, prob_last_coded);
                    vp8_write(w, (rf == GOLDEN_FRAME) ? 0 : 1, prob_gf_coded);
                }

                {
                    int_mv n1, n2;
                    int ct[4];

                    vp8_find_near_mvs(xd, m, &n1, &n2, &best_mv, ct, rf, cpi->common.ref_frame_sign_bias);
                    vp8_mv_ref_probs(mv_ref_p, ct);

#ifdef ENTROPY_STATS
                    accum_mv_refs(mode, ct);
#endif

                }

#ifdef ENTROPY_STATS
                active_section = 3;
#endif

                write_mv_ref(w, mode, mv_ref_p);

                switch (mode)   /* new, split require MVs */
                {
                case NEWMV:

#ifdef ENTROPY_STATS
                    active_section = 5;
#endif

                    write_mv(w, &mi->mv.as_mv, &best_mv, mvc);
                    break;

                case SPLITMV:
                {
                    int j = 0;

#ifdef MODE_STATS
                    ++count_mb_seg [mi->partitioning];
#endif

                    write_split(w, mi->partitioning);

                    do
                    {
                        B_PREDICTION_MODE blockmode;
                        int_mv blockmv;
                        const int *const  L = vp8_mbsplits [mi->partitioning];
                        int k = -1;  /* first block in subset j */
                        int mv_contz;
                        int_mv leftmv, abovemv;

                        blockmode =  cpi->mb.partition_info->bmi[j].mode;
                        blockmv =  cpi->mb.partition_info->bmi[j].mv;
#if CONFIG_DEBUG
                        while (j != L[++k])
                            if (k >= 16)
                                assert(0);
#else
                        while (j != L[++k]);
#endif
                        leftmv.as_int = left_block_mv(m, k);
                        abovemv.as_int = above_block_mv(m, k, mis);
                        mv_contz = vp8_mv_cont(&leftmv, &abovemv);

                        write_sub_mv_ref(w, blockmode, vp8_sub_mv_ref_prob2 [mv_contz]);

                        if (blockmode == NEW4X4)
                        {
#ifdef ENTROPY_STATS
                            active_section = 11;
#endif
                            write_mv(w, &blockmv.as_mv, &best_mv, (const MV_CONTEXT *) mvc);
                        }
                    }
                    while (++j < cpi->mb.partition_info->count);
                }
                break;
                default:
                    break;
                }
            }

            ++m;
            cpi->mb.partition_info++;
        }

        ++m;  /* skip L prediction border */
        cpi->mb.partition_info++;
    }
}


static void write_kfmodes(VP8_COMP *cpi)
{
    vp8_writer *const bc = & cpi->bc;
    const VP8_COMMON *const c = & cpi->common;
    /* const */
    MODE_INFO *m = c->mi;

    int mb_row = -1;
    int prob_skip_false = 0;

    if (c->mb_no_coeff_skip)
    {
        prob_skip_false = cpi->skip_false_count * 256 / (cpi->skip_false_count + cpi->skip_true_count);

        if (prob_skip_false <= 1)
            prob_skip_false = 1;

        if (prob_skip_false >= 255)
            prob_skip_false = 255;

        cpi->prob_skip_false = prob_skip_false;
        vp8_write_literal(bc, prob_skip_false, 8);
    }

    while (++mb_row < c->mb_rows)
    {
        int mb_col = -1;

        while (++mb_col < c->mb_cols)
        {
            const int ym = m->mbmi.mode;

            if (cpi->mb.e_mbd.update_mb_segmentation_map)
                write_mb_features(bc, &m->mbmi, &cpi->mb.e_mbd);

            if (c->mb_no_coeff_skip)
                vp8_encode_bool(bc, m->mbmi.mb_skip_coeff, prob_skip_false);

            kfwrite_ymode(bc, ym, c->kf_ymode_prob);

            if (ym == B_PRED)
            {
                const int mis = c->mode_info_stride;
                int i = 0;

                do
                {
                    const B_PREDICTION_MODE A = above_block_mode(m, i, mis);
                    const B_PREDICTION_MODE L = left_block_mode(m, i);
                    const int bm = m->bmi[i].as_mode;

#ifdef ENTROPY_STATS
                    ++intra_mode_stats [A] [L] [bm];
#endif

                    write_bmode(bc, bm, c->kf_bmode_prob [A] [L]);
                }
                while (++i < 16);
            }

            write_uv_mode(bc, (m++)->mbmi.uv_mode, c->kf_uv_mode_prob);
        }

        m++;    // skip L prediction border
    }
}

/* This function is used for debugging probability trees. */
static void print_prob_tree(vp8_prob
     coef_probs[BLOCK_TYPES][COEF_BANDS][PREV_COEF_CONTEXTS][ENTROPY_NODES])
{
    /* print coef probability tree */
    int i,j,k,l;
    FILE* f = fopen("enc_tree_probs.txt", "a");
    fprintf(f, "{\n");
    for (i = 0; i < BLOCK_TYPES; i++)
    {
        fprintf(f, "  {\n");
        for (j = 0; j < COEF_BANDS; j++)
        {
            fprintf(f, "    {\n");
            for (k = 0; k < PREV_COEF_CONTEXTS; k++)
            {
                fprintf(f, "      {");
                for (l = 0; l < ENTROPY_NODES; l++)
                {
                    fprintf(f, "%3u, ",
                            (unsigned int)(coef_probs [i][j][k][l]));
                }
                fprintf(f, " }\n");
            }
            fprintf(f, "    }\n");
        }
        fprintf(f, "  }\n");
    }
    fprintf(f, "}\n");
    fclose(f);
}

static void sum_probs_over_prev_coef_context(
        const unsigned int probs[PREV_COEF_CONTEXTS][MAX_ENTROPY_TOKENS],
        unsigned int* out)
{
    int i, j;
    for (i=0; i < MAX_ENTROPY_TOKENS; ++i)
    {
        for (j=0; j < PREV_COEF_CONTEXTS; ++j)
        {
            const int tmp = out[i];
            out[i] += probs[j][i];
            /* check for wrap */
            if (out[i] < tmp)
                out[i] = UINT_MAX;
        }
    }
}

static int prob_update_savings(const unsigned int *ct,
                                   const vp8_prob oldp, const vp8_prob newp,
                                   const vp8_prob upd)
{
    const int old_b = vp8_cost_branch(ct, oldp);
    const int new_b = vp8_cost_branch(ct, newp);
    const int update_b = 8 +
                         ((vp8_cost_one(upd) - vp8_cost_zero(upd)) >> 8);

    return old_b - new_b - update_b;
}

static int independent_coef_context_savings(VP8_COMP *cpi)
{
    int savings = 0;
    int i = 0;
    do
    {
        int j = 0;
        do
        {
            int k = 0;
            unsigned int prev_coef_count_sum[MAX_ENTROPY_TOKENS] = {0};
            int prev_coef_savings[MAX_ENTROPY_TOKENS] = {0};
            const unsigned int (*probs)[MAX_ENTROPY_TOKENS];
            /* Calculate new probabilities given the constraint that
             * they must be equal over the prev coef contexts
             */

            probs = (const unsigned int (*)[MAX_ENTROPY_TOKENS])
                                                    cpi->coef_counts[i][j];

            /* Reset to default probabilities at key frames */
            if (cpi->common.frame_type == KEY_FRAME)
                probs = default_coef_counts[i][j];

            sum_probs_over_prev_coef_context(probs, prev_coef_count_sum);

            do
            {
                /* at every context */

                /* calc probs and branch cts for this frame only */
                //vp8_prob new_p           [ENTROPY_NODES];
                //unsigned int branch_ct   [ENTROPY_NODES] [2];

                int t = 0;      /* token/prob index */

                vp8_tree_probs_from_distribution(
                    MAX_ENTROPY_TOKENS, vp8_coef_encodings, vp8_coef_tree,
                    cpi->frame_coef_probs[i][j][k],
                    cpi->frame_branch_ct [i][j][k],
                    prev_coef_count_sum,
                    256, 1);

                do
                {
                    const unsigned int *ct  = cpi->frame_branch_ct [i][j][k][t];
                    const vp8_prob newp = cpi->frame_coef_probs [i][j][k][t];
                    const vp8_prob oldp = cpi->common.fc.coef_probs [i][j][k][t];
                    const vp8_prob upd = vp8_coef_update_probs [i][j][k][t];
                    const int s = prob_update_savings(ct, oldp, newp, upd);

                    if (cpi->common.frame_type != KEY_FRAME ||
                        (cpi->common.frame_type == KEY_FRAME && newp != oldp))
                        prev_coef_savings[t] += s;
                }
                while (++t < ENTROPY_NODES);
            }
            while (++k < PREV_COEF_CONTEXTS);
            k = 0;
            do
            {
                /* We only update probabilities if we can save bits, except
                 * for key frames where we have to update all probabilities
                 * to get the equal probabilities across the prev coef
                 * contexts.
                 */
                if (prev_coef_savings[k] > 0 ||
                    cpi->common.frame_type == KEY_FRAME)
                    savings += prev_coef_savings[k];
            }
            while (++k < ENTROPY_NODES);
        }
        while (++j < COEF_BANDS);
    }
    while (++i < BLOCK_TYPES);
    return savings;
}

static int default_coef_context_savings(VP8_COMP *cpi)
{
    int savings = 0;
    int i = 0;
    do
    {
        int j = 0;
        do
        {
            int k = 0;
            do
            {
                /* at every context */

                /* calc probs and branch cts for this frame only */
                //vp8_prob new_p           [ENTROPY_NODES];
                //unsigned int branch_ct   [ENTROPY_NODES] [2];

                int t = 0;      /* token/prob index */


                vp8_tree_probs_from_distribution(
                    MAX_ENTROPY_TOKENS, vp8_coef_encodings, vp8_coef_tree,
                    cpi->frame_coef_probs [i][j][k],
                    cpi->frame_branch_ct [i][j][k],
                    cpi->coef_counts [i][j][k],
                    256, 1
                );

                do
                {
                    const unsigned int *ct  = cpi->frame_branch_ct [i][j][k][t];
                    const vp8_prob newp = cpi->frame_coef_probs [i][j][k][t];
                    const vp8_prob oldp = cpi->common.fc.coef_probs [i][j][k][t];
                    const vp8_prob upd = vp8_coef_update_probs [i][j][k][t];
                    const int s = prob_update_savings(ct, oldp, newp, upd);

                    if (s > 0)
                    {
                        savings += s;
                    }
                }
                while (++t < ENTROPY_NODES);
            }
            while (++k < PREV_COEF_CONTEXTS);
        }
        while (++j < COEF_BANDS);
    }
    while (++i < BLOCK_TYPES);
    return savings;
}

int vp8_estimate_entropy_savings(VP8_COMP *cpi)
{
    int savings = 0;

    const int *const rfct = cpi->count_mb_ref_frame_usage;
    const int rf_intra = rfct[INTRA_FRAME];
    const int rf_inter = rfct[LAST_FRAME] + rfct[GOLDEN_FRAME] + rfct[ALTREF_FRAME];
    int new_intra, new_last, gf_last, oldtotal, newtotal;
    int ref_frame_cost[MAX_REF_FRAMES];

    vp8_clear_system_state(); //__asm emms;

    if (cpi->common.frame_type != KEY_FRAME)
    {
        if (!(new_intra = rf_intra * 255 / (rf_intra + rf_inter)))
            new_intra = 1;

        new_last = rf_inter ? (rfct[LAST_FRAME] * 255) / rf_inter : 128;

        gf_last = (rfct[GOLDEN_FRAME] + rfct[ALTREF_FRAME])
                  ? (rfct[GOLDEN_FRAME] * 255) / (rfct[GOLDEN_FRAME] + rfct[ALTREF_FRAME]) : 128;

        // new costs
        ref_frame_cost[INTRA_FRAME]   = vp8_cost_zero(new_intra);
        ref_frame_cost[LAST_FRAME]    = vp8_cost_one(new_intra)
                                        + vp8_cost_zero(new_last);
        ref_frame_cost[GOLDEN_FRAME]  = vp8_cost_one(new_intra)
                                        + vp8_cost_one(new_last)
                                        + vp8_cost_zero(gf_last);
        ref_frame_cost[ALTREF_FRAME]  = vp8_cost_one(new_intra)
                                        + vp8_cost_one(new_last)
                                        + vp8_cost_one(gf_last);

        newtotal =
            rfct[INTRA_FRAME] * ref_frame_cost[INTRA_FRAME] +
            rfct[LAST_FRAME] * ref_frame_cost[LAST_FRAME] +
            rfct[GOLDEN_FRAME] * ref_frame_cost[GOLDEN_FRAME] +
            rfct[ALTREF_FRAME] * ref_frame_cost[ALTREF_FRAME];


        // old costs
        ref_frame_cost[INTRA_FRAME]   = vp8_cost_zero(cpi->prob_intra_coded);
        ref_frame_cost[LAST_FRAME]    = vp8_cost_one(cpi->prob_intra_coded)
                                        + vp8_cost_zero(cpi->prob_last_coded);
        ref_frame_cost[GOLDEN_FRAME]  = vp8_cost_one(cpi->prob_intra_coded)
                                        + vp8_cost_one(cpi->prob_last_coded)
                                        + vp8_cost_zero(cpi->prob_gf_coded);
        ref_frame_cost[ALTREF_FRAME]  = vp8_cost_one(cpi->prob_intra_coded)
                                        + vp8_cost_one(cpi->prob_last_coded)
                                        + vp8_cost_one(cpi->prob_gf_coded);

        oldtotal =
            rfct[INTRA_FRAME] * ref_frame_cost[INTRA_FRAME] +
            rfct[LAST_FRAME] * ref_frame_cost[LAST_FRAME] +
            rfct[GOLDEN_FRAME] * ref_frame_cost[GOLDEN_FRAME] +
            rfct[ALTREF_FRAME] * ref_frame_cost[ALTREF_FRAME];

        savings += (oldtotal - newtotal) / 256;
    }


    if (cpi->oxcf.error_resilient_mode & VPX_ERROR_RESILIENT_PARTITIONS)
        savings += independent_coef_context_savings(cpi);
    else
        savings += default_coef_context_savings(cpi);


    return savings;
}

static void update_coef_probs(VP8_COMP *cpi)
{
    int i = 0;
    vp8_writer *const w = & cpi->bc;
    int savings = 0;

    vp8_clear_system_state(); //__asm emms;

    do
    {
        int j = 0;

        do
        {
            int k = 0;
            int prev_coef_savings[ENTROPY_NODES] = {0};
            if (cpi->oxcf.error_resilient_mode & VPX_ERROR_RESILIENT_PARTITIONS)
            {
                for (k = 0; k < PREV_COEF_CONTEXTS; ++k)
                {
                    int t;      /* token/prob index */
                    for (t = 0; t < ENTROPY_NODES; ++t)
                    {
                        const unsigned int *ct = cpi->frame_branch_ct [i][j]
                                                                      [k][t];
                        const vp8_prob newp = cpi->frame_coef_probs[i][j][k][t];
                        const vp8_prob oldp = cpi->common.fc.coef_probs[i][j]
                                                                       [k][t];
                        const vp8_prob upd = vp8_coef_update_probs[i][j][k][t];

                        prev_coef_savings[t] +=
                                prob_update_savings(ct, oldp, newp, upd);
                    }
                }
                k = 0;
            }
            do
            {
                //note: use result from vp8_estimate_entropy_savings, so no need to call vp8_tree_probs_from_distribution here.
                /* at every context */

                /* calc probs and branch cts for this frame only */
                //vp8_prob new_p           [ENTROPY_NODES];
                //unsigned int branch_ct   [ENTROPY_NODES] [2];

                int t = 0;      /* token/prob index */

                //vp8_tree_probs_from_distribution(
                //    MAX_ENTROPY_TOKENS, vp8_coef_encodings, vp8_coef_tree,
                //    new_p, branch_ct, (unsigned int *)cpi->coef_counts [i][j][k],
                //    256, 1
                //    );

                do
                {
                    const vp8_prob newp = cpi->frame_coef_probs [i][j][k][t];

                    vp8_prob *Pold = cpi->common.fc.coef_probs [i][j][k] + t;
                    const vp8_prob upd = vp8_coef_update_probs [i][j][k][t];

                    int s = prev_coef_savings[t];
                    int u = 0;

                    if (!(cpi->oxcf.error_resilient_mode &
                            VPX_ERROR_RESILIENT_PARTITIONS))
                    {
                        s = prob_update_savings(
                                cpi->frame_branch_ct [i][j][k][t],
                                *Pold, newp, upd);
                    }

                    if (s > 0)
                        u = 1;

                    /* Force updates on key frames if the new is different,
                     * so that we can be sure we end up with equal probabilities
                     * over the prev coef contexts.
                     */
                    if ((cpi->oxcf.error_resilient_mode &
                            VPX_ERROR_RESILIENT_PARTITIONS) &&
                        cpi->common.frame_type == KEY_FRAME && newp != *Pold)
                        u = 1;

                    vp8_write(w, u, upd);


#ifdef ENTROPY_STATS
                    ++ tree_update_hist [i][j][k][t] [u];
#endif

                    if (u)
                    {
                        /* send/use new probability */

                        *Pold = newp;
                        vp8_write_literal(w, newp, 8);

                        savings += s;

                    }

                }
                while (++t < ENTROPY_NODES);

                /* Accum token counts for generation of default statistics */
#ifdef ENTROPY_STATS
                t = 0;

                do
                {
                    context_counters [i][j][k][t] += cpi->coef_counts [i][j][k][t];
                }
                while (++t < MAX_ENTROPY_TOKENS);

#endif

            }
            while (++k < PREV_COEF_CONTEXTS);
        }
        while (++j < COEF_BANDS);
    }
    while (++i < BLOCK_TYPES);

}
#ifdef PACKET_TESTING
FILE *vpxlogc = 0;
#endif

static void put_delta_q(vp8_writer *bc, int delta_q)
{
    if (delta_q != 0)
    {
        vp8_write_bit(bc, 1);
        vp8_write_literal(bc, abs(delta_q), 4);

        if (delta_q < 0)
            vp8_write_bit(bc, 1);
        else
            vp8_write_bit(bc, 0);
    }
    else
        vp8_write_bit(bc, 0);
}

void vp8_pack_bitstream(VP8_COMP *cpi, unsigned char *dest, unsigned char * dest_end, unsigned long *size)
{
    int i, j;
    VP8_HEADER oh;
    VP8_COMMON *const pc = & cpi->common;
    vp8_writer *const bc = & cpi->bc;
    MACROBLOCKD *const xd = & cpi->mb.e_mbd;
    int extra_bytes_packed = 0;

    unsigned char *cx_data = dest;
    unsigned char *cx_data_end = dest_end;
    const int *mb_feature_data_bits;

    oh.show_frame = (int) pc->show_frame;
    oh.type = (int)pc->frame_type;
    oh.version = pc->version;
    oh.first_partition_length_in_bytes = 0;

    mb_feature_data_bits = vp8_mb_feature_data_bits;

    validate_buffer(cx_data, 3, cx_data_end, &cpi->common.error);
    cx_data += 3;

#if defined(SECTIONBITS_OUTPUT)
    Sectionbits[active_section = 1] += sizeof(VP8_HEADER) * 8 * 256;
#endif

    //vp8_kf_default_bmode_probs() is called in vp8_setup_key_frame() once for each
    //K frame before encode frame. pc->kf_bmode_prob doesn't get changed anywhere
    //else. No need to call it again here. --yw
    //vp8_kf_default_bmode_probs( pc->kf_bmode_prob);

    // every keyframe send startcode, width, height, scale factor, clamp and color type
    if (oh.type == KEY_FRAME)
    {
        int v;

        validate_buffer(cx_data, 7, cx_data_end, &cpi->common.error);

        // Start / synch code
        cx_data[0] = 0x9D;
        cx_data[1] = 0x01;
        cx_data[2] = 0x2a;

        v = (pc->horiz_scale << 14) | pc->Width;
        cx_data[3] = v;
        cx_data[4] = v >> 8;

        v = (pc->vert_scale << 14) | pc->Height;
        cx_data[5] = v;
        cx_data[6] = v >> 8;


        extra_bytes_packed = 7;
        cx_data += extra_bytes_packed ;

        vp8_start_encode(bc, cx_data, cx_data_end);

        // signal clr type
        vp8_write_bit(bc, pc->clr_type);
        vp8_write_bit(bc, pc->clamp_type);

    }
    else
        vp8_start_encode(bc, cx_data, cx_data_end);


    // Signal whether or not Segmentation is enabled
    vp8_write_bit(bc, (xd->segmentation_enabled) ? 1 : 0);

    // Indicate which features are enabled
    if (xd->segmentation_enabled)
    {
        // Signal whether or not the segmentation map is being updated.
        vp8_write_bit(bc, (xd->update_mb_segmentation_map) ? 1 : 0);
        vp8_write_bit(bc, (xd->update_mb_segmentation_data) ? 1 : 0);

        if (xd->update_mb_segmentation_data)
        {
            signed char Data;

            vp8_write_bit(bc, (xd->mb_segement_abs_delta) ? 1 : 0);

            // For each segmentation feature (Quant and loop filter level)
            for (i = 0; i < MB_LVL_MAX; i++)
            {
                // For each of the segments
                for (j = 0; j < MAX_MB_SEGMENTS; j++)
                {
                    Data = xd->segment_feature_data[i][j];

                    // Frame level data
                    if (Data)
                    {
                        vp8_write_bit(bc, 1);

                        if (Data < 0)
                        {
                            Data = - Data;
                            vp8_write_literal(bc, Data, mb_feature_data_bits[i]);
                            vp8_write_bit(bc, 1);
                        }
                        else
                        {
                            vp8_write_literal(bc, Data, mb_feature_data_bits[i]);
                            vp8_write_bit(bc, 0);
                        }
                    }
                    else
                        vp8_write_bit(bc, 0);
                }
            }
        }

        if (xd->update_mb_segmentation_map)
        {
            // Write the probs used to decode the segment id for each macro block.
            for (i = 0; i < MB_FEATURE_TREE_PROBS; i++)
            {
                int Data = xd->mb_segment_tree_probs[i];

                if (Data != 255)
                {
                    vp8_write_bit(bc, 1);
                    vp8_write_literal(bc, Data, 8);
                }
                else
                    vp8_write_bit(bc, 0);
            }
        }
    }

    // Code to determine whether or not to update the scan order.
    vp8_write_bit(bc, pc->filter_type);
    vp8_write_literal(bc, pc->filter_level, 6);
    vp8_write_literal(bc, pc->sharpness_level, 3);

    // Write out loop filter deltas applied at the MB level based on mode or ref frame (if they are enabled).
    vp8_write_bit(bc, (xd->mode_ref_lf_delta_enabled) ? 1 : 0);

    if (xd->mode_ref_lf_delta_enabled)
    {
        // Do the deltas need to be updated
        int send_update = xd->mode_ref_lf_delta_update
                          || cpi->oxcf.error_resilient_mode;

        vp8_write_bit(bc, send_update);
        if (send_update)
        {
            int Data;

            // Send update
            for (i = 0; i < MAX_REF_LF_DELTAS; i++)
            {
                Data = xd->ref_lf_deltas[i];

                // Frame level data
                if (xd->ref_lf_deltas[i] != xd->last_ref_lf_deltas[i]
                    || cpi->oxcf.error_resilient_mode)
                {
                    xd->last_ref_lf_deltas[i] = xd->ref_lf_deltas[i];
                    vp8_write_bit(bc, 1);

                    if (Data > 0)
                    {
                        vp8_write_literal(bc, (Data & 0x3F), 6);
                        vp8_write_bit(bc, 0);    // sign
                    }
                    else
                    {
                        Data = -Data;
                        vp8_write_literal(bc, (Data & 0x3F), 6);
                        vp8_write_bit(bc, 1);    // sign
                    }
                }
                else
                    vp8_write_bit(bc, 0);
            }

            // Send update
            for (i = 0; i < MAX_MODE_LF_DELTAS; i++)
            {
                Data = xd->mode_lf_deltas[i];

                if (xd->mode_lf_deltas[i] != xd->last_mode_lf_deltas[i]
                    || cpi->oxcf.error_resilient_mode)
                {
                    xd->last_mode_lf_deltas[i] = xd->mode_lf_deltas[i];
                    vp8_write_bit(bc, 1);

                    if (Data > 0)
                    {
                        vp8_write_literal(bc, (Data & 0x3F), 6);
                        vp8_write_bit(bc, 0);    // sign
                    }
                    else
                    {
                        Data = -Data;
                        vp8_write_literal(bc, (Data & 0x3F), 6);
                        vp8_write_bit(bc, 1);    // sign
                    }
                }
                else
                    vp8_write_bit(bc, 0);
            }
        }
    }

    //signal here is multi token partition is enabled
    vp8_write_literal(bc, pc->multi_token_partition, 2);

    // Frame Qbaseline quantizer index
    vp8_write_literal(bc, pc->base_qindex, 7);

    // Transmit Dc, Second order and Uv quantizer delta information
    put_delta_q(bc, pc->y1dc_delta_q);
    put_delta_q(bc, pc->y2dc_delta_q);
    put_delta_q(bc, pc->y2ac_delta_q);
    put_delta_q(bc, pc->uvdc_delta_q);
    put_delta_q(bc, pc->uvac_delta_q);

    // When there is a key frame all reference buffers are updated using the new key frame
    if (pc->frame_type != KEY_FRAME)
    {
        // Should the GF or ARF be updated using the transmitted frame or buffer
        vp8_write_bit(bc, pc->refresh_golden_frame);
        vp8_write_bit(bc, pc->refresh_alt_ref_frame);

        // If not being updated from current frame should either GF or ARF be updated from another buffer
        if (!pc->refresh_golden_frame)
            vp8_write_literal(bc, pc->copy_buffer_to_gf, 2);

        if (!pc->refresh_alt_ref_frame)
            vp8_write_literal(bc, pc->copy_buffer_to_arf, 2);

        // Indicate reference frame sign bias for Golden and ARF frames (always 0 for last frame buffer)
        vp8_write_bit(bc, pc->ref_frame_sign_bias[GOLDEN_FRAME]);
        vp8_write_bit(bc, pc->ref_frame_sign_bias[ALTREF_FRAME]);
    }

    if (cpi->oxcf.error_resilient_mode & VPX_ERROR_RESILIENT_PARTITIONS)
    {
        if (pc->frame_type == KEY_FRAME)
            pc->refresh_entropy_probs = 1;
        else
            pc->refresh_entropy_probs = 0;
    }

    vp8_write_bit(bc, pc->refresh_entropy_probs);

    if (pc->frame_type != KEY_FRAME)
        vp8_write_bit(bc, pc->refresh_last_frame);

#ifdef ENTROPY_STATS

    if (pc->frame_type == INTER_FRAME)
        active_section = 0;
    else
        active_section = 7;

#endif

    vp8_clear_system_state();  //__asm emms;

    if (pc->refresh_entropy_probs == 0)
    {
        // save a copy for later refresh
        vpx_memcpy(&cpi->common.lfc, &cpi->common.fc, sizeof(cpi->common.fc));
    }

    update_coef_probs(cpi);

#ifdef ENTROPY_STATS
    active_section = 2;
#endif

    // Write out the mb_no_coeff_skip flag
    vp8_write_bit(bc, pc->mb_no_coeff_skip);

    if (pc->frame_type == KEY_FRAME)
    {
        write_kfmodes(cpi);

#ifdef ENTROPY_STATS
        active_section = 8;
#endif
    }
    else
    {
        pack_inter_mode_mvs(cpi);

#ifdef ENTROPY_STATS
        active_section = 1;
#endif
    }

    vp8_stop_encode(bc);

    oh.first_partition_length_in_bytes = cpi->bc.pos;

    /* update frame tag */
    {
        int v = (oh.first_partition_length_in_bytes << 5) |
                (oh.show_frame << 4) |
                (oh.version << 1) |
                oh.type;

        dest[0] = v;
        dest[1] = v >> 8;
        dest[2] = v >> 16;
    }

    *size = VP8_HEADER_SIZE + extra_bytes_packed + cpi->bc.pos;
    cpi->partition_sz[0] = *size;

    if (pc->multi_token_partition != ONE_PARTITION)
    {
        int num_part;
        int asize;
        num_part = 1 << pc->multi_token_partition;

        pack_tokens_into_partitions(cpi, cx_data + bc->pos, cx_data_end, num_part, &asize);

        *size += asize;
    }
    else
    {
        vp8_start_encode(&cpi->bc2, cx_data + bc->pos, cx_data_end);

#if CONFIG_MULTITHREAD
        if (cpi->b_multi_threaded)
            pack_mb_row_tokens(cpi, &cpi->bc2);
        else
#endif
            pack_tokens(&cpi->bc2, cpi->tok, cpi->tok_count);

        vp8_stop_encode(&cpi->bc2);

        *size += cpi->bc2.pos;
        cpi->partition_sz[1] = cpi->bc2.pos;
    }
}

#ifdef ENTROPY_STATS
void print_tree_update_probs()
{
    int i, j, k, l;
    FILE *f = fopen("context.c", "a");
    int Sum;
    fprintf(f, "\n/* Update probabilities for token entropy tree. */\n\n");
    fprintf(f, "const vp8_prob tree_update_probs[BLOCK_TYPES] [COEF_BANDS] [PREV_COEF_CONTEXTS] [ENTROPY_NODES] = {\n");

    for (i = 0; i < BLOCK_TYPES; i++)
    {
        fprintf(f, "  { \n");

        for (j = 0; j < COEF_BANDS; j++)
        {
            fprintf(f, "    {\n");

            for (k = 0; k < PREV_COEF_CONTEXTS; k++)
            {
                fprintf(f, "      {");

                for (l = 0; l < ENTROPY_NODES; l++)
                {
                    Sum = tree_update_hist[i][j][k][l][0] + tree_update_hist[i][j][k][l][1];

                    if (Sum > 0)
                    {
                        if (((tree_update_hist[i][j][k][l][0] * 255) / Sum) > 0)
                            fprintf(f, "%3ld, ", (tree_update_hist[i][j][k][l][0] * 255) / Sum);
                        else
                            fprintf(f, "%3ld, ", 1);
                    }
                    else
                        fprintf(f, "%3ld, ", 128);
                }

                fprintf(f, "},\n");
            }

            fprintf(f, "    },\n");
        }

        fprintf(f, "  },\n");
    }

    fprintf(f, "};\n");
    fclose(f);
}
#endif
