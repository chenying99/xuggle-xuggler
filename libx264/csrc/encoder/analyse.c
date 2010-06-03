/*****************************************************************************
 * analyse.c: h264 encoder library
 *****************************************************************************
 * Copyright (C) 2003-2008 x264 project
 *
 * Authors: Laurent Aimar <fenrir@via.ecp.fr>
 *          Loren Merritt <lorenm@u.washington.edu>
 *          Jason Garrett-Glaser <darkshikari@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111, USA.
 *****************************************************************************/

#define _ISOC99_SOURCE
#include <math.h>
#include <unistd.h>

#include "common/common.h"
#include "macroblock.h"
#include "me.h"
#include "ratecontrol.h"
#include "analyse.h"
#include "rdo.c"

typedef struct
{
    /* 16x16 */
    int       i_rd16x16;
    x264_me_t me16x16;
    x264_me_t bi16x16;      /* for b16x16 BI mode, since MVs can differ from l0/l1 */

    /* 8x8 */
    int       i_cost8x8;
    /* [ref][0] is 16x16 mv, [ref][1..4] are 8x8 mv from partition [0..3] */
    ALIGNED_4( int16_t mvc[32][5][2] );
    x264_me_t me8x8[4];

    /* Sub 4x4 */
    int       i_cost4x4[4]; /* cost per 8x8 partition */
    x264_me_t me4x4[4][4];

    /* Sub 8x4 */
    int       i_cost8x4[4]; /* cost per 8x8 partition */
    x264_me_t me8x4[4][2];

    /* Sub 4x8 */
    int       i_cost4x8[4]; /* cost per 8x8 partition */
    x264_me_t me4x8[4][2];

    /* 16x8 */
    int       i_cost16x8;
    x264_me_t me16x8[2];

    /* 8x16 */
    int       i_cost8x16;
    x264_me_t me8x16[2];

} x264_mb_analysis_list_t;

typedef struct
{
    /* conduct the analysis using this lamda and QP */
    int i_lambda;
    int i_lambda2;
    int i_qp;
    uint16_t *p_cost_mv;
    uint16_t *p_cost_ref[2];
    int i_mbrd;


    /* I: Intra part */
    /* Take some shortcuts in intra search if intra is deemed unlikely */
    int b_fast_intra;
    int b_force_intra; /* For Periodic Intra Refresh.  Only supported in P-frames. */
    int b_try_skip;

    /* Luma part */
    int i_satd_i16x16;
    int i_satd_i16x16_dir[7];
    int i_predict16x16;

    int i_satd_i8x8;
    int i_cbp_i8x8_luma;
    int i_satd_i8x8_dir[12][4];
    int i_predict8x8[4];

    int i_satd_i4x4;
    int i_predict4x4[16];

    int i_satd_pcm;

    /* Chroma part */
    int i_satd_i8x8chroma;
    int i_satd_i8x8chroma_dir[7];
    int i_predict8x8chroma;

    /* II: Inter part P/B frame */
    x264_mb_analysis_list_t l0;
    x264_mb_analysis_list_t l1;

    int i_cost16x16bi; /* used the same ref and mv as l0 and l1 (at least for now) */
    int i_cost16x16direct;
    int i_cost8x8bi;
    int i_cost8x8direct[4];
    int i_satd8x8[3][4]; /* [L0,L1,BI][8x8 0..3] SATD only */
    int i_cost_est16x8[2]; /* Per-partition estimated cost */
    int i_cost_est8x16[2];
    int i_cost16x8bi;
    int i_cost8x16bi;
    int i_rd16x16bi;
    int i_rd16x16direct;
    int i_rd16x8bi;
    int i_rd8x16bi;
    int i_rd8x8bi;

    int i_mb_partition16x8[2]; /* mb_partition_e */
    int i_mb_partition8x16[2];
    int i_mb_type16x8; /* mb_class_e */
    int i_mb_type8x16;

    int b_direct_available;

} x264_mb_analysis_t;

/* lambda = pow(2,qp/6-2) */
const uint8_t x264_lambda_tab[52] = {
   1, 1, 1, 1, 1, 1, 1, 1,  /*  0-7 */
   1, 1, 1, 1,              /*  8-11 */
   1, 1, 1, 1, 2, 2, 2, 2,  /* 12-19 */
   3, 3, 3, 4, 4, 4, 5, 6,  /* 20-27 */
   6, 7, 8, 9,10,11,13,14,  /* 28-35 */
  16,18,20,23,25,29,32,36,  /* 36-43 */
  40,45,51,57,64,72,81,91   /* 44-51 */
};

/* lambda2 = pow(lambda,2) * .9 * 256 */
const int x264_lambda2_tab[52] = {
    14,      18,      22,      28,     36,     45,     57,     72, /*  0 -  7 */
    91,     115,     145,     182,    230,    290,    365,    460, /*  8 - 15 */
   580,     731,     921,    1161,   1462,   1843,   2322,   2925, /* 16 - 23 */
  3686,    4644,    5851,    7372,   9289,  11703,  14745,  18578, /* 24 - 31 */
 23407,   29491,   37156,   46814,  58982,  74313,  93628, 117964, /* 32 - 39 */
148626,  187257,  235929,  297252, 374514, 471859, 594505, 749029, /* 40 - 47 */
943718, 1189010, 1498059, 1887436                                  /* 48 - 51 */
};

const uint8_t x264_exp2_lut[64] = {
      0,   3,   6,   8,  11,  14,  17,  20,  23,  26,  29,  32,  36,  39,  42,  45,
     48,  52,  55,  58,  62,  65,  69,  72,  76,  80,  83,  87,  91,  94,  98, 102,
    106, 110, 114, 118, 122, 126, 130, 135, 139, 143, 147, 152, 156, 161, 165, 170,
    175, 179, 184, 189, 194, 198, 203, 208, 214, 219, 224, 229, 234, 240, 245, 250
};

const float x264_log2_lut[128] = {
    0.00000, 0.01123, 0.02237, 0.03342, 0.04439, 0.05528, 0.06609, 0.07682,
    0.08746, 0.09803, 0.10852, 0.11894, 0.12928, 0.13955, 0.14975, 0.15987,
    0.16993, 0.17991, 0.18982, 0.19967, 0.20945, 0.21917, 0.22882, 0.23840,
    0.24793, 0.25739, 0.26679, 0.27612, 0.28540, 0.29462, 0.30378, 0.31288,
    0.32193, 0.33092, 0.33985, 0.34873, 0.35755, 0.36632, 0.37504, 0.38370,
    0.39232, 0.40088, 0.40939, 0.41785, 0.42626, 0.43463, 0.44294, 0.45121,
    0.45943, 0.46761, 0.47573, 0.48382, 0.49185, 0.49985, 0.50779, 0.51570,
    0.52356, 0.53138, 0.53916, 0.54689, 0.55459, 0.56224, 0.56986, 0.57743,
    0.58496, 0.59246, 0.59991, 0.60733, 0.61471, 0.62205, 0.62936, 0.63662,
    0.64386, 0.65105, 0.65821, 0.66534, 0.67243, 0.67948, 0.68650, 0.69349,
    0.70044, 0.70736, 0.71425, 0.72110, 0.72792, 0.73471, 0.74147, 0.74819,
    0.75489, 0.76155, 0.76818, 0.77479, 0.78136, 0.78790, 0.79442, 0.80090,
    0.80735, 0.81378, 0.82018, 0.82655, 0.83289, 0.83920, 0.84549, 0.85175,
    0.85798, 0.86419, 0.87036, 0.87652, 0.88264, 0.88874, 0.89482, 0.90087,
    0.90689, 0.91289, 0.91886, 0.92481, 0.93074, 0.93664, 0.94251, 0.94837,
    0.95420, 0.96000, 0.96578, 0.97154, 0.97728, 0.98299, 0.98868, 0.99435,
};

/* Avoid an int/float conversion. */
const float x264_log2_lz_lut[32] = {
    31,30,29,28,27,26,25,24,23,22,21,20,19,18,17,16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0
};

// should the intra and inter lambdas be different?
// I'm just matching the behaviour of deadzone quant.
static const int x264_trellis_lambda2_tab[2][52] = {
    // inter lambda = .85 * .85 * 2**(qp/3. + 10 - LAMBDA_BITS)
    {    46,      58,      73,      92,     117,     147,
        185,     233,     294,     370,     466,     587,
        740,     932,    1174,    1480,    1864,    2349,
       2959,    3728,    4697,    5918,    7457,    9395,
      11837,   14914,   18790,   23674,   29828,   37581,
      47349,   59656,   75163,   94699,  119313,  150326,
     189399,  238627,  300652,  378798,  477255,  601304,
     757596,  954511, 1202608, 1515192, 1909022, 2405217,
    3030384, 3818045, 4810435, 6060769 },
    // intra lambda = .65 * .65 * 2**(qp/3. + 10 - LAMBDA_BITS)
    {    27,      34,      43,      54,      68,      86,
        108,     136,     172,     216,     273,     343,
        433,     545,     687,     865,    1090,    1374,
       1731,    2180,    2747,    3461,    4361,    5494,
       6922,    8721,   10988,   13844,   17442,   21976,
      27688,   34885,   43953,   55377,   69771,   87906,
     110755,  139543,  175813,  221511,  279087,  351627,
     443023,  558174,  703255,  886046, 1116348, 1406511,
    1772093, 2232697, 2813022, 3544186 }
};

static const uint16_t x264_chroma_lambda2_offset_tab[] = {
       16,    20,    25,    32,    40,    50,
       64,    80,   101,   128,   161,   203,
      256,   322,   406,   512,   645,   812,
     1024,  1290,  1625,  2048,  2580,  3250,
     4096,  5160,  6501,  8192, 10321, 13003,
    16384, 20642, 26007, 32768, 41285, 52015,
    65535
};

/* TODO: calculate CABAC costs */
static const uint8_t i_mb_b_cost_table[X264_MBTYPE_MAX] = {
    9, 9, 9, 9, 0, 0, 0, 1, 3, 7, 7, 7, 3, 7, 7, 7, 5, 9, 0
};
static const uint8_t i_mb_b16x8_cost_table[17] = {
    0, 0, 0, 0, 0, 0, 0, 0, 5, 7, 7, 7, 5, 7, 9, 9, 9
};
static const uint8_t i_sub_mb_b_cost_table[13] = {
    7, 5, 5, 3, 7, 5, 7, 3, 7, 7, 7, 5, 1
};
static const uint8_t i_sub_mb_p_cost_table[4] = {
    5, 3, 3, 1
};

static void x264_analyse_update_cache( x264_t *h, x264_mb_analysis_t *a );

static uint16_t x264_cost_ref[92][3][33];
static UNUSED x264_pthread_mutex_t cost_ref_mutex = X264_PTHREAD_MUTEX_INITIALIZER;

int x264_analyse_init_costs( x264_t *h, int qp )
{
    int lambda = x264_lambda_tab[qp];
    if( h->cost_mv[lambda] )
        return 0;
    /* factor of 4 from qpel, 2 from sign, and 2 because mv can be opposite from mvp */
    CHECKED_MALLOC( h->cost_mv[lambda], (4*4*2048 + 1) * sizeof(uint16_t) );
    h->cost_mv[lambda] += 2*4*2048;
    for( int i = 0; i <= 2*4*2048; i++ )
    {
        h->cost_mv[lambda][-i] =
        h->cost_mv[lambda][i]  = lambda * (log2f(i+1)*2 + 0.718f + !!i) + .5f;
    }
    x264_pthread_mutex_lock( &cost_ref_mutex );
    for( int i = 0; i < 3; i++ )
        for( int j = 0; j < 33; j++ )
            x264_cost_ref[lambda][i][j] = i ? lambda * bs_size_te( i, j ) : 0;
    x264_pthread_mutex_unlock( &cost_ref_mutex );
    if( h->param.analyse.i_me_method >= X264_ME_ESA && !h->cost_mv_fpel[lambda][0] )
    {
        for( int j = 0; j < 4; j++ )
        {
            CHECKED_MALLOC( h->cost_mv_fpel[lambda][j], (4*2048 + 1) * sizeof(uint16_t) );
            h->cost_mv_fpel[lambda][j] += 2*2048;
            for( int i = -2*2048; i < 2*2048; i++ )
                h->cost_mv_fpel[lambda][j][i] = h->cost_mv[lambda][i*4+j];
        }
    }
    return 0;
fail:
    return -1;
}

void x264_analyse_free_costs( x264_t *h )
{
    for( int i = 0; i < 92; i++ )
    {
        if( h->cost_mv[i] )
            x264_free( h->cost_mv[i] - 2*4*2048 );
        if( h->cost_mv_fpel[i][0] )
            for( int j = 0; j < 4; j++ )
                x264_free( h->cost_mv_fpel[i][j] - 2*2048 );
    }
}

void x264_analyse_weight_frame( x264_t *h, int end )
{
    for( int j = 0; j < h->i_ref0; j++ )
    {
        if( h->sh.weight[j][0].weightfn )
        {
            x264_frame_t *frame = h->fref0[j];
            int width = frame->i_width[0] + 2*PADH;
            int i_padv = PADV << h->param.b_interlaced;
            int offset, height;
            pixel *src = frame->filtered[0] - frame->i_stride[0]*i_padv - PADH;
            height = X264_MIN( 16 + end + i_padv, h->fref0[j]->i_lines[0] + i_padv*2 ) - h->fenc->i_lines_weighted;
            offset = h->fenc->i_lines_weighted*frame->i_stride[0];
            h->fenc->i_lines_weighted += height;
            if( height )
                for( int k = j; k < h->i_ref0; k++ )
                    if( h->sh.weight[k][0].weightfn )
                    {
                        pixel *dst = h->fenc->weighted[k] - h->fenc->i_stride[0]*i_padv - PADH;
                        x264_weight_scale_plane( h, dst + offset, frame->i_stride[0],
                                                 src + offset, frame->i_stride[0],
                                                 width, height, &h->sh.weight[k][0] );
                    }
            break;
        }
    }
}

/* initialize an array of lambda*nbits for all possible mvs */
static void x264_mb_analyse_load_costs( x264_t *h, x264_mb_analysis_t *a )
{
    a->p_cost_mv = h->cost_mv[a->i_lambda];
    a->p_cost_ref[0] = x264_cost_ref[a->i_lambda][x264_clip3(h->sh.i_num_ref_idx_l0_active-1,0,2)];
    a->p_cost_ref[1] = x264_cost_ref[a->i_lambda][x264_clip3(h->sh.i_num_ref_idx_l1_active-1,0,2)];
}

static void x264_mb_analyse_init_qp( x264_t *h, x264_mb_analysis_t *a, int i_qp )
{
    /* conduct the analysis using this lamda and QP */
    a->i_qp = h->mb.i_qp = i_qp;
    h->mb.i_chroma_qp = h->chroma_qp_table[i_qp];

    a->i_lambda = x264_lambda_tab[i_qp];
    a->i_lambda2 = x264_lambda2_tab[i_qp];

    h->mb.b_trellis = h->param.analyse.i_trellis > 1 && a->i_mbrd;
    if( h->param.analyse.i_trellis )
    {
        h->mb.i_trellis_lambda2[0][0] = x264_trellis_lambda2_tab[0][h->mb.i_qp];
        h->mb.i_trellis_lambda2[0][1] = x264_trellis_lambda2_tab[1][h->mb.i_qp];
        h->mb.i_trellis_lambda2[1][0] = x264_trellis_lambda2_tab[0][h->mb.i_chroma_qp];
        h->mb.i_trellis_lambda2[1][1] = x264_trellis_lambda2_tab[1][h->mb.i_chroma_qp];
    }
    h->mb.i_psy_rd_lambda = a->i_lambda;
    /* Adjusting chroma lambda based on QP offset hurts PSNR but improves visual quality. */
    h->mb.i_chroma_lambda2_offset = h->param.analyse.b_psy ? x264_chroma_lambda2_offset_tab[h->mb.i_qp-h->mb.i_chroma_qp+12] : 256;
}

static void x264_mb_analyse_init( x264_t *h, x264_mb_analysis_t *a, int i_qp )
{
    int subme = h->param.analyse.i_subpel_refine - (h->sh.i_type == SLICE_TYPE_B);

    /* mbrd == 1 -> RD mode decision */
    /* mbrd == 2 -> RD refinement */
    /* mbrd == 3 -> QPRD */
    a->i_mbrd = (subme>=6) + (subme>=8) + (h->param.analyse.i_subpel_refine>=10);

    x264_mb_analyse_init_qp( h, a, i_qp );

    h->mb.b_transform_8x8 = 0;
    h->mb.b_noise_reduction = 0;

    /* I: Intra part */
    a->i_satd_i16x16 =
    a->i_satd_i8x8   =
    a->i_satd_i4x4   =
    a->i_satd_i8x8chroma = COST_MAX;

    /* non-RD PCM decision is inaccurate (as is psy-rd), so don't do it */
    a->i_satd_pcm = !h->mb.i_psy_rd && a->i_mbrd ? ((uint64_t)X264_PCM_COST*a->i_lambda2 + 128) >> 8 : COST_MAX;

    a->b_fast_intra = 0;
    h->mb.i_skip_intra =
        h->mb.b_lossless ? 0 :
        a->i_mbrd ? 2 :
        !h->param.analyse.i_trellis && !h->param.analyse.i_noise_reduction;

    /* II: Inter part P/B frame */
    if( h->sh.i_type != SLICE_TYPE_I )
    {
        int i_fmv_range = 4 * h->param.analyse.i_mv_range;
        // limit motion search to a slightly smaller range than the theoretical limit,
        // since the search may go a few iterations past its given range
        int i_fpel_border = 6; // umh: 1 for diamond, 2 for octagon, 2 for hpel

        /* Calculate max allowed MV range */
#define CLIP_FMV(mv) x264_clip3( mv, -i_fmv_range, i_fmv_range-1 )
        h->mb.mv_min[0] = 4*( -16*h->mb.i_mb_x - 24 );
        h->mb.mv_max[0] = 4*( 16*( h->sps->i_mb_width - h->mb.i_mb_x - 1 ) + 24 );
        h->mb.mv_min_spel[0] = CLIP_FMV( h->mb.mv_min[0] );
        h->mb.mv_max_spel[0] = CLIP_FMV( h->mb.mv_max[0] );
        if( h->param.b_intra_refresh && h->sh.i_type == SLICE_TYPE_P )
        {
            int max_x = (h->fref0[0]->i_pir_end_col * 16 - 3)*4; /* 3 pixels of hpel border */
            int max_mv = max_x - 4*16*h->mb.i_mb_x;
            /* If we're left of the refresh bar, don't reference right of it. */
            if( max_mv > 0 && h->mb.i_mb_x < h->fdec->i_pir_start_col )
                h->mb.mv_max_spel[0] = X264_MIN( h->mb.mv_max_spel[0], max_mv );
        }
        h->mb.mv_min_fpel[0] = (h->mb.mv_min_spel[0]>>2) + i_fpel_border;
        h->mb.mv_max_fpel[0] = (h->mb.mv_max_spel[0]>>2) - i_fpel_border;
        if( h->mb.i_mb_x == 0 )
        {
            int mb_y = h->mb.i_mb_y >> h->sh.b_mbaff;
            int mb_height = h->sps->i_mb_height >> h->sh.b_mbaff;
            int thread_mvy_range = i_fmv_range;

            if( h->i_thread_frames > 1 )
            {
                int pix_y = (h->mb.i_mb_y | h->mb.b_interlaced) * 16;
                int thresh = pix_y + h->param.analyse.i_mv_range_thread;
                for( int i = (h->sh.i_type == SLICE_TYPE_B); i >= 0; i-- )
                {
                    x264_frame_t **fref = i ? h->fref1 : h->fref0;
                    int i_ref = i ? h->i_ref1 : h->i_ref0;
                    for( int j = 0; j < i_ref; j++ )
                    {
                        x264_frame_cond_wait( fref[j]->orig, thresh );
                        thread_mvy_range = X264_MIN( thread_mvy_range, fref[j]->orig->i_lines_completed - pix_y );
                    }
                }

                if( h->param.b_deterministic )
                    thread_mvy_range = h->param.analyse.i_mv_range_thread;
                if( h->mb.b_interlaced )
                    thread_mvy_range >>= 1;

                x264_analyse_weight_frame( h, pix_y + thread_mvy_range );
            }

            h->mb.mv_min[1] = 4*( -16*mb_y - 24 );
            h->mb.mv_max[1] = 4*( 16*( mb_height - mb_y - 1 ) + 24 );
            h->mb.mv_min_spel[1] = x264_clip3( h->mb.mv_min[1], -i_fmv_range, i_fmv_range );
            h->mb.mv_max_spel[1] = CLIP_FMV( h->mb.mv_max[1] );
            h->mb.mv_max_spel[1] = X264_MIN( h->mb.mv_max_spel[1], thread_mvy_range*4 );
            h->mb.mv_min_fpel[1] = (h->mb.mv_min_spel[1]>>2) + i_fpel_border;
            h->mb.mv_max_fpel[1] = (h->mb.mv_max_spel[1]>>2) - i_fpel_border;
        }
#undef CLIP_FMV

        a->l0.me16x16.cost =
        a->l0.i_rd16x16    =
        a->l0.i_cost8x8    =
        a->l0.i_cost16x8   =
        a->l0.i_cost8x16   = COST_MAX;
        if( h->sh.i_type == SLICE_TYPE_B )
        {
            a->l1.me16x16.cost =
            a->l1.i_rd16x16    =
            a->l1.i_cost8x8    =
            a->i_cost8x8direct[0] =
            a->i_cost8x8direct[1] =
            a->i_cost8x8direct[2] =
            a->i_cost8x8direct[3] =
            a->l1.i_cost16x8   =
            a->l1.i_cost8x16   =
            a->i_rd16x16bi     =
            a->i_rd16x16direct =
            a->i_rd8x8bi       =
            a->i_rd16x8bi      =
            a->i_rd8x16bi      =
            a->i_cost16x16bi   =
            a->i_cost16x16direct =
            a->i_cost8x8bi     =
            a->i_cost16x8bi    =
            a->i_cost8x16bi    = COST_MAX;
        }
        else if( h->param.analyse.inter & X264_ANALYSE_PSUB8x8 )
            for( int i = 0; i < 4; i++ )
            {
                a->l0.i_cost4x4[i] =
                a->l0.i_cost8x4[i] =
                a->l0.i_cost4x8[i] = COST_MAX;
            }

        /* Fast intra decision */
        if( h->mb.i_mb_xy - h->sh.i_first_mb > 4 )
        {
            /* Always run in fast-intra mode for subme < 3 */
            if( h->mb.i_subpel_refine > 2 &&
              ( IS_INTRA( h->mb.i_mb_type_left ) ||
                IS_INTRA( h->mb.i_mb_type_top ) ||
                IS_INTRA( h->mb.i_mb_type_topleft ) ||
                IS_INTRA( h->mb.i_mb_type_topright ) ||
                (h->sh.i_type == SLICE_TYPE_P && IS_INTRA( h->fref0[0]->mb_type[h->mb.i_mb_xy] )) ||
                (h->mb.i_mb_xy - h->sh.i_first_mb < 3*(h->stat.frame.i_mb_count[I_4x4] + h->stat.frame.i_mb_count[I_8x8] + h->stat.frame.i_mb_count[I_16x16])) ) )
            { /* intra is likely */ }
            else
            {
                a->b_fast_intra = 1;
            }
        }
        h->mb.b_skip_mc = 0;
        if( h->param.b_intra_refresh && h->sh.i_type == SLICE_TYPE_P &&
            h->mb.i_mb_x >= h->fdec->i_pir_start_col && h->mb.i_mb_x <= h->fdec->i_pir_end_col )
        {
            a->b_force_intra = 1;
            a->b_fast_intra = 0;
        }
        else
            a->b_force_intra = 0;
    }
}

