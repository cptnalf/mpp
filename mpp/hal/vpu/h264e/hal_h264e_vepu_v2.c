/*
 * Copyright 2015 - 2017 Rockchip Electronics Co. LTD
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#define MODULE_TAG "hal_h264e_vepu_v2"

#include <string.h>

#include "mpp_mem.h"
#include "mpp_common.h"
#include "mpp_buffer.h"

#include "h264e_slice.h"
#include "hal_h264e_debug.h"
#include "hal_h264e_com.h"
#include "hal_h264e_vepu_v2.h"
#include "hal_h264e_vpu_tbl_v2.h"

typedef struct HalH264eVepuMbRcImpl_t {
    RK_S32          width;
    RK_S32          height;
    RK_S32          mb_w;
    RK_S32          mb_h;

    RK_S32          pels;
    RK_S32          mbs;

    RK_S32          bits_per_pic;

    RK_S32          mb_bit_rc_enable;

    /* frame rate control */
    RK_S32          fps_in_num;
    RK_S32          fps_in_denorm;
    RK_S32          fps_out_num;
    RK_S32          fps_out_denorm;

    RK_S32          fps_count;
    RK_S32          fps_step;
    RK_S32          fps_threshold;

    /*
     * MAD based QP adjustment
     * mad_qp_change    [-8..7]
     * mad_threshold    MAD threshold div256
     */
    RK_S32          mad_qp_change;
    RK_S32          mad_threshold;

    /*
     * check point parameter
     */
    RK_S32          check_point_count;
    RK_S32          check_point_distance;

    /* estimated first I frame qp */
    RK_S32          qp_init_est;

    RK_S32          frame_type;
    RK_S32          pre_frame_type;
} HalH264eVepuMbRcImpl;

static void vepu_swap_endian(RK_U32 *buf, RK_S32 size_bytes)
{
    RK_U32 i = 0;
    RK_S32 words = size_bytes / 4;
    RK_U32 val, val2, tmp, tmp2;

    mpp_assert((size_bytes % 8) == 0);

    while (words > 0) {
        val = buf[i];
        tmp = 0;

        tmp |= (val & 0xFF) << 24;
        tmp |= (val & 0xFF00) << 8;
        tmp |= (val & 0xFF0000) >> 8;
        tmp |= (val & 0xFF000000) >> 24;
        {
            val2 = buf[i + 1];
            tmp2 = 0;

            tmp2 |= (val2 & 0xFF) << 24;
            tmp2 |= (val2 & 0xFF00) << 8;
            tmp2 |= (val2 & 0xFF0000) >> 8;
            tmp2 |= (val2 & 0xFF000000) >> 24;

            buf[i] = tmp2;
            words--;
            i++;
        }
        buf[i] = tmp;
        words--;
        i++;
    }
}

static void vepu_write_cabac_table(MppBuffer buf, RK_S32 cabac_init_idc)
{
    const RK_S32(*context)[460][2];
    RK_S32 i, j, qp;
    RK_U8 table[H264E_CABAC_TABLE_BUF_SIZE] = {0};

    for (qp = 0; qp < 52; qp++) { /* All QP values */
        for (j = 0; j < 2; j++) { /* Intra/Inter */
            if (j == 0)
                /*lint -e(545) */
                context = &h264_context_init_intra;
            else
                /*lint -e(545) */
                context = &h264_context_init[cabac_init_idc];

            for (i = 0; i < 460; i++) {
                RK_S32 m = (RK_S32)(*context)[i][0];
                RK_S32 n = (RK_S32)(*context)[i][1];

                RK_S32 pre_ctx_state = H264E_HAL_CLIP3(((m * (RK_S32)qp) >> 4)
                                                       + n, 1, 126);

                if (pre_ctx_state <= 63)
                    table[qp * 464 * 2 + j * 464 + i] =
                        (RK_U8)((63 - pre_ctx_state) << 1);
                else
                    table[qp * 464 * 2 + j * 464 + i] =
                        (RK_U8)(((pre_ctx_state - 64) << 1) | 1);
            }
        }
    }

    vepu_swap_endian((RK_U32 *)table, H264E_CABAC_TABLE_BUF_SIZE);
    mpp_buffer_write(buf, 0, table, H264E_CABAC_TABLE_BUF_SIZE);
}

MPP_RET h264e_vepu_buf_init(HalH264eVepuBufs *bufs)
{
    MPP_RET ret = MPP_OK;

    hal_h264e_dbg_buffer("enter %p\n", bufs);

    memset(bufs, 0, sizeof(*bufs));

    // do not create buffer on cavlc case
    bufs->cabac_init_idc = -1;
    ret = mpp_buffer_group_get_internal(&bufs->group, MPP_BUFFER_TYPE_ION);
    if (ret)
        mpp_err_f("get buffer group failed ret %d\n", ret);

    hal_h264e_dbg_buffer("leave %p\n", bufs);

    return ret;
}

