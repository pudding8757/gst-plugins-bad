/* GStreamer
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
#include <gst/codecparsers/gsth264parser.h>
#include <gst/video/video.h>

G_BEGIN_DECLS

#define GST_TYPE_H26X_BASE_PARSE \
  (gst_h26x_base_parse_get_type())
#define GST_H26X_BASE_PARSE(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_H26X_BASE_PARSE,GstH26xBaseParse))
#define GST_H26X_BASE_PARSE_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_H26X_BASE_PARSE,GstH26xBaseParseClass))
#define GST_H26X_BASE_PARSE_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS((obj),GST_TYPE_H26X_BASE_PARSE,GstH26xBaseParseClass))
#define GST_IS_H26X_BASE_PARSE(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_H26X_BASE_PARSE))
#define GST_IS_H26X_BASE_PARSE_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_H26X_BASE_PARSE))

GType gst_h26x_base_parse_get_type (void);

typedef struct _GstH26xSPSInfo GstH26xSPSInfo;
typedef struct _GstH26xProfileTierLevel GstH26xProfileTierLevel;
typedef struct _GstH26xBaseParse GstH26xBaseParse;
typedef struct _GstH26xBaseParseClass GstH26xBaseParseClass;

#define GST_H26X_BASE_PARSE_FORMAT_NONE 0
#define GST_H26X_BASE_PARSE_FORMAT_BYTE 1
/* and more packetized formats defined by subclass */

typedef enum
{
  GST_H26X_BASE_PARSE_ALIGN_NONE,
  GST_H26X_BASE_PARSE_ALIGN_NAL,
  GST_H26X_BASE_PARSE_ALIGN_AU
} GstH26xAlign;

typedef enum
{
  GST_H26X_HANDLE_FRAME_OK,
  GST_H26X_HANDLE_FRAME_MORE,
  GST_H26X_HANDLE_FRAME_DROP,
  GST_H26X_HANDLE_FRAME_SKIP,
  GST_H26X_HANDLE_FRAME_INVALID_STREAM
} GstH26xHandleFrameReturn;

enum
{
  GST_H26X_BASE_PARSE_STATE_GOT_SPS = 1 << 0,
  GST_H26X_BASE_PARSE_STATE_GOT_PPS = 1 << 1,
  GST_H26X_BASE_PARSE_STATE_GOT_SLICE = 1 << 2,

  GST_H26X_BASE_PARSE_STATE_VALID_PICTURE_HEADERS =
      (GST_H26X_BASE_PARSE_STATE_GOT_SPS | GST_H26X_BASE_PARSE_STATE_GOT_PPS),
  GST_H26X_BASE_PARSE_STATE_VALID_PICTURE =
      (GST_H26X_BASE_PARSE_STATE_VALID_PICTURE_HEADERS |
      GST_H26X_BASE_PARSE_STATE_GOT_SLICE)
};

#define GST_H26X_BASE_PARSE_STATE_VALID(parse, expected_state) \
  ((GST_H26X_BASE_PARSE(parse)->state & (expected_state)) == (expected_state))

struct _GstH26xSPSInfo
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

struct _GstH26xProfileTierLevel
{
  const gchar * profile;
  const gchar * tier;
  const gchar * level;
};

struct _GstH26xBaseParse
{
  GstBaseParse baseparse;

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
  guint state;
  GstH26xAlign in_align;
  GstH26xAlign align;
  guint format;
  gint current_off;
  /* True if input format and alignment match negotiated output */
  gboolean can_passthrough;

  GstClockTime last_report;
  gboolean push_codec;
  /* The following variables have a meaning in context of "have
   * SPS/PPS to push downstream", e.g. to update caps */
  gboolean have_vps;
  gboolean have_sps;
  gboolean have_pps;

  /* collected VPS, SPS and PPS NALUs */
  GstBuffer **vps_nals;
  GstBuffer **sps_nals;
  GstBuffer **pps_nals;

  guint max_vps_count;
  guint max_sps_count;
  guint max_pps_count;

  gboolean do_ts;

  gboolean discont;

  /* frame parsing */
  gint idr_pos, sei_pos;
  gboolean update_caps;
  GstAdapter *frame_out;
  gboolean keyframe;
  gboolean header;
  gboolean frame_start;

  /* props */
  gint interval;

  gboolean sent_codec_tag;

  GstClockTime pending_key_unit_ts;
  GstEvent *force_key_unit_event;

  /* Stereo / multiview info */
  GstVideoMultiviewMode multiview_mode;
  GstVideoMultiviewFlags multiview_flags;
  gboolean first_in_bundle;

  guint min_nal_size;
};

struct _GstH26xBaseParseClass
{
  GstBaseParseClass parent_class;

  /* reset */
  void (*reset) (GstH26xBaseParse * self);
  void (*reset_frame) (GstH26xBaseParse * self);

  void (*get_max_vps_sps_pps_count) (GstH26xBaseParse * self,
                                    guint * max_vps_count,
                                    guint * max_sps_count,
                                    guint * max_pps_count);
  guint (*get_min_nal_size)         (GstH26xBaseParse * self);

  GstBuffer * (*prepare_pre_push_frame) (GstH26xBaseParse * self,
                                        GstBaseParseFrame * frame);

  GstBuffer * (*make_codec_data) (GstH26xBaseParse * parse);
  GstCaps * (*get_default_caps) (GstH26xBaseParse * parse);

  gboolean (*fixate_format) (GstH26xBaseParse * parse,
                            guint * format,
                            GstH26xAlign * align,
                            const GValue *codec_data_value);

  gboolean (*handle_codec_data) (GstH26xBaseParse * parse,
                                 GstMapInfo * map);

  const gchar * (*format_to_string) (GstH26xBaseParse * parse,
                                     guint format);
  guint (*format_from_string) (GstH26xBaseParse * parse,
                              const gchar * format);

  void (*get_timestamp) (GstH26xBaseParse * parse, GstClockTime * ts, GstClockTime * dur, gboolean frame);

  /* Get stream information form sps */
  gboolean (*has_last_sps) (GstH26xBaseParse * parse);
  gboolean (*fill_sps_info) (GstH26xBaseParse * parse, GstH26xSPSInfo * info);
  gboolean (*fill_profile_tier_level) (GstH26xBaseParse * parse,
      GstH26xProfileTierLevel *str);
  GstCaps * (*get_compatible_profile_caps_from_last_sps) (GstH26xBaseParse * parse);

  GstFlowReturn (*handle_frame_packetized)     (GstH26xBaseParse * parse,
                                           GstBaseParseFrame * frame);

  GstH26xHandleFrameReturn (*handle_frame_check_initial_skip)
                                          (GstH26xBaseParse * parse,
                                           gint * skipsize,
                                           gint * framesize,
                                           GstMapInfo * map);

  GstH26xHandleFrameReturn (*handle_frame_bytestream)
                                          (GstH26xBaseParse * parse,
                                           gint * skipsize,
                                           gint * framesize,
                                           gint * current_off,
                                           GstMapInfo * map,
                                           gboolean drain);
};

GstFlowReturn gst_h26x_base_parse_parse_frame (GstH26xBaseParse * self,
    GstBaseParseFrame * frame);
GstBuffer * gst_h26x_base_parse_wrap_nal (GstH26xBaseParse * self,
    guint format, guint8 * data, guint size);
GstFlowReturn
gst_h26x_base_parse_push_codec_buffer (GstH26xBaseParse * self,
    GstBuffer * nal, GstClockTime ts);
void gst_h26x_base_parse_update_src_caps (GstH26xBaseParse * self, GstCaps * caps);


G_END_DECLS

#endif
