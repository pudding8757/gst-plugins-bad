/*
 * Copyright (C) <2010> Collabora ltd
 * Copyright (C) <2010> Nokia Corporation
 * Copyright (C) <2011> Intel Corporation
 *
 * Copyright (C) <2010> Mark Nauwelaerts <mark.nauwelaerts@collabora.co.uk>
 * Copyright (C) <2011> Thibault Saunier <thibault.saunier@collabora.com>
 * Copyright (C) <2018> Seungha Yang <seungha.yang@navercorp.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifndef __GST_H26X_BASE_PARSE_H__
#define __GST_H26X_BASE_PARSE_H__

#include <gst/gst.h>
#include <gst/base/gstbaseparse.h>
#include <gst/video/video.h>

G_BEGIN_DECLS

#define GST_TYPE_H26X_BASE_PARSE \
  (gst_h26x_base_parse_get_type())
#define GST_H26X_BASE_PARSE(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_H26X_BASE_PARSE,GstH26XBaseParse))
#define GST_H26X_BASE_PARSE_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_H26X_BASE_PARSE,GstH26XBaseParseClass))
#define GST_H26X_BASE_PARSE_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS((obj),GST_TYPE_H26X_BASE_PARSE,GstH26XBaseParseClass))
#define GST_IS_H26X_BASE_PARSE(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_H26X_BASE_PARSE))
#define GST_IS_H26X_BASE_PARSE_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_H26X_BASE_PARSE))

GType gst_h26x_base_parse_get_type (void);

typedef struct _GstH26XBaseParseSPSInfo GstH26XBaseParseSPSInfo;
typedef struct _GstH26XBaseParseProfileTierLevel GstH26XBaseParseProfileTierLevel;
typedef struct _GstH26XBaseParse GstH26XBaseParse;
typedef struct _GstH26XBaseParseClass GstH26XBaseParseClass;
typedef struct _GstH26XBaseParsePrivate GstH26XBaseParsePrivate;

typedef enum
{
  GST_H26X_BASE_PARSE_HANDLE_FRAME_OK,
  GST_H26X_BASE_PARSE_HANDLE_FRAME_MORE,
  GST_H26X_BASE_PARSE_HANDLE_FRAME_DROP,
  GST_H26X_BASE_PARSE_HANDLE_FRAME_SKIP,
  GST_H26X_BASE_PARSE_HANDLE_FRAME_INVALID_STREAM
} GstH26XBaseParseHandleFrameReturn;

typedef enum
{
  GST_H26X_BASE_PARSE_ALIGN_NONE,
  GST_H26X_BASE_PARSE_ALIGN_NAL,
  GST_H26X_BASE_PARSE_ALIGN_AU
} GstH26XBaseParseAlign;

typedef enum
{
  GST_H26X_BASE_PARSE_STORE_NAL_TYPE_NONE,
  GST_H26X_BASE_PARSE_STORE_NAL_TYPE_VPS,
  GST_H26X_BASE_PARSE_STORE_NAL_TYPE_SPS,
  GST_H26X_BASE_PARSE_STORE_NAL_TYPE_PPS
} GstH26XBaseParseStoreNalType;

#define GST_H26X_BASE_PARSE_FORMAT_NONE 0
#define GST_H26X_BASE_PARSE_FORMAT_BYTE 1

typedef enum
{
  GST_H26X_BASE_PARSE_STATE_INIT = 0,
  GST_H26X_BASE_PARSE_STATE_GOT_SPS = 1 << 0,
  GST_H26X_BASE_PARSE_STATE_GOT_PPS = 1 << 1,
  GST_H26X_BASE_PARSE_STATE_GOT_SLICE = 1 << 2,

  GST_H26X_BASE_PARSE_STATE_VALID_PICTURE_HEADERS =
      (GST_H26X_BASE_PARSE_STATE_GOT_SPS | GST_H26X_BASE_PARSE_STATE_GOT_PPS),
  GST_H26X_BASE_PARSE_STATE_VALID_PICTURE =
      (GST_H26X_BASE_PARSE_STATE_VALID_PICTURE_HEADERS |
      GST_H26X_BASE_PARSE_STATE_GOT_SLICE)
} GstH26XBaseParseState;

struct _GstH26XBaseParseSPSInfo
{
  guint width;
  guint height;
  gint fps_num;
  gint fps_den;
  gint par_num;
  gint par_den;
  GstVideoInterlaceMode interlace_mode;
  const gchar * chroma_format;
  guint bit_depth_luma;
  guint bit_depth_chroma;

  const gchar * profile;
  const gchar * tier;
  const gchar * level;
};

struct _GstH26XBaseParse
{
  GstBaseParse baseparse;

  /*< protected >*/
  /* stream */
  guint nal_length_size;

  /* state */
  GstH26XBaseParseAlign align;
  guint format;

  /* collected VPS, SPS and PPS NALUs */
  GstBuffer **vps_nals;
  GstBuffer **sps_nals;
  GstBuffer **pps_nals;

  /* AU state */
  gboolean picture_start;

  /* Stereo / multiview info */
  GstVideoMultiviewMode multiview_mode;
  GstVideoMultiviewFlags multiview_flags;

  /*< private >*/
  GstH26XBaseParsePrivate *priv;
};