/* Prediction modes allowed for various combinations of neighbors. */
/* Terminated by a -1. */
/* In order, no neighbors, left, top, top/left, top/left/topleft */
static const int8_t i16x16_mode_available[5][5] =
{
    {I_PRED_16x16_DC_128, -1, -1, -1, -1},
    {I_PRED_16x16_DC_LEFT, I_PRED_16x16_H, -1, -1, -1},
    {I_PRED_16x16_DC_TOP, I_PRED_16x16_V, -1, -1, -1},
    {I_PRED_16x16_V, I_PRED_16x16_H, I_PRED_16x16_DC, -1, -1},
    {I_PRED_16x16_V, I_PRED_16x16_H, I_PRED_16x16_DC, I_PRED_16x16_P, -1},
};

static const int8_t i8x8chroma_mode_available[5][5] =
{
    {I_PRED_CHROMA_DC_128, -1, -1, -1, -1},
    {I_PRED_CHROMA_DC_LEFT, I_PRED_CHROMA_H, -1, -1, -1},
    {I_PRED_CHROMA_DC_TOP, I_PRED_CHROMA_V, -1, -1, -1},
    {I_PRED_CHROMA_V, I_PRED_CHROMA_H, I_PRED_CHROMA_DC, -1, -1},
    {I_PRED_CHROMA_V, I_PRED_CHROMA_H, I_PRED_CHROMA_DC, I_PRED_CHROMA_P, -1},
};