MPP_RET h264e_vepu_buf_deinit(HalH264eVepuBufs *bufs)
{
    RK_S32 i;

    hal_h264e_dbg_buffer("enter %p\n", bufs);

    if (bufs->cabac_table)
        mpp_buffer_put(bufs->cabac_table);

    if (bufs->nal_size_table)
        mpp_buffer_put(bufs->nal_size_table);

    for (i = 0; i < (RK_S32)MPP_ARRAY_ELEMS(bufs->frm_buf); i++) {
        if (bufs->frm_buf[i])
            mpp_buffer_put(bufs->frm_buf[i]);
    }

    if (bufs->group)
        mpp_buffer_group_put(bufs->group);

    memset(bufs, 0, sizeof(*bufs));
    bufs->cabac_init_idc = -1;

    hal_h264e_dbg_buffer("leave %p\n", bufs);

    return MPP_OK;
}

MPP_RET h264e_vepu_buf_set_cabac_idc(HalH264eVepuBufs *bufs, RK_S32 idc)
{
    hal_h264e_dbg_buffer("enter %p\n", bufs);

    if (idc >= 0 && !bufs->cabac_table)
        mpp_buffer_get(bufs->group, &bufs->cabac_table, H264E_CABAC_TABLE_BUF_SIZE);

    if (bufs->cabac_table && idc != bufs->cabac_init_idc && idc >= 0)
        vepu_write_cabac_table(bufs->cabac_table, idc);

    bufs->cabac_init_idc = idc;

    hal_h264e_dbg_buffer("leave %p\n", bufs);

    return MPP_OK;
}

MPP_RET h264e_vepu_buf_set_frame_size(HalH264eVepuBufs *bufs, RK_S32 w, RK_S32 h)
{
    RK_S32 aligned_w = MPP_ALIGN(w, 16);
    RK_S32 aligned_h = MPP_ALIGN(h, 16);
    size_t yuv_size = aligned_w * aligned_h;
    size_t frm_size = yuv_size * 3 / 2;
    RK_S32 i;
    RK_S32 cnt = (RK_S32)MPP_ARRAY_ELEMS(bufs->frm_buf);

    hal_h264e_dbg_buffer("enter %p\n", bufs);

    mpp_assert(frm_size);

    if (frm_size != bufs->frm_size) {
        if (bufs->frm_size) {
            /* reallocate only on larger frame size to save time */
            mpp_log("new frame size [%d:%d] require buffer %d not equal to %d\n",
                    w, h, frm_size, bufs->frm_size);
        }

        for (i = 0; i < cnt; i++) {
            if (bufs->frm_buf[i]) {
                mpp_buffer_put(bufs->frm_buf[i]);
                bufs->frm_buf[i] = NULL;
                bufs->frm_cnt--;
            }
        }
    }

    bufs->mb_h = aligned_h >> 4;
    if (bufs->mb_h)
        bufs->nal_tab_size = MPP_ALIGN((bufs->mb_h + 1) * sizeof(RK_U32), 8);
    else
        bufs->nal_tab_size = 0;

    bufs->yuv_size = yuv_size;
    bufs->frm_size = frm_size;

    hal_h264e_dbg_buffer("leave %p\n", bufs);

    return MPP_OK;
}

MppBuffer h264e_vepu_buf_get_nal_size_table(HalH264eVepuBufs *bufs)
{
    MppBuffer buf = bufs->nal_size_table;

    hal_h264e_dbg_buffer("enter %p\n", bufs);

    if (NULL == buf) {
        mpp_buffer_get(bufs->group, &buf, bufs->nal_tab_size);
        mpp_assert(buf);
        bufs->nal_size_table = buf;
    }

    hal_h264e_dbg_buffer("leave %p\n", bufs);

    return buf;
}

MppBuffer h264e_vepu_buf_get_frame_buffer(HalH264eVepuBufs *bufs, RK_S32 index)
{
    MppBuffer buf = bufs->frm_buf[index];

    hal_h264e_dbg_buffer("enter\n", bufs);

    if (NULL == buf) {
        mpp_buffer_get(bufs->group, &buf, bufs->frm_size);
        mpp_assert(buf);
        bufs->frm_buf[index] = buf;
        bufs->frm_cnt++;
    }

    hal_h264e_dbg_buffer("leave %p\n", bufs);

    return buf;
}

