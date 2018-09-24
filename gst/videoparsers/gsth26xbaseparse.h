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

#define GST_H26X_BASE_PARSE_FORMAT_NONE 0
#define GST_H26X_BASE_PARSE_FORMAT_BYTE 1

enum
{
  GST_H26X_BASE_PARSE_STATE_GOT_SPS = 1 << 0,
  GST_H26X_BASE_PARSE_STATE_GOT_PPS = 1 << 1,
  GST_H26X_BASE_PARSE_STATE_GOT_SLICE = 1 << 2,

  GST_H26X_BASE_PARSE_STATE_VALID_PICTURE_HEADERS = (GST_H26X_BASE_PARSE_STATE_GOT_SPS |
      GST_H26X_BASE_PARSE_STATE_GOT_PPS),
  GST_H26X_BASE_PARSE_STATE_VALID_PICTURE =
      (GST_H26X_BASE_PARSE_STATE_VALID_PICTURE_HEADERS |
      GST_H26X_BASE_PARSE_STATE_GOT_SLICE)
};

#define GST_H26X_BASE_PARSE_STATE_VALID(parse, expected_state) \
  ((GST_H26X_BASE_PARSE(parse)->state & (expected_state)) == (expected_state))

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
};

struct _GstH26XBaseParseProfileTierLevel
{
  const gchar * profile;
  const gchar * tier;
  const gchar * level;
};

struct _GstH26XBaseParse
{
  GstBaseParse baseparse;

  guint max_vps_count;
  guint max_sps_count;
  guint max_pps_count;

  guint min_nalu_size;

  /* stream */
  gint width, height;
  gint fps_num, fps_den;
  gint upstream_par_n, upstream_par_d;
  gint parsed_par_n, parsed_par_d;
  /* current codec_data in output caps, if any */
  GstBuffer *codec_data;
  /* input codec_data, if any */
  GstBuffer *codec_data_in;
  guint nal_length_size;
  gboolean packetized;
  gboolean split_packetized;
  gboolean transform;

  /* state */
#if 0
  GstH264NalParser *nalparser;
#endif
  guint state;
  GstH26XBaseParseAlign in_align;
  GstH26XBaseParseAlign align;
  guint format;
  gint current_off;
  /* True if input format and alignment match negotiated output */
  gboolean can_passthrough;

  GstClockTime last_report;
  gboolean push_codec;
  /* The following variables have a meaning in context of "have
   * VPS/SPS/PPS to push downstream", e.g. to update caps */
  gboolean have_vps;
  gboolean have_sps;
  gboolean have_pps;

  gboolean sent_codec_tag;

  /* collected VPS, SPS and PPS NALUs */
  GstBuffer **vps_nals;
  GstBuffer **sps_nals;
  GstBuffer **pps_nals;

#if 0
  /* Infos we need to keep track of */
  guint32 sei_cpb_removal_delay;
  guint8 sei_pic_struct;
  guint8 sei_pic_struct_pres_flag;
  guint field_pic_flag;
#endif

  /* cached timestamps */
  /* (trying to) track upstream dts and interpolate */
  GstClockTime dts;
  /* dts at start of last buffering period */
  GstClockTime ts_trn_nb;
  gboolean do_ts;

  gboolean discont;

  /* frame parsing */
  gint idr_pos, sei_pos;
  gboolean update_caps;
  GstAdapter *frame_out;
  gboolean keyframe;
  gboolean header;
  gboolean frame_start;
  /* AU state */
  gboolean picture_start;

  /* props */
  gint interval;

  GstClockTime pending_key_unit_ts;
  GstEvent *force_key_unit_event;

  /* Stereo / multiview info */
  GstVideoMultiviewMode multiview_mode;
  GstVideoMultiviewFlags multiview_flags;
  gboolean first_in_bundle;

  /* For insertion of AU Delimiter */
  gboolean aud_needed;
  gboolean aud_insert;
};