static const int8_t i4x4_mode_available[5][10] =
{
    {I_PRED_4x4_DC_128, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {I_PRED_4x4_DC_LEFT, I_PRED_4x4_H, I_PRED_4x4_HU, -1, -1, -1, -1, -1, -1, -1},
    {I_PRED_4x4_DC_TOP, I_PRED_4x4_V, I_PRED_4x4_DDL, I_PRED_4x4_VL, -1, -1, -1, -1, -1, -1},
    {I_PRED_4x4_DC, I_PRED_4x4_H, I_PRED_4x4_V, I_PRED_4x4_DDL, I_PRED_4x4_VL, I_PRED_4x4_HU, -1, -1, -1, -1},
    {I_PRED_4x4_DC, I_PRED_4x4_H, I_PRED_4x4_V, I_PRED_4x4_DDL, I_PRED_4x4_DDR, I_PRED_4x4_VR, I_PRED_4x4_HD, I_PRED_4x4_VL, I_PRED_4x4_HU, -1},
};

static ALWAYS_INLINE const int8_t *predict_16x16_mode_available( int i_neighbour )
{
    int idx = i_neighbour & (MB_TOP|MB_LEFT|MB_TOPLEFT);
    return i16x16_mode_available[(idx&MB_TOPLEFT)?4:idx];
}

static ALWAYS_INLINE const int8_t *predict_8x8chroma_mode_available( int i_neighbour )
{
    int idx = i_neighbour & (MB_TOP|MB_LEFT|MB_TOPLEFT);
    return i8x8chroma_mode_available[(idx&MB_TOPLEFT)?4:idx];
}

static ALWAYS_INLINE const int8_t *predict_4x4_mode_available( int i_neighbour )
{
    int idx = i_neighbour & (MB_TOP|MB_LEFT|MB_TOPLEFT);
    return i4x4_mode_available[(idx&MB_TOPLEFT)?4:idx];
}

/* For trellis=2, we need to do this for both sizes of DCT, for trellis=1 we only need to use it on the chosen mode. */
static void inline x264_psy_trellis_init( x264_t *h, int do_both_dct )
{
    ALIGNED_16( static pixel zero[16*FDEC_STRIDE] ) = {0};

    if( do_both_dct || h->mb.b_transform_8x8 )
        h->dctf.sub16x16_dct8( h->mb.pic.fenc_dct8, h->mb.pic.p_fenc[0], zero );
    if( do_both_dct || !h->mb.b_transform_8x8 )
        h->dctf.sub16x16_dct( h->mb.pic.fenc_dct4, h->mb.pic.p_fenc[0], zero );
}

/* Reset fenc satd scores cache for psy RD */
static inline void x264_mb_init_fenc_cache( x264_t *h, int b_satd )
{
    if( h->param.analyse.i_trellis == 2 && h->mb.i_psy_trellis )
        x264_psy_trellis_init( h, h->param.analyse.b_transform_8x8 );
    if( !h->mb.i_psy_rd )
        return;
    /* Writes beyond the end of the array, but not a problem since fenc_satd_cache is right after. */
    h->mc.memzero_aligned( h->mb.pic.fenc_hadamard_cache, sizeof(h->mb.pic.fenc_hadamard_cache) );
    if( b_satd )
        h->mc.memzero_aligned( h->mb.pic.fenc_satd_cache, sizeof(h->mb.pic.fenc_satd_cache) );
}

static void x264_mb_analyse_intra_chroma( x264_t *h, x264_mb_analysis_t *a )
{
    int b_merged_satd = !!h->pixf.intra_mbcmp_x3_8x8c && !h->mb.b_lossless;

    if( a->i_satd_i8x8chroma < COST_MAX )
        return;

    const int8_t *predict_mode = predict_8x8chroma_mode_available( h->mb.i_neighbour_intra );

    /* 8x8 prediction selection for chroma */
    if( predict_mode[3] >= 0 && b_merged_satd )
    {
        int satdu[4], satdv[4];
        h->pixf.intra_mbcmp_x3_8x8c( h->mb.pic.p_fenc[1], h->mb.pic.p_fdec[1], satdu );
        h->pixf.intra_mbcmp_x3_8x8c( h->mb.pic.p_fenc[2], h->mb.pic.p_fdec[2], satdv );
        h->predict_8x8c[I_PRED_CHROMA_P]( h->mb.pic.p_fdec[1] );
        h->predict_8x8c[I_PRED_CHROMA_P]( h->mb.pic.p_fdec[2] );
        satdu[I_PRED_CHROMA_P] = h->pixf.mbcmp[PIXEL_8x8]( h->mb.pic.p_fdec[1], FDEC_STRIDE, h->mb.pic.p_fenc[1], FENC_STRIDE );
        satdv[I_PRED_CHROMA_P] = h->pixf.mbcmp[PIXEL_8x8]( h->mb.pic.p_fdec[2], FDEC_STRIDE, h->mb.pic.p_fenc[2], FENC_STRIDE );

        for( ; *predict_mode >= 0; predict_mode++ )
        {
            int i_mode = *predict_mode;
            int i_satd = satdu[i_mode] + satdv[i_mode] + a->i_lambda * bs_size_ue( i_mode );

            a->i_satd_i8x8chroma_dir[i_mode] = i_satd;
            COPY2_IF_LT( a->i_satd_i8x8chroma, i_satd, a->i_predict8x8chroma, i_mode );
        }
    }
    else
    {
        for( ; *predict_mode >= 0; predict_mode++ )
        {
            int i_satd;
            int i_mode = *predict_mode;

            /* we do the prediction */
            if( h->mb.b_lossless )
                x264_predict_lossless_8x8_chroma( h, i_mode );
            else
            {
                h->predict_8x8c[i_mode]( h->mb.pic.p_fdec[1] );
                h->predict_8x8c[i_mode]( h->mb.pic.p_fdec[2] );
            }

            /* we calculate the cost */
            i_satd = h->pixf.mbcmp[PIXEL_8x8]( h->mb.pic.p_fdec[1], FDEC_STRIDE, h->mb.pic.p_fenc[1], FENC_STRIDE ) +
                     h->pixf.mbcmp[PIXEL_8x8]( h->mb.pic.p_fdec[2], FDEC_STRIDE, h->mb.pic.p_fenc[2], FENC_STRIDE ) +
                     a->i_lambda * bs_size_ue( x264_mb_pred_mode8x8c_fix[i_mode] );

            a->i_satd_i8x8chroma_dir[i_mode] = i_satd;
            COPY2_IF_LT( a->i_satd_i8x8chroma, i_satd, a->i_predict8x8chroma, i_mode );
        }
    }

    h->mb.i_chroma_pred_mode = a->i_predict8x8chroma;
}

static void x264_mb_analyse_intra( x264_t *h, x264_mb_analysis_t *a, int i_satd_inter )
{
    const unsigned int flags = h->sh.i_type == SLICE_TYPE_I ? h->param.analyse.intra : h->param.analyse.inter;
    pixel *p_src = h->mb.pic.p_fenc[0];
    pixel *p_dst = h->mb.pic.p_fdec[0];

    int idx;
    int b_merged_satd = !!h->pixf.intra_mbcmp_x3_16x16 && !h->mb.b_lossless;

    /*---------------- Try all mode and calculate their score ---------------*/

    /* 16x16 prediction selection */
    const int8_t *predict_mode = predict_16x16_mode_available( h->mb.i_neighbour_intra );

    if( b_merged_satd && predict_mode[3] >= 0 )
    {
        h->pixf.intra_mbcmp_x3_16x16( p_src, p_dst, a->i_satd_i16x16_dir );
        h->predict_16x16[I_PRED_16x16_P]( p_dst );
        a->i_satd_i16x16_dir[I_PRED_16x16_P] =
            h->pixf.mbcmp[PIXEL_16x16]( p_dst, FDEC_STRIDE, p_src, FENC_STRIDE );
        for( int i = 0; i < 4; i++ )
        {
            int cost = a->i_satd_i16x16_dir[i] += a->i_lambda * bs_size_ue(i);
            COPY2_IF_LT( a->i_satd_i16x16, cost, a->i_predict16x16, i );
        }
    }
    else
    {
        for( ; *predict_mode >= 0; predict_mode++ )
        {
            int i_satd;
            int i_mode = *predict_mode;

            if( h->mb.b_lossless )
                x264_predict_lossless_16x16( h, i_mode );
            else
                h->predict_16x16[i_mode]( p_dst );

            i_satd = h->pixf.mbcmp[PIXEL_16x16]( p_dst, FDEC_STRIDE, p_src, FENC_STRIDE ) +
                    a->i_lambda * bs_size_ue( x264_mb_pred_mode16x16_fix[i_mode] );
            COPY2_IF_LT( a->i_satd_i16x16, i_satd, a->i_predict16x16, i_mode );
            a->i_satd_i16x16_dir[i_mode] = i_satd;
        }
    }

    if( h->sh.i_type == SLICE_TYPE_B )
        /* cavlc mb type prefix */
        a->i_satd_i16x16 += a->i_lambda * i_mb_b_cost_table[I_16x16];

    /* Not heavily tuned */
    const uint8_t i16x16_thresh[11] = { 2, 2, 2, 3, 3, 4, 4, 4, 4, 4, 4 };
    if( a->b_fast_intra && a->i_satd_i16x16 > (i16x16_thresh[h->mb.i_subpel_refine]*i_satd_inter)>>1 )
        return;

    /* 8x8 prediction selection */
    if( flags & X264_ANALYSE_I8x8 )
    {
        ALIGNED_ARRAY_16( pixel, edge,[33] );
        x264_pixel_cmp_t sa8d = (h->pixf.mbcmp[0] == h->pixf.satd[0]) ? h->pixf.sa8d[PIXEL_8x8] : h->pixf.mbcmp[PIXEL_8x8];
        int i_satd_thresh = a->i_mbrd ? COST_MAX : X264_MIN( i_satd_inter, a->i_satd_i16x16 );

        // FIXME some bias like in i4x4?
        int i_cost = a->i_lambda * 4; /* base predmode costs */
        h->mb.i_cbp_luma = 0;
        b_merged_satd = h->pixf.intra_mbcmp_x3_8x8 && !h->mb.b_lossless;

        if( h->sh.i_type == SLICE_TYPE_B )
            i_cost += a->i_lambda * i_mb_b_cost_table[I_8x8];

        for( idx = 0;; idx++ )
        {
            int x = idx&1;
            int y = idx>>1;
            pixel *p_src_by = p_src + 8*x + 8*y*FENC_STRIDE;
            pixel *p_dst_by = p_dst + 8*x + 8*y*FDEC_STRIDE;
            int i_best = COST_MAX;
            int i_pred_mode = x264_mb_predict_intra4x4_mode( h, 4*idx );

            predict_mode = predict_4x4_mode_available( h->mb.i_neighbour8[idx] );
            h->predict_8x8_filter( p_dst_by, edge, h->mb.i_neighbour8[idx], ALL_NEIGHBORS );

            if( b_merged_satd && predict_mode[8] >= 0 )
            {
                int satd[9];
                h->pixf.intra_mbcmp_x3_8x8( p_src_by, edge, satd );
                satd[i_pred_mode] -= 3 * a->i_lambda;
                for( int i = 2; i >= 0; i-- )
                {
                    int cost = a->i_satd_i8x8_dir[i][idx] = satd[i];
                    COPY2_IF_LT( i_best, cost, a->i_predict8x8[idx], i );
                }
                predict_mode += 3;
            }

            for( ; *predict_mode >= 0 && (i_best >= 0 || a->i_mbrd >= 2); predict_mode++ )
            {
                int i_satd;
                int i_mode = *predict_mode;

                if( h->mb.b_lossless )
                    x264_predict_lossless_8x8( h, p_dst_by, idx, i_mode, edge );
                else
                    h->predict_8x8[i_mode]( p_dst_by, edge );

                i_satd = sa8d( p_dst_by, FDEC_STRIDE, p_src_by, FENC_STRIDE );
                if( i_pred_mode == x264_mb_pred_mode4x4_fix(i_mode) )
                    i_satd -= 3 * a->i_lambda;

                COPY2_IF_LT( i_best, i_satd, a->i_predict8x8[idx], i_mode );
                a->i_satd_i8x8_dir[i_mode][idx] = i_satd + 4 * a->i_lambda;
            }
            i_cost += i_best + 3 * a->i_lambda;

            if( idx == 3 || i_cost > i_satd_thresh )
                break;

            /* we need to encode this block now (for next ones) */
            h->predict_8x8[a->i_predict8x8[idx]]( p_dst_by, edge );
            x264_mb_encode_i8x8( h, idx, a->i_qp );

            x264_macroblock_cache_intra8x8_pred( h, 2*x, 2*y, a->i_predict8x8[idx] );
        }

        if( idx == 3 )
        {
            a->i_satd_i8x8 = i_cost;
            if( h->mb.i_skip_intra )
            {
                h->mc.copy[PIXEL_16x16]( h->mb.pic.i8x8_fdec_buf, 16, p_dst, FDEC_STRIDE, 16 );
                h->mb.pic.i8x8_nnz_buf[0] = M32( &h->mb.cache.non_zero_count[x264_scan8[ 0]] );
                h->mb.pic.i8x8_nnz_buf[1] = M32( &h->mb.cache.non_zero_count[x264_scan8[ 2]] );
                h->mb.pic.i8x8_nnz_buf[2] = M32( &h->mb.cache.non_zero_count[x264_scan8[ 8]] );
                h->mb.pic.i8x8_nnz_buf[3] = M32( &h->mb.cache.non_zero_count[x264_scan8[10]] );
                h->mb.pic.i8x8_cbp = h->mb.i_cbp_luma;
                if( h->mb.i_skip_intra == 2 )
                    h->mc.memcpy_aligned( h->mb.pic.i8x8_dct_buf, h->dct.luma8x8, sizeof(h->mb.pic.i8x8_dct_buf) );
            }
        }
        else
        {
            static const uint16_t cost_div_fix8[3] = {1024,512,341};
            a->i_satd_i8x8 = COST_MAX;
            i_cost = (i_cost * cost_div_fix8[idx]) >> 8;
        }
        /* Not heavily tuned */
        const uint8_t i8x8_thresh[11] = { 4, 4, 4, 5, 5, 5, 6, 6, 6, 6, 6 };
        if( X264_MIN(i_cost, a->i_satd_i16x16) > (i_satd_inter*i8x8_thresh[h->mb.i_subpel_refine])>>2 )
            return;
    }

    /* 4x4 prediction selection */
    if( flags & X264_ANALYSE_I4x4 )
    {
        int i_cost = a->i_lambda * (24+16); /* 24from JVT (SATD0), 16 from base predmode costs */
        int i_satd_thresh = X264_MIN3( i_satd_inter, a->i_satd_i16x16, a->i_satd_i8x8 );
        h->mb.i_cbp_luma = 0;
        b_merged_satd = h->pixf.intra_mbcmp_x3_4x4 && !h->mb.b_lossless;
        if( a->i_mbrd )
            i_satd_thresh = i_satd_thresh * (10-a->b_fast_intra)/8;

        if( h->sh.i_type == SLICE_TYPE_B )
            i_cost += a->i_lambda * i_mb_b_cost_table[I_4x4];

        for( idx = 0;; idx++ )
        {
            pixel *p_src_by = p_src + block_idx_xy_fenc[idx];
            pixel *p_dst_by = p_dst + block_idx_xy_fdec[idx];
            int i_best = COST_MAX;
            int i_pred_mode = x264_mb_predict_intra4x4_mode( h, idx );

            predict_mode = predict_4x4_mode_available( h->mb.i_neighbour4[idx] );

            if( (h->mb.i_neighbour4[idx] & (MB_TOPRIGHT|MB_TOP)) == MB_TOP )
                /* emulate missing topright samples */
                MPIXEL_X4( &p_dst_by[4 - FDEC_STRIDE] ) = PIXEL_SPLAT_X4( p_dst_by[3 - FDEC_STRIDE] );

            if( b_merged_satd && predict_mode[5] >= 0 )
            {
                int satd[9];
                h->pixf.intra_mbcmp_x3_4x4( p_src_by, p_dst_by, satd );
                satd[i_pred_mode] -= 3 * a->i_lambda;
                for( int i = 2; i >= 0; i-- )
                    COPY2_IF_LT( i_best, satd[i], a->i_predict4x4[idx], i );
                predict_mode += 3;
            }

            if( i_best > 0 )
            {
                for( ; *predict_mode >= 0; predict_mode++ )
                {
                    int i_satd;
                    int i_mode = *predict_mode;

                    if( h->mb.b_lossless )
                        x264_predict_lossless_4x4( h, p_dst_by, idx, i_mode );
                    else
                        h->predict_4x4[i_mode]( p_dst_by );

                    i_satd = h->pixf.mbcmp[PIXEL_4x4]( p_dst_by, FDEC_STRIDE, p_src_by, FENC_STRIDE );
                    if( i_pred_mode == x264_mb_pred_mode4x4_fix(i_mode) )
                    {
                        i_satd -= a->i_lambda * 3;
                        if( i_satd <= 0 )
                        {
                            i_best = i_satd;
                            a->i_predict4x4[idx] = i_mode;
                            break;
                        }
                    }

                    COPY2_IF_LT( i_best, i_satd, a->i_predict4x4[idx], i_mode );
                }
            }
            i_cost += i_best + 3 * a->i_lambda;

            if( i_cost > i_satd_thresh || idx == 15 )
                break;

            /* we need to encode this block now (for next ones) */
            h->predict_4x4[a->i_predict4x4[idx]]( p_dst_by );
            x264_mb_encode_i4x4( h, idx, a->i_qp );

            h->mb.cache.intra4x4_pred_mode[x264_scan8[idx]] = a->i_predict4x4[idx];
        }
        if( idx == 15 )
        {
            a->i_satd_i4x4 = i_cost;
            if( h->mb.i_skip_intra )
            {
                h->mc.copy[PIXEL_16x16]( h->mb.pic.i4x4_fdec_buf, 16, p_dst, FDEC_STRIDE, 16 );
                h->mb.pic.i4x4_nnz_buf[0] = M32( &h->mb.cache.non_zero_count[x264_scan8[ 0]] );
                h->mb.pic.i4x4_nnz_buf[1] = M32( &h->mb.cache.non_zero_count[x264_scan8[ 2]] );
                h->mb.pic.i4x4_nnz_buf[2] = M32( &h->mb.cache.non_zero_count[x264_scan8[ 8]] );
                h->mb.pic.i4x4_nnz_buf[3] = M32( &h->mb.cache.non_zero_count[x264_scan8[10]] );
                h->mb.pic.i4x4_cbp = h->mb.i_cbp_luma;
                if( h->mb.i_skip_intra == 2 )
                    h->mc.memcpy_aligned( h->mb.pic.i4x4_dct_buf, h->dct.luma4x4, sizeof(h->mb.pic.i4x4_dct_buf) );
            }
        }
        else
            a->i_satd_i4x4 = COST_MAX;
    }
}

static void x264_intra_rd( x264_t *h, x264_mb_analysis_t *a, int i_satd_thresh )
{
    if( a->i_satd_i16x16 <= i_satd_thresh )
    {
        h->mb.i_type = I_16x16;
        x264_analyse_update_cache( h, a );
        a->i_satd_i16x16 = x264_rd_cost_mb( h, a->i_lambda2 );
    }
    else
        a->i_satd_i16x16 = COST_MAX;

    if( a->i_satd_i4x4 <= i_satd_thresh && a->i_satd_i4x4 < COST_MAX )
    {
        h->mb.i_type = I_4x4;
        x264_analyse_update_cache( h, a );
        a->i_satd_i4x4 = x264_rd_cost_mb( h, a->i_lambda2 );
    }
    else
        a->i_satd_i4x4 = COST_MAX;

    if( a->i_satd_i8x8 <= i_satd_thresh && a->i_satd_i8x8 < COST_MAX )
    {
        h->mb.i_type = I_8x8;
        x264_analyse_update_cache( h, a );
        a->i_satd_i8x8 = x264_rd_cost_mb( h, a->i_lambda2 );
        a->i_cbp_i8x8_luma = h->mb.i_cbp_luma;
    }
    else
        a->i_satd_i8x8 = COST_MAX;
}

static void x264_intra_rd_refine( x264_t *h, x264_mb_analysis_t *a )
{
    pixel *p_dst = h->mb.pic.p_fdec[0];
    uint64_t i_satd, i_best;
    h->mb.i_skip_intra = 0;

    if( h->mb.i_type == I_16x16 )
    {
        int old_pred_mode = a->i_predict16x16;
        const int8_t *predict_mode = predict_16x16_mode_available( h->mb.i_neighbour_intra );
        int i_thresh = a->i_satd_i16x16_dir[old_pred_mode] * 9/8;
        i_best = a->i_satd_i16x16;
        for( ; *predict_mode >= 0; predict_mode++ )
        {
            int i_mode = *predict_mode;
            if( i_mode == old_pred_mode || a->i_satd_i16x16_dir[i_mode] > i_thresh )
                continue;
            h->mb.i_intra16x16_pred_mode = i_mode;
            i_satd = x264_rd_cost_mb( h, a->i_lambda2 );
            COPY2_IF_LT( i_best, i_satd, a->i_predict16x16, i_mode );
        }
    }

    /* RD selection for chroma prediction */
    const int8_t *predict_mode = predict_8x8chroma_mode_available( h->mb.i_neighbour_intra );
    if( predict_mode[1] >= 0 )
    {
        int8_t predict_mode_sorted[4];
        int i_max;
        int i_thresh = a->i_satd_i8x8chroma * 5/4;

        for( i_max = 0; *predict_mode >= 0; predict_mode++ )
        {
            int i_mode = *predict_mode;
            if( a->i_satd_i8x8chroma_dir[i_mode] < i_thresh && i_mode != a->i_predict8x8chroma )
                predict_mode_sorted[i_max++] = i_mode;
        }

        if( i_max > 0 )
        {
            int i_cbp_chroma_best = h->mb.i_cbp_chroma;
            int i_chroma_lambda = x264_lambda2_tab[h->mb.i_chroma_qp];
            /* the previous thing encoded was x264_intra_rd(), so the pixels and
             * coefs for the current chroma mode are still around, so we only
             * have to recount the bits. */
            i_best = x264_rd_cost_i8x8_chroma( h, i_chroma_lambda, a->i_predict8x8chroma, 0 );
            for( int i = 0; i < i_max; i++ )
            {
                int i_mode = predict_mode_sorted[i];
                if( h->mb.b_lossless )
                    x264_predict_lossless_8x8_chroma( h, i_mode );
                else
                {
                    h->predict_8x8c[i_mode]( h->mb.pic.p_fdec[1] );
                    h->predict_8x8c[i_mode]( h->mb.pic.p_fdec[2] );
                }
                /* if we've already found a mode that needs no residual, then
                 * probably any mode with a residual will be worse.
                 * so avoid dct on the remaining modes to improve speed. */
                i_satd = x264_rd_cost_i8x8_chroma( h, i_chroma_lambda, i_mode, h->mb.i_cbp_chroma != 0x00 );
                COPY3_IF_LT( i_best, i_satd, a->i_predict8x8chroma, i_mode, i_cbp_chroma_best, h->mb.i_cbp_chroma );
            }
            h->mb.i_chroma_pred_mode = a->i_predict8x8chroma;
            h->mb.i_cbp_chroma = i_cbp_chroma_best;
        }
    }

    if( h->mb.i_type == I_4x4 )
    {
        pixel4 pels[4] = {0}; // doesn't need initting, just shuts up a gcc warning
        int i_nnz = 0;
        for( int idx = 0; idx < 16; idx++ )
        {
            pixel *p_dst_by = p_dst + block_idx_xy_fdec[idx];
            i_best = COST_MAX64;

            predict_mode = predict_4x4_mode_available( h->mb.i_neighbour4[idx] );

            if( (h->mb.i_neighbour4[idx] & (MB_TOPRIGHT|MB_TOP)) == MB_TOP )
                /* emulate missing topright samples */
                MPIXEL_X4( &p_dst_by[4 - FDEC_STRIDE] ) = PIXEL_SPLAT_X4( p_dst_by[3 - FDEC_STRIDE] );

            for( ; *predict_mode >= 0; predict_mode++ )
            {
                int i_mode = *predict_mode;
                if( h->mb.b_lossless )
                    x264_predict_lossless_4x4( h, p_dst_by, idx, i_mode );
                else
                    h->predict_4x4[i_mode]( p_dst_by );
                i_satd = x264_rd_cost_i4x4( h, a->i_lambda2, idx, i_mode );

                if( i_best > i_satd )
                {
                    a->i_predict4x4[idx] = i_mode;
                    i_best = i_satd;
                    pels[0] = MPIXEL_X4( p_dst_by+0*FDEC_STRIDE );
                    pels[1] = MPIXEL_X4( p_dst_by+1*FDEC_STRIDE );
                    pels[2] = MPIXEL_X4( p_dst_by+2*FDEC_STRIDE );
                    pels[3] = MPIXEL_X4( p_dst_by+3*FDEC_STRIDE );
                    i_nnz = h->mb.cache.non_zero_count[x264_scan8[idx]];
                }
            }

            MPIXEL_X4( p_dst_by+0*FDEC_STRIDE ) = pels[0];
            MPIXEL_X4( p_dst_by+1*FDEC_STRIDE ) = pels[1];
            MPIXEL_X4( p_dst_by+2*FDEC_STRIDE ) = pels[2];
            MPIXEL_X4( p_dst_by+3*FDEC_STRIDE ) = pels[3];
            h->mb.cache.non_zero_count[x264_scan8[idx]] = i_nnz;

            h->mb.cache.intra4x4_pred_mode[x264_scan8[idx]] = a->i_predict4x4[idx];
        }
    }
    else if( h->mb.i_type == I_8x8 )
    {
        ALIGNED_ARRAY_16( pixel, edge,[33] );
        for( int idx = 0; idx < 4; idx++ )
        {
            pixel4 pels_h[2] = {0};
            pixel pels_v[7];
            uint16_t i_nnz[2] = {0}; //shut up gcc
            pixel *p_dst_by;
            int cbp_luma_new = 0;
            int i_thresh = a->i_satd_i8x8_dir[a->i_predict8x8[idx]][idx] * 11/8;

            i_best = COST_MAX64;
            int x = idx&1;
            int y = idx>>1;
            int s8 = X264_SCAN8_0 + 2*x + 16*y;

            p_dst_by = p_dst + 8*x + 8*y*FDEC_STRIDE;
            predict_mode = predict_4x4_mode_available( h->mb.i_neighbour8[idx] );
            h->predict_8x8_filter( p_dst_by, edge, h->mb.i_neighbour8[idx], ALL_NEIGHBORS );

            for( ; *predict_mode >= 0; predict_mode++ )
            {
                int i_mode = *predict_mode;
                if( a->i_satd_i8x8_dir[i_mode][idx] > i_thresh )
                    continue;

                if( h->mb.b_lossless )
                    x264_predict_lossless_8x8( h, p_dst_by, idx, i_mode, edge );
                else
                    h->predict_8x8[i_mode]( p_dst_by, edge );
                h->mb.i_cbp_luma = a->i_cbp_i8x8_luma;
                i_satd = x264_rd_cost_i8x8( h, a->i_lambda2, idx, i_mode );

                if( i_best > i_satd )
                {
                    a->i_predict8x8[idx] = i_mode;
                    cbp_luma_new = h->mb.i_cbp_luma;
                    i_best = i_satd;

                    pels_h[0] = MPIXEL_X4( p_dst_by+7*FDEC_STRIDE+0 );
                    pels_h[1] = MPIXEL_X4( p_dst_by+7*FDEC_STRIDE+4 );
                    if( !(idx&1) )
                        for( int j = 0; j < 7; j++ )
                            pels_v[j] = p_dst_by[7+j*FDEC_STRIDE];
                    i_nnz[0] = M16( &h->mb.cache.non_zero_count[s8 + 0*8] );
                    i_nnz[1] = M16( &h->mb.cache.non_zero_count[s8 + 1*8] );
                }
            }
            a->i_cbp_i8x8_luma = cbp_luma_new;
            MPIXEL_X4( p_dst_by+7*FDEC_STRIDE+0 ) = pels_h[0];
            MPIXEL_X4( p_dst_by+7*FDEC_STRIDE+4 ) = pels_h[1];
            if( !(idx&1) )
                for( int j = 0; j < 7; j++ )
                    p_dst_by[7+j*FDEC_STRIDE] = pels_v[j];
            M16( &h->mb.cache.non_zero_count[s8 + 0*8] ) = i_nnz[0];
            M16( &h->mb.cache.non_zero_count[s8 + 1*8] ) = i_nnz[1];

            x264_macroblock_cache_intra8x8_pred( h, 2*x, 2*y, a->i_predict8x8[idx] );
        }
    }
}

#define LOAD_FENC( m, src, xoff, yoff) \
    (m)->p_cost_mv = a->p_cost_mv; \
    (m)->i_stride[0] = h->mb.pic.i_stride[0]; \
    (m)->i_stride[1] = h->mb.pic.i_stride[1]; \
    (m)->p_fenc[0] = &(src)[0][(xoff)+(yoff)*FENC_STRIDE]; \
    (m)->p_fenc[1] = &(src)[1][((xoff)>>1)+((yoff)>>1)*FENC_STRIDE]; \
    (m)->p_fenc[2] = &(src)[2][((xoff)>>1)+((yoff)>>1)*FENC_STRIDE];

#define LOAD_HPELS(m, src, list, ref, xoff, yoff) \
    (m)->p_fref_w = (m)->p_fref[0] = &(src)[0][(xoff)+(yoff)*(m)->i_stride[0]]; \
    (m)->p_fref[1] = &(src)[1][(xoff)+(yoff)*(m)->i_stride[0]]; \
    (m)->p_fref[2] = &(src)[2][(xoff)+(yoff)*(m)->i_stride[0]]; \
    (m)->p_fref[3] = &(src)[3][(xoff)+(yoff)*(m)->i_stride[0]]; \
    (m)->p_fref[4] = &(src)[4][((xoff)>>1)+((yoff)>>1)*(m)->i_stride[1]]; \
    (m)->p_fref[5] = &(src)[5][((xoff)>>1)+((yoff)>>1)*(m)->i_stride[1]]; \
    (m)->integral = &h->mb.pic.p_integral[list][ref][(xoff)+(yoff)*(m)->i_stride[0]]; \
    (m)->weight = weight_none; \
    (m)->i_ref = ref;

#define LOAD_WPELS(m, src, list, ref, xoff, yoff) \
    (m)->p_fref_w = &(src)[(xoff)+(yoff)*(m)->i_stride[0]]; \
    (m)->weight = h->sh.weight[i_ref];

#define REF_COST(list, ref) \
    (a->p_cost_ref[list][ref])

static void x264_mb_analyse_inter_p16x16( x264_t *h, x264_mb_analysis_t *a )
{
    x264_me_t m;
    int i_mvc;
    ALIGNED_4( int16_t mvc[8][2] );
    int i_halfpel_thresh = INT_MAX;
    int *p_halfpel_thresh = h->mb.pic.i_fref[0]>1 ? &i_halfpel_thresh : NULL;

    /* 16x16 Search on all ref frame */
    m.i_pixel = PIXEL_16x16;
    LOAD_FENC( &m, h->mb.pic.p_fenc, 0, 0 );

    a->l0.me16x16.cost = INT_MAX;
    for( int i_ref = 0; i_ref < h->mb.pic.i_fref[0]; i_ref++ )
    {
        m.i_ref_cost = REF_COST( 0, i_ref );
        i_halfpel_thresh -= m.i_ref_cost;

        /* search with ref */
        LOAD_HPELS( &m, h->mb.pic.p_fref[0][i_ref], 0, i_ref, 0, 0 );
        LOAD_WPELS( &m, h->mb.pic.p_fref_w[i_ref], 0, i_ref, 0, 0 );

        x264_mb_predict_mv_16x16( h, 0, i_ref, m.mvp );

        if( h->mb.ref_blind_dupe == i_ref )
        {
            CP32( m.mv, a->l0.mvc[0][0] );
            x264_me_refine_qpel_refdupe( h, &m, p_halfpel_thresh );
        }
        else
        {
            x264_mb_predict_mv_ref16x16( h, 0, i_ref, mvc, &i_mvc );
            x264_me_search_ref( h, &m, mvc, i_mvc, p_halfpel_thresh );
        }

        /* save mv for predicting neighbors */
        CP32( h->mb.mvr[0][i_ref][h->mb.i_mb_xy], m.mv );
        CP32( a->l0.mvc[i_ref][0], m.mv );

        /* early termination
         * SSD threshold would probably be better than SATD */
        if( i_ref == 0
            && a->b_try_skip
            && m.cost-m.cost_mv < 300*a->i_lambda
            &&  abs(m.mv[0]-h->mb.cache.pskip_mv[0])
              + abs(m.mv[1]-h->mb.cache.pskip_mv[1]) <= 1
            && x264_macroblock_probe_pskip( h ) )
        {
            h->mb.i_type = P_SKIP;
            x264_analyse_update_cache( h, a );
            assert( h->mb.cache.pskip_mv[1] <= h->mb.mv_max_spel[1] || h->i_thread_frames == 1 );
            return;
        }

        m.cost += m.i_ref_cost;
        i_halfpel_thresh += m.i_ref_cost;

        if( m.cost < a->l0.me16x16.cost )
            h->mc.memcpy_aligned( &a->l0.me16x16, &m, sizeof(x264_me_t) );
    }

    x264_macroblock_cache_ref( h, 0, 0, 4, 4, 0, a->l0.me16x16.i_ref );
    assert( a->l0.me16x16.mv[1] <= h->mb.mv_max_spel[1] || h->i_thread_frames == 1 );

    h->mb.i_type = P_L0;
    if( a->i_mbrd )
    {
        x264_mb_init_fenc_cache( h, a->i_mbrd >= 2 || h->param.analyse.inter & X264_ANALYSE_PSUB8x8 );
        if( a->l0.me16x16.i_ref == 0 && M32( a->l0.me16x16.mv ) == M32( h->mb.cache.pskip_mv ) && !a->b_force_intra )
        {
            h->mb.i_partition = D_16x16;
            x264_macroblock_cache_mv_ptr( h, 0, 0, 4, 4, 0, a->l0.me16x16.mv );
            a->l0.i_rd16x16 = x264_rd_cost_mb( h, a->i_lambda2 );
            if( !(h->mb.i_cbp_luma|h->mb.i_cbp_chroma) )
                h->mb.i_type = P_SKIP;
        }
    }
}

static void x264_mb_analyse_inter_p8x8_mixed_ref( x264_t *h, x264_mb_analysis_t *a )
{
    x264_me_t m;
    pixel **p_fenc = h->mb.pic.p_fenc;
    int i_maxref = h->mb.pic.i_fref[0]-1;

    h->mb.i_partition = D_8x8;

    #define CHECK_NEIGHBOUR(i)\
    {\
        int ref = h->mb.cache.ref[0][X264_SCAN8_0+i];\
        if( ref > i_maxref && ref != h->mb.ref_blind_dupe )\
            i_maxref = ref;\
    }

    /* early termination: if 16x16 chose ref 0, then evalute no refs older
     * than those used by the neighbors */
    if( i_maxref > 0 && (a->l0.me16x16.i_ref == 0 || a->l0.me16x16.i_ref == h->mb.ref_blind_dupe) &&
        h->mb.i_mb_type_top > 0 && h->mb.i_mb_type_left > 0 )
    {
        i_maxref = 0;
        CHECK_NEIGHBOUR(  -8 - 1 );
        CHECK_NEIGHBOUR(  -8 + 0 );
        CHECK_NEIGHBOUR(  -8 + 2 );
        CHECK_NEIGHBOUR(  -8 + 4 );
        CHECK_NEIGHBOUR(   0 - 1 );
        CHECK_NEIGHBOUR( 2*8 - 1 );
    }
    #undef CHECK_NEIGHBOUR

    for( int i_ref = 0; i_ref <= i_maxref; i_ref++ )
        CP32( a->l0.mvc[i_ref][0], h->mb.mvr[0][i_ref][h->mb.i_mb_xy] );

    for( int i = 0; i < 4; i++ )
    {
        x264_me_t *l0m = &a->l0.me8x8[i];
        const int x8 = i%2;
        const int y8 = i/2;

        m.i_pixel = PIXEL_8x8;

        LOAD_FENC( &m, p_fenc, 8*x8, 8*y8 );
        l0m->cost = INT_MAX;
        for( int i_ref = 0; i_ref <= i_maxref || i_ref == h->mb.ref_blind_dupe; )
        {
            m.i_ref_cost = REF_COST( 0, i_ref );

            LOAD_HPELS( &m, h->mb.pic.p_fref[0][i_ref], 0, i_ref, 8*x8, 8*y8 );
            LOAD_WPELS( &m, h->mb.pic.p_fref_w[i_ref], 0, i_ref, 8*x8, 8*y8 );

            x264_macroblock_cache_ref( h, 2*x8, 2*y8, 2, 2, 0, i_ref );
            x264_mb_predict_mv( h, 0, 4*i, 2, m.mvp );
            if( h->mb.ref_blind_dupe == i_ref )
            {
                CP32( m.mv, a->l0.mvc[0][i+1] );
                x264_me_refine_qpel_refdupe( h, &m, NULL );
            }
            else
                x264_me_search( h, &m, a->l0.mvc[i_ref], i+1 );

            m.cost += m.i_ref_cost;

            CP32( a->l0.mvc[i_ref][i+1], m.mv );

            if( m.cost < l0m->cost )
                h->mc.memcpy_aligned( l0m, &m, sizeof(x264_me_t) );
            if( i_ref == i_maxref && i_maxref < h->mb.ref_blind_dupe )
                i_ref = h->mb.ref_blind_dupe;
            else
                i_ref++;
        }
        x264_macroblock_cache_mv_ptr( h, 2*x8, 2*y8, 2, 2, 0, l0m->mv );
        x264_macroblock_cache_ref( h, 2*x8, 2*y8, 2, 2, 0, l0m->i_ref );

        a->i_satd8x8[0][i] = l0m->cost - ( l0m->cost_mv + l0m->i_ref_cost );

        /* If CABAC is on and we're not doing sub-8x8 analysis, the costs
           are effectively zero. */
        if( !h->param.b_cabac || (h->param.analyse.inter & X264_ANALYSE_PSUB8x8) )
            l0m->cost += a->i_lambda * i_sub_mb_p_cost_table[D_L0_8x8];
    }

    a->l0.i_cost8x8 = a->l0.me8x8[0].cost + a->l0.me8x8[1].cost +
                      a->l0.me8x8[2].cost + a->l0.me8x8[3].cost;
    /* P_8x8 ref0 has no ref cost */
    if( !h->param.b_cabac && !(a->l0.me8x8[0].i_ref | a->l0.me8x8[1].i_ref |
                               a->l0.me8x8[2].i_ref | a->l0.me8x8[3].i_ref) )
        a->l0.i_cost8x8 -= REF_COST( 0, 0 ) * 4;
    h->mb.i_sub_partition[0] = h->mb.i_sub_partition[1] =
    h->mb.i_sub_partition[2] = h->mb.i_sub_partition[3] = D_L0_8x8;
}

static void x264_mb_analyse_inter_p8x8( x264_t *h, x264_mb_analysis_t *a )
{
    /* Duplicate refs are rarely useful in p8x8 due to the high cost of the
     * reference frame flags.  Thus, if we're not doing mixedrefs, just
     * don't bother analysing the dupes. */
    const int i_ref = h->mb.ref_blind_dupe == a->l0.me16x16.i_ref ? 0 : a->l0.me16x16.i_ref;
    const int i_ref_cost = h->param.b_cabac || i_ref ? REF_COST( 0, i_ref ) : 0;
    pixel **p_fenc = h->mb.pic.p_fenc;
    int i_mvc;
    int16_t (*mvc)[2] = a->l0.mvc[i_ref];

    /* XXX Needed for x264_mb_predict_mv */
    h->mb.i_partition = D_8x8;

    i_mvc = 1;
    CP32( mvc[0], a->l0.me16x16.mv );

    for( int i = 0; i < 4; i++ )
    {
        x264_me_t *m = &a->l0.me8x8[i];
        const int x8 = i%2;
        const int y8 = i/2;

        m->i_pixel = PIXEL_8x8;
        m->i_ref_cost = i_ref_cost;

        LOAD_FENC( m, p_fenc, 8*x8, 8*y8 );
        LOAD_HPELS( m, h->mb.pic.p_fref[0][i_ref], 0, i_ref, 8*x8, 8*y8 );
        LOAD_WPELS( m, h->mb.pic.p_fref_w[i_ref], 0, i_ref, 8*x8, 8*y8 );

        x264_mb_predict_mv( h, 0, 4*i, 2, m->mvp );
        x264_me_search( h, m, mvc, i_mvc );

        x264_macroblock_cache_mv_ptr( h, 2*x8, 2*y8, 2, 2, 0, m->mv );

        CP32( mvc[i_mvc], m->mv );
        i_mvc++;

        a->i_satd8x8[0][i] = m->cost - m->cost_mv;

        /* mb type cost */
        m->cost += i_ref_cost;
        if( !h->param.b_cabac || (h->param.analyse.inter & X264_ANALYSE_PSUB8x8) )
            m->cost += a->i_lambda * i_sub_mb_p_cost_table[D_L0_8x8];
    }

    a->l0.i_cost8x8 = a->l0.me8x8[0].cost + a->l0.me8x8[1].cost +
                      a->l0.me8x8[2].cost + a->l0.me8x8[3].cost;
    /* theoretically this should include 4*ref_cost,
     * but 3 seems a better approximation of cabac. */
    if( h->param.b_cabac )
        a->l0.i_cost8x8 -= i_ref_cost;
    h->mb.i_sub_partition[0] = h->mb.i_sub_partition[1] =
    h->mb.i_sub_partition[2] = h->mb.i_sub_partition[3] = D_L0_8x8;
}

static void x264_mb_analyse_inter_p16x8( x264_t *h, x264_mb_analysis_t *a, int i_best_satd )
{
    x264_me_t m;
    pixel **p_fenc = h->mb.pic.p_fenc;
    ALIGNED_4( int16_t mvc[3][2] );

    /* XXX Needed for x264_mb_predict_mv */
    h->mb.i_partition = D_16x8;

    for( int i = 0; i < 2; i++ )
    {
        x264_me_t *l0m = &a->l0.me16x8[i];
        const int minref = X264_MIN( a->l0.me8x8[2*i].i_ref, a->l0.me8x8[2*i+1].i_ref );
        const int maxref = X264_MAX( a->l0.me8x8[2*i].i_ref, a->l0.me8x8[2*i+1].i_ref );
        const int ref8[2] = { minref, maxref };
        const int i_ref8s = ( ref8[0] == ref8[1] ) ? 1 : 2;

        m.i_pixel = PIXEL_16x8;

        LOAD_FENC( &m, p_fenc, 0, 8*i );
        l0m->cost = INT_MAX;
        for( int j = 0; j < i_ref8s; j++ )
        {
            const int i_ref = ref8[j];
            m.i_ref_cost = REF_COST( 0, i_ref );

            /* if we skipped the 16x16 predictor, we wouldn't have to copy anything... */
            CP32( mvc[0], a->l0.mvc[i_ref][0] );
            CP32( mvc[1], a->l0.mvc[i_ref][2*i+1] );
            CP32( mvc[2], a->l0.mvc[i_ref][2*i+2] );

            LOAD_HPELS( &m, h->mb.pic.p_fref[0][i_ref], 0, i_ref, 0, 8*i );
            LOAD_WPELS( &m, h->mb.pic.p_fref_w[i_ref], 0, i_ref, 0, 8*i );

            x264_macroblock_cache_ref( h, 0, 2*i, 4, 2, 0, i_ref );
            x264_mb_predict_mv( h, 0, 8*i, 4, m.mvp );
            /* We can only take this shortcut if the first search was performed on ref0. */
            if( h->mb.ref_blind_dupe == i_ref && !ref8[0] )
            {
                /* We can just leave the MV from the previous ref search. */
                x264_me_refine_qpel_refdupe( h, &m, NULL );
            }
            else
                x264_me_search( h, &m, mvc, 3 );

            m.cost += m.i_ref_cost;

            if( m.cost < l0m->cost )
                h->mc.memcpy_aligned( l0m, &m, sizeof(x264_me_t) );
        }

        /* Early termination based on the current SATD score of partition[0]
           plus the estimated SATD score of partition[1] */
        if( !i && l0m->cost + a->i_cost_est16x8[1] > i_best_satd * (4 + !!a->i_mbrd) / 4 )
        {
            a->l0.i_cost16x8 = COST_MAX;
            return;
        }

        x264_macroblock_cache_mv_ptr( h, 0, 2*i, 4, 2, 0, l0m->mv );
        x264_macroblock_cache_ref( h, 0, 2*i, 4, 2, 0, l0m->i_ref );
    }

    a->l0.i_cost16x8 = a->l0.me16x8[0].cost + a->l0.me16x8[1].cost;
}

static void x264_mb_analyse_inter_p8x16( x264_t *h, x264_mb_analysis_t *a, int i_best_satd )
{
    x264_me_t m;
    pixel **p_fenc = h->mb.pic.p_fenc;
    ALIGNED_4( int16_t mvc[3][2] );

    /* XXX Needed for x264_mb_predict_mv */
    h->mb.i_partition = D_8x16;

    for( int i = 0; i < 2; i++ )
    {
        x264_me_t *l0m = &a->l0.me8x16[i];
        const int minref = X264_MIN( a->l0.me8x8[i].i_ref, a->l0.me8x8[i+2].i_ref );
        const int maxref = X264_MAX( a->l0.me8x8[i].i_ref, a->l0.me8x8[i+2].i_ref );
        const int ref8[2] = { minref, maxref };
        const int i_ref8s = ( ref8[0] == ref8[1] ) ? 1 : 2;

        m.i_pixel = PIXEL_8x16;

        LOAD_FENC( &m, p_fenc, 8*i, 0 );
        l0m->cost = INT_MAX;
        for( int j = 0; j < i_ref8s; j++ )
        {
            const int i_ref = ref8[j];
            m.i_ref_cost = REF_COST( 0, i_ref );

            CP32( mvc[0], a->l0.mvc[i_ref][0] );
            CP32( mvc[1], a->l0.mvc[i_ref][i+1] );
            CP32( mvc[2], a->l0.mvc[i_ref][i+3] );

            LOAD_HPELS( &m, h->mb.pic.p_fref[0][i_ref], 0, i_ref, 8*i, 0 );
            LOAD_WPELS( &m, h->mb.pic.p_fref_w[i_ref], 0, i_ref, 8*i, 0 );

            x264_macroblock_cache_ref( h, 2*i, 0, 2, 4, 0, i_ref );
            x264_mb_predict_mv( h, 0, 4*i, 2, m.mvp );
            /* We can only take this shortcut if the first search was performed on ref0. */
            if( h->mb.ref_blind_dupe == i_ref && !ref8[0] )
            {
                /* We can just leave the MV from the previous ref search. */
                x264_me_refine_qpel_refdupe( h, &m, NULL );
            }
            else
                x264_me_search( h, &m, mvc, 3 );

            m.cost += m.i_ref_cost;

            if( m.cost < l0m->cost )
                h->mc.memcpy_aligned( l0m, &m, sizeof(x264_me_t) );
        }

        /* Early termination based on the current SATD score of partition[0]
           plus the estimated SATD score of partition[1] */
        if( !i && l0m->cost + a->i_cost_est8x16[1] > i_best_satd * (4 + !!a->i_mbrd) / 4 )
        {
            a->l0.i_cost8x16 = COST_MAX;
            return;
        }

        x264_macroblock_cache_mv_ptr( h, 2*i, 0, 2, 4, 0, l0m->mv );
        x264_macroblock_cache_ref( h, 2*i, 0, 2, 4, 0, l0m->i_ref );
    }

    a->l0.i_cost8x16 = a->l0.me8x16[0].cost + a->l0.me8x16[1].cost;
}

static int x264_mb_analyse_inter_p4x4_chroma( x264_t *h, x264_mb_analysis_t *a, pixel **p_fref, int i8x8, int size )
{
    ALIGNED_ARRAY_8( pixel, pix1,[16*8] );
    pixel *pix2 = pix1+8;
    const int i_stride = h->mb.pic.i_stride[1];
    const int or = 4*(i8x8&1) + 2*(i8x8&2)*i_stride;
    const int oe = 4*(i8x8&1) + 2*(i8x8&2)*FENC_STRIDE;
    const int i_ref = a->l0.me8x8[i8x8].i_ref;
    const int mvy_offset = h->mb.b_interlaced & i_ref ? (h->mb.i_mb_y & 1)*4 - 2 : 0;
    x264_weight_t *weight = h->sh.weight[i_ref];

#define CHROMA4x4MC( width, height, me, x, y ) \
    h->mc.mc_chroma( &pix1[x+y*16], 16, &p_fref[4][or+x+y*i_stride], i_stride, (me).mv[0], (me).mv[1]+mvy_offset, width, height ); \
    if( weight[1].weightfn ) \
        weight[1].weightfn[width>>2]( &pix1[x+y*16], 16, &pix1[x+y*16], 16, &weight[1], height ); \
    h->mc.mc_chroma( &pix2[x+y*16], 16, &p_fref[5][or+x+y*i_stride], i_stride, (me).mv[0], (me).mv[1]+mvy_offset, width, height ); \
    if( weight[2].weightfn ) \
        weight[2].weightfn[width>>2]( &pix2[x+y*16], 16, &pix2[x+y*16], 16, &weight[2], height );


    if( size == PIXEL_4x4 )
    {
        x264_me_t *m = a->l0.me4x4[i8x8];
        CHROMA4x4MC( 2,2, m[0], 0,0 );
        CHROMA4x4MC( 2,2, m[1], 2,0 );
        CHROMA4x4MC( 2,2, m[2], 0,2 );
        CHROMA4x4MC( 2,2, m[3], 2,2 );
    }
    else if( size == PIXEL_8x4 )
    {
        x264_me_t *m = a->l0.me8x4[i8x8];
        CHROMA4x4MC( 4,2, m[0], 0,0 );
        CHROMA4x4MC( 4,2, m[1], 0,2 );
    }
    else
    {
        x264_me_t *m = a->l0.me4x8[i8x8];
        CHROMA4x4MC( 2,4, m[0], 0,0 );
        CHROMA4x4MC( 2,4, m[1], 2,0 );
    }

    return h->pixf.mbcmp[PIXEL_4x4]( &h->mb.pic.p_fenc[1][oe], FENC_STRIDE, pix1, 16 )
         + h->pixf.mbcmp[PIXEL_4x4]( &h->mb.pic.p_fenc[2][oe], FENC_STRIDE, pix2, 16 );
}

static void x264_mb_analyse_inter_p4x4( x264_t *h, x264_mb_analysis_t *a, int i8x8 )
{
    pixel **p_fref = h->mb.pic.p_fref[0][a->l0.me8x8[i8x8].i_ref];
    pixel **p_fenc = h->mb.pic.p_fenc;
    const int i_ref = a->l0.me8x8[i8x8].i_ref;

    /* XXX Needed for x264_mb_predict_mv */
    h->mb.i_partition = D_8x8;

    for( int i4x4 = 0; i4x4 < 4; i4x4++ )
    {
        const int idx = 4*i8x8 + i4x4;
        const int x4 = block_idx_x[idx];
        const int y4 = block_idx_y[idx];
        const int i_mvc = (i4x4 == 0);

        x264_me_t *m = &a->l0.me4x4[i8x8][i4x4];

        m->i_pixel = PIXEL_4x4;

        LOAD_FENC( m, p_fenc, 4*x4, 4*y4 );
        LOAD_HPELS( m, p_fref, 0, i_ref, 4*x4, 4*y4 );
        LOAD_WPELS( m, h->mb.pic.p_fref_w[i_ref], 0, i_ref, 4*x4, 4*y4 );

        x264_mb_predict_mv( h, 0, idx, 1, m->mvp );
        x264_me_search( h, m, &a->l0.me8x8[i8x8].mv, i_mvc );

        x264_macroblock_cache_mv_ptr( h, x4, y4, 1, 1, 0, m->mv );
    }
    a->l0.i_cost4x4[i8x8] = a->l0.me4x4[i8x8][0].cost +
                            a->l0.me4x4[i8x8][1].cost +
                            a->l0.me4x4[i8x8][2].cost +
                            a->l0.me4x4[i8x8][3].cost +
                            REF_COST( 0, i_ref ) +
                            a->i_lambda * i_sub_mb_p_cost_table[D_L0_4x4];
    if( h->mb.b_chroma_me )
        a->l0.i_cost4x4[i8x8] += x264_mb_analyse_inter_p4x4_chroma( h, a, p_fref, i8x8, PIXEL_4x4 );
}

static void x264_mb_analyse_inter_p8x4( x264_t *h, x264_mb_analysis_t *a, int i8x8 )
{
    pixel **p_fref = h->mb.pic.p_fref[0][a->l0.me8x8[i8x8].i_ref];
    pixel **p_fenc = h->mb.pic.p_fenc;
    const int i_ref = a->l0.me8x8[i8x8].i_ref;

    /* XXX Needed for x264_mb_predict_mv */
    h->mb.i_partition = D_8x8;

    for( int i8x4 = 0; i8x4 < 2; i8x4++ )
    {
        const int idx = 4*i8x8 + 2*i8x4;
        const int x4 = block_idx_x[idx];
        const int y4 = block_idx_y[idx];
        const int i_mvc = (i8x4 == 0);

        x264_me_t *m = &a->l0.me8x4[i8x8][i8x4];

        m->i_pixel = PIXEL_8x4;

        LOAD_FENC( m, p_fenc, 4*x4, 4*y4 );
        LOAD_HPELS( m, p_fref, 0, i_ref, 4*x4, 4*y4 );
        LOAD_WPELS( m, h->mb.pic.p_fref_w[i_ref], 0, i_ref, 4*x4, 4*y4 );

        x264_mb_predict_mv( h, 0, idx, 2, m->mvp );
        x264_me_search( h, m, &a->l0.me4x4[i8x8][0].mv, i_mvc );

        x264_macroblock_cache_mv_ptr( h, x4, y4, 2, 1, 0, m->mv );
    }
    a->l0.i_cost8x4[i8x8] = a->l0.me8x4[i8x8][0].cost + a->l0.me8x4[i8x8][1].cost +
                            REF_COST( 0, i_ref ) +
                            a->i_lambda * i_sub_mb_p_cost_table[D_L0_8x4];
    if( h->mb.b_chroma_me )
        a->l0.i_cost8x4[i8x8] += x264_mb_analyse_inter_p4x4_chroma( h, a, p_fref, i8x8, PIXEL_8x4 );
}

static void x264_mb_analyse_inter_p4x8( x264_t *h, x264_mb_analysis_t *a, int i8x8 )
{
    pixel **p_fref = h->mb.pic.p_fref[0][a->l0.me8x8[i8x8].i_ref];
    pixel **p_fenc = h->mb.pic.p_fenc;
    const int i_ref = a->l0.me8x8[i8x8].i_ref;

    /* XXX Needed for x264_mb_predict_mv */
    h->mb.i_partition = D_8x8;

    for( int i4x8 = 0; i4x8 < 2; i4x8++ )
    {
        const int idx = 4*i8x8 + i4x8;
        const int x4 = block_idx_x[idx];
        const int y4 = block_idx_y[idx];
        const int i_mvc = (i4x8 == 0);

        x264_me_t *m = &a->l0.me4x8[i8x8][i4x8];

        m->i_pixel = PIXEL_4x8;

        LOAD_FENC( m, p_fenc, 4*x4, 4*y4 );
        LOAD_HPELS( m, p_fref, 0, i_ref, 4*x4, 4*y4 );
        LOAD_WPELS( m, h->mb.pic.p_fref_w[i_ref], 0, i_ref, 4*x4, 4*y4 );

        x264_mb_predict_mv( h, 0, idx, 1, m->mvp );
        x264_me_search( h, m, &a->l0.me4x4[i8x8][0].mv, i_mvc );

        x264_macroblock_cache_mv_ptr( h, x4, y4, 1, 2, 0, m->mv );
    }
    a->l0.i_cost4x8[i8x8] = a->l0.me4x8[i8x8][0].cost + a->l0.me4x8[i8x8][1].cost +
                            REF_COST( 0, i_ref ) +
                            a->i_lambda * i_sub_mb_p_cost_table[D_L0_4x8];
    if( h->mb.b_chroma_me )
        a->l0.i_cost4x8[i8x8] += x264_mb_analyse_inter_p4x4_chroma( h, a, p_fref, i8x8, PIXEL_4x8 );
}

static void x264_mb_analyse_inter_direct( x264_t *h, x264_mb_analysis_t *a )
{
    /* Assumes that fdec still contains the results of
     * x264_mb_predict_mv_direct16x16 and x264_mb_mc */

    pixel *p_fenc = h->mb.pic.p_fenc[0];
    pixel *p_fdec = h->mb.pic.p_fdec[0];

    a->i_cost16x16direct = a->i_lambda * i_mb_b_cost_table[B_DIRECT];
    if( h->param.analyse.inter & X264_ANALYSE_BSUB16x16 )
        for( int i = 0; i < 4; i++ )
        {
            const int x = (i&1)*8;
            const int y = (i>>1)*8;
            a->i_cost16x16direct +=
            a->i_cost8x8direct[i] =
                h->pixf.mbcmp[PIXEL_8x8]( &p_fenc[x+y*FENC_STRIDE], FENC_STRIDE, &p_fdec[x+y*FDEC_STRIDE], FDEC_STRIDE );

            /* mb type cost */
            a->i_cost8x8direct[i] += a->i_lambda * i_sub_mb_b_cost_table[D_DIRECT_8x8];
        }
    else
        a->i_cost16x16direct += h->pixf.mbcmp[PIXEL_16x16]( p_fenc, FENC_STRIDE, p_fdec, FDEC_STRIDE );
}

static void x264_mb_analyse_inter_b16x16( x264_t *h, x264_mb_analysis_t *a )
{
    ALIGNED_ARRAY_16( pixel, pix0,[16*16] );
    ALIGNED_ARRAY_16( pixel, pix1,[16*16] );
    pixel *src0, *src1;
    int stride0 = 16, stride1 = 16;
    int i_ref, i_mvc;
    ALIGNED_4( int16_t mvc[9][2] );
    int try_skip = a->b_try_skip;
    int list1_skipped = 0;
    int i_halfpel_thresh[2] = {INT_MAX, INT_MAX};
    int *p_halfpel_thresh[2] = {h->mb.pic.i_fref[0]>1 ? &i_halfpel_thresh[0] : NULL,
                                h->mb.pic.i_fref[1]>1 ? &i_halfpel_thresh[1] : NULL};

    x264_me_t m;
    m.i_pixel = PIXEL_16x16;

    LOAD_FENC( &m, h->mb.pic.p_fenc, 0, 0 );

    /* 16x16 Search on list 0 and list 1 */
    a->l0.me16x16.cost = INT_MAX;
    a->l1.me16x16.cost = INT_MAX;
    for( int l = 1; l >= 0; )
    {
        x264_mb_analysis_list_t *lX = l ? &a->l1 : &a->l0;

        /* This loop is extremely munged in order to facilitate the following order of operations,
         * necessary for an efficient fast skip.
         * 1.  Search list1 ref0.
         * 2.  Search list0 ref0.
         * 3.  Try skip.
         * 4.  Search the rest of list0.
         * 5.  Go back and finish list1.
         */
        for( i_ref = (list1_skipped && l == 1) ? 1 : 0; i_ref < h->mb.pic.i_fref[l]; i_ref++ )
        {
            if( try_skip && l == 1 && i_ref > 0 )
            {
                list1_skipped = 1;
                break;
            }

            m.i_ref_cost = REF_COST( l, i_ref );

            /* search with ref */
            LOAD_HPELS( &m, h->mb.pic.p_fref[l][i_ref], l, i_ref, 0, 0 );
            x264_mb_predict_mv_16x16( h, l, i_ref, m.mvp );
            x264_mb_predict_mv_ref16x16( h, l, i_ref, mvc, &i_mvc );
            x264_me_search_ref( h, &m, mvc, i_mvc, p_halfpel_thresh[l] );

            /* add ref cost */
            m.cost += m.i_ref_cost;

            if( m.cost < lX->me16x16.cost )
                h->mc.memcpy_aligned( &lX->me16x16, &m, sizeof(x264_me_t) );

            /* save mv for predicting neighbors */
            CP32( lX->mvc[i_ref][0], m.mv );
            CP32( h->mb.mvr[l][i_ref][h->mb.i_mb_xy], m.mv );

            /* Fast skip detection. */
            if( i_ref == 0 && try_skip )
            {
                if( abs(lX->me16x16.mv[0]-h->mb.cache.direct_mv[l][0][0]) +
                    abs(lX->me16x16.mv[1]-h->mb.cache.direct_mv[l][0][1]) > 1 )
                {
                    try_skip = 0;
                }
                else if( !l )
                {
                    /* We already tested skip */
                    h->mb.i_type = B_SKIP;
                    x264_analyse_update_cache( h, a );
                    return;
                }
            }
        }
        if( list1_skipped && l == 1 && i_ref == h->mb.pic.i_fref[1] )
            break;
        if( list1_skipped && l == 0 )
            l = 1;
        else
            l--;
    }

    /* get cost of BI mode */
    h->mc.memcpy_aligned( &a->l0.bi16x16, &a->l0.me16x16, sizeof(x264_me_t) );
    h->mc.memcpy_aligned( &a->l1.bi16x16, &a->l1.me16x16, sizeof(x264_me_t) );
    int ref_costs = REF_COST( 0, a->l0.bi16x16.i_ref ) + REF_COST( 1, a->l1.bi16x16.i_ref );
    src0 = h->mc.get_ref( pix0, &stride0,
                          h->mb.pic.p_fref[0][a->l0.bi16x16.i_ref], h->mb.pic.i_stride[0],
                          a->l0.bi16x16.mv[0], a->l0.bi16x16.mv[1], 16, 16, weight_none );
    src1 = h->mc.get_ref( pix1, &stride1,
                          h->mb.pic.p_fref[1][a->l1.bi16x16.i_ref], h->mb.pic.i_stride[0],
                          a->l1.bi16x16.mv[0], a->l1.bi16x16.mv[1], 16, 16, weight_none );

    h->mc.avg[PIXEL_16x16]( pix0, 16, src0, stride0, src1, stride1, h->mb.bipred_weight[a->l0.bi16x16.i_ref][a->l1.bi16x16.i_ref] );

    a->i_cost16x16bi = h->pixf.mbcmp[PIXEL_16x16]( h->mb.pic.p_fenc[0], FENC_STRIDE, pix0, 16 )
                     + ref_costs
                     + a->l0.bi16x16.cost_mv
                     + a->l1.bi16x16.cost_mv;

    /* Always try the 0,0,0,0 vector; helps avoid errant motion vectors in fades */
    if( M32( a->l0.bi16x16.mv ) | M32( a->l1.bi16x16.mv ) )
    {
        int l0_mv_cost = a->l0.bi16x16.p_cost_mv[-a->l0.bi16x16.mvp[0]]
                       + a->l0.bi16x16.p_cost_mv[-a->l0.bi16x16.mvp[1]];
        int l1_mv_cost = a->l1.bi16x16.p_cost_mv[-a->l1.bi16x16.mvp[0]]
                       + a->l1.bi16x16.p_cost_mv[-a->l1.bi16x16.mvp[1]];
        h->mc.avg[PIXEL_16x16]( pix0, 16, h->mb.pic.p_fref[0][a->l0.bi16x16.i_ref][0], h->mb.pic.i_stride[0],
                                h->mb.pic.p_fref[1][a->l1.bi16x16.i_ref][0], h->mb.pic.i_stride[0],
                                h->mb.bipred_weight[a->l0.bi16x16.i_ref][a->l1.bi16x16.i_ref] );
        int cost00 = h->pixf.mbcmp[PIXEL_16x16]( h->mb.pic.p_fenc[0], FENC_STRIDE, pix0, 16 )
                   + ref_costs + l0_mv_cost + l1_mv_cost;
        if( cost00 < a->i_cost16x16bi )
        {
            M32( a->l0.bi16x16.mv ) = 0;
            M32( a->l1.bi16x16.mv ) = 0;
            a->l0.bi16x16.cost_mv = l0_mv_cost;
            a->l1.bi16x16.cost_mv = l1_mv_cost;
            a->i_cost16x16bi = cost00;
        }
    }

    /* mb type cost */
    a->i_cost16x16bi   += a->i_lambda * i_mb_b_cost_table[B_BI_BI];
    a->l0.me16x16.cost += a->i_lambda * i_mb_b_cost_table[B_L0_L0];
    a->l1.me16x16.cost += a->i_lambda * i_mb_b_cost_table[B_L1_L1];
}

static inline void x264_mb_cache_mv_p8x8( x264_t *h, x264_mb_analysis_t *a, int i )
{
    const int x = 2*(i%2);
    const int y = 2*(i/2);

    switch( h->mb.i_sub_partition[i] )
    {
        case D_L0_8x8:
            x264_macroblock_cache_mv_ptr( h, x, y, 2, 2, 0, a->l0.me8x8[i].mv );
            break;
        case D_L0_8x4:
            x264_macroblock_cache_mv_ptr( h, x, y+0, 2, 1, 0, a->l0.me8x4[i][0].mv );
            x264_macroblock_cache_mv_ptr( h, x, y+1, 2, 1, 0, a->l0.me8x4[i][1].mv );
            break;
        case D_L0_4x8:
            x264_macroblock_cache_mv_ptr( h, x+0, y, 1, 2, 0, a->l0.me4x8[i][0].mv );
            x264_macroblock_cache_mv_ptr( h, x+1, y, 1, 2, 0, a->l0.me4x8[i][1].mv );
            break;
        case D_L0_4x4:
            x264_macroblock_cache_mv_ptr( h, x+0, y+0, 1, 1, 0, a->l0.me4x4[i][0].mv );
            x264_macroblock_cache_mv_ptr( h, x+1, y+0, 1, 1, 0, a->l0.me4x4[i][1].mv );
            x264_macroblock_cache_mv_ptr( h, x+0, y+1, 1, 1, 0, a->l0.me4x4[i][2].mv );
            x264_macroblock_cache_mv_ptr( h, x+1, y+1, 1, 1, 0, a->l0.me4x4[i][3].mv );
            break;
        default:
            x264_log( h, X264_LOG_ERROR, "internal error\n" );
            break;
    }
}

static void x264_mb_load_mv_direct8x8( x264_t *h, int idx )
{
    const int x = 2*(idx&1);
    const int y = 2*(idx>>1);
    x264_macroblock_cache_ref( h, x, y, 2, 2, 0, h->mb.cache.direct_ref[0][idx] );
    x264_macroblock_cache_ref( h, x, y, 2, 2, 1, h->mb.cache.direct_ref[1][idx] );
    x264_macroblock_cache_mv_ptr( h, x, y, 2, 2, 0, h->mb.cache.direct_mv[0][idx] );
    x264_macroblock_cache_mv_ptr( h, x, y, 2, 2, 1, h->mb.cache.direct_mv[1][idx] );
}

#define CACHE_MV_BI(x,y,dx,dy,me0,me1,part) \
    if( x264_mb_partition_listX_table[0][part] ) \
    { \
        x264_macroblock_cache_ref( h, x,y,dx,dy, 0, me0.i_ref ); \
        x264_macroblock_cache_mv_ptr( h, x,y,dx,dy, 0, me0.mv ); \
    } \
    else \
    { \
        x264_macroblock_cache_ref( h, x,y,dx,dy, 0, -1 ); \
        x264_macroblock_cache_mv(  h, x,y,dx,dy, 0, 0 ); \
        if( b_mvd ) \
            x264_macroblock_cache_mvd( h, x,y,dx,dy, 0, 0 ); \
    } \
    if( x264_mb_partition_listX_table[1][part] ) \
    { \
        x264_macroblock_cache_ref( h, x,y,dx,dy, 1, me1.i_ref ); \
        x264_macroblock_cache_mv_ptr( h, x,y,dx,dy, 1, me1.mv ); \
    } \
    else \
    { \
        x264_macroblock_cache_ref( h, x,y,dx,dy, 1, -1 ); \
        x264_macroblock_cache_mv(  h, x,y,dx,dy, 1, 0 ); \
        if( b_mvd ) \
            x264_macroblock_cache_mvd( h, x,y,dx,dy, 1, 0 ); \
    }

static inline void x264_mb_cache_mv_b8x8( x264_t *h, x264_mb_analysis_t *a, int i, int b_mvd )
{
    int x = (i%2)*2;
    int y = (i/2)*2;
    if( h->mb.i_sub_partition[i] == D_DIRECT_8x8 )
    {
        x264_mb_load_mv_direct8x8( h, i );
        if( b_mvd )
        {
            x264_macroblock_cache_mvd(  h, x, y, 2, 2, 0, 0 );
            x264_macroblock_cache_mvd(  h, x, y, 2, 2, 1, 0 );
            x264_macroblock_cache_skip( h, x, y, 2, 2, 1 );
        }
    }
    else
    {
        CACHE_MV_BI( x, y, 2, 2, a->l0.me8x8[i], a->l1.me8x8[i], h->mb.i_sub_partition[i] );
    }
}
static inline void x264_mb_cache_mv_b16x8( x264_t *h, x264_mb_analysis_t *a, int i, int b_mvd )
{
    CACHE_MV_BI( 0, 2*i, 4, 2, a->l0.me16x8[i], a->l1.me16x8[i], a->i_mb_partition16x8[i] );
}
static inline void x264_mb_cache_mv_b8x16( x264_t *h, x264_mb_analysis_t *a, int i, int b_mvd )
{
    CACHE_MV_BI( 2*i, 0, 2, 4, a->l0.me8x16[i], a->l1.me8x16[i], a->i_mb_partition8x16[i] );
}
#undef CACHE_MV_BI

static void x264_mb_analyse_inter_b8x8_mixed_ref( x264_t *h, x264_mb_analysis_t *a )
{
    ALIGNED_ARRAY_8( pixel, pix,[2],[8*8] );
    int i_maxref[2] = {h->mb.pic.i_fref[0]-1, h->mb.pic.i_fref[1]-1};

    /* early termination: if 16x16 chose ref 0, then evalute no refs older
     * than those used by the neighbors */
    #define CHECK_NEIGHBOUR(i)\
    {\
        int ref = h->mb.cache.ref[l][X264_SCAN8_0+i];\
        if( ref > i_maxref[l] )\
            i_maxref[l] = ref;\
    }

    for( int l = 0; l < 2; l++ )
    {
        x264_mb_analysis_list_t *lX = l ? &a->l1 : &a->l0;
        if( i_maxref[l] > 0 && lX->me16x16.i_ref == 0 &&
            h->mb.i_mb_type_top > 0 && h->mb.i_mb_type_left > 0 )
        {
            i_maxref[l] = 0;
            CHECK_NEIGHBOUR(  -8 - 1 );
            CHECK_NEIGHBOUR(  -8 + 0 );
            CHECK_NEIGHBOUR(  -8 + 2 );
            CHECK_NEIGHBOUR(  -8 + 4 );
            CHECK_NEIGHBOUR(   0 - 1 );
            CHECK_NEIGHBOUR( 2*8 - 1 );
        }
    }

    /* XXX Needed for x264_mb_predict_mv */
    h->mb.i_partition = D_8x8;

    a->i_cost8x8bi = 0;

    for( int i = 0; i < 4; i++ )
    {
        int x8 = i%2;
        int y8 = i/2;
        int i_part_cost;
        int i_part_cost_bi;
        int stride[2] = {8,8};
        pixel *src[2];
        x264_me_t m;
        m.i_pixel = PIXEL_8x8;
        LOAD_FENC( &m, h->mb.pic.p_fenc, 8*x8, 8*y8 );

        for( int l = 0; l < 2; l++ )
        {
            x264_mb_analysis_list_t *lX = l ? &a->l1 : &a->l0;

            lX->me8x8[i].cost = INT_MAX;
            for( int i_ref = 0; i_ref <= i_maxref[l]; i_ref++ )
            {
                m.i_ref_cost = REF_COST( l, i_ref );;

                LOAD_HPELS( &m, h->mb.pic.p_fref[l][i_ref], l, i_ref, 8*x8, 8*y8 );

                x264_macroblock_cache_ref( h, x8*2, y8*2, 2, 2, l, i_ref );
                x264_mb_predict_mv( h, l, 4*i, 2, m.mvp );
                x264_me_search( h, &m, lX->mvc[i_ref], i+1 );
                m.cost += m.i_ref_cost;

                if( m.cost < lX->me8x8[i].cost )
                {
                    h->mc.memcpy_aligned( &lX->me8x8[i], &m, sizeof(x264_me_t) );
                    a->i_satd8x8[l][i] = m.cost - ( m.cost_mv + m.i_ref_cost );
                }

                /* save mv for predicting other partitions within this MB */
                CP32( lX->mvc[i_ref][i+1], m.mv );
            }
        }

        /* BI mode */
        src[0] = h->mc.get_ref( pix[0], &stride[0], a->l0.me8x8[i].p_fref, a->l0.me8x8[i].i_stride[0],
                                a->l0.me8x8[i].mv[0], a->l0.me8x8[i].mv[1], 8, 8, weight_none );
        src[1] = h->mc.get_ref( pix[1], &stride[1], a->l1.me8x8[i].p_fref, a->l1.me8x8[i].i_stride[0],
                                a->l1.me8x8[i].mv[0], a->l1.me8x8[i].mv[1], 8, 8, weight_none );
        h->mc.avg[PIXEL_8x8]( pix[0], 8, src[0], stride[0], src[1], stride[1],
                                h->mb.bipred_weight[a->l0.me8x8[i].i_ref][a->l1.me8x8[i].i_ref] );

        a->i_satd8x8[2][i] = h->pixf.mbcmp[PIXEL_8x8]( a->l0.me8x8[i].p_fenc[0], FENC_STRIDE, pix[0], 8 );
        i_part_cost_bi = a->i_satd8x8[2][i] + a->l0.me8x8[i].cost_mv + a->l1.me8x8[i].cost_mv
                         + a->l0.me8x8[i].i_ref_cost + a->l1.me8x8[i].i_ref_cost
                         + a->i_lambda * i_sub_mb_b_cost_table[D_BI_8x8];

        a->l0.me8x8[i].cost += a->i_lambda * i_sub_mb_b_cost_table[D_L0_8x8];
        a->l1.me8x8[i].cost += a->i_lambda * i_sub_mb_b_cost_table[D_L1_8x8];

        i_part_cost = a->l0.me8x8[i].cost;
        h->mb.i_sub_partition[i] = D_L0_8x8;
        COPY2_IF_LT( i_part_cost, a->l1.me8x8[i].cost, h->mb.i_sub_partition[i], D_L1_8x8 );
        COPY2_IF_LT( i_part_cost, i_part_cost_bi, h->mb.i_sub_partition[i], D_BI_8x8 );
        COPY2_IF_LT( i_part_cost, a->i_cost8x8direct[i], h->mb.i_sub_partition[i], D_DIRECT_8x8 );
        a->i_cost8x8bi += i_part_cost;

        /* XXX Needed for x264_mb_predict_mv */
        x264_mb_cache_mv_b8x8( h, a, i, 0 );
    }

    /* mb type cost */
    a->i_cost8x8bi += a->i_lambda * i_mb_b_cost_table[B_8x8];
}

static void x264_mb_analyse_inter_b8x8( x264_t *h, x264_mb_analysis_t *a )
{
    pixel **p_fref[2] =
        { h->mb.pic.p_fref[0][a->l0.me16x16.i_ref],
          h->mb.pic.p_fref[1][a->l1.me16x16.i_ref] };
    ALIGNED_ARRAY_8( pixel, pix,[2],[8*8] );

    /* XXX Needed for x264_mb_predict_mv */
    h->mb.i_partition = D_8x8;

    a->i_cost8x8bi = 0;

    for( int i = 0; i < 4; i++ )
    {
        const int x8 = i%2;
        const int y8 = i/2;
        int i_part_cost;
        int i_part_cost_bi = 0;
        int stride[2] = {8,8};
        pixel *src[2];

        for( int l = 0; l < 2; l++ )
        {
            x264_mb_analysis_list_t *lX = l ? &a->l1 : &a->l0;
            x264_me_t *m = &lX->me8x8[i];
            m->i_pixel = PIXEL_8x8;
            LOAD_FENC( m, h->mb.pic.p_fenc, 8*x8, 8*y8 );

            m->i_ref_cost = REF_COST( l, lX->me16x16.i_ref );
            m->i_ref = lX->me16x16.i_ref;

            LOAD_HPELS( m, p_fref[l], l, lX->me16x16.i_ref, 8*x8, 8*y8 );

            x264_macroblock_cache_ref( h, x8*2, y8*2, 2, 2, l, lX->me16x16.i_ref );
            x264_mb_predict_mv( h, l, 4*i, 2, m->mvp );
            x264_me_search( h, m, &lX->me16x16.mv, 1 );
            a->i_satd8x8[l][i] = m->cost - m->cost_mv;
            m->cost += m->i_ref_cost;

            x264_macroblock_cache_mv_ptr( h, 2*x8, 2*y8, 2, 2, l, m->mv );

            /* save mv for predicting other partitions within this MB */
            CP32( lX->mvc[lX->me16x16.i_ref][i+1], m->mv );

            /* BI mode */
            src[l] = h->mc.get_ref( pix[l], &stride[l], m->p_fref, m->i_stride[0],
                                    m->mv[0], m->mv[1], 8, 8, weight_none );
            i_part_cost_bi += m->cost_mv + m->i_ref_cost;
        }
        h->mc.avg[PIXEL_8x8]( pix[0], 8, src[0], stride[0], src[1], stride[1], h->mb.bipred_weight[a->l0.me16x16.i_ref][a->l1.me16x16.i_ref] );
        a->i_satd8x8[2][i] = h->pixf.mbcmp[PIXEL_8x8]( a->l0.me8x8[i].p_fenc[0], FENC_STRIDE, pix[0], 8 );
        i_part_cost_bi += a->i_satd8x8[2][i] + a->i_lambda * i_sub_mb_b_cost_table[D_BI_8x8];
        a->l0.me8x8[i].cost += a->i_lambda * i_sub_mb_b_cost_table[D_L0_8x8];
        a->l1.me8x8[i].cost += a->i_lambda * i_sub_mb_b_cost_table[D_L1_8x8];

        i_part_cost = a->l0.me8x8[i].cost;
        h->mb.i_sub_partition[i] = D_L0_8x8;
        COPY2_IF_LT( i_part_cost, a->l1.me8x8[i].cost, h->mb.i_sub_partition[i], D_L1_8x8 );
        COPY2_IF_LT( i_part_cost, i_part_cost_bi, h->mb.i_sub_partition[i], D_BI_8x8 );
        COPY2_IF_LT( i_part_cost, a->i_cost8x8direct[i], h->mb.i_sub_partition[i], D_DIRECT_8x8 );
        a->i_cost8x8bi += i_part_cost;

        /* XXX Needed for x264_mb_predict_mv */
        x264_mb_cache_mv_b8x8( h, a, i, 0 );
    }

    /* mb type cost */
    a->i_cost8x8bi += a->i_lambda * i_mb_b_cost_table[B_8x8];
}

static void x264_mb_analyse_inter_b16x8( x264_t *h, x264_mb_analysis_t *a, int i_best_satd )
{
    ALIGNED_ARRAY_16( pixel, pix,[2],[16*8] );
    ALIGNED_4( int16_t mvc[3][2] );

    h->mb.i_partition = D_16x8;
    a->i_cost16x8bi = 0;

    for( int i = 0; i < 2; i++ )
    {
        int i_part_cost;
        int i_part_cost_bi = 0;
        int stride[2] = {16,16};
        pixel *src[2];
        x264_me_t m;
        m.i_pixel = PIXEL_16x8;
        LOAD_FENC( &m, h->mb.pic.p_fenc, 0, 8*i );

        for( int l = 0; l < 2; l++ )
        {
            x264_mb_analysis_list_t *lX = l ? &a->l1 : &a->l0;
            int ref8[2] = { lX->me8x8[2*i].i_ref, lX->me8x8[2*i+1].i_ref };
            int i_ref8s = ( ref8[0] == ref8[1] ) ? 1 : 2;
            lX->me16x8[i].cost = INT_MAX;
            for( int j = 0; j < i_ref8s; j++ )
            {
                int i_ref = ref8[j];
                m.i_ref_cost = REF_COST( l, i_ref );;

                LOAD_HPELS( &m, h->mb.pic.p_fref[l][i_ref], l, i_ref, 0, 8*i );

                CP32( mvc[0], lX->mvc[i_ref][0] );
                CP32( mvc[1], lX->mvc[i_ref][2*i+1] );
                CP32( mvc[2], lX->mvc[i_ref][2*i+2] );

                x264_macroblock_cache_ref( h, 0, 2*i, 4, 2, l, i_ref );
                x264_mb_predict_mv( h, l, 8*i, 4, m.mvp );
                x264_me_search( h, &m, mvc, 3 );
                m.cost += m.i_ref_cost;

                if( m.cost < lX->me16x8[i].cost )
                    h->mc.memcpy_aligned( &lX->me16x8[i], &m, sizeof(x264_me_t) );
            }
        }

        /* BI mode */
        src[0] = h->mc.get_ref( pix[0], &stride[0], a->l0.me16x8[i].p_fref, a->l0.me16x8[i].i_stride[0],
                                a->l0.me16x8[i].mv[0], a->l0.me16x8[i].mv[1], 16, 8, weight_none );
        src[1] = h->mc.get_ref( pix[1], &stride[1], a->l1.me16x8[i].p_fref, a->l1.me16x8[i].i_stride[0],
                                a->l1.me16x8[i].mv[0], a->l1.me16x8[i].mv[1], 16, 8, weight_none );
        h->mc.avg[PIXEL_16x8]( pix[0], 16, src[0], stride[0], src[1], stride[1],
                                h->mb.bipred_weight[a->l0.me16x8[i].i_ref][a->l1.me16x8[i].i_ref] );

        i_part_cost_bi = h->pixf.mbcmp[PIXEL_16x8]( a->l0.me16x8[i].p_fenc[0], FENC_STRIDE, pix[0], 16 )
                        + a->l0.me16x8[i].cost_mv + a->l1.me16x8[i].cost_mv + a->l0.me16x8[i].i_ref_cost
                        + a->l1.me16x8[i].i_ref_cost;

        i_part_cost = a->l0.me16x8[i].cost;
        a->i_mb_partition16x8[i] = D_L0_8x8; /* not actually 8x8, only the L0 matters */

        if( a->l1.me16x8[i].cost < i_part_cost )
        {
            i_part_cost = a->l1.me16x8[i].cost;
            a->i_mb_partition16x8[i] = D_L1_8x8;
        }
        if( i_part_cost_bi + a->i_lambda * 1 < i_part_cost )
        {
            i_part_cost = i_part_cost_bi;
            a->i_mb_partition16x8[i] = D_BI_8x8;
        }
        a->i_cost16x8bi += i_part_cost;

        /* Early termination based on the current SATD score of partition[0]
           plus the estimated SATD score of partition[1] */
        if( !i && i_part_cost + a->i_cost_est16x8[1] > i_best_satd
            * (16 + (!!a->i_mbrd + !!h->mb.i_psy_rd))/16 )
        {
            a->i_cost16x8bi = COST_MAX;
            return;
        }

        x264_mb_cache_mv_b16x8( h, a, i, 0 );
    }

    /* mb type cost */
    a->i_mb_type16x8 = B_L0_L0
        + (a->i_mb_partition16x8[0]>>2) * 3
        + (a->i_mb_partition16x8[1]>>2);
    a->i_cost16x8bi += a->i_lambda * i_mb_b16x8_cost_table[a->i_mb_type16x8];
}

static void x264_mb_analyse_inter_b8x16( x264_t *h, x264_mb_analysis_t *a, int i_best_satd )
{
    ALIGNED_ARRAY_8( pixel, pix,[2],[8*16] );
    ALIGNED_4( int16_t mvc[3][2] );

    h->mb.i_partition = D_8x16;
    a->i_cost8x16bi = 0;

    for( int i = 0; i < 2; i++ )
    {
        int i_part_cost;
        int i_part_cost_bi = 0;
        int stride[2] = {8,8};
        pixel *src[2];
        x264_me_t m;
        m.i_pixel = PIXEL_8x16;
        LOAD_FENC( &m, h->mb.pic.p_fenc, 8*i, 0 );

        for( int l = 0; l < 2; l++ )
        {
            x264_mb_analysis_list_t *lX = l ? &a->l1 : &a->l0;
            int ref8[2] = { lX->me8x8[i].i_ref, lX->me8x8[i+2].i_ref };
            int i_ref8s = ( ref8[0] == ref8[1] ) ? 1 : 2;
            lX->me8x16[i].cost = INT_MAX;
            for( int j = 0; j < i_ref8s; j++ )
            {
                int i_ref = ref8[j];
                m.i_ref_cost = REF_COST( l, i_ref );

                LOAD_HPELS( &m, h->mb.pic.p_fref[l][i_ref], l, i_ref, 8*i, 0 );

                CP32( mvc[0], lX->mvc[i_ref][0] );
                CP32( mvc[1], lX->mvc[i_ref][i+1] );
                CP32( mvc[2], lX->mvc[i_ref][i+3] );

                x264_macroblock_cache_ref( h, 2*i, 0, 2, 4, l, i_ref );
                x264_mb_predict_mv( h, l, 4*i, 2, m.mvp );
                x264_me_search( h, &m, mvc, 3 );
                m.cost += m.i_ref_cost;

                if( m.cost < lX->me8x16[i].cost )
                    h->mc.memcpy_aligned( &lX->me8x16[i], &m, sizeof(x264_me_t) );
            }
        }

        /* BI mode */
        src[0] = h->mc.get_ref( pix[0], &stride[0], a->l0.me8x16[i].p_fref, a->l0.me8x16[i].i_stride[0],
                                a->l0.me8x16[i].mv[0], a->l0.me8x16[i].mv[1], 8, 16, weight_none );
        src[1] = h->mc.get_ref( pix[1], &stride[1], a->l1.me8x16[i].p_fref, a->l1.me8x16[i].i_stride[0],
                                a->l1.me8x16[i].mv[0], a->l1.me8x16[i].mv[1], 8, 16, weight_none );
        h->mc.avg[PIXEL_8x16]( pix[0], 8, src[0], stride[0], src[1], stride[1], h->mb.bipred_weight[a->l0.me8x16[i].i_ref][a->l1.me8x16[i].i_ref] );

        i_part_cost_bi = h->pixf.mbcmp[PIXEL_8x16]( a->l0.me8x16[i].p_fenc[0], FENC_STRIDE, pix[0], 8 )
                        + a->l0.me8x16[i].cost_mv + a->l1.me8x16[i].cost_mv + a->l0.me8x16[i].i_ref_cost
                        + a->l1.me8x16[i].i_ref_cost;

        i_part_cost = a->l0.me8x16[i].cost;
        a->i_mb_partition8x16[i] = D_L0_8x8;

        if( a->l1.me8x16[i].cost < i_part_cost )
        {
            i_part_cost = a->l1.me8x16[i].cost;
            a->i_mb_partition8x16[i] = D_L1_8x8;
        }
        if( i_part_cost_bi + a->i_lambda * 1 < i_part_cost )
        {
            i_part_cost = i_part_cost_bi;
            a->i_mb_partition8x16[i] = D_BI_8x8;
        }
        a->i_cost8x16bi += i_part_cost;

        /* Early termination based on the current SATD score of partition[0]
           plus the estimated SATD score of partition[1] */
        if( !i && i_part_cost + a->i_cost_est8x16[1] > i_best_satd
            * (16 + (!!a->i_mbrd + !!h->mb.i_psy_rd))/16 )
        {
            a->i_cost8x16bi = COST_MAX;
            return;
        }

        x264_mb_cache_mv_b8x16( h, a, i, 0 );
    }

    /* mb type cost */
    a->i_mb_type8x16 = B_L0_L0
        + (a->i_mb_partition8x16[0]>>2) * 3
        + (a->i_mb_partition8x16[1]>>2);
    a->i_cost8x16bi += a->i_lambda * i_mb_b16x8_cost_table[a->i_mb_type8x16];
}

static void x264_mb_analyse_p_rd( x264_t *h, x264_mb_analysis_t *a, int i_satd )
{
    int thresh = i_satd * 5/4;

    h->mb.i_type = P_L0;
    if( a->l0.i_rd16x16 == COST_MAX && a->l0.me16x16.cost <= i_satd * 3/2 )
    {
        h->mb.i_partition = D_16x16;
        x264_analyse_update_cache( h, a );
        a->l0.i_rd16x16 = x264_rd_cost_mb( h, a->i_lambda2 );
    }

    if( a->l0.i_cost16x8 <= thresh )
    {
        h->mb.i_partition = D_16x8;
        x264_analyse_update_cache( h, a );
        a->l0.i_cost16x8 = x264_rd_cost_mb( h, a->i_lambda2 );
    }
    else
        a->l0.i_cost16x8 = COST_MAX;

    if( a->l0.i_cost8x16 <= thresh )
    {
        h->mb.i_partition = D_8x16;
        x264_analyse_update_cache( h, a );
        a->l0.i_cost8x16 = x264_rd_cost_mb( h, a->i_lambda2 );
    }
    else
        a->l0.i_cost8x16 = COST_MAX;

    if( a->l0.i_cost8x8 <= thresh )
    {
        h->mb.i_type = P_8x8;
        h->mb.i_partition = D_8x8;
        if( h->param.analyse.inter & X264_ANALYSE_PSUB8x8 )
        {
            x264_macroblock_cache_ref( h, 0, 0, 2, 2, 0, a->l0.me8x8[0].i_ref );
            x264_macroblock_cache_ref( h, 2, 0, 2, 2, 0, a->l0.me8x8[1].i_ref );
            x264_macroblock_cache_ref( h, 0, 2, 2, 2, 0, a->l0.me8x8[2].i_ref );
            x264_macroblock_cache_ref( h, 2, 2, 2, 2, 0, a->l0.me8x8[3].i_ref );
            /* FIXME: In the 8x8 blocks where RDO isn't run, the NNZ values used for context selection
             * for future blocks are those left over from previous RDO calls. */
            for( int i = 0; i < 4; i++ )
            {
                int costs[4] = {a->l0.i_cost4x4[i], a->l0.i_cost8x4[i], a->l0.i_cost4x8[i], a->l0.me8x8[i].cost};
                int sub8x8_thresh = X264_MIN4( costs[0], costs[1], costs[2], costs[3] ) * 5 / 4;
                int subtype, btype = D_L0_8x8;
                uint64_t bcost = COST_MAX64;
                for( subtype = D_L0_4x4; subtype <= D_L0_8x8; subtype++ )
                {
                    uint64_t cost;
                    if( costs[subtype] > sub8x8_thresh || (subtype == D_L0_8x8 && bcost == COST_MAX64) )
                        continue;
                    h->mb.i_sub_partition[i] = subtype;
                    x264_mb_cache_mv_p8x8( h, a, i );
                    cost = x264_rd_cost_part( h, a->i_lambda2, i<<2, PIXEL_8x8 );
                    COPY2_IF_LT( bcost, cost, btype, subtype );
                }
                if( h->mb.i_sub_partition[i] != btype )
                {
                    h->mb.i_sub_partition[i] = btype;
                    x264_mb_cache_mv_p8x8( h, a, i );
                }
            }
        }
        else
            x264_analyse_update_cache( h, a );
        a->l0.i_cost8x8 = x264_rd_cost_mb( h, a->i_lambda2 );
    }
    else
        a->l0.i_cost8x8 = COST_MAX;
}

static void x264_mb_analyse_b_rd( x264_t *h, x264_mb_analysis_t *a, int i_satd_inter )
{
    int thresh = i_satd_inter * (17 + (!!h->mb.i_psy_rd))/16;

    if( a->b_direct_available && a->i_rd16x16direct == COST_MAX )
    {
        h->mb.i_type = B_DIRECT;
        /* Assumes direct/skip MC is still in fdec */
        /* Requires b-rdo to be done before intra analysis */
        h->mb.b_skip_mc = 1;
        x264_analyse_update_cache( h, a );
        a->i_rd16x16direct = x264_rd_cost_mb( h, a->i_lambda2 );
        h->mb.b_skip_mc = 0;
    }

    //FIXME not all the update_cache calls are needed
    h->mb.i_partition = D_16x16;
    /* L0 */
    if( a->l0.me16x16.cost <= thresh && a->l0.i_rd16x16 == COST_MAX )
    {
        h->mb.i_type = B_L0_L0;
        x264_analyse_update_cache( h, a );
        a->l0.i_rd16x16 = x264_rd_cost_mb( h, a->i_lambda2 );
    }

    /* L1 */
    if( a->l1.me16x16.cost <= thresh && a->l1.i_rd16x16 == COST_MAX )
    {
        h->mb.i_type = B_L1_L1;
        x264_analyse_update_cache( h, a );
        a->l1.i_rd16x16 = x264_rd_cost_mb( h, a->i_lambda2 );
    }

    /* BI */
    if( a->i_cost16x16bi <= thresh && a->i_rd16x16bi == COST_MAX )
    {
        h->mb.i_type = B_BI_BI;
        x264_analyse_update_cache( h, a );
        a->i_rd16x16bi = x264_rd_cost_mb( h, a->i_lambda2 );
    }

    /* 8x8 */
    if( a->i_cost8x8bi <= thresh && a->i_rd8x8bi == COST_MAX )
    {
        h->mb.i_type = B_8x8;
        h->mb.i_partition = D_8x8;
        x264_analyse_update_cache( h, a );
        a->i_rd8x8bi = x264_rd_cost_mb( h, a->i_lambda2 );
        x264_macroblock_cache_skip( h, 0, 0, 4, 4, 0 );
    }

    /* 16x8 */
    if( a->i_cost16x8bi <= thresh && a->i_rd16x8bi == COST_MAX )
    {
        h->mb.i_type = a->i_mb_type16x8;
        h->mb.i_partition = D_16x8;
        x264_analyse_update_cache( h, a );
        a->i_rd16x8bi = x264_rd_cost_mb( h, a->i_lambda2 );
    }

    /* 8x16 */
    if( a->i_cost8x16bi <= thresh && a->i_rd8x16bi == COST_MAX )
    {
        h->mb.i_type = a->i_mb_type8x16;
        h->mb.i_partition = D_8x16;
        x264_analyse_update_cache( h, a );
        a->i_rd8x16bi = x264_rd_cost_mb( h, a->i_lambda2 );
    }
}

static void x264_refine_bidir( x264_t *h, x264_mb_analysis_t *a )
{
    int i_biweight;

    if( IS_INTRA(h->mb.i_type) )
        return;

    switch( h->mb.i_partition )
    {
        case D_16x16:
            if( h->mb.i_type == B_BI_BI )
            {
                i_biweight = h->mb.bipred_weight[a->l0.bi16x16.i_ref][a->l1.bi16x16.i_ref];
                x264_me_refine_bidir_satd( h, &a->l0.bi16x16, &a->l1.bi16x16, i_biweight );
            }
            break;
        case D_16x8:
            for( int i = 0; i < 2; i++ )
                if( a->i_mb_partition16x8[i] == D_BI_8x8 )
                {
                    i_biweight = h->mb.bipred_weight[a->l0.me16x8[i].i_ref][a->l1.me16x8[i].i_ref];
                    x264_me_refine_bidir_satd( h, &a->l0.me16x8[i], &a->l1.me16x8[i], i_biweight );
                }
            break;
        case D_8x16:
            for( int i = 0; i < 2; i++ )
                if( a->i_mb_partition8x16[i] == D_BI_8x8 )
                {
                    i_biweight = h->mb.bipred_weight[a->l0.me8x16[i].i_ref][a->l1.me8x16[i].i_ref];
                    x264_me_refine_bidir_satd( h, &a->l0.me8x16[i], &a->l1.me8x16[i], i_biweight );
                }
            break;
        case D_8x8:
            for( int i = 0; i < 4; i++ )
                if( h->mb.i_sub_partition[i] == D_BI_8x8 )
                {
                    i_biweight = h->mb.bipred_weight[a->l0.me8x8[i].i_ref][a->l1.me8x8[i].i_ref];
                    x264_me_refine_bidir_satd( h, &a->l0.me8x8[i], &a->l1.me8x8[i], i_biweight );
                }
            break;
    }
}

static inline void x264_mb_analyse_transform( x264_t *h )
{
    if( x264_mb_transform_8x8_allowed( h ) && h->param.analyse.b_transform_8x8 && !h->mb.b_lossless )
    {
        /* Only luma MC is really needed, but the full MC is re-used in macroblock_encode. */
        x264_mb_mc( h );

        int i_cost8 = h->pixf.sa8d[PIXEL_16x16]( h->mb.pic.p_fenc[0], FENC_STRIDE,
                                             h->mb.pic.p_fdec[0], FDEC_STRIDE );
        int i_cost4 = h->pixf.satd[PIXEL_16x16]( h->mb.pic.p_fenc[0], FENC_STRIDE,
                                             h->mb.pic.p_fdec[0], FDEC_STRIDE );

        h->mb.b_transform_8x8 = i_cost8 < i_cost4;
        h->mb.b_skip_mc = 1;
    }
}

static inline void x264_mb_analyse_transform_rd( x264_t *h, x264_mb_analysis_t *a, int *i_satd, int *i_rd )
{
    if( x264_mb_transform_8x8_allowed( h ) && h->param.analyse.b_transform_8x8 )
    {
        x264_analyse_update_cache( h, a );
        h->mb.b_transform_8x8 ^= 1;
        /* FIXME only luma is needed, but the score for comparison already includes chroma */
        int i_rd8 = x264_rd_cost_mb( h, a->i_lambda2 );

        if( *i_rd >= i_rd8 )
        {
            if( *i_rd > 0 )
                *i_satd = (int64_t)(*i_satd) * i_rd8 / *i_rd;
            *i_rd = i_rd8;
        }
        else
            h->mb.b_transform_8x8 ^= 1;
    }
}

/* Rate-distortion optimal QP selection.
 * FIXME: More than half of the benefit of this function seems to be
 * in the way it improves the coding of chroma DC (by decimating or
 * finding a better way to code a single DC coefficient.)
 * There must be a more efficient way to get that portion of the benefit
 * without doing full QP-RD, but RD-decimation doesn't seem to do the
 * trick. */
static inline void x264_mb_analyse_qp_rd( x264_t *h, x264_mb_analysis_t *a )
{
    int bcost, cost, failures, prevcost, origcost;
    int orig_qp = h->mb.i_qp, bqp = h->mb.i_qp;
    int last_qp_tried = 0;
    origcost = bcost = x264_rd_cost_mb( h, a->i_lambda2 );
    int origcbp = h->mb.cbp[h->mb.i_mb_xy];

    /* If CBP is already zero, don't raise the quantizer any higher. */
    for( int direction = origcbp ? 1 : -1; direction >= -1; direction-=2 )
    {
        /* Without psy-RD, require monotonicity when moving quant away from previous
         * macroblock's quant; allow 1 failure when moving quant towards previous quant.
         * With psy-RD, allow 1 failure when moving quant away from previous quant,
         * allow 2 failures when moving quant towards previous quant.
         * Psy-RD generally seems to result in more chaotic RD score-vs-quantizer curves. */
        int threshold = (!!h->mb.i_psy_rd);
        /* Raise the threshold for failures if we're moving towards the last QP. */
        if( ( h->mb.i_last_qp < orig_qp && direction == -1 ) ||
            ( h->mb.i_last_qp > orig_qp && direction ==  1 ) )
            threshold++;
        h->mb.i_qp = orig_qp;
        failures = 0;
        prevcost = origcost;

        /* If the current QP results in an empty CBP, it's highly likely that lower QPs
         * (up to a point) will too.  So, jump down to where the threshold will kick in
         * and check the QP there.  If the CBP is still empty, skip the main loop.
         * If it isn't empty, we would have ended up having to check this QP anyways,
         * so as long as we store it for later lookup, we lose nothing. */
        int already_checked_qp = -1;
        int already_checked_cost = COST_MAX;
        if( direction == -1 )
        {
            if( !origcbp )
            {
                h->mb.i_qp = X264_MAX( h->mb.i_qp - threshold - 1, h->param.rc.i_qp_min );
                h->mb.i_chroma_qp = h->chroma_qp_table[h->mb.i_qp];
                already_checked_cost = x264_rd_cost_mb( h, a->i_lambda2 );
                if( !h->mb.cbp[h->mb.i_mb_xy] )
                {
                    /* If our empty-CBP block is lower QP than the last QP,
                     * the last QP almost surely doesn't have a CBP either. */
                    if( h->mb.i_last_qp > h->mb.i_qp )
                        last_qp_tried = 1;
                    break;
                }
                already_checked_qp = h->mb.i_qp;
                h->mb.i_qp = orig_qp;
            }
        }

        h->mb.i_qp += direction;
        while( h->mb.i_qp >= h->param.rc.i_qp_min && h->mb.i_qp <= h->param.rc.i_qp_max )
        {
            if( h->mb.i_last_qp == h->mb.i_qp )
                last_qp_tried = 1;
            if( h->mb.i_qp == already_checked_qp )
                cost = already_checked_cost;
            else
            {
                h->mb.i_chroma_qp = h->chroma_qp_table[h->mb.i_qp];
                cost = x264_rd_cost_mb( h, a->i_lambda2 );
                COPY2_IF_LT( bcost, cost, bqp, h->mb.i_qp );
            }

            /* We can't assume that the costs are monotonic over QPs.
             * Tie case-as-failure seems to give better results. */
            if( cost < prevcost )
                failures = 0;
            else
                failures++;
            prevcost = cost;

            if( failures > threshold )
                break;
            if( direction == 1 && !h->mb.cbp[h->mb.i_mb_xy] )
                break;
            h->mb.i_qp += direction;
        }
    }

    /* Always try the last block's QP. */
    if( !last_qp_tried )
    {
        h->mb.i_qp = h->mb.i_last_qp;
        h->mb.i_chroma_qp = h->chroma_qp_table[h->mb.i_qp];
        cost = x264_rd_cost_mb( h, a->i_lambda2 );
        COPY2_IF_LT( bcost, cost, bqp, h->mb.i_qp );
    }

    h->mb.i_qp = bqp;
    h->mb.i_chroma_qp = h->chroma_qp_table[h->mb.i_qp];

    /* Check transform again; decision from before may no longer be optimal. */
    if( h->mb.i_qp != orig_qp && h->param.analyse.b_transform_8x8 &&
        x264_mb_transform_8x8_allowed( h ) )
    {
        h->mb.b_transform_8x8 ^= 1;
        cost = x264_rd_cost_mb( h, a->i_lambda2 );
        if( cost > bcost )
            h->mb.b_transform_8x8 ^= 1;
    }
}

/*****************************************************************************
 * x264_macroblock_analyse:
 *****************************************************************************/
void x264_macroblock_analyse( x264_t *h )
{
    x264_mb_analysis_t analysis;
    int i_cost = COST_MAX;

    h->mb.i_qp = x264_ratecontrol_mb_qp( h );
    /* If the QP of this MB is within 1 of the previous MB, code the same QP as the previous MB,
     * to lower the bit cost of the qp_delta.  Don't do this if QPRD is enabled. */
    if( h->param.rc.i_aq_mode && h->param.analyse.i_subpel_refine < 10 && abs(h->mb.i_qp - h->mb.i_last_qp) == 1 )
        h->mb.i_qp = h->mb.i_last_qp;

    x264_mb_analyse_init( h, &analysis, h->mb.i_qp );

    /*--------------------------- Do the analysis ---------------------------*/
    if( h->sh.i_type == SLICE_TYPE_I )
    {
intra_analysis:
        if( analysis.i_mbrd )
            x264_mb_init_fenc_cache( h, analysis.i_mbrd >= 2 );
        x264_mb_analyse_intra( h, &analysis, COST_MAX );
        if( analysis.i_mbrd )
            x264_intra_rd( h, &analysis, COST_MAX );

        i_cost = analysis.i_satd_i16x16;
        h->mb.i_type = I_16x16;
        COPY2_IF_LT( i_cost, analysis.i_satd_i4x4, h->mb.i_type, I_4x4 );
        COPY2_IF_LT( i_cost, analysis.i_satd_i8x8, h->mb.i_type, I_8x8 );
        if( analysis.i_satd_pcm < i_cost )
            h->mb.i_type = I_PCM;

        else if( analysis.i_mbrd >= 2 )
            x264_intra_rd_refine( h, &analysis );
    }
    else if( h->sh.i_type == SLICE_TYPE_P )
    {
        int b_skip = 0;

        h->mc.prefetch_ref( h->mb.pic.p_fref[0][0][h->mb.i_mb_x&3], h->mb.pic.i_stride[0], 0 );

        analysis.b_try_skip = 0;
        if( analysis.b_force_intra )
        {
            if( !h->param.analyse.b_psy )
            {
                x264_mb_analyse_init_qp( h, &analysis, X264_MAX( h->mb.i_qp - h->mb.ip_offset, h->param.rc.i_qp_min ) );
                goto intra_analysis;
            }
        }
        else
        {
            /* Fast P_SKIP detection */
            if( h->param.analyse.b_fast_pskip )
            {
                if( h->i_thread_frames > 1 && h->mb.cache.pskip_mv[1] > h->mb.mv_max_spel[1] )
                    // FIXME don't need to check this if the reference frame is done
                    {}
                else if( h->param.analyse.i_subpel_refine >= 3 )
                    analysis.b_try_skip = 1;
                else if( h->mb.i_mb_type_left == P_SKIP ||
                         h->mb.i_mb_type_top == P_SKIP ||
                         h->mb.i_mb_type_topleft == P_SKIP ||
                         h->mb.i_mb_type_topright == P_SKIP )
                    b_skip = x264_macroblock_probe_pskip( h );
            }
        }

        h->mc.prefetch_ref( h->mb.pic.p_fref[0][0][h->mb.i_mb_x&3], h->mb.pic.i_stride[0], 1 );

        if( b_skip )
        {
            h->mb.i_type = P_SKIP;
            h->mb.i_partition = D_16x16;
            assert( h->mb.cache.pskip_mv[1] <= h->mb.mv_max_spel[1] || h->i_thread_frames == 1 );
            /* Set up MVs for future predictors */
            for( int i = 0; i < h->mb.pic.i_fref[0]; i++ )
                M32( h->mb.mvr[0][i][h->mb.i_mb_xy] ) = 0;
        }
        else
        {
            const unsigned int flags = h->param.analyse.inter;
            int i_type;
            int i_partition;
            int i_thresh16x8;
            int i_satd_inter, i_satd_intra;

            x264_mb_analyse_load_costs( h, &analysis );

            x264_mb_analyse_inter_p16x16( h, &analysis );

            if( h->mb.i_type == P_SKIP )
            {
                for( int i = 1; i < h->mb.pic.i_fref[0]; i++ )
                    M32( h->mb.mvr[0][i][h->mb.i_mb_xy] ) = 0;
                return;
            }

            if( flags & X264_ANALYSE_PSUB16x16 )
            {
                if( h->param.analyse.b_mixed_references )
                    x264_mb_analyse_inter_p8x8_mixed_ref( h, &analysis );
                else
                    x264_mb_analyse_inter_p8x8( h, &analysis );
            }

            /* Select best inter mode */
            i_type = P_L0;
            i_partition = D_16x16;
            i_cost = analysis.l0.me16x16.cost;

            if( ( flags & X264_ANALYSE_PSUB16x16 ) &&
                analysis.l0.i_cost8x8 < analysis.l0.me16x16.cost )
            {
                i_type = P_8x8;
                i_partition = D_8x8;
                i_cost = analysis.l0.i_cost8x8;

                /* Do sub 8x8 */
                if( flags & X264_ANALYSE_PSUB8x8 )
                {
                    for( int i = 0; i < 4; i++ )
                    {
                        x264_mb_analyse_inter_p4x4( h, &analysis, i );
                        if( analysis.l0.i_cost4x4[i] < analysis.l0.me8x8[i].cost )
                        {
                            int i_cost8x8 = analysis.l0.i_cost4x4[i];
                            h->mb.i_sub_partition[i] = D_L0_4x4;

                            x264_mb_analyse_inter_p8x4( h, &analysis, i );
                            COPY2_IF_LT( i_cost8x8, analysis.l0.i_cost8x4[i],
                                         h->mb.i_sub_partition[i], D_L0_8x4 );

                            x264_mb_analyse_inter_p4x8( h, &analysis, i );
                            COPY2_IF_LT( i_cost8x8, analysis.l0.i_cost4x8[i],
                                         h->mb.i_sub_partition[i], D_L0_4x8 );

                            i_cost += i_cost8x8 - analysis.l0.me8x8[i].cost;
                        }
                        x264_mb_cache_mv_p8x8( h, &analysis, i );
                    }
                    analysis.l0.i_cost8x8 = i_cost;
                }
            }

            /* Now do 16x8/8x16 */
            i_thresh16x8 = analysis.l0.me8x8[1].cost_mv + analysis.l0.me8x8[2].cost_mv;
            if( ( flags & X264_ANALYSE_PSUB16x16 ) &&
                analysis.l0.i_cost8x8 < analysis.l0.me16x16.cost + i_thresh16x8 )
            {
                int i_avg_mv_ref_cost = (analysis.l0.me8x8[2].cost_mv + analysis.l0.me8x8[2].i_ref_cost
                                      + analysis.l0.me8x8[3].cost_mv + analysis.l0.me8x8[3].i_ref_cost + 1) >> 1;
                analysis.i_cost_est16x8[1] = analysis.i_satd8x8[0][2] + analysis.i_satd8x8[0][3] + i_avg_mv_ref_cost;

                x264_mb_analyse_inter_p16x8( h, &analysis, i_cost );
                COPY3_IF_LT( i_cost, analysis.l0.i_cost16x8, i_type, P_L0, i_partition, D_16x8 );

                i_avg_mv_ref_cost = (analysis.l0.me8x8[1].cost_mv + analysis.l0.me8x8[1].i_ref_cost
                                  + analysis.l0.me8x8[3].cost_mv + analysis.l0.me8x8[3].i_ref_cost + 1) >> 1;
                analysis.i_cost_est8x16[1] = analysis.i_satd8x8[0][1] + analysis.i_satd8x8[0][3] + i_avg_mv_ref_cost;

                x264_mb_analyse_inter_p8x16( h, &analysis, i_cost );
                COPY3_IF_LT( i_cost, analysis.l0.i_cost8x16, i_type, P_L0, i_partition, D_8x16 );
            }

            h->mb.i_partition = i_partition;

            /* refine qpel */
            //FIXME mb_type costs?
            if( analysis.i_mbrd || !h->mb.i_subpel_refine )
            {
                /* refine later */
            }
            else if( i_partition == D_16x16 )
            {
                x264_me_refine_qpel( h, &analysis.l0.me16x16 );
                i_cost = analysis.l0.me16x16.cost;
            }
            else if( i_partition == D_16x8 )
            {
                x264_me_refine_qpel( h, &analysis.l0.me16x8[0] );
                x264_me_refine_qpel( h, &analysis.l0.me16x8[1] );
                i_cost = analysis.l0.me16x8[0].cost + analysis.l0.me16x8[1].cost;
            }
            else if( i_partition == D_8x16 )
            {
                x264_me_refine_qpel( h, &analysis.l0.me8x16[0] );
                x264_me_refine_qpel( h, &analysis.l0.me8x16[1] );
                i_cost = analysis.l0.me8x16[0].cost + analysis.l0.me8x16[1].cost;
            }
            else if( i_partition == D_8x8 )
            {
                i_cost = 0;
                for( int i8x8 = 0; i8x8 < 4; i8x8++ )
                {
                    switch( h->mb.i_sub_partition[i8x8] )
                    {
                        case D_L0_8x8:
                            x264_me_refine_qpel( h, &analysis.l0.me8x8[i8x8] );
                            i_cost += analysis.l0.me8x8[i8x8].cost;
                            break;
                        case D_L0_8x4:
                            x264_me_refine_qpel( h, &analysis.l0.me8x4[i8x8][0] );
                            x264_me_refine_qpel( h, &analysis.l0.me8x4[i8x8][1] );
                            i_cost += analysis.l0.me8x4[i8x8][0].cost +
                                      analysis.l0.me8x4[i8x8][1].cost;
                            break;
                        case D_L0_4x8:
                            x264_me_refine_qpel( h, &analysis.l0.me4x8[i8x8][0] );
                            x264_me_refine_qpel( h, &analysis.l0.me4x8[i8x8][1] );
                            i_cost += analysis.l0.me4x8[i8x8][0].cost +
                                      analysis.l0.me4x8[i8x8][1].cost;
                            break;

                        case D_L0_4x4:
                            x264_me_refine_qpel( h, &analysis.l0.me4x4[i8x8][0] );
                            x264_me_refine_qpel( h, &analysis.l0.me4x4[i8x8][1] );
                            x264_me_refine_qpel( h, &analysis.l0.me4x4[i8x8][2] );
                            x264_me_refine_qpel( h, &analysis.l0.me4x4[i8x8][3] );
                            i_cost += analysis.l0.me4x4[i8x8][0].cost +
                                      analysis.l0.me4x4[i8x8][1].cost +
                                      analysis.l0.me4x4[i8x8][2].cost +
                                      analysis.l0.me4x4[i8x8][3].cost;
                            break;
                        default:
                            x264_log( h, X264_LOG_ERROR, "internal error (!8x8 && !4x4)\n" );
                            break;
                    }
                }
            }

            if( h->mb.b_chroma_me )
            {
                x264_mb_analyse_intra_chroma( h, &analysis );
                x264_mb_analyse_intra( h, &analysis, i_cost - analysis.i_satd_i8x8chroma );
                analysis.i_satd_i16x16 += analysis.i_satd_i8x8chroma;
                analysis.i_satd_i8x8 += analysis.i_satd_i8x8chroma;
                analysis.i_satd_i4x4 += analysis.i_satd_i8x8chroma;
            }
            else
                x264_mb_analyse_intra( h, &analysis, i_cost );

            i_satd_inter = i_cost;
            i_satd_intra = X264_MIN3( analysis.i_satd_i16x16,
                                      analysis.i_satd_i8x8,
                                      analysis.i_satd_i4x4 );

            if( analysis.i_mbrd )
            {
                x264_mb_analyse_p_rd( h, &analysis, X264_MIN(i_satd_inter, i_satd_intra) );
                i_type = P_L0;
                i_partition = D_16x16;
                i_cost = analysis.l0.i_rd16x16;
                COPY2_IF_LT( i_cost, analysis.l0.i_cost16x8, i_partition, D_16x8 );
                COPY2_IF_LT( i_cost, analysis.l0.i_cost8x16, i_partition, D_8x16 );
                COPY3_IF_LT( i_cost, analysis.l0.i_cost8x8, i_partition, D_8x8, i_type, P_8x8 );
                h->mb.i_type = i_type;
                h->mb.i_partition = i_partition;
                if( i_cost < COST_MAX )
                    x264_mb_analyse_transform_rd( h, &analysis, &i_satd_inter, &i_cost );
                x264_intra_rd( h, &analysis, i_satd_inter * 5/4 );
            }

            COPY2_IF_LT( i_cost, analysis.i_satd_i16x16, i_type, I_16x16 );
            COPY2_IF_LT( i_cost, analysis.i_satd_i8x8, i_type, I_8x8 );
            COPY2_IF_LT( i_cost, analysis.i_satd_i4x4, i_type, I_4x4 );
            COPY2_IF_LT( i_cost, analysis.i_satd_pcm, i_type, I_PCM );

            h->mb.i_type = i_type;

            if( analysis.b_force_intra && !IS_INTRA(i_type) )
            {
                /* Intra masking: copy fdec to fenc and re-encode the block as intra in order to make it appear as if
                 * it was an inter block. */
                x264_analyse_update_cache( h, &analysis );
                x264_macroblock_encode( h );
                h->mc.copy[PIXEL_16x16]( h->mb.pic.p_fenc[0], FENC_STRIDE, h->mb.pic.p_fdec[0], FDEC_STRIDE, 16 );
                h->mc.copy[PIXEL_8x8]  ( h->mb.pic.p_fenc[1], FENC_STRIDE, h->mb.pic.p_fdec[1], FDEC_STRIDE, 8 );
                h->mc.copy[PIXEL_8x8]  ( h->mb.pic.p_fenc[2], FENC_STRIDE, h->mb.pic.p_fdec[2], FDEC_STRIDE, 8 );
                x264_mb_analyse_init_qp( h, &analysis, X264_MAX( h->mb.i_qp - h->mb.ip_offset, h->param.rc.i_qp_min ) );
                goto intra_analysis;
            }

            if( analysis.i_mbrd >= 2 && h->mb.i_type != I_PCM )
            {
                if( IS_INTRA( h->mb.i_type ) )
                {
                    x264_intra_rd_refine( h, &analysis );
                }
                else if( i_partition == D_16x16 )
                {
                    x264_macroblock_cache_ref( h, 0, 0, 4, 4, 0, analysis.l0.me16x16.i_ref );
                    analysis.l0.me16x16.cost = i_cost;
                    x264_me_refine_qpel_rd( h, &analysis.l0.me16x16, analysis.i_lambda2, 0, 0 );
                }
                else if( i_partition == D_16x8 )
                {
                    h->mb.i_sub_partition[0] = h->mb.i_sub_partition[1] =
                    h->mb.i_sub_partition[2] = h->mb.i_sub_partition[3] = D_L0_8x8;
                    x264_macroblock_cache_ref( h, 0, 0, 4, 2, 0, analysis.l0.me16x8[0].i_ref );
                    x264_macroblock_cache_ref( h, 0, 2, 4, 2, 0, analysis.l0.me16x8[1].i_ref );
                    x264_me_refine_qpel_rd( h, &analysis.l0.me16x8[0], analysis.i_lambda2, 0, 0 );
                    x264_me_refine_qpel_rd( h, &analysis.l0.me16x8[1], analysis.i_lambda2, 8, 0 );
                }
                else if( i_partition == D_8x16 )
                {
                    h->mb.i_sub_partition[0] = h->mb.i_sub_partition[1] =
                    h->mb.i_sub_partition[2] = h->mb.i_sub_partition[3] = D_L0_8x8;
                    x264_macroblock_cache_ref( h, 0, 0, 2, 4, 0, analysis.l0.me8x16[0].i_ref );
                    x264_macroblock_cache_ref( h, 2, 0, 2, 4, 0, analysis.l0.me8x16[1].i_ref );
                    x264_me_refine_qpel_rd( h, &analysis.l0.me8x16[0], analysis.i_lambda2, 0, 0 );
                    x264_me_refine_qpel_rd( h, &analysis.l0.me8x16[1], analysis.i_lambda2, 4, 0 );
                }
                else if( i_partition == D_8x8 )
                {
                    x264_analyse_update_cache( h, &analysis );
                    for( int i8x8 = 0; i8x8 < 4; i8x8++ )
                    {
                        if( h->mb.i_sub_partition[i8x8] == D_L0_8x8 )
                        {
                            x264_me_refine_qpel_rd( h, &analysis.l0.me8x8[i8x8], analysis.i_lambda2, i8x8*4, 0 );
                        }
                        else if( h->mb.i_sub_partition[i8x8] == D_L0_8x4 )
                        {
                            x264_me_refine_qpel_rd( h, &analysis.l0.me8x4[i8x8][0], analysis.i_lambda2, i8x8*4+0, 0 );
                            x264_me_refine_qpel_rd( h, &analysis.l0.me8x4[i8x8][1], analysis.i_lambda2, i8x8*4+2, 0 );
                        }
                        else if( h->mb.i_sub_partition[i8x8] == D_L0_4x8 )
                        {
                            x264_me_refine_qpel_rd( h, &analysis.l0.me4x8[i8x8][0], analysis.i_lambda2, i8x8*4+0, 0 );
                            x264_me_refine_qpel_rd( h, &analysis.l0.me4x8[i8x8][1], analysis.i_lambda2, i8x8*4+1, 0 );
                        }
                        else if( h->mb.i_sub_partition[i8x8] == D_L0_4x4 )
                        {
                            x264_me_refine_qpel_rd( h, &analysis.l0.me4x4[i8x8][0], analysis.i_lambda2, i8x8*4+0, 0 );
                            x264_me_refine_qpel_rd( h, &analysis.l0.me4x4[i8x8][1], analysis.i_lambda2, i8x8*4+1, 0 );
                            x264_me_refine_qpel_rd( h, &analysis.l0.me4x4[i8x8][2], analysis.i_lambda2, i8x8*4+2, 0 );
                            x264_me_refine_qpel_rd( h, &analysis.l0.me4x4[i8x8][3], analysis.i_lambda2, i8x8*4+3, 0 );
                        }
                    }
                }
            }
        }
    }
    else if( h->sh.i_type == SLICE_TYPE_B )
    {
        int i_bskip_cost = COST_MAX;
        int b_skip = 0;

        if( analysis.i_mbrd )
            x264_mb_init_fenc_cache( h, analysis.i_mbrd >= 2 );

        h->mb.i_type = B_SKIP;
        if( h->mb.b_direct_auto_write )
        {
            /* direct=auto heuristic: prefer whichever mode allows more Skip macroblocks */
            for( int i = 0; i < 2; i++ )
            {
                int b_changed = 1;
                h->sh.b_direct_spatial_mv_pred ^= 1;
                analysis.b_direct_available = x264_mb_predict_mv_direct16x16( h, i && analysis.b_direct_available ? &b_changed : NULL );
                if( analysis.b_direct_available )
                {
                    if( b_changed )
                    {
                        x264_mb_mc( h );
                        b_skip = x264_macroblock_probe_bskip( h );
                    }
                    h->stat.frame.i_direct_score[ h->sh.b_direct_spatial_mv_pred ] += b_skip;
                }
                else
                    b_skip = 0;
            }
        }
        else
            analysis.b_direct_available = x264_mb_predict_mv_direct16x16( h, NULL );

        analysis.b_try_skip = 0;
        if( analysis.b_direct_available )
        {
            if( !h->mb.b_direct_auto_write )
                x264_mb_mc( h );
            if( analysis.i_mbrd )
            {
                i_bskip_cost = ssd_mb( h );
                /* 6 = minimum cavlc cost of a non-skipped MB */
                b_skip = h->mb.b_skip_mc = i_bskip_cost <= ((6 * analysis.i_lambda2 + 128) >> 8);
            }
            else if( !h->mb.b_direct_auto_write )
            {
                /* Conditioning the probe on neighboring block types
                 * doesn't seem to help speed or quality. */
                analysis.b_try_skip = x264_macroblock_probe_bskip( h );
                if( h->param.analyse.i_subpel_refine < 3 )
                    b_skip = analysis.b_try_skip;
            }
            /* Set up MVs for future predictors */
            if( b_skip )
            {
                for( int i = 0; i < h->mb.pic.i_fref[0]; i++ )
                    M32( h->mb.mvr[0][i][h->mb.i_mb_xy] ) = 0;
                for( int i = 0; i < h->mb.pic.i_fref[1]; i++ )
                    M32( h->mb.mvr[1][i][h->mb.i_mb_xy] ) = 0;
            }
        }

        if( !b_skip )
        {
            const unsigned int flags = h->param.analyse.inter;
            int i_type;
            int i_partition;
            int i_satd_inter;
            h->mb.b_skip_mc = 0;
            h->mb.i_type = B_DIRECT;

            x264_mb_analyse_load_costs( h, &analysis );

            /* select best inter mode */
            /* direct must be first */
            if( analysis.b_direct_available )
                x264_mb_analyse_inter_direct( h, &analysis );

            x264_mb_analyse_inter_b16x16( h, &analysis );

            if( h->mb.i_type == B_SKIP )
            {
                for( int i = 1; i < h->mb.pic.i_fref[0]; i++ )
                    M32( h->mb.mvr[0][i][h->mb.i_mb_xy] ) = 0;
                for( int i = 1; i < h->mb.pic.i_fref[1]; i++ )
                    M32( h->mb.mvr[0][i][h->mb.i_mb_xy] ) = 0;
                return;
            }

            i_type = B_L0_L0;
            i_partition = D_16x16;
            i_cost = analysis.l0.me16x16.cost;
            COPY2_IF_LT( i_cost, analysis.l1.me16x16.cost, i_type, B_L1_L1 );
            COPY2_IF_LT( i_cost, analysis.i_cost16x16bi, i_type, B_BI_BI );
            COPY2_IF_LT( i_cost, analysis.i_cost16x16direct, i_type, B_DIRECT );

            if( analysis.i_mbrd && analysis.i_cost16x16direct <= i_cost * 33/32 )
            {
                x264_mb_analyse_b_rd( h, &analysis, i_cost );
                if( i_bskip_cost < analysis.i_rd16x16direct &&
                    i_bskip_cost < analysis.i_rd16x16bi &&
                    i_bskip_cost < analysis.l0.i_rd16x16 &&
                    i_bskip_cost < analysis.l1.i_rd16x16 )
                {
                    h->mb.i_type = B_SKIP;
                    x264_analyse_update_cache( h, &analysis );
                    return;
                }
            }

            if( flags & X264_ANALYSE_BSUB16x16 )
            {
                if( h->param.analyse.b_mixed_references )
                    x264_mb_analyse_inter_b8x8_mixed_ref( h, &analysis );
                else
                    x264_mb_analyse_inter_b8x8( h, &analysis );

                COPY3_IF_LT( i_cost, analysis.i_cost8x8bi, i_type, B_8x8, i_partition, D_8x8 );

                /* Try to estimate the cost of b16x8/b8x16 based on the satd scores of the b8x8 modes */
                int i_cost_est16x8bi_total = 0, i_cost_est8x16bi_total = 0;
                int i_mb_type, i_partition16x8[2], i_partition8x16[2];
                for( int i = 0; i < 2; i++ )
                {
                    int avg_l0_mv_ref_cost, avg_l1_mv_ref_cost;
                    int i_l0_satd, i_l1_satd, i_bi_satd, i_best_cost;
                    // 16x8
                    i_best_cost = COST_MAX;
                    i_l0_satd = analysis.i_satd8x8[0][i*2] + analysis.i_satd8x8[0][i*2+1];
                    i_l1_satd = analysis.i_satd8x8[1][i*2] + analysis.i_satd8x8[1][i*2+1];
                    i_bi_satd = analysis.i_satd8x8[2][i*2] + analysis.i_satd8x8[2][i*2+1];
                    avg_l0_mv_ref_cost = ( analysis.l0.me8x8[i*2].cost_mv + analysis.l0.me8x8[i*2].i_ref_cost
                                         + analysis.l0.me8x8[i*2+1].cost_mv + analysis.l0.me8x8[i*2+1].i_ref_cost + 1 ) >> 1;
                    avg_l1_mv_ref_cost = ( analysis.l1.me8x8[i*2].cost_mv + analysis.l1.me8x8[i*2].i_ref_cost
                                         + analysis.l1.me8x8[i*2+1].cost_mv + analysis.l1.me8x8[i*2+1].i_ref_cost + 1 ) >> 1;
                    COPY2_IF_LT( i_best_cost, i_l0_satd + avg_l0_mv_ref_cost, i_partition16x8[i], D_L0_8x8 );
                    COPY2_IF_LT( i_best_cost, i_l1_satd + avg_l1_mv_ref_cost, i_partition16x8[i], D_L1_8x8 );
                    COPY2_IF_LT( i_best_cost, i_bi_satd + avg_l0_mv_ref_cost + avg_l1_mv_ref_cost, i_partition16x8[i], D_BI_8x8 );
                    analysis.i_cost_est16x8[i] = i_best_cost;

                    // 8x16
                    i_best_cost = COST_MAX;
                    i_l0_satd = analysis.i_satd8x8[0][i] + analysis.i_satd8x8[0][i+2];
                    i_l1_satd = analysis.i_satd8x8[1][i] + analysis.i_satd8x8[1][i+2];
                    i_bi_satd = analysis.i_satd8x8[2][i] + analysis.i_satd8x8[2][i+2];
                    avg_l0_mv_ref_cost = ( analysis.l0.me8x8[i].cost_mv + analysis.l0.me8x8[i].i_ref_cost
                                         + analysis.l0.me8x8[i+2].cost_mv + analysis.l0.me8x8[i+2].i_ref_cost + 1 ) >> 1;
                    avg_l1_mv_ref_cost = ( analysis.l1.me8x8[i].cost_mv + analysis.l1.me8x8[i].i_ref_cost
                                         + analysis.l1.me8x8[i+2].cost_mv + analysis.l1.me8x8[i+2].i_ref_cost + 1 ) >> 1;
                    COPY2_IF_LT( i_best_cost, i_l0_satd + avg_l0_mv_ref_cost, i_partition8x16[i], D_L0_8x8 );
                    COPY2_IF_LT( i_best_cost, i_l1_satd + avg_l1_mv_ref_cost, i_partition8x16[i], D_L1_8x8 );
                    COPY2_IF_LT( i_best_cost, i_bi_satd + avg_l0_mv_ref_cost + avg_l1_mv_ref_cost, i_partition8x16[i], D_BI_8x8 );
                    analysis.i_cost_est8x16[i] = i_best_cost;
                }
                i_mb_type = B_L0_L0 + (i_partition16x8[0]>>2) * 3 + (i_partition16x8[1]>>2);
                analysis.i_cost_est16x8[1] += analysis.i_lambda * i_mb_b16x8_cost_table[i_mb_type];
                i_cost_est16x8bi_total = analysis.i_cost_est16x8[0] + analysis.i_cost_est16x8[1];
                i_mb_type = B_L0_L0 + (i_partition8x16[0]>>2) * 3 + (i_partition8x16[1]>>2);
                analysis.i_cost_est8x16[1] += analysis.i_lambda * i_mb_b16x8_cost_table[i_mb_type];
                i_cost_est8x16bi_total = analysis.i_cost_est8x16[0] + analysis.i_cost_est8x16[1];

                /* We can gain a little speed by checking the mode with the lowest estimated cost first */
                int try_16x8_first = i_cost_est16x8bi_total < i_cost_est8x16bi_total;
                if( try_16x8_first && i_cost_est16x8bi_total < i_cost )
                {
                    x264_mb_analyse_inter_b16x8( h, &analysis, i_cost );
                    COPY3_IF_LT( i_cost, analysis.i_cost16x8bi, i_type, analysis.i_mb_type16x8, i_partition, D_16x8 );
                }
                if( i_cost_est8x16bi_total < i_cost )
                {
                    x264_mb_analyse_inter_b8x16( h, &analysis, i_cost );
                    COPY3_IF_LT( i_cost, analysis.i_cost8x16bi, i_type, analysis.i_mb_type8x16, i_partition, D_8x16 );
                }
                if( !try_16x8_first && i_cost_est16x8bi_total < i_cost )
                {
                    x264_mb_analyse_inter_b16x8( h, &analysis, i_cost );
                    COPY3_IF_LT( i_cost, analysis.i_cost16x8bi, i_type, analysis.i_mb_type16x8, i_partition, D_16x8 );
                }
            }

            if( analysis.i_mbrd || !h->mb.i_subpel_refine )
            {
                /* refine later */
            }
            /* refine qpel */
            else if( i_partition == D_16x16 )
            {
                analysis.l0.me16x16.cost -= analysis.i_lambda * i_mb_b_cost_table[B_L0_L0];
                analysis.l1.me16x16.cost -= analysis.i_lambda * i_mb_b_cost_table[B_L1_L1];
                if( i_type == B_L0_L0 )
                {
                    x264_me_refine_qpel( h, &analysis.l0.me16x16 );
                    i_cost = analysis.l0.me16x16.cost
                           + analysis.i_lambda * i_mb_b_cost_table[B_L0_L0];
                }
                else if( i_type == B_L1_L1 )
                {
                    x264_me_refine_qpel( h, &analysis.l1.me16x16 );
                    i_cost = analysis.l1.me16x16.cost
                           + analysis.i_lambda * i_mb_b_cost_table[B_L1_L1];
                }
                else if( i_type == B_BI_BI )
                {
                    x264_me_refine_qpel( h, &analysis.l0.bi16x16 );
                    x264_me_refine_qpel( h, &analysis.l1.bi16x16 );
                }
            }
            else if( i_partition == D_16x8 )
            {
                for( int i = 0; i < 2; i++ )
                {
                    if( analysis.i_mb_partition16x8[i] != D_L1_8x8 )
                        x264_me_refine_qpel( h, &analysis.l0.me16x8[i] );
                    if( analysis.i_mb_partition16x8[i] != D_L0_8x8 )
                        x264_me_refine_qpel( h, &analysis.l1.me16x8[i] );
                }
            }
            else if( i_partition == D_8x16 )
            {
                for( int i = 0; i < 2; i++ )
                {
                    if( analysis.i_mb_partition8x16[i] != D_L1_8x8 )
                        x264_me_refine_qpel( h, &analysis.l0.me8x16[i] );
                    if( analysis.i_mb_partition8x16[i] != D_L0_8x8 )
                        x264_me_refine_qpel( h, &analysis.l1.me8x16[i] );
                }
            }
            else if( i_partition == D_8x8 )
            {
                for( int i = 0; i < 4; i++ )
                {
                    x264_me_t *m;
                    int i_part_cost_old;
                    int i_type_cost;
                    int i_part_type = h->mb.i_sub_partition[i];
                    int b_bidir = (i_part_type == D_BI_8x8);

                    if( i_part_type == D_DIRECT_8x8 )
                        continue;
                    if( x264_mb_partition_listX_table[0][i_part_type] )
                    {
                        m = &analysis.l0.me8x8[i];
                        i_part_cost_old = m->cost;
                        i_type_cost = analysis.i_lambda * i_sub_mb_b_cost_table[D_L0_8x8];
                        m->cost -= i_type_cost;
                        x264_me_refine_qpel( h, m );
                        if( !b_bidir )
                            analysis.i_cost8x8bi += m->cost + i_type_cost - i_part_cost_old;
                    }
                    if( x264_mb_partition_listX_table[1][i_part_type] )
                    {
                        m = &analysis.l1.me8x8[i];
                        i_part_cost_old = m->cost;
                        i_type_cost = analysis.i_lambda * i_sub_mb_b_cost_table[D_L1_8x8];
                        m->cost -= i_type_cost;
                        x264_me_refine_qpel( h, m );
                        if( !b_bidir )
                            analysis.i_cost8x8bi += m->cost + i_type_cost - i_part_cost_old;
                    }
                    /* TODO: update mvp? */
                }
            }

            i_satd_inter = i_cost;

            if( analysis.i_mbrd )
            {
                x264_mb_analyse_b_rd( h, &analysis, i_satd_inter );
                i_type = B_SKIP;
                i_cost = i_bskip_cost;
                i_partition = D_16x16;
                COPY2_IF_LT( i_cost, analysis.l0.i_rd16x16, i_type, B_L0_L0 );
                COPY2_IF_LT( i_cost, analysis.l1.i_rd16x16, i_type, B_L1_L1 );
                COPY2_IF_LT( i_cost, analysis.i_rd16x16bi, i_type, B_BI_BI );
                COPY2_IF_LT( i_cost, analysis.i_rd16x16direct, i_type, B_DIRECT );
                COPY3_IF_LT( i_cost, analysis.i_rd16x8bi, i_type, analysis.i_mb_type16x8, i_partition, D_16x8 );
                COPY3_IF_LT( i_cost, analysis.i_rd8x16bi, i_type, analysis.i_mb_type8x16, i_partition, D_8x16 );
                COPY3_IF_LT( i_cost, analysis.i_rd8x8bi, i_type, B_8x8, i_partition, D_8x8 );

                h->mb.i_type = i_type;
                h->mb.i_partition = i_partition;
            }

            x264_mb_analyse_intra( h, &analysis, i_satd_inter );

            if( analysis.i_mbrd )
            {
                x264_mb_analyse_transform_rd( h, &analysis, &i_satd_inter, &i_cost );
                x264_intra_rd( h, &analysis, i_satd_inter * 17/16 );
            }

            COPY2_IF_LT( i_cost, analysis.i_satd_i16x16, i_type, I_16x16 );
            COPY2_IF_LT( i_cost, analysis.i_satd_i8x8, i_type, I_8x8 );
            COPY2_IF_LT( i_cost, analysis.i_satd_i4x4, i_type, I_4x4 );
            COPY2_IF_LT( i_cost, analysis.i_satd_pcm, i_type, I_PCM );

            h->mb.i_type = i_type;
            h->mb.i_partition = i_partition;

            if( analysis.i_mbrd >= 2 && IS_INTRA( i_type ) && i_type != I_PCM )
                x264_intra_rd_refine( h, &analysis );
            if( h->mb.i_subpel_refine >= 5 )
                x264_refine_bidir( h, &analysis );

            if( analysis.i_mbrd >= 2 && i_type > B_DIRECT && i_type < B_SKIP )
            {
                int i_biweight;
                x264_analyse_update_cache( h, &analysis );

                if( i_partition == D_16x16 )
                {
                    if( i_type == B_L0_L0 )
                    {
                        analysis.l0.me16x16.cost = i_cost;
                        x264_me_refine_qpel_rd( h, &analysis.l0.me16x16, analysis.i_lambda2, 0, 0 );
                    }
                    else if( i_type == B_L1_L1 )
                    {
                        analysis.l1.me16x16.cost = i_cost;
                        x264_me_refine_qpel_rd( h, &analysis.l1.me16x16, analysis.i_lambda2, 0, 1 );
                    }
                    else if( i_type == B_BI_BI )
                    {
                        i_biweight = h->mb.bipred_weight[analysis.l0.bi16x16.i_ref][analysis.l1.bi16x16.i_ref];
                        x264_me_refine_bidir_rd( h, &analysis.l0.bi16x16, &analysis.l1.bi16x16, i_biweight, 0, analysis.i_lambda2 );
                    }
                }
                else if( i_partition == D_16x8 )
                {
                    for( int i = 0; i < 2; i++ )
                    {
                        h->mb.i_sub_partition[i*2] = h->mb.i_sub_partition[i*2+1] = analysis.i_mb_partition16x8[i];
                        if( analysis.i_mb_partition16x8[i] == D_L0_8x8 )
                            x264_me_refine_qpel_rd( h, &analysis.l0.me16x8[i], analysis.i_lambda2, i*8, 0 );
                        else if( analysis.i_mb_partition16x8[i] == D_L1_8x8 )
                            x264_me_refine_qpel_rd( h, &analysis.l1.me16x8[i], analysis.i_lambda2, i*8, 1 );
                        else if( analysis.i_mb_partition16x8[i] == D_BI_8x8 )
                        {
                            i_biweight = h->mb.bipred_weight[analysis.l0.me16x8[i].i_ref][analysis.l1.me16x8[i].i_ref];
                            x264_me_refine_bidir_rd( h, &analysis.l0.me16x8[i], &analysis.l1.me16x8[i], i_biweight, i*2, analysis.i_lambda2 );
                        }
                    }
                }
                else if( i_partition == D_8x16 )
                {
                    for( int i = 0; i < 2; i++ )
                    {
                        h->mb.i_sub_partition[i] = h->mb.i_sub_partition[i+2] = analysis.i_mb_partition8x16[i];
                        if( analysis.i_mb_partition8x16[i] == D_L0_8x8 )
                            x264_me_refine_qpel_rd( h, &analysis.l0.me8x16[i], analysis.i_lambda2, i*4, 0 );
                        else if( analysis.i_mb_partition8x16[i] == D_L1_8x8 )
                            x264_me_refine_qpel_rd( h, &analysis.l1.me8x16[i], analysis.i_lambda2, i*4, 1 );
                        else if( analysis.i_mb_partition8x16[i] == D_BI_8x8 )
                        {
                            i_biweight = h->mb.bipred_weight[analysis.l0.me8x16[i].i_ref][analysis.l1.me8x16[i].i_ref];
                            x264_me_refine_bidir_rd( h, &analysis.l0.me8x16[i], &analysis.l1.me8x16[i], i_biweight, i, analysis.i_lambda2 );
                        }
                    }
                }
                else if( i_partition == D_8x8 )
                {
                    for( int i = 0; i < 4; i++ )
                    {
                        if( h->mb.i_sub_partition[i] == D_L0_8x8 )
                            x264_me_refine_qpel_rd( h, &analysis.l0.me8x8[i], analysis.i_lambda2, i*4, 0 );
                        else if( h->mb.i_sub_partition[i] == D_L1_8x8 )
                            x264_me_refine_qpel_rd( h, &analysis.l1.me8x8[i], analysis.i_lambda2, i*4, 1 );
                        else if( h->mb.i_sub_partition[i] == D_BI_8x8 )
                        {
                            i_biweight = h->mb.bipred_weight[analysis.l0.me8x8[i].i_ref][analysis.l1.me8x8[i].i_ref];
                            x264_me_refine_bidir_rd( h, &analysis.l0.me8x8[i], &analysis.l1.me8x8[i], i_biweight, i, analysis.i_lambda2 );
                        }
                    }
                }
            }
        }
    }

    x264_analyse_update_cache( h, &analysis );

    /* In rare cases we can end up qpel-RDing our way back to a larger partition size
     * without realizing it.  Check for this and account for it if necessary. */
    if( analysis.i_mbrd >= 2 )
    {
        /* Don't bother with bipred or 8x8-and-below, the odds are incredibly low. */
        static const uint8_t check_mv_lists[X264_MBTYPE_MAX] = {[P_L0]=1, [B_L0_L0]=1, [B_L1_L1]=2};
        int list = check_mv_lists[h->mb.i_type] - 1;
        if( list >= 0 && h->mb.i_partition != D_16x16 &&
            M32( &h->mb.cache.mv[list][x264_scan8[0]] ) == M32( &h->mb.cache.mv[list][x264_scan8[12]] ) &&
            h->mb.cache.ref[list][x264_scan8[0]] == h->mb.cache.ref[list][x264_scan8[12]] )
                h->mb.i_partition = D_16x16;
    }

    if( !analysis.i_mbrd )
        x264_mb_analyse_transform( h );

    if( analysis.i_mbrd == 3 && !IS_SKIP(h->mb.i_type) )
        x264_mb_analyse_qp_rd( h, &analysis );

    h->mb.b_trellis = h->param.analyse.i_trellis;
    h->mb.b_noise_reduction = !!h->param.analyse.i_noise_reduction;
    if( !IS_SKIP(h->mb.i_type) && h->mb.i_psy_trellis && h->param.analyse.i_trellis == 1 )
        x264_psy_trellis_init( h, 0 );
    if( h->mb.b_trellis == 1 || h->mb.b_noise_reduction )
        h->mb.i_skip_intra = 0;
}

/*-------------------- Update MB from the analysis ----------------------*/
static void x264_analyse_update_cache( x264_t *h, x264_mb_analysis_t *a  )
{
    switch( h->mb.i_type )
    {
        case I_4x4:
            for( int i = 0; i < 16; i++ )
                h->mb.cache.intra4x4_pred_mode[x264_scan8[i]] = a->i_predict4x4[i];

            x264_mb_analyse_intra_chroma( h, a );
            break;
        case I_8x8:
            for( int i = 0; i < 4; i++ )
                x264_macroblock_cache_intra8x8_pred( h, 2*(i&1), 2*(i>>1), a->i_predict8x8[i] );

            x264_mb_analyse_intra_chroma( h, a );
            break;
        case I_16x16:
            h->mb.i_intra16x16_pred_mode = a->i_predict16x16;
            x264_mb_analyse_intra_chroma( h, a );
            break;

        case I_PCM:
            break;

        case P_L0:
            switch( h->mb.i_partition )
            {
                case D_16x16:
                    x264_macroblock_cache_ref( h, 0, 0, 4, 4, 0, a->l0.me16x16.i_ref );
                    x264_macroblock_cache_mv_ptr( h, 0, 0, 4, 4, 0, a->l0.me16x16.mv );
                    break;

                case D_16x8:
                    x264_macroblock_cache_ref( h, 0, 0, 4, 2, 0, a->l0.me16x8[0].i_ref );
                    x264_macroblock_cache_ref( h, 0, 2, 4, 2, 0, a->l0.me16x8[1].i_ref );
                    x264_macroblock_cache_mv_ptr( h, 0, 0, 4, 2, 0, a->l0.me16x8[0].mv );
                    x264_macroblock_cache_mv_ptr( h, 0, 2, 4, 2, 0, a->l0.me16x8[1].mv );
                    break;

                case D_8x16:
                    x264_macroblock_cache_ref( h, 0, 0, 2, 4, 0, a->l0.me8x16[0].i_ref );
                    x264_macroblock_cache_ref( h, 2, 0, 2, 4, 0, a->l0.me8x16[1].i_ref );
                    x264_macroblock_cache_mv_ptr( h, 0, 0, 2, 4, 0, a->l0.me8x16[0].mv );
                    x264_macroblock_cache_mv_ptr( h, 2, 0, 2, 4, 0, a->l0.me8x16[1].mv );
                    break;

                default:
                    x264_log( h, X264_LOG_ERROR, "internal error P_L0 and partition=%d\n", h->mb.i_partition );
                    break;
            }
            break;

        case P_8x8:
            x264_macroblock_cache_ref( h, 0, 0, 2, 2, 0, a->l0.me8x8[0].i_ref );
            x264_macroblock_cache_ref( h, 2, 0, 2, 2, 0, a->l0.me8x8[1].i_ref );
            x264_macroblock_cache_ref( h, 0, 2, 2, 2, 0, a->l0.me8x8[2].i_ref );
            x264_macroblock_cache_ref( h, 2, 2, 2, 2, 0, a->l0.me8x8[3].i_ref );
            for( int i = 0; i < 4; i++ )
                x264_mb_cache_mv_p8x8( h, a, i );
            break;

        case P_SKIP:
        {
            h->mb.i_partition = D_16x16;
            x264_macroblock_cache_ref( h, 0, 0, 4, 4, 0, 0 );
            x264_macroblock_cache_mv_ptr( h, 0, 0, 4, 4, 0, h->mb.cache.pskip_mv );
            break;
        }

        case B_SKIP:
        case B_DIRECT:
            h->mb.i_partition = h->mb.cache.direct_partition;
            x264_mb_load_mv_direct8x8( h, 0 );
            x264_mb_load_mv_direct8x8( h, 1 );
            x264_mb_load_mv_direct8x8( h, 2 );
            x264_mb_load_mv_direct8x8( h, 3 );
            break;

        case B_8x8:
            /* optimize: cache might not need to be rewritten */
            for( int i = 0; i < 4; i++ )
                x264_mb_cache_mv_b8x8( h, a, i, 1 );
            break;

        default: /* the rest of the B types */
            switch( h->mb.i_partition )
            {
            case D_16x16:
                switch( h->mb.i_type )
                {
                case B_L0_L0:
                    x264_macroblock_cache_ref( h, 0, 0, 4, 4, 0, a->l0.me16x16.i_ref );
                    x264_macroblock_cache_mv_ptr( h, 0, 0, 4, 4, 0, a->l0.me16x16.mv );

                    x264_macroblock_cache_ref( h, 0, 0, 4, 4, 1, -1 );
                    x264_macroblock_cache_mv ( h, 0, 0, 4, 4, 1, 0 );
                    x264_macroblock_cache_mvd( h, 0, 0, 4, 4, 1, 0 );
                    break;
                case B_L1_L1:
                    x264_macroblock_cache_ref( h, 0, 0, 4, 4, 0, -1 );
                    x264_macroblock_cache_mv ( h, 0, 0, 4, 4, 0, 0 );
                    x264_macroblock_cache_mvd( h, 0, 0, 4, 4, 0, 0 );

                    x264_macroblock_cache_ref( h, 0, 0, 4, 4, 1, a->l1.me16x16.i_ref );
                    x264_macroblock_cache_mv_ptr( h, 0, 0, 4, 4, 1, a->l1.me16x16.mv );
                    break;
                case B_BI_BI:
                    x264_macroblock_cache_ref( h, 0, 0, 4, 4, 0, a->l0.bi16x16.i_ref );
                    x264_macroblock_cache_mv_ptr( h, 0, 0, 4, 4, 0, a->l0.bi16x16.mv );

                    x264_macroblock_cache_ref( h, 0, 0, 4, 4, 1, a->l1.bi16x16.i_ref );
                    x264_macroblock_cache_mv_ptr( h, 0, 0, 4, 4, 1, a->l1.bi16x16.mv );
                    break;
                }
                break;
            case D_16x8:
                x264_mb_cache_mv_b16x8( h, a, 0, 1 );
                x264_mb_cache_mv_b16x8( h, a, 1, 1 );
                break;
            case D_8x16:
                x264_mb_cache_mv_b8x16( h, a, 0, 1 );
                x264_mb_cache_mv_b8x16( h, a, 1, 1 );
                break;
            default:
                x264_log( h, X264_LOG_ERROR, "internal error (invalid MB type)\n" );
                break;
            }
    }

#ifndef NDEBUG
    if( h->i_thread_frames > 1 && !IS_INTRA(h->mb.i_type) )
    {
        for( int l = 0; l <= (h->sh.i_type == SLICE_TYPE_B); l++ )
        {
            int completed;
            int ref = h->mb.cache.ref[l][x264_scan8[0]];
            if( ref < 0 )
                continue;
            completed = (l ? h->fref1 : h->fref0)[ ref >> h->mb.b_interlaced ]->orig->i_lines_completed;
            if( (h->mb.cache.mv[l][x264_scan8[15]][1] >> (2 - h->mb.b_interlaced)) + h->mb.i_mb_y*16 > completed )
            {
                x264_log( h, X264_LOG_WARNING, "internal error (MV out of thread range)\n");
                x264_log( h, X264_LOG_DEBUG, "mb type: %d \n", h->mb.i_type);
                x264_log( h, X264_LOG_DEBUG, "mv: l%dr%d (%d,%d) \n", l, ref,
                                h->mb.cache.mv[l][x264_scan8[15]][0],
                                h->mb.cache.mv[l][x264_scan8[15]][1] );
                x264_log( h, X264_LOG_DEBUG, "limit: %d \n", h->mb.mv_max_spel[1]);
                x264_log( h, X264_LOG_DEBUG, "mb_xy: %d,%d \n", h->mb.i_mb_x, h->mb.i_mb_y);
                x264_log( h, X264_LOG_DEBUG, "completed: %d \n", completed );
                x264_log( h, X264_LOG_WARNING, "recovering by using intra mode\n");
                x264_mb_analyse_intra( h, a, COST_MAX );
                h->mb.i_type = I_16x16;
                h->mb.i_intra16x16_pred_mode = a->i_predict16x16;
                x264_mb_analyse_intra_chroma( h, a );
            }
        }
    }
#endif
}

#include "slicetype.c"