static H264eVpuCsp fmt_to_vepu_csp_yuv[MPP_FMT_YUV_BUTT] = {
    H264E_VPU_CSP_YUV420SP,     // MPP_FMT_YUV420SP         /* YYYY... UV... (NV12)     */
    H264E_VPU_CSP_NONE,         // MPP_FMT_YUV420SP_10BIT   ///< Not part of ABI
    H264E_VPU_CSP_NONE,         // MPP_FMT_YUV422SP         /* YYYY... UVUV... (NV16)   */
    H264E_VPU_CSP_NONE,         // MPP_FMT_YUV422SP_10BIT   ///< Not part of ABI
    H264E_VPU_CSP_YUV420P,      // MPP_FMT_YUV420P          /* YYYY... U...V...  (I420) */
    H264E_VPU_CSP_NONE,         // MPP_FMT_YUV420SP_VU      /* YYYY... VUVUVU... (NV21) */
    H264E_VPU_CSP_NONE,         // MPP_FMT_YUV422P          /* YYYY... UU...VV...(422P) */
    H264E_VPU_CSP_NONE,         // MPP_FMT_YUV422SP_VU      /* YYYY... VUVUVU... (NV61) */
    H264E_VPU_CSP_YUYV422,      // MPP_FMT_YUV422_YUYV      /* YUYVYUYV... (YUY2)       */
    H264E_VPU_CSP_UYVY422,      // MPP_FMT_YUV422_UYVY      /* UYVYUYVY... (UYVY)       */
    H264E_VPU_CSP_NONE,         // MPP_FMT_YUV400           /* YYYY...                  */
    H264E_VPU_CSP_NONE,         // MPP_FMT_YUV440SP         /* YYYY... UVUV...          */
    H264E_VPU_CSP_NONE,         // MPP_FMT_YUV411SP         /* YYYY... UV...            */
    H264E_VPU_CSP_NONE,         // MPP_FMT_YUV444SP         /* YYYY... UVUVUVUV...      */
};

static H264eVpuCsp fmt_to_vepu_csp_rgb[MPP_FMT_RGB_BUTT - MPP_FRAME_FMT_RGB] = {
    H264E_VPU_CSP_RGB565,       // MPP_FMT_RGB565           /* 16-bit RGB               */
    H264E_VPU_CSP_RGB565,       // MPP_FMT_BGR565           /* 16-bit RGB               */
    H264E_VPU_CSP_RGB555,       // MPP_FMT_RGB555           /* 15-bit RGB               */
    H264E_VPU_CSP_RGB555,       // MPP_FMT_BGR555           /* 15-bit RGB               */
    H264E_VPU_CSP_RGB444,       // MPP_FMT_RGB444           /* 12-bit RGB               */
    H264E_VPU_CSP_RGB444,       // MPP_FMT_BGR444           /* 12-bit RGB               */
    H264E_VPU_CSP_RGB888,       // MPP_FMT_RGB888           /* 24-bit RGB               */
    H264E_VPU_CSP_RGB888,       // MPP_FMT_BGR888           /* 24-bit RGB               */
    H264E_VPU_CSP_RGB101010,    // MPP_FMT_RGB101010        /* 30-bit RGB               */
    H264E_VPU_CSP_RGB101010,    // MPP_FMT_BGR101010        /* 30-bit RGB               */
    H264E_VPU_CSP_ARGB8888,     // MPP_FMT_ARGB8888         /* 32-bit RGB               */
    H264E_VPU_CSP_ARGB8888,     // MPP_FMT_ABGR8888         /* 32-bit RGB               */
};

static RK_S32 fmt_to_vepu_mask_msb[MPP_FMT_RGB_BUTT - MPP_FRAME_FMT_RGB][3] = {
    //   R   G   B              // mask msb position
    {   15, 10,  4, },          // MPP_FMT_RGB565           /* 16-bit RGB               */
    {    4, 10, 15, },          // MPP_FMT_BGR565           /* 16-bit RGB               */
    {   14,  9,  4, },          // MPP_FMT_RGB555           /* 15-bit RGB               */
    {    4,  9, 14, },          // MPP_FMT_BGR555           /* 15-bit RGB               */
    {   11,  7,  3, },          // MPP_FMT_RGB444           /* 12-bit RGB               */
    {    3,  7, 11, },          // MPP_FMT_BGR444           /* 12-bit RGB               */
    {   23, 15,  7, },          // MPP_FMT_RGB888           /* 24-bit RGB               */
    {    7, 15, 23, },          // MPP_FMT_BGR888           /* 24-bit RGB               */
    {   29, 19,  9, },          // MPP_FMT_RGB101010        /* 30-bit RGB               */
    {    9, 19, 29, },          // MPP_FMT_BGR101010        /* 30-bit RGB               */
    {   23, 15,  7, },          // MPP_FMT_ARGB8888         /* 32-bit RGB               */
    {    7, 15, 23, },          // MPP_FMT_ABGR8888         /* 32-bit RGB               */
};