struct _GstH26XBaseParseClass
{
  GstBaseParseClass parent_class;

  gboolean      (*allow_passthrough)            (GstH26XBaseParse * parse);

  void          (*get_max_vps_sps_pps_count)    (GstH26XBaseParse * parse,
                                                 guint * max_vps_count,
                                                 guint * max_sps_count,
                                                 guint * max_pps_count);

  guint         (*get_min_nalu_size)            (GstH26XBaseParse * parse);

  const gchar * (*format_to_string)             (GstH26XBaseParse * parse,
                                                 guint format);

  guint         (*format_from_string)           (GstH26XBaseParse * parse,
                                                 const gchar * format);

  GstCaps *     (*new_empty_caps)               (GstH26XBaseParse * parse);

  gboolean      (*fixate_format)                (GstH26XBaseParse * parse,
                                                 guint * format,
                                                 guint * align,
                                                 const GValue *codec_data_value);

  gboolean      (*handle_codec_data)            (GstH26XBaseParse * parse,
                                                 GstMapInfo * map);

  void          (*get_timestamp)                (GstH26XBaseParse * parse,
                                                 GstClockTime * ts,
                                                 GstClockTime * dur,
                                                 gboolean frame);

  gboolean      (*fill_sps_info)                (GstH26XBaseParse * parse,
                                                 GstH26XBaseParseSPSInfo * info);

  GstCaps *     (*get_compatible_profile_caps)  (GstH26XBaseParse * parse);

  GstMemory *   (*make_aud_memory)              (GstH26XBaseParse * parse,
                                                 gboolean prepend_startcode,
                                                 guint *size);

  GstBuffer *   (*make_codec_data)              (GstH26XBaseParse * parse);

  GstFlowReturn (*handle_frame_packetized)      (GstH26XBaseParse * parse,
                                                 GstBaseParseFrame * frame,
                                                 gboolean split);

  GstH26XBaseParseHandleFrameReturn (*handle_frame_check_initial_skip) (GstH26XBaseParse * parse,
                                                                        gint * skipsize,
                                                                        gint * dropsize,
                                                                        GstMapInfo * map);

  GstH26XBaseParseHandleFrameReturn (*handle_frame_bytestream)         (GstH26XBaseParse * parse,
                                                                        gint * skipsize,
                                                                        gint * framesize,
                                                                        gint * current_off,
                                                                        gboolean * aud_complete,
                                                                        GstMapInfo * map,
                                                                        gboolean drain);
};

void          gst_h26x_base_parse_clear_state        (GstH26XBaseParse * parse,
                                                      GstH26XBaseParseState at_most);

gboolean      gst_h26x_base_parse_is_valid_state     (GstH26XBaseParse * parse,
                                                      GstH26XBaseParseState expected);

GstBuffer *   gst_h26x_base_parse_wrap_nal           (GstH26XBaseParse * parse,
                                                      guint format,
                                                      guint8 * data,
                                                      guint size);

void          gst_h26x_base_parse_vps_parsed         (GstH26XBaseParse * parse);

void          gst_h26x_base_parse_sps_parsed         (GstH26XBaseParse * parse);

void          gst_h26x_base_parse_pps_parsed         (GstH26XBaseParse * parse);

void          gst_h26x_base_parse_sei_parsed         (GstH26XBaseParse * parse,
                                                      guint nalu_offset);

void          gst_h26x_base_parse_aud_parsed         (GstH26XBaseParse * parse);

void          gst_h26x_base_parse_frame_started      (GstH26XBaseParse * parse);

void          gst_h26x_base_parse_slice_hdr_parsed   (GstH26XBaseParse * parse,
                                                      gboolean keyframe);

void          gst_h26x_base_parse_update_idr_pos     (GstH26XBaseParse * parse,
                                                      guint nalu_offset,
                                                      gboolean is_idr);

void          gst_h26x_base_pares_finish_process_nal (GstH26XBaseParse * parse,
                                                      guint8 * data,
                                                      guint size);

void          gst_h26x_base_parse_store_header_nal   (GstH26XBaseParse * parse,
                                                      guint id,
                                                      GstH26XBaseParseStoreNalType naltype,
                                                      const guint8 * data,
                                                      guint size);

void          gst_h26x_base_parse_update_src_caps    (GstH26XBaseParse * parse,
                                                      GstCaps * caps);

GstFlowReturn gst_h26x_base_parse_parse_frame        (GstH26XBaseParse * parse,
                                                      GstBaseParseFrame * frame);

G_END_DECLS
#endif