struct _GstH26XBaseParseClass
{
  GstBaseParseClass parent_class;

  void          (*get_max_vps_sps_pps_count) (GstH26XBaseParse * parse,
                                              guint * max_vps_count,
                                              guint * max_sps_count,
                                              guint * max_pps_count);

  guint         (*get_min_nalu_size)  (GstH26XBaseParse * parse);

  const gchar * (*format_to_string)   (GstH26XBaseParse * parse,
                                       guint format);

  guint         (*format_from_string) (GstH26XBaseParse * parse,
                                       const gchar * format);

  GstCaps *     (*get_default_caps)   (GstH26XBaseParse * parse);

  gboolean      (*fixate_format)      (GstH26XBaseParse * parse,
                                       guint * format,
                                       guint * align,
                                       const GValue *codec_data_value);

  gboolean      (*handle_codec_data)  (GstH26XBaseParse * parse,
                                       GstMapInfo * map);

  void          (*get_timestamp)      (GstH26XBaseParse * parse,
                                       GstClockTime * ts,
                                       GstClockTime * dur);

  gboolean      (*has_last_sps)       (GstH26XBaseParse * parse);

  gboolean      (*fill_sps_info)      (GstH26XBaseParse * parse,
                                       GstH26XBaseParseSPSInfo * info);

  gboolean      (*fill_profile_tier_level) (GstH26XBaseParse * parse,
                                            GstH26XBaseParseProfileTierLevel *ptl);

  GstCaps *     (*get_compatible_profile_caps_from_last_sps)  (GstH26XBaseParse * parse);

  GstBuffer *   (*prepare_pre_push_frame)   (GstH26XBaseParse * parse,
                                             GstBaseParseFrame * frame);

  GstBuffer *   (*make_codec_data)          (GstH26XBaseParse * parse);

  GstFlowReturn (*handle_frame_packetized)  (GstH26XBaseParse * parse,
                                             GstBaseParseFrame * frame);

  GstH26XBaseParseHandleFrameReturn   (*handle_frame_check_initial_skip)  (GstH26XBaseParse * parse,
                                                                       gint * skipsize,
                                                                       gint * dropsize,
                                                                       GstMapInfo * map);

  GstH26XBaseParseHandleFrameReturn   (*handle_frame_bytestream)          (GstH26XBaseParse * parse,
                                                                       gint * skipsize,
                                                                       gint * framesize,
                                                                       gint * current_off,
                                                                       GstMapInfo * map,
                                                                       gboolean drain);
};

GstBuffer * gst_h26x_base_parse_wrap_nal (GstH26XBaseParse * parse,
                                          guint format, guint8 * data,
                                          guint size);

void        gst_h26x_base_parse_sps_parsed (GstH26XBaseParse * parse);

void        gst_h26x_base_parse_pps_parsed (GstH26XBaseParse * parse);

void        gst_h26x_base_parse_sei_parsed (GstH26XBaseParse * parse,
                                            guint nalu_offset);

void        gst_h26x_base_parse_frame_started (GstH26XBaseParse * parse);

void        gst_h26x_base_parse_slice_hdr_parsed (GstH26XBaseParse * parse,
                                                  gboolean keyframe);

void        gst_h26x_base_parse_update_idr_pos (GstH26XBaseParse * parse,
                                                guint nalu_offset);

void        gst_h26x_base_pares_finish_process_nal (GstH26XBaseParse * parse,
                                              guint8 * data,
                                              guint size);

void        gst_h26x_base_parse_update_src_caps (GstH26XBaseParse * parse,
                                                 GstCaps * caps);

GstFlowReturn gst_h26x_base_parse_parse_frame (GstH26XBaseParse * parse,
                                               GstBaseParseFrame * frame);

GstFlowReturn gst_h26x_base_parse_push_codec_buffer (GstH26XBaseParse * parse,
                                                     GstBuffer * nal,
                                                     GstClockTime ts);

G_END_DECLS
#endif