MPP_RET h264e_vepu_prep_setup(HalH264eVepuPrep *prep, MppEncPrepCfg *cfg)
{
    MPP_RET ret = MPP_OK;
    MppFrameFormat format = cfg->format;

    hal_h264e_dbg_buffer("enter\n");

    prep->src_fmt = format;
    prep->src_w = cfg->width;
    prep->src_h = cfg->height;

    if (format < MPP_FRAME_FMT_RGB) {
        // YUV case
        prep->src_fmt = fmt_to_vepu_csp_yuv[format];
        if (prep->src_fmt == H264E_VPU_CSP_NONE) {
            mpp_err("vepu do not support input frame format %d\n", format);
            ret = MPP_NOK;
        }
        prep->r_mask_msb = 0;
        prep->g_mask_msb = 0;
        prep->b_mask_msb = 0;

        prep->color_conversion_coeff_a = 0;
        prep->color_conversion_coeff_b = 0;
        prep->color_conversion_coeff_c = 0;
        prep->color_conversion_coeff_e = 0;
        prep->color_conversion_coeff_f = 0;
    } else {
        // RGB case
        RK_S32 rgb_idx = format - MPP_FRAME_FMT_RGB;

        mpp_assert(rgb_idx < MPP_FMT_RGB_BUTT - MPP_FRAME_FMT_RGB);

        prep->src_fmt = fmt_to_vepu_csp_rgb[rgb_idx];
        if (prep->src_fmt == H264E_VPU_CSP_NONE) {
            mpp_err("vepu do not support input frame format %d\n", format);
            ret = MPP_NOK;
        }
        prep->r_mask_msb = fmt_to_vepu_mask_msb[rgb_idx][0];
        prep->g_mask_msb = fmt_to_vepu_mask_msb[rgb_idx][1];
        prep->b_mask_msb = fmt_to_vepu_mask_msb[rgb_idx][2];

        switch (cfg->color) {
        case MPP_FRAME_SPC_RGB : {
            /* BT.601 */
            /* Y  = 0.2989 R + 0.5866 G + 0.1145 B
             * Cb = 0.5647 (B - Y) + 128
             * Cr = 0.7132 (R - Y) + 128
             */
            prep->color_conversion_coeff_a = 19589;
            prep->color_conversion_coeff_b = 38443;
            prep->color_conversion_coeff_c = 7504;
            prep->color_conversion_coeff_e = 37008;
            prep->color_conversion_coeff_f = 46740;
        } break;
        case MPP_FRAME_SPC_BT709 : {
            /* BT.709 */
            /* Y  = 0.2126 R + 0.7152 G + 0.0722 B
             * Cb = 0.5389 (B - Y) + 128
             * Cr = 0.6350 (R - Y) + 128
             */
            prep->color_conversion_coeff_a = 13933;
            prep->color_conversion_coeff_b = 46871;
            prep->color_conversion_coeff_c = 4732;
            prep->color_conversion_coeff_e = 35317;
            prep->color_conversion_coeff_f = 41615;
        } break;
        default : {
            prep->color_conversion_coeff_a = 19589;
            prep->color_conversion_coeff_b = 38443;
            prep->color_conversion_coeff_c = 7504;
            prep->color_conversion_coeff_e = 37008;
            prep->color_conversion_coeff_f = 46740;
        } break;
        }
    }

    RK_S32 hor_stride = cfg->hor_stride;
    RK_S32 ver_stride = cfg->ver_stride;
    prep->offset_cb = 0;
    prep->offset_cr = 0;

    switch (format) {
    case MPP_FMT_YUV420SP : {
        prep->offset_cb = hor_stride * ver_stride;
        prep->size_y = hor_stride * MPP_ALIGN(prep->src_h, 16);
        prep->size_c = hor_stride / 2 * MPP_ALIGN(prep->src_h / 2, 8);
    } break;
    case MPP_FMT_YUV420P : {
        prep->offset_cb = hor_stride * ver_stride;
        prep->offset_cr = prep->offset_cb + ((hor_stride * ver_stride) / 4);
        prep->size_y = hor_stride * MPP_ALIGN(prep->src_h, 16);
        prep->size_c = hor_stride / 2 * MPP_ALIGN(prep->src_h / 2, 8);
    } break;
    case MPP_FMT_YUV422_YUYV :
    case MPP_FMT_YUV422_UYVY : {
        prep->size_y = hor_stride * 2 * MPP_ALIGN(prep->src_h, 16);
        prep->size_c = 0;
    } break;
    case MPP_FMT_RGB565 :
    case MPP_FMT_BGR444 : {
        prep->size_y = hor_stride * 2 * MPP_ALIGN(prep->src_h, 16);
        prep->size_c = 0;
    } break;
    case MPP_FMT_BGR888 :
    case MPP_FMT_RGB888 :
    case MPP_FMT_ARGB8888 :
    case MPP_FMT_ABGR8888 :
    case MPP_FMT_BGR101010 : {
        prep->size_y = hor_stride * 4 * MPP_ALIGN(prep->src_h, 16);
        prep->size_c = 0;
    } break;
    default: {
        mpp_err_f("invalid format %d", format);
        ret = MPP_NOK;
    }
    }

    hal_h264e_dbg_buffer("leave\n");

    return ret;
}

MPP_RET h264e_vepu_prep_get_addr(HalH264eVepuPrep *prep, MppBuffer buffer,
                                 RK_U32 (*addr)[3])
{
    RK_U32 fd = (RK_U32)mpp_buffer_get_fd(buffer);
    size_t size = mpp_buffer_get_size(buffer);

    hal_h264e_dbg_buffer("enter\n");

    (*addr)[0] = fd;
    (*addr)[1] = fd + (prep->offset_cb << 10);
    (*addr)[2] = fd + (prep->offset_cr << 10);

    if (size < prep->size_y)
        mpp_err("warnning: input buffer size 0x%x is smaller then required size 0x%x",
                size, prep->size_y);

    if (prep->size_c && (prep->offset_cb || prep->offset_cr)) {
        if (prep->offset_cb && (size < prep->offset_cb + prep->size_c))
            mpp_err("warnning: input buffer size 0x%x is smaller then cb requirement 0x%x + 0x%x",
                    size, prep->offset_cb, prep->size_c);

        if (prep->offset_cr && (size < prep->offset_cr + prep->size_c))
            mpp_err("warnning: input buffer size 0x%x is smaller then cb requirement 0x%x + 0x%x",
                    size, prep->offset_cr, prep->size_c);
    }

    hal_h264e_dbg_buffer("leave\n");

    return MPP_OK;
}

MPP_RET h264e_vepu_mbrc_init(HalH264eVepuMbRcCtx *ctx, HalH264eVepuMbRc *mbrc)
{
    MPP_RET ret = MPP_OK;
    HalH264eVepuMbRcImpl *p = mpp_calloc(HalH264eVepuMbRcImpl, 1);
    if (!p) {
        mpp_err_f("failed to alloc rate control context\n");
        ret = MPP_ERR_NOMEM;
    }

    memset(mbrc, 0, sizeof(*mbrc));
    mbrc->qp_init   = -1;
    mbrc->qp_max    = 48;
    mbrc->qp_min    = 16;

    *ctx = p;
    return ret;
}

MPP_RET h264e_vepu_mbrc_deinit(HalH264eVepuMbRcCtx ctx)
{
    MPP_FREE(ctx);
    return MPP_OK;
}

MPP_RET h264e_vepu_mbrc_setup(HalH264eVepuMbRcCtx ctx, MppEncCfgSet*cfg)
{
    HalH264eVepuMbRcImpl *p = (HalH264eVepuMbRcImpl *)ctx;
    MppEncPrepCfg *prep = &cfg->prep;
    MppEncRcCfg *rc = &cfg->rc;

    hal_h264e_dbg_func("enter\n");

    // get necessary parameter from config
    p->width    = prep->width;
    p->height   = prep->height;
    p->mb_w     = MPP_ALIGN(prep->width, 16) / 16;
    p->mb_h     = MPP_ALIGN(prep->height, 16) / 16;
    p->pels     = p->width * p->height;
    p->mbs      = p->mb_w * p->mb_h;
    p->bits_per_pic = axb_div_c(rc->bps_target, rc->fps_out_denorm,
                                rc->fps_out_num);

    mpp_assert(p->pels);

    // frame rate control
    mpp_assert(rc->fps_out_num / rc->fps_out_denorm <= rc->fps_in_num / rc->fps_in_denorm);

    p->fps_in_num       = rc->fps_in_num;
    p->fps_in_denorm    = rc->fps_in_denorm;
    p->fps_out_num      = rc->fps_out_num;
    p->fps_out_denorm   = rc->fps_out_denorm;

    p->fps_step         = rc->fps_in_denorm * rc->fps_out_num;
    p->fps_threshold    = rc->fps_in_num * rc->fps_out_denorm;
    p->fps_count        = p->fps_threshold;

    // if not constant
    p->mb_bit_rc_enable = rc->rc_mode != MPP_ENC_RC_MODE_FIXQP;

    hal_h264e_dbg_rc("estimated init qp %d\n", p->qp_init_est);

    // init first frame mad parameter
    p->mad_qp_change = 2;
    p->mad_threshold = 256 * 6;

    // init check point position
    if (p->mb_bit_rc_enable) {
        p->check_point_count = MPP_MIN(p->mb_h - 1, CHECK_POINTS_MAX);
        p->check_point_distance = p->mbs / (p->check_point_count + 1);
    } else {
        p->check_point_count = 0;
        p->check_point_distance = 0;
    }

    p->frame_type = INTRA_FRAME;
    p->pre_frame_type = INTRA_FRAME;

    hal_h264e_dbg_func("leave\n");
    return MPP_OK;
}

#define WORD_CNT_MAX    65535

MPP_RET h264e_vepu_mbrc_prepare(HalH264eVepuMbRcCtx ctx, HalH264eVepuMbRc *mbrc,
                                EncRcTask *rc_task)
{
    HalH264eVepuMbRcImpl *p = (HalH264eVepuMbRcImpl *)ctx;
    EncFrmStatus *frm = &rc_task->frm;
    EncRcTaskInfo *info = &rc_task->info;

    RK_S32 i;
    const RK_S32 sscale = 256;
    RK_S32 scaler, srcPrm;
    RK_S32 tmp, nonZeroTarget;
    RK_S32 coeffCntMax = p->mbs * 24 * 16;

    mbrc->qp_init = info->quality_target;
    mbrc->qp_min = info->quality_min;
    mbrc->qp_max = info->quality_max;

    if (!p->mb_bit_rc_enable)
        return MPP_OK;

    p->pre_frame_type = p->frame_type;
    p->frame_type = (frm->is_intra) ? INTRA_FRAME : INTER_P_FRAME;

    if (mbrc->rlc_count == 0) {
        mbrc->rlc_count = 1;
    }

    srcPrm = axb_div_c(mbrc->out_strm_size * 8, 256, mbrc->rlc_count);
    /* Disable Mb Rc for Intra Slices, because coeffTarget will be wrong */
    if (frm->is_intra || srcPrm == 0)
        return 0;

    /* Required zero cnt */
    nonZeroTarget = axb_div_c(info->bit_target, 256, srcPrm);
    nonZeroTarget = MPP_MIN(coeffCntMax, MPP_MAX(0, nonZeroTarget));
    nonZeroTarget = MPP_MIN(0x7FFFFFFFU / 1024U, (RK_U32)nonZeroTarget);

    if (nonZeroTarget > 0) {
        scaler = axb_div_c(nonZeroTarget, sscale, (RK_S32) p->mbs);
    } else {
        return 0;
    }

    if ((p->frame_type != p->pre_frame_type) || (mbrc->rlc_count == 0)) {
        for (i = 0; i < VEPU_CHECK_POINTS_MAX; i++) {
            tmp = (scaler * (p->check_point_distance * (i + 1) + 1)) / sscale;
            tmp = MPP_MIN(WORD_CNT_MAX, tmp / 32 + 1);
            if (tmp < 0) tmp = WORD_CNT_MAX;    /* Detect overflow */
            mbrc->cp_target[i] = tmp; /* div32 for regs */
        }

        tmp = axb_div_c(p->bits_per_pic, 256, srcPrm);
    } else {
        for (i = 0; i < VEPU_CHECK_POINTS_MAX; i++) {
            tmp = (RK_S32) (mbrc->cp_usage[i] * scaler) / sscale;
            tmp = MPP_MIN(WORD_CNT_MAX, tmp / 32 + 1);
            if (tmp < 0) tmp = WORD_CNT_MAX;    /* Detect overflow */
            mbrc->cp_target[i] = tmp; /* div32 for regs */
        }
        tmp = axb_div_c(p->bits_per_pic, 256, srcPrm);
    }

    mbrc->cp_error[0] = -tmp * 3;
    mbrc->cp_delta_qp[0] = 3;
    mbrc->cp_error[1] = -tmp * 2;
    mbrc->cp_delta_qp[1] = 2;
    mbrc->cp_error[2] = -tmp * 1;
    mbrc->cp_delta_qp[2] = 1;
    mbrc->cp_error[3] = tmp * 1;
    mbrc->cp_delta_qp[3] = 0;
    mbrc->cp_error[4] = tmp * 2;
    mbrc->cp_delta_qp[4] = -1;
    mbrc->cp_error[5] = tmp * 3;
    mbrc->cp_delta_qp[5] = -2;
    mbrc->cp_error[6] = tmp * 4;
    mbrc->cp_delta_qp[6] = -3;

    for (i = 0; i < CTRL_LEVELS; i++) {
        tmp = mbrc->cp_error[i];
        tmp = mpp_clip(tmp / 4, -32768, 32767);
        mbrc->cp_error[i] = tmp;
    }

    mbrc->mad_qp_change = 0;
    mbrc->mad_threshold = 0;
    mbrc->cp_distance_mbs = p->check_point_distance;

    return MPP_OK;
}

MPP_RET h264e_vepu_slice_split_cfg(H264eSlice *slice, HalH264eVepuMbRc *mbrc,
                                   EncRcTask *rc_task, MppEncCfgSet *cfg)
{
    MppEncSliceSplit *split = &cfg->split;
    EncRcTaskInfo *info = &rc_task->info;
    RK_U32 slice_mb_rows = 0;

    hal_h264e_dbg_func("enter\n");

    switch (split->split_mode) {
    case MPP_ENC_SPLIT_NONE : {
        mbrc->slice_size_mb_rows = 0;
    } break;
    case MPP_ENC_SPLIT_BY_BYTE : {
        RK_U32 mb_per_col = (cfg->prep.height + 15) / 16;
        mpp_assert(split->split_arg > 0);
        RK_U32 slice_num = info->bit_target / (split->split_arg * 8);

        if (slice_num <= 0)
            slice_num = 4;

        slice_mb_rows = (mb_per_col + slice_num - 1) / slice_num;
        mbrc->slice_size_mb_rows = mpp_clip(slice_mb_rows, 2, 127);
    } break;
    case MPP_ENC_SPLIT_BY_CTU : {
        mpp_assert(split->split_arg > 0);
        RK_U32 mb_per_line = (cfg->prep.width + 15) / 16;

        slice_mb_rows = (split->split_arg + mb_per_line - 1) / mb_per_line;
        mbrc->slice_size_mb_rows = mpp_clip(slice_mb_rows, 2, 127);
    } break;
    default : {
        mpp_log_f("invalide slice split mode %d\n", split->split_mode);
    } break;
    }

    slice->is_multi_slice = (mbrc->slice_size_mb_rows > 0);
    split->change = 0;

    hal_h264e_dbg_func("leave\n");
    return MPP_OK;
}

MPP_RET h264e_vepu_mbrc_update(HalH264eVepuMbRcCtx ctx, HalH264eVepuMbRc *mbrc)
{
    HalH264eVepuMbRcImpl *p = (HalH264eVepuMbRcImpl *)ctx;
    (void) p;
    (void) mbrc;

    hal_h264e_dbg_func("enter\n");
    hal_h264e_dbg_func("leave\n");
    return MPP_OK;
}

#define START_CODE 0x000001 ///< start_code_prefix_one_3bytes

static RK_S32 get_next_nal(RK_U8 *buf, RK_S32 *length)
{
    RK_S32 i, consumed = 0;
    RK_S32 len = *length;
    RK_U8 *tmp_buf = buf;

    /* search start code */
    while (len >= 4) {
        if (tmp_buf[2] == 0) {
            len--;
            tmp_buf++;
            continue;
        }

        if (tmp_buf[0] != 0 || tmp_buf[1] != 0 || tmp_buf[2] != 1) {
            RK_U32 state = (RK_U32) - 1;
            RK_S32 has_nal = 0;

            for (i = 0; i < (RK_S32)len; i++) {
                state = (state << 8) | tmp_buf[i];
                if (((state >> 8) & 0xFFFFFF) == START_CODE) {
                    has_nal = 1;
                    i = i - 3;
                    break;
                }
            }

            if (has_nal) {
                len -= i;
                tmp_buf += i;
                consumed = *length - len - 1;
                break;
            }

            consumed = *length;
            break;
        }
        tmp_buf   += 3;
        len       -= 3;
    }

    *length = *length - consumed;
    return consumed;
}

MPP_RET h264e_vepu_stream_amend_init(HalH264eVepuStreamAmend *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->buf_size = SZ_128K;
    return MPP_OK;
}

MPP_RET h264e_vepu_stream_amend_deinit(HalH264eVepuStreamAmend *ctx)
{
    MPP_FREE(ctx->src_buf);
    MPP_FREE(ctx->dst_buf);
    return MPP_OK;
}

MPP_RET h264e_vepu_stream_amend_config(HalH264eVepuStreamAmend *ctx,
                                       MppPacket packet, MppEncCfgSet *cfg,
                                       H264eSlice *slice, H264ePrefixNal *prefix)
{
    MppEncRefCfgImpl *ref = (MppEncRefCfgImpl *)cfg->ref_cfg;

    if (ref->lt_cfg_cnt || ref->st_cfg_cnt > 1) {
        ctx->enable = 1;
        ctx->slice_enabled = 0;
        ctx->slice = slice;
        ctx->prefix = prefix;

        ctx->packet = packet;
        ctx->buf_base = mpp_packet_get_length(packet);
        ctx->old_length = 0;
        ctx->new_length = 0;

        if (NULL == ctx->dst_buf)
            ctx->dst_buf = mpp_calloc(RK_U8, ctx->buf_size);
        if (NULL == ctx->src_buf)
            ctx->src_buf = mpp_calloc(RK_U8, ctx->buf_size);
    } else {
        MPP_FREE(ctx->dst_buf);
        MPP_FREE(ctx->src_buf);
        memset(ctx, 0, sizeof(*ctx));
    }

    return MPP_OK;
}

MPP_RET h264e_vepu_stream_amend_proc(HalH264eVepuStreamAmend *ctx)
{
    H264eSlice *slice = ctx->slice;
    MppPacket pkt = ctx->packet;
    RK_U8 *p = mpp_packet_get_pos(pkt);
    RK_S32 size = mpp_packet_get_size(pkt);
    RK_S32 base = ctx->buf_base;
    RK_S32 len = ctx->old_length;
    RK_S32 hw_len_bit = 0;
    RK_S32 sw_len_bit = 0;
    RK_S32 hw_len_byte = 0;
    RK_S32 sw_len_byte = 0;
    RK_S32 diff_size = 0;
    RK_S32 tail_0bit = 0;
    RK_U8  tail_byte = 0;
    RK_U8  tail_tmp = 0;
    RK_U8 *dst_buf = NULL;
    RK_S32 buf_size;
    RK_S32 final_len = 0;
    RK_S32 first_slice = 1;
    RK_S32 last_slice = 0;

    {
        RK_S32 more_buf = 0;
        while (len > ctx->buf_size - 16) {
            ctx->buf_size *= 2;
            more_buf = 1;
        }

        if (more_buf) {
            MPP_FREE(ctx->src_buf);
            MPP_FREE(ctx->dst_buf);
            ctx->src_buf = mpp_malloc(RK_U8, ctx->buf_size);
            ctx->dst_buf = mpp_malloc(RK_U8, ctx->buf_size);
        }
    }

    memset(ctx->dst_buf, 0, ctx->buf_size);
    memset(ctx->src_buf, 0, ctx->buf_size);
    dst_buf = ctx->dst_buf;
    buf_size = ctx->buf_size;
    p += base;

    do {
        RK_U32 nal_len = 0;
        tail_0bit = 0;
        // copy hw stream to stream buffer first
        if (slice->is_multi_slice) {
            nal_len = get_next_nal(p, &len);

            memcpy(ctx->src_buf, p, nal_len);
            p += nal_len;

            hal_h264e_dbg_amend("nal_len %d last byte %1x", nal_len, ctx->src_buf[nal_len - 1]);
            last_slice = (len == 0);
        } else {
            memcpy(ctx->src_buf, p, len);
            nal_len = len;
            last_slice = 1;
        }

        mpp_log_f("multi %d [%d %d] nal_len %d\n", slice->is_multi_slice,
                  first_slice, last_slice, nal_len);

        if (!first_slice && ctx->prefix) {
            /* add prefix for each slice */
            H264ePrefixNal *prefix = ctx->prefix;

            RK_S32 prefix_bit = h264e_slice_write_prefix_nal_unit_svc(prefix, dst_buf, buf_size);

            prefix_bit /= 8;

            dst_buf += prefix_bit;
            buf_size -= prefix_bit;
            final_len += prefix_bit;
        }

        H264eSlice slice_rd;

        memcpy(&slice_rd, slice, sizeof(slice_rd));
        slice_rd.log2_max_frame_num = 16;
        slice_rd.pic_order_cnt_type = 2;

        hw_len_bit = h264e_slice_read(&slice_rd, ctx->src_buf, size);

        // write new header to header buffer
        slice->qp_delta = slice_rd.qp_delta;
        slice->first_mb_in_slice = slice_rd.first_mb_in_slice;
        sw_len_bit = h264e_slice_write(slice, dst_buf, buf_size);

        hw_len_byte = (hw_len_bit + 7) / 8;
        sw_len_byte = (sw_len_bit + 7) / 8;

        tail_byte = ctx->src_buf[nal_len - 1];
        tail_tmp = tail_byte;

        while (!(tail_tmp & 1) && tail_0bit < 8) {
            tail_tmp >>= 1;
            tail_0bit++;
        }

        mpp_assert(tail_0bit < 8);

        // move the reset slice data from src buffer to dst buffer
        diff_size = h264e_slice_move(dst_buf, ctx->src_buf,
                                     sw_len_bit, hw_len_bit, nal_len);

        hal_h264e_dbg_amend("tail 0x%02x %d hw_hdr %d sw_hdr %d len %d hw_byte %d sw_byte %d diff %d\n",
                            tail_byte, tail_0bit, hw_len_bit, sw_len_bit, nal_len, hw_len_byte, sw_len_byte, diff_size);

        if (slice->entropy_coding_mode) {
            memcpy(dst_buf + sw_len_byte, ctx->src_buf + hw_len_byte,
                   nal_len - hw_len_byte);
            final_len += nal_len - hw_len_byte + sw_len_byte;
            nal_len = nal_len - hw_len_byte + sw_len_byte;
        } else {
            RK_S32 hdr_diff_bit = sw_len_bit - hw_len_bit;
            RK_S32 bit_len = nal_len * 8 - tail_0bit + hdr_diff_bit;
            RK_S32 new_len = (bit_len + diff_size * 8 + 7) / 8;

            hal_h264e_dbg_amend("frm %4d %c len %d bit hw %d sw %d byte hw %d sw %d diff %d -> %d\n",
                                slice->frame_num, (slice->idr_flag ? 'I' : 'P'),
                                nal_len, hw_len_bit, sw_len_bit,
                                hw_len_byte, sw_len_byte, diff_size, new_len);

            hal_h264e_dbg_amend("%02x %02x %02x %02x -> %02x %02x %02x %02x\n",
                                ctx->src_buf[nal_len - 4], ctx->src_buf[nal_len - 3],
                                ctx->src_buf[nal_len - 2], ctx->src_buf[nal_len - 1],
                                dst_buf[new_len - 4], dst_buf[new_len - 3],
                                dst_buf[new_len - 2], dst_buf[new_len - 1]);
            nal_len = new_len;
            final_len += new_len;
        }

        if (last_slice) {
            p = mpp_packet_get_pos(pkt);
            p += base;
            memcpy(p, ctx->dst_buf, final_len);

            if (slice->entropy_coding_mode) {
                if (final_len < ctx->old_length)
                    memset(p + final_len, 0,  ctx->old_length - final_len);
            } else
                p[final_len] = 0;

            break;
        }

        dst_buf += nal_len;
        buf_size -= nal_len;
        first_slice = 0;
    } while (1);

    ctx->new_length = final_len;

    return MPP_OK;
}
