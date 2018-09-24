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

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <gst/base/base.h>
#include <gst/pbutils/pbutils.h>
#include <gst/video/video.h>
#include "gsth26xbaseparse.h"

#include <string.h>

GST_DEBUG_CATEGORY (h26x_base_parse_debug);
#define GST_CAT_DEFAULT h26x_base_parse_debug

#define DEFAULT_CONFIG_INTERVAL      (0)

enum
{
  PROP_0,
  PROP_CONFIG_INTERVAL
};

#define parent_class gst_h26x_base_parse_parent_class
G_DEFINE_ABSTRACT_TYPE (GstH26XBaseParse, gst_h26x_base_parse,
    GST_TYPE_BASE_PARSE);

static void gst_h26x_base_parse_finalize (GObject * object);

static gboolean gst_h26x_base_parse_start (GstBaseParse * parse);
static gboolean gst_h26x_base_parse_stop (GstBaseParse * parse);
static GstFlowReturn gst_h26x_base_parse_handle_frame (GstBaseParse * parse,
    GstBaseParseFrame * frame, gint * skipsize);
static GstFlowReturn gst_h26x_base_parse_pre_push_frame (GstBaseParse * parse,
    GstBaseParseFrame * frame);

static void gst_h26x_base_parse_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_h26x_base_parse_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static gboolean gst_h26x_base_parse_set_caps (GstBaseParse * parse,
    GstCaps * caps);
static GstCaps *gst_h26x_base_parse_get_caps (GstBaseParse * parse,
    GstCaps * filter);
static gboolean gst_h26x_base_parse_sink_event (GstBaseParse * parse,
    GstEvent * event);
static gboolean gst_h26x_base_parse_src_event (GstBaseParse * parse,
    GstEvent * event);

static void
gst_h26x_base_parse_class_init (GstH26XBaseParseClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GstBaseParseClass *parse_class = GST_BASE_PARSE_CLASS (klass);

  GST_DEBUG_CATEGORY_INIT (h26x_base_parse_debug,
      "h26xbaseparse", 0, "h26x base parser");

  gobject_class->finalize = gst_h26x_base_parse_finalize;
  gobject_class->set_property = gst_h26x_base_parse_set_property;
  gobject_class->get_property = gst_h26x_base_parse_get_property;

  g_object_class_install_property (gobject_class, PROP_CONFIG_INTERVAL,
      g_param_spec_int ("config-interval",
          "SPS PPS Send Interval",
          "Send SPS and PPS Insertion Interval in seconds (sprop parameter sets "
          "will be multiplexed in the data stream when detected.) "
          "(0 = disabled, -1 = send with every IDR frame)",
          -1, 3600, DEFAULT_CONFIG_INTERVAL,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));

  /* Override BaseParse vfuncs */
  parse_class->start = GST_DEBUG_FUNCPTR (gst_h26x_base_parse_start);
  parse_class->stop = GST_DEBUG_FUNCPTR (gst_h26x_base_parse_stop);
  parse_class->handle_frame =
      GST_DEBUG_FUNCPTR (gst_h26x_base_parse_handle_frame);
  parse_class->pre_push_frame =
      GST_DEBUG_FUNCPTR (gst_h26x_base_parse_pre_push_frame);
  parse_class->set_sink_caps = GST_DEBUG_FUNCPTR (gst_h26x_base_parse_set_caps);
  parse_class->get_sink_caps = GST_DEBUG_FUNCPTR (gst_h26x_base_parse_get_caps);
  parse_class->sink_event = GST_DEBUG_FUNCPTR (gst_h26x_base_parse_sink_event);
  parse_class->src_event = GST_DEBUG_FUNCPTR (gst_h26x_base_parse_src_event);
}

static void
gst_h26x_base_parse_init (GstH26XBaseParse * self)
{
  self->frame_out = gst_adapter_new ();
  gst_base_parse_set_pts_interpolation (GST_BASE_PARSE (self), FALSE);
  GST_PAD_SET_ACCEPT_INTERSECT (GST_BASE_PARSE_SINK_PAD (self));
  GST_PAD_SET_ACCEPT_TEMPLATE (GST_BASE_PARSE_SINK_PAD (self));

  self->aud_needed = TRUE;
  self->aud_insert = TRUE;
}

static void
gst_h26x_base_parse_finalize (GObject * object)
{
  GstH26XBaseParse *self = GST_H26X_BASE_PARSE (object);

  g_object_unref (self->frame_out);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_h26x_base_parse_reset_frame (GstH26XBaseParse * self)
{
  GST_DEBUG_OBJECT (self, "reset frame");

  /* done parsing; reset state */
  self->current_off = -1;

  self->picture_start = FALSE;
  self->update_caps = FALSE;
  self->idr_pos = -1;
  self->sei_pos = -1;
  self->keyframe = FALSE;
  self->header = FALSE;
  self->frame_start = FALSE;
  self->aud_insert = TRUE;
  gst_adapter_clear (self->frame_out);
}

static void
gst_h26x_base_parse_reset_stream_info (GstH26XBaseParse * self)
{
  gint i;

  self->width = 0;
  self->height = 0;
  self->fps_num = 0;
  self->fps_den = 0;
  self->upstream_par_n = -1;
  self->upstream_par_d = -1;
  self->parsed_par_n = 0;
  self->parsed_par_d = 0;
  self->have_pps = FALSE;
  self->have_sps = FALSE;

  self->multiview_mode = GST_VIDEO_MULTIVIEW_MODE_NONE;
  self->multiview_flags = GST_VIDEO_MULTIVIEW_FLAGS_NONE;
  self->first_in_bundle = TRUE;

  self->align = GST_H26X_BASE_PARSE_ALIGN_NONE;
  self->format = GST_H26X_BASE_PARSE_FORMAT_NONE;

  self->transform = FALSE;
  self->nal_length_size = 4;
  self->packetized = FALSE;
  self->push_codec = FALSE;

  gst_buffer_replace (&self->codec_data, NULL);
  gst_buffer_replace (&self->codec_data_in, NULL);

  gst_h26x_base_parse_reset_frame (self);

  for (i = 0; i < self->max_vps_count; i++)
    gst_buffer_replace (&self->vps_nals[i], NULL);
  for (i = 0; i < self->max_sps_count; i++)
    gst_buffer_replace (&self->sps_nals[i], NULL);
  for (i = 0; i < self->max_pps_count; i++)
    gst_buffer_replace (&self->pps_nals[i], NULL);
}

static void
gst_h26x_base_parse_reset (GstH26XBaseParse * self)
{
  self->last_report = GST_CLOCK_TIME_NONE;

  self->dts = GST_CLOCK_TIME_NONE;
  self->ts_trn_nb = GST_CLOCK_TIME_NONE;
  self->do_ts = TRUE;

  self->sent_codec_tag = FALSE;

  self->pending_key_unit_ts = GST_CLOCK_TIME_NONE;
  gst_event_replace (&self->force_key_unit_event, NULL);

  self->discont = FALSE;

  gst_h26x_base_parse_reset_stream_info (self);
}

static gboolean
gst_h26x_base_parse_start (GstBaseParse * parse)
{
  GstH26XBaseParse *self = GST_H26X_BASE_PARSE (parse);
  GstH26XBaseParseClass *klass = GST_H26X_BASE_PARSE_GET_CLASS (self);

  GST_DEBUG_OBJECT (parse, "start");

  klass->get_max_vps_sps_pps_count (self,
      &self->max_vps_count, &self->max_sps_count, &self->max_pps_count);
  self->min_nalu_size = klass->get_min_nalu_size (self);

  if (self->max_vps_count)
    self->vps_nals = g_new0 (GstBuffer *, self->max_vps_count);
  self->sps_nals = g_new0 (GstBuffer *, self->max_sps_count);
  self->pps_nals = g_new0 (GstBuffer *, self->max_pps_count);

  gst_h26x_base_parse_reset (self);

#if 0
  self->nalparser = gst_h264_nal_parser_new ();
#endif

  self->state = 0;

#if 0
  self->dts = GST_CLOCK_TIME_NONE;
  self->ts_trn_nb = GST_CLOCK_TIME_NONE;
  self->sei_pic_struct_pres_flag = FALSE;
  self->sei_pic_struct = 0;
  self->field_pic_flag = 0;
#endif

  gst_base_parse_set_min_frame_size (parse, self->min_nalu_size + 1);

  return TRUE;
}

static gboolean
gst_h26x_base_parse_stop (GstBaseParse * parse)
{
  GstH26XBaseParse *self = GST_H26X_BASE_PARSE (parse);

  GST_DEBUG_OBJECT (parse, "stop");
  gst_h26x_base_parse_reset (self);

  g_clear_pointer (&self->vps_nals, g_free);
  g_clear_pointer (&self->sps_nals, g_free);
  g_clear_pointer (&self->pps_nals, g_free);

#if 0
  gst_h264_nal_parser_free (self->nalparser);
#endif

  return TRUE;
}

static const gchar *
gst_h26x_base_parse_get_string (GstH26XBaseParse * parse, gboolean format,
    gint code)
{
  GstH26XBaseParseClass *klass = GST_H26X_BASE_PARSE_GET_CLASS (parse);

  if (format) {
    return klass->format_to_string (parse, code);
  } else {
    switch (code) {
      case GST_H26X_BASE_PARSE_ALIGN_NAL:
        return "nal";
      case GST_H26X_BASE_PARSE_ALIGN_AU:
        return "au";
      default:
        return "none";
    }
  }
}

static void
gst_h26x_base_parse_format_from_caps (GstH26XBaseParse * parse,
    GstCaps * caps, guint * format, guint * align)
{
  GstH26XBaseParseClass *klass = GST_H26X_BASE_PARSE_GET_CLASS (parse);

  if (format)
    *format = GST_H26X_BASE_PARSE_FORMAT_NONE;

  if (align)
    *align = GST_H26X_BASE_PARSE_ALIGN_NONE;

  g_return_if_fail (gst_caps_is_fixed (caps));

  GST_DEBUG ("parsing caps: %" GST_PTR_FORMAT, caps);

  if (caps && gst_caps_get_size (caps) > 0) {
    GstStructure *s = gst_caps_get_structure (caps, 0);
    const gchar *str = NULL;

    if (format) {
      if ((str = gst_structure_get_string (s, "stream-format"))) {
        *format = klass->format_from_string (parse, str);
      }
    }

    if (align) {
      if ((str = gst_structure_get_string (s, "alignment"))) {
        if (strcmp (str, "au") == 0)
          *align = GST_H26X_BASE_PARSE_ALIGN_AU;
        else if (strcmp (str, "nal") == 0)
          *align = GST_H26X_BASE_PARSE_ALIGN_NAL;
      }
    }
  }
}

/* check downstream caps to configure format and alignment */
static void
gst_h26x_base_parse_negotiate (GstH26XBaseParse * self, gint in_format,
    GstCaps * in_caps)
{
  GstCaps *caps;
  guint format = self->format;
  guint align = self->align;

  g_return_if_fail ((in_caps == NULL) || gst_caps_is_fixed (in_caps));

  caps = gst_pad_get_allowed_caps (GST_BASE_PARSE_SRC_PAD (self));
  GST_DEBUG_OBJECT (self, "allowed caps: %" GST_PTR_FORMAT, caps);

  /* concentrate on leading structure, since decodebin parser
   * capsfilter always includes parser template caps */
  if (caps) {
    caps = gst_caps_truncate (caps);
    GST_DEBUG_OBJECT (self, "negotiating with caps: %" GST_PTR_FORMAT, caps);
  }

  self->can_passthrough = FALSE;

  if (in_caps && caps) {
    if (gst_caps_can_intersect (in_caps, caps)) {
      GST_DEBUG_OBJECT (self, "downstream accepts upstream caps");
      gst_h26x_base_parse_format_from_caps (self, in_caps, &format, &align);
      gst_caps_unref (caps);
      caps = NULL;
      self->can_passthrough = TRUE;
    }
  }

  /* FIXME We could fail the negotiation immediatly if caps are empty */
  if (caps && !gst_caps_is_empty (caps)) {
    /* fixate to avoid ambiguity with lists when parsing */
    caps = gst_caps_fixate (caps);
    gst_h26x_base_parse_format_from_caps (self, caps, &format, &align);
  }

  /* default */
  if (!format)
    format = GST_H26X_BASE_PARSE_FORMAT_BYTE;
  if (!align)
    align = GST_H26X_BASE_PARSE_ALIGN_AU;

  GST_DEBUG_OBJECT (self, "selected format %s, alignment %s",
      gst_h26x_base_parse_get_string (self, TRUE, format),
      gst_h26x_base_parse_get_string (self, FALSE, align));

  self->format = format;
  self->align = align;

  self->transform = in_format != self->format ||
      align == GST_H26X_BASE_PARSE_ALIGN_AU;

  if (caps)
    gst_caps_unref (caps);
}

GstBuffer *
gst_h26x_base_parse_wrap_nal (GstH26XBaseParse * self, guint format,
    guint8 * data, guint size)
{
  GstBuffer *buf;
  guint nl = self->nal_length_size;
  guint32 tmp;

  GST_DEBUG_OBJECT (self, "nal length %d", size);

  buf = gst_buffer_new_allocate (NULL, 4 + size, NULL);
  if (format != GST_H26X_BASE_PARSE_FORMAT_BYTE) {
    tmp = GUINT32_TO_BE (size << (32 - 8 * nl));
  } else {
    /* HACK: nl should always be 4 here, otherwise this won't work.
     * There are legit cases where nl in avc/hvc stream is 2, but byte-stream
     * SC is still always 4 bytes. */
    nl = 4;
    tmp = GUINT32_TO_BE (1);
  }

  gst_buffer_fill (buf, 0, &tmp, sizeof (guint32));
  gst_buffer_fill (buf, nl, data, size);
  gst_buffer_set_size (buf, size + nl);

  return buf;
}

void
gst_h26x_base_parse_sps_parsed (GstH26XBaseParse * self)
{
  GST_DEBUG_OBJECT (self, "SPS parsed, triggering src caps check");
  self->update_caps = TRUE;
  self->have_sps = TRUE;
  if (self->push_codec && self->have_pps) {
    /* SPS and PPS found in stream before the first pre_push_frame, no need
     * to forcibly push at start */
    GST_INFO_OBJECT (self, "have SPS/PPS in stream");
    self->push_codec = FALSE;
    self->have_sps = FALSE;
    self->have_pps = FALSE;
  }

  self->state |= GST_H26X_BASE_PARSE_STATE_GOT_SPS;
  self->header |= TRUE;
}

void
gst_h26x_base_parse_pps_parsed (GstH26XBaseParse * self)
{
  /* parameters might have changed, force caps check */
  if (!self->have_pps) {
    GST_DEBUG_OBJECT (self, "PPS parsed, triggering src caps check");
    self->update_caps = TRUE;
  }
  self->have_pps = TRUE;
  if (self->push_codec && self->have_sps) {
    /* SPS and PPS found in stream before the first pre_push_frame, no need
     * to forcibly push at start */
    GST_INFO_OBJECT (self, "have SPS/PPS in stream");
    self->push_codec = FALSE;
    self->have_sps = FALSE;
    self->have_pps = FALSE;
  }

  self->state |= GST_H26X_BASE_PARSE_STATE_GOT_PPS;
  self->header |= TRUE;
}

void
gst_h26x_base_parse_sei_parsed (GstH26XBaseParse * self, guint nalu_offset)
{
  self->header |= TRUE;

  /* mark SEI pos */
  if (self->sei_pos == -1) {
    if (self->transform)
      self->sei_pos = gst_adapter_available (self->frame_out);
    else
      self->sei_pos = nalu_offset;
    GST_DEBUG_OBJECT (self, "marking SEI in frame at offset %d", self->sei_pos);
  }
}

void
gst_h26x_base_parse_frame_started (GstH26XBaseParse * self)
{
  GST_DEBUG_OBJECT (self, "frame start");
  self->frame_start = TRUE;
}

void
gst_h26x_base_parse_slice_hdr_parsed (GstH26XBaseParse * self,
    gboolean keyframe)
{
  if (keyframe)
    self->keyframe |= TRUE;
  self->state |= GST_H26X_BASE_PARSE_STATE_GOT_SLICE;
}

void
gst_h26x_base_parse_update_idr_pos (GstH26XBaseParse * self, guint nalu_offset)
{
  if (self->idr_pos == -1) {
    if (self->transform)
      self->idr_pos = gst_adapter_available (self->frame_out);
    else
      self->idr_pos = nalu_offset;
    GST_DEBUG_OBJECT (self, "marking IDR in frame at offset %d", self->idr_pos);
  }
  /* if SEI preceeds (faked) IDR, then we have to insert config there */
  if (self->sei_pos >= 0 && self->idr_pos > self->sei_pos) {
    self->idr_pos = self->sei_pos;
    GST_DEBUG_OBJECT (self, "moved IDR mark to SEI position %d", self->idr_pos);
  }
}

void
gst_h26x_base_pares_finish_process_nal (GstH26XBaseParse * self, guint8 * data,
    guint size)
{
  /* if packetized format output needed, collect properly prefixed nal in adapter,
   * and use that to replace outgoing buffer data later on */
  if (self->transform) {
    GstBuffer *buf;

    GST_LOG_OBJECT (self, "collecting NAL in frame");
    buf = gst_h26x_base_parse_wrap_nal (self, self->format, data, size);
    gst_adapter_push (self->frame_out, buf);
  }
}

static GstFlowReturn
gst_h26x_base_parse_handle_frame (GstBaseParse * parse,
    GstBaseParseFrame * frame, gint * skipsize)
{
  GstH26XBaseParse *self = GST_H26X_BASE_PARSE (parse);
  GstH26XBaseParseClass *klass = GST_H26X_BASE_PARSE_GET_CLASS (self);
  GstBuffer *buffer = frame->buffer;
  GstMapInfo map;
  gsize size;
  gint current_off = 0;
  gboolean drain;
  gint framesize;
  GstFlowReturn ret;
  GstH26XBaseParseHandleFrameReturn handle_ret;

  if (G_UNLIKELY (GST_BUFFER_FLAG_IS_SET (frame->buffer,
              GST_BUFFER_FLAG_DISCONT))) {
    self->discont = TRUE;
  }

  /* delegate in packetized case, no skipping should be needed */
  if (self->packetized)
    return klass->handle_frame_packetized (self, frame);

  gst_buffer_map (buffer, &map, GST_MAP_READ);
  size = map.size;

  /* expect at least 3 bytes startcode == sc, and 2 bytes NALU payload */
  if (G_UNLIKELY (size < 5)) {
    gst_buffer_unmap (buffer, &map);
    *skipsize = 1;
    return GST_FLOW_OK;
  }

  /* need to configure aggregation */
  if (G_UNLIKELY (self->format == GST_H26X_BASE_PARSE_FORMAT_NONE))
    gst_h26x_base_parse_negotiate (self, GST_H26X_BASE_PARSE_FORMAT_BYTE, NULL);

  /* avoid stale cached parsing state */
  if (frame->flags & GST_BASE_PARSE_FRAME_FLAG_NEW_FRAME) {
    GST_LOG_OBJECT (self, "parsing new frame");
    gst_h26x_base_parse_reset_frame (self);
  } else {
    GST_LOG_OBJECT (self, "resuming frame parsing");
  }

  /* Always consume the entire input buffer when in_align == ALIGN_AU */
  drain = GST_BASE_PARSE_DRAINING (parse)
      || self->in_align == GST_H26X_BASE_PARSE_ALIGN_AU;

  current_off = self->current_off;
  if (current_off < 0)
    current_off = 0;
  g_assert (current_off < size);
  GST_DEBUG_OBJECT (self, "last parse position %d", current_off);

  /* check for initial skip */
  if (self->current_off == -1) {
    gint dropsize = 0;
    handle_ret = klass->handle_frame_check_initial_skip (self, skipsize,
        &dropsize, &map);

    switch (handle_ret) {
      case GST_H26X_BASE_PARSE_HANDLE_FRAME_DROP:
        frame->flags |= GST_BASE_PARSE_FRAME_FLAG_DROP;
        gst_buffer_unmap (buffer, &map);
        ret = gst_base_parse_finish_frame (parse, frame, dropsize);
        goto drop;
      case GST_H26X_BASE_PARSE_HANDLE_FRAME_SKIP:
        goto skip;
      case GST_H26X_BASE_PARSE_HANDLE_FRAME_INVALID_STREAM:
        goto invalid_stream;
      default:
        break;
    }
  }

  handle_ret = klass->handle_frame_bytestream (self, skipsize, &framesize,
      &current_off, &map, drain);

  switch (handle_ret) {
    case GST_H26X_BASE_PARSE_HANDLE_FRAME_SKIP:
      goto skip;
    case GST_H26X_BASE_PARSE_HANDLE_FRAME_INVALID_STREAM:
      goto invalid_stream;
    case GST_H26X_BASE_PARSE_HANDLE_FRAME_MORE:
      goto more;
    default:
      break;
  }

  gst_buffer_unmap (buffer, &map);

  gst_h26x_base_parse_parse_frame (self, frame);

  return gst_base_parse_finish_frame (parse, frame, framesize);

more:
  *skipsize = 0;

  /* Restart parsing from here next time */
  if (current_off > 0)
    self->current_off = current_off;

  /* Fall-through. */
out:
  gst_buffer_unmap (buffer, &map);
  return GST_FLOW_OK;

drop:
  GST_DEBUG_OBJECT (self, "Dropped data");
  return ret;

skip:
  GST_DEBUG_OBJECT (self, "skipping %d", *skipsize);
  /* If we are collecting access units, we need to preserve the initial
   * config headers (SPS, PPS et al.) and only reset the frame if another
   * slice NAL was received. This means that broken pictures are discarded */
  if (self->align != GST_H26X_BASE_PARSE_ALIGN_AU ||
      !(self->state & GST_H26X_BASE_PARSE_STATE_VALID_PICTURE_HEADERS) ||
      (self->state & GST_H26X_BASE_PARSE_STATE_GOT_SLICE))
    gst_h26x_base_parse_reset_frame (self);
  goto out;

invalid_stream:
  gst_buffer_unmap (buffer, &map);
  return GST_FLOW_ERROR;
}

static void
gst_h26x_base_parse_get_par (GstH26XBaseParse * self, gint * num, gint * den)
{
  if (self->upstream_par_n != -1 && self->upstream_par_d != -1) {
    *num = self->upstream_par_n;
    *den = self->upstream_par_d;
  } else {
    *num = self->parsed_par_n;
    *den = self->parsed_par_d;
  }
}

/* if downstream didn't support the exact profile indicated in sps header,
 * check for the compatible profiles also */
static void
ensure_caps_profile (GstH26XBaseParse * self, GstCaps * caps)
{
  GstCaps *peer_caps, *compat_caps;
  GstH26XBaseParseClass *klass = GST_H26X_BASE_PARSE_GET_CLASS (self);

  peer_caps = gst_pad_get_current_caps (GST_BASE_PARSE_SRC_PAD (self));
  if (!peer_caps || !gst_caps_can_intersect (caps, peer_caps)) {
    GstCaps *filter_caps = klass->get_default_caps (self);

    if (peer_caps)
      gst_caps_unref (peer_caps);
    peer_caps =
        gst_pad_peer_query_caps (GST_BASE_PARSE_SRC_PAD (self), filter_caps);

    gst_caps_unref (filter_caps);
  }

  if (peer_caps && !gst_caps_can_intersect (caps, peer_caps)) {
    GstStructure *structure;

    compat_caps = klass->get_compatible_profile_caps_from_last_sps (self);
    if (compat_caps != NULL) {
      GstCaps *res_caps = NULL;

      res_caps = gst_caps_intersect (peer_caps, compat_caps);

      if (res_caps && !gst_caps_is_empty (res_caps)) {
        const gchar *profile_str = NULL;

        res_caps = gst_caps_fixate (res_caps);
        structure = gst_caps_get_structure (res_caps, 0);
        profile_str = gst_structure_get_string (structure, "profile");
        if (profile_str) {
          gst_caps_set_simple (caps, "profile", G_TYPE_STRING, profile_str,
              NULL);
          GST_DEBUG_OBJECT (self,
              "Setting compatible profile %s to the caps", profile_str);
        }
      }
      if (res_caps)
        gst_caps_unref (res_caps);
      gst_caps_unref (compat_caps);
    }
  }
  if (peer_caps)
    gst_caps_unref (peer_caps);
}

void
gst_h26x_base_parse_update_src_caps (GstH26XBaseParse * self, GstCaps * caps)
{
  GstCaps *sink_caps, *src_caps;
  gboolean modified = FALSE;
  GstBuffer *buf = NULL;
  GstStructure *s = NULL;
  GstH26XBaseParseClass *klass;
  gboolean has_sps;
  GstH26XBaseParseSPSInfo info = { 0 };

  if (G_UNLIKELY (!gst_pad_has_current_caps (GST_BASE_PARSE_SRC_PAD (self))))
    modified = TRUE;
  else if (G_UNLIKELY (!self->update_caps))
    return;

  klass = GST_H26X_BASE_PARSE_GET_CLASS (self);

  /* if this is being called from the first _setcaps call, caps on the sinkpad
   * aren't set yet and so they need to be passed as an argument */
  if (caps)
    sink_caps = gst_caps_ref (caps);
  else
    sink_caps = gst_pad_get_current_caps (GST_BASE_PARSE_SINK_PAD (self));

  /* carry over input caps as much as possible; override with our own stuff */
  if (!sink_caps)
    sink_caps = klass->get_default_caps (self);
  else
    s = gst_caps_get_structure (sink_caps, 0);

  has_sps = klass->has_last_sps (self);
  GST_DEBUG_OBJECT (self, "has sps: %d", has_sps);

  /* only codec-data for nice-and-clean au aligned packetized avc format */
  if ((self->format != GST_H26X_BASE_PARSE_FORMAT_BYTE)
      && self->align == GST_H26X_BASE_PARSE_ALIGN_AU) {
    buf = klass->make_codec_data (self);
    if (buf && self->codec_data) {
      GstMapInfo map;

      gst_buffer_map (buf, &map, GST_MAP_READ);
      if (map.size != gst_buffer_get_size (self->codec_data) ||
          gst_buffer_memcmp (self->codec_data, 0, map.data, map.size))
        modified = TRUE;

      gst_buffer_unmap (buf, &map);
    } else {
      if (!buf && self->codec_data_in)
        buf = gst_buffer_ref (self->codec_data_in);
      modified = TRUE;
    }
  }

  caps = NULL;
  if (G_UNLIKELY (!has_sps)) {
    caps = gst_caps_copy (sink_caps);
  } else {
    klass->fill_sps_info (self, &info);

    if (G_UNLIKELY (self->width != info.width || self->height != info.height)) {
      GST_INFO_OBJECT (self, "resolution changed %dx%d",
          info.width, info.height);
      self->width = info.width;
      self->height = info.height;
      modified = TRUE;
    }

    if (G_UNLIKELY (self->fps_num != info.fps_num
            || self->fps_den != info.fps_den)) {
      GST_DEBUG_OBJECT (self,
          "framerate changed %d/%d", info.fps_num, info.fps_den);
      self->fps_num = info.fps_num;
      self->fps_den = info.fps_den;
      modified = TRUE;
    }

    if (info.par_num > 0 && info.par_den > 0) {
      if (G_UNLIKELY ((self->parsed_par_n != info.par_num)
              || (self->parsed_par_d != info.par_den))) {
        self->parsed_par_n = info.par_num;
        self->parsed_par_d = info.par_den;
        GST_INFO_OBJECT (self, "pixel aspect ratio has been changed %d/%d",
            self->parsed_par_n, self->parsed_par_d);
      }
    }

    if (G_UNLIKELY (modified || self->update_caps)) {
      gint width, height;
      GstClockTime latency;
      gint fps_num, fps_den;
      gint par_n, par_d;
      const gchar *caps_mview_mode = NULL;
      GstVideoMultiviewMode mview_mode = self->multiview_mode;
      GstVideoMultiviewFlags mview_flags = self->multiview_flags;

      fps_num = self->fps_num;
      fps_den = self->fps_den;

      caps = gst_caps_copy (sink_caps);

      /* sps should give this but upstream overrides */
      if (s && gst_structure_has_field (s, "width"))
        gst_structure_get_int (s, "width", &width);
      else
        width = self->width;

      if (s && gst_structure_has_field (s, "height"))
        gst_structure_get_int (s, "height", &height);
      else
        height = self->height;

      if (s == NULL ||
          !gst_structure_get_fraction (s, "pixel-aspect-ratio", &par_n,
              &par_d)) {
        gst_h26x_base_parse_get_par (self, &par_n, &par_d);
        if (par_n != 0 && par_d != 0) {
          GST_INFO_OBJECT (self, "PAR %d/%d", par_n, par_d);
          gst_caps_set_simple (caps, "pixel-aspect-ratio", GST_TYPE_FRACTION,
              par_n, par_d, NULL);
        } else {
          /* Assume par_n/par_d of 1/1 for calcs below, but don't set into caps */
          par_n = par_d = 1;
        }
      }

      /* Pass through or set output stereo/multiview config */
      if (s && gst_structure_has_field (s, "multiview-mode")) {
        caps_mview_mode = gst_structure_get_string (s, "multiview-mode");
        gst_structure_get_flagset (s, "multiview-flags",
            (guint *) & mview_flags, NULL);
      } else if (mview_mode != GST_VIDEO_MULTIVIEW_MODE_NONE) {
        if (gst_video_multiview_guess_half_aspect (mview_mode,
                width, height, par_n, par_d)) {
          mview_flags |= GST_VIDEO_MULTIVIEW_FLAGS_HALF_ASPECT;
        }

        caps_mview_mode = gst_video_multiview_mode_to_caps_string (mview_mode);
        gst_caps_set_simple (caps, "multiview-mode", G_TYPE_STRING,
            caps_mview_mode, "multiview-flags",
            GST_TYPE_VIDEO_MULTIVIEW_FLAGSET, mview_flags,
            GST_FLAG_SET_MASK_EXACT, NULL);
      }

      gst_caps_set_simple (caps, "width", G_TYPE_INT, width,
          "height", G_TYPE_INT, height, NULL);

      /* upstream overrides */
      if (s && gst_structure_has_field (s, "framerate"))
        gst_structure_get_fraction (s, "framerate", &fps_num, &fps_den);

      /* but not necessarily or reliably this */
      if (fps_den > 0) {
        gst_caps_set_simple (caps, "framerate",
            GST_TYPE_FRACTION, fps_num, fps_den, NULL);
        gst_base_parse_set_frame_rate (GST_BASE_PARSE (self),
            fps_num, fps_den, 0, 0);
        if (fps_num > 0) {
          latency = gst_util_uint64_scale (GST_SECOND, fps_den, fps_num);
          gst_base_parse_set_latency (GST_BASE_PARSE (self), latency, latency);
        }

      }

      if (s && !gst_structure_has_field (s, "interlace-mode"))
        gst_caps_set_simple (caps, "interlace-mode", G_TYPE_STRING,
            gst_video_interlace_mode_to_string (info.interlace_mode), NULL);

      if (info.chroma_format)
        gst_caps_set_simple (caps,
            "chroma-format", G_TYPE_STRING, info.chroma_format,
            "bit-depth-luma", G_TYPE_UINT, info.bit_depth_luma,
            "bit-depth-chroma", G_TYPE_UINT, info.bit_depth_chroma, NULL);
    }
  }

  if (caps) {
    gst_caps_set_simple (caps, "parsed", G_TYPE_BOOLEAN, TRUE,
        "stream-format", G_TYPE_STRING,
        gst_h26x_base_parse_get_string (self, TRUE, self->format),
        "alignment", G_TYPE_STRING,
        gst_h26x_base_parse_get_string (self, FALSE, self->align), NULL);

    /* set profile and level in caps */
    if (has_sps) {
      GstH26XBaseParseProfileTierLevel ptl = { 0 };

      if (ptl.profile != NULL)
        gst_caps_set_simple (caps, "profile", G_TYPE_STRING, ptl.profile, NULL);

      if (ptl.level != NULL)
        gst_caps_set_simple (caps, "level", G_TYPE_STRING, ptl.level, NULL);

      /* relax the profile constraint to find a suitable decoder */
      ensure_caps_profile (self, caps);
    }

    src_caps = gst_pad_get_current_caps (GST_BASE_PARSE_SRC_PAD (self));

    if (src_caps) {
      /* use codec data from old caps for comparison; we don't want to resend caps
         if everything is same except codec data; */
      if (gst_structure_has_field (gst_caps_get_structure (src_caps, 0),
              "codec_data")) {
        gst_caps_set_value (caps, "codec_data",
            gst_structure_get_value (gst_caps_get_structure (src_caps, 0),
                "codec_data"));
      } else if (!buf) {
        GstStructure *s;
        /* remove any left-over codec-data hanging around */
        s = gst_caps_get_structure (caps, 0);
        gst_structure_remove_field (s, "codec_data");
      }
    }

    if (!(src_caps && gst_caps_is_strictly_equal (src_caps, caps))) {
      /* update codec data to new value */
      if (buf) {
        gst_caps_set_simple (caps, "codec_data", GST_TYPE_BUFFER, buf, NULL);
        gst_buffer_replace (&self->codec_data, buf);
        gst_buffer_unref (buf);
        buf = NULL;
      } else {
        GstStructure *s;
        /* remove any left-over codec-data hanging around */
        s = gst_caps_get_structure (caps, 0);
        gst_structure_remove_field (s, "codec_data");
        gst_buffer_replace (&self->codec_data, NULL);
      }

      gst_pad_set_caps (GST_BASE_PARSE_SRC_PAD (self), caps);
    }

    if (src_caps)
      gst_caps_unref (src_caps);
    gst_caps_unref (caps);
  }

  gst_caps_unref (sink_caps);
  if (buf)
    gst_buffer_unref (buf);
}

GstFlowReturn
gst_h26x_base_parse_parse_frame (GstH26XBaseParse * self,
    GstBaseParseFrame * frame)
{
  GstBuffer *buffer;
  guint av;
  GstH26XBaseParseClass *klass;

  klass = GST_H26X_BASE_PARSE_GET_CLASS (self);
  buffer = frame->buffer;

  gst_h26x_base_parse_update_src_caps (self, NULL);

  /* don't mess with timestamps if provided by upstream,
   * particularly since our ts not that good they handle seeking etc */
  if (self->do_ts) {
    if (klass->get_timestamp) {
      klass->get_timestamp (self,
          &GST_BUFFER_TIMESTAMP (buffer), &GST_BUFFER_DURATION (buffer));
    } else {
      GST_FIXME_OBJECT (self,
          "Implement timestamp/duration interpolation based on SEI message");
    }
  }

  if (self->keyframe)
    GST_BUFFER_FLAG_UNSET (buffer, GST_BUFFER_FLAG_DELTA_UNIT);
  else
    GST_BUFFER_FLAG_SET (buffer, GST_BUFFER_FLAG_DELTA_UNIT);

  if (self->header)
    GST_BUFFER_FLAG_SET (buffer, GST_BUFFER_FLAG_HEADER);
  else
    GST_BUFFER_FLAG_UNSET (buffer, GST_BUFFER_FLAG_HEADER);

  if (self->discont) {
    GST_BUFFER_FLAG_SET (buffer, GST_BUFFER_FLAG_DISCONT);
    self->discont = FALSE;
  }

  /* replace with transformed output if applicable */
  av = gst_adapter_available (self->frame_out);
  if (av) {
    GstBuffer *buf;

    buf = gst_adapter_take_buffer (self->frame_out, av);
    gst_buffer_copy_into (buf, buffer, GST_BUFFER_COPY_METADATA, 0, -1);
    gst_buffer_replace (&frame->out_buffer, buf);
    gst_buffer_unref (buf);
  }

  return GST_FLOW_OK;
}

/* sends a codec NAL downstream, decorating and transforming as needed.
 * No ownership is taken of @nal */
GstFlowReturn
gst_h26x_base_parse_push_codec_buffer (GstH26XBaseParse * self,
    GstBuffer * nal, GstClockTime ts)
{
  GstMapInfo map;

  gst_buffer_map (nal, &map, GST_MAP_READ);
  nal = gst_h26x_base_parse_wrap_nal (self, self->format, map.data, map.size);
  gst_buffer_unmap (nal, &map);

  GST_BUFFER_TIMESTAMP (nal) = ts;
  GST_BUFFER_DURATION (nal) = 0;

  return gst_pad_push (GST_BASE_PARSE_SRC_PAD (self), nal);
}

static GstEvent *
check_pending_key_unit_event (GstEvent * pending_event,
    GstSegment * segment, GstClockTime timestamp, guint flags,
    GstClockTime pending_key_unit_ts)
{
  GstClockTime running_time, stream_time;
  gboolean all_headers;
  guint count;
  GstEvent *event = NULL;

  g_return_val_if_fail (segment != NULL, NULL);

  if (pending_event == NULL)
    goto out;

  if (GST_CLOCK_TIME_IS_VALID (pending_key_unit_ts) &&
      timestamp == GST_CLOCK_TIME_NONE)
    goto out;

  running_time = gst_segment_to_running_time (segment,
      GST_FORMAT_TIME, timestamp);

  GST_INFO ("now %" GST_TIME_FORMAT " wanted %" GST_TIME_FORMAT,
      GST_TIME_ARGS (running_time), GST_TIME_ARGS (pending_key_unit_ts));
  if (GST_CLOCK_TIME_IS_VALID (pending_key_unit_ts) &&
      running_time < pending_key_unit_ts)
    goto out;

  if (flags & GST_BUFFER_FLAG_DELTA_UNIT) {
    GST_DEBUG ("pending force key unit, waiting for keyframe");
    goto out;
  }

  stream_time = gst_segment_to_stream_time (segment,
      GST_FORMAT_TIME, timestamp);

  if (!gst_video_event_parse_upstream_force_key_unit (pending_event,
          NULL, &all_headers, &count)) {
    gst_video_event_parse_downstream_force_key_unit (pending_event, NULL,
        NULL, NULL, &all_headers, &count);
  }

  event =
      gst_video_event_new_downstream_force_key_unit (timestamp, stream_time,
      running_time, all_headers, count);
  gst_event_set_seqnum (event, gst_event_get_seqnum (pending_event));

out:
  return event;
}

static void
gst_h26x_base_parse_prepare_key_unit (GstH26XBaseParse * self, GstEvent * event)
{
  GstClockTime running_time;
  guint count;
#ifndef GST_DISABLE_GST_DEBUG
  gboolean have_vps, have_sps, have_pps;
  gint i;
#endif

  self->pending_key_unit_ts = GST_CLOCK_TIME_NONE;
  gst_event_replace (&self->force_key_unit_event, NULL);

  gst_video_event_parse_downstream_force_key_unit (event,
      NULL, NULL, &running_time, NULL, &count);

  GST_INFO_OBJECT (self, "pushing downstream force-key-unit event %d "
      "%" GST_TIME_FORMAT " count %d", gst_event_get_seqnum (event),
      GST_TIME_ARGS (running_time), count);
  gst_pad_push_event (GST_BASE_PARSE_SRC_PAD (self), event);

#ifndef GST_DISABLE_GST_DEBUG
  have_vps = have_sps = have_pps = FALSE;
  for (i = 0; i < self->max_vps_count; i++) {
    if (self->vps_nals[i] != NULL) {
      have_vps = TRUE;
      break;
    }
  }
  for (i = 0; i < self->max_sps_count; i++) {
    if (self->sps_nals[i] != NULL) {
      have_sps = TRUE;
      break;
    }
  }
  for (i = 0; i < self->max_pps_count; i++) {
    if (self->pps_nals[i] != NULL) {
      have_pps = TRUE;
      break;
    }
  }

  if (self->max_vps_count) {
    GST_INFO_OBJECT (self,
        "preparing key unit, have vps %d have sps %d have pps %d", have_vps,
        have_sps, have_pps);
  } else {
    GST_INFO_OBJECT (self, "preparing key unit, have sps %d have pps %d",
        have_sps, have_pps);
  }
#endif

  /* set push_codec to TRUE so that pre_push_frame sends SPS/PPS again */
  self->push_codec = TRUE;
}

static gboolean
gst_h26x_base_parse_handle_vps_sps_pps_nals (GstH26XBaseParse * self,
    GstBuffer * buffer, GstBaseParseFrame * frame)
{
  GstBuffer *codec_nal;
  gint i;
  gboolean send_done = FALSE;
  GstClockTime timestamp = GST_BUFFER_TIMESTAMP (buffer);

  if (self->align == GST_H26X_BASE_PARSE_ALIGN_NAL) {
    /* send separate config NAL buffers */
    GST_DEBUG_OBJECT (self, "- sending %sSPS/PPS",
        self->max_vps_count ? "VPS/" : "");
    for (i = 0; i < self->max_vps_count; i++) {
      if ((codec_nal = self->vps_nals[i])) {
        GST_DEBUG_OBJECT (self, "sending VPS nal");
        gst_h26x_base_parse_push_codec_buffer (self, codec_nal, timestamp);
        send_done = TRUE;
      }
    }
    for (i = 0; i < self->max_sps_count; i++) {
      if ((codec_nal = self->sps_nals[i])) {
        GST_DEBUG_OBJECT (self, "sending SPS nal");
        gst_h26x_base_parse_push_codec_buffer (self, codec_nal, timestamp);
        send_done = TRUE;
      }
    }
    for (i = 0; i < self->max_pps_count; i++) {
      if ((codec_nal = self->pps_nals[i])) {
        GST_DEBUG_OBJECT (self, "sending PPS nal");
        gst_h26x_base_parse_push_codec_buffer (self, codec_nal, timestamp);
        send_done = TRUE;
      }
    }
  } else {
    /* insert config NALs into AU */
    GstByteWriter bw;
    GstBuffer *new_buf;
    const gboolean bs = self->format == GST_H26X_BASE_PARSE_FORMAT_BYTE;
    const gint nls = 4 - self->nal_length_size;
    gboolean ok;

    gst_byte_writer_init_with_size (&bw, gst_buffer_get_size (buffer), FALSE);
    ok = gst_byte_writer_put_buffer (&bw, buffer, 0, self->idr_pos);
    GST_DEBUG_OBJECT (self, "- inserting %sSPS/PPS",
        self->max_vps_count ? "VPS/" : "");
    for (i = 0; i < self->max_vps_count; i++) {
      if ((codec_nal = self->vps_nals[i])) {
        gsize nal_size = gst_buffer_get_size (codec_nal);
        GST_DEBUG_OBJECT (self, "inserting VPS nal");
        if (bs) {
          ok &= gst_byte_writer_put_uint32_be (&bw, 1);
        } else {
          ok &= gst_byte_writer_put_uint32_be (&bw, (nal_size << (nls * 8)));
          ok &= gst_byte_writer_set_pos (&bw,
              gst_byte_writer_get_pos (&bw) - nls);
        }

        ok &= gst_byte_writer_put_buffer (&bw, codec_nal, 0, nal_size);
        send_done = TRUE;
      }
    }
    for (i = 0; i < self->max_sps_count; i++) {
      if ((codec_nal = self->sps_nals[i])) {
        gsize nal_size = gst_buffer_get_size (codec_nal);
        GST_DEBUG_OBJECT (self, "inserting SPS nal");
        if (bs) {
          ok &= gst_byte_writer_put_uint32_be (&bw, 1);
        } else {
          ok &= gst_byte_writer_put_uint32_be (&bw, (nal_size << (nls * 8)));
          ok &= gst_byte_writer_set_pos (&bw,
              gst_byte_writer_get_pos (&bw) - nls);
        }

        ok &= gst_byte_writer_put_buffer (&bw, codec_nal, 0, nal_size);
        send_done = TRUE;
      }
    }
    for (i = 0; i < self->max_pps_count; i++) {
      if ((codec_nal = self->pps_nals[i])) {
        gsize nal_size = gst_buffer_get_size (codec_nal);
        GST_DEBUG_OBJECT (self, "inserting PPS nal");
        if (bs) {
          ok &= gst_byte_writer_put_uint32_be (&bw, 1);
        } else {
          ok &= gst_byte_writer_put_uint32_be (&bw, (nal_size << (nls * 8)));
          ok &= gst_byte_writer_set_pos (&bw,
              gst_byte_writer_get_pos (&bw) - nls);
        }
        ok &= gst_byte_writer_put_buffer (&bw, codec_nal, 0, nal_size);
        send_done = TRUE;
      }
    }
    ok &= gst_byte_writer_put_buffer (&bw, buffer, self->idr_pos, -1);
    /* collect result and push */
    new_buf = gst_byte_writer_reset_and_get_buffer (&bw);
    gst_buffer_copy_into (new_buf, buffer, GST_BUFFER_COPY_METADATA, 0, -1);
    /* should already be keyframe/IDR, but it may not have been,
     * so mark it as such to avoid being discarded by picky decoder */
    GST_BUFFER_FLAG_UNSET (new_buf, GST_BUFFER_FLAG_DELTA_UNIT);
    gst_buffer_replace (&frame->out_buffer, new_buf);
    gst_buffer_unref (new_buf);
    /* some result checking seems to make some compilers happy */
    if (G_UNLIKELY (!ok)) {
      GST_ERROR_OBJECT (self, "failed to insert %sSPS/PPS",
          self->max_vps_count ? "VPS/" : "");
    }
  }

  return send_done;
}

static GstFlowReturn
gst_h26x_base_parse_pre_push_frame (GstBaseParse * parse,
    GstBaseParseFrame * frame)
{
  GstH26XBaseParse *self;
  GstBuffer *buffer;
  GstEvent *event;
  GstH26XBaseParseClass *klass;

  self = GST_H26X_BASE_PARSE (parse);
  klass = GST_H26X_BASE_PARSE_GET_CLASS (self);

  if (!self->sent_codec_tag) {
    GstTagList *taglist;
    GstCaps *caps;

    /* codec tag */
    caps = gst_pad_get_current_caps (GST_BASE_PARSE_SRC_PAD (parse));
    if (caps == NULL) {
      if (GST_PAD_IS_FLUSHING (GST_BASE_PARSE_SRC_PAD (self))) {
        GST_INFO_OBJECT (self, "Src pad is flushing");
        return GST_FLOW_FLUSHING;
      } else {
        GST_INFO_OBJECT (self, "Src pad is not negotiated!");
        return GST_FLOW_NOT_NEGOTIATED;
      }
    }

    taglist = gst_tag_list_new_empty ();
    gst_pb_utils_add_codec_description_to_tag_list (taglist,
        GST_TAG_VIDEO_CODEC, caps);
    gst_caps_unref (caps);

    gst_base_parse_merge_tags (parse, taglist, GST_TAG_MERGE_REPLACE);
    gst_tag_list_unref (taglist);

    /* also signals the end of first-frame processing */
    self->sent_codec_tag = TRUE;
  }

  if (klass->prepare_pre_push_frame)
    buffer = klass->prepare_pre_push_frame (self, frame);
  else
    buffer = frame->buffer;

  if ((event = check_pending_key_unit_event (self->force_key_unit_event,
              &parse->segment, GST_BUFFER_TIMESTAMP (buffer),
              GST_BUFFER_FLAGS (buffer), self->pending_key_unit_ts))) {
    gst_h26x_base_parse_prepare_key_unit (self, event);
  }

  /* periodic SPS/PPS sending */
  if (self->interval > 0 || self->push_codec) {
    GstClockTime timestamp = GST_BUFFER_TIMESTAMP (buffer);
    guint64 diff;
    gboolean initial_frame = FALSE;

    /* init */
    if (!GST_CLOCK_TIME_IS_VALID (self->last_report)) {
      self->last_report = timestamp;
      initial_frame = TRUE;
    }

    if (self->idr_pos >= 0) {
      GST_LOG_OBJECT (self, "IDR nal at offset %d", self->idr_pos);

      if (timestamp > self->last_report)
        diff = timestamp - self->last_report;
      else
        diff = 0;

      GST_LOG_OBJECT (self,
          "now %" GST_TIME_FORMAT ", last SPS/PPS %" GST_TIME_FORMAT,
          GST_TIME_ARGS (timestamp), GST_TIME_ARGS (self->last_report));

      GST_DEBUG_OBJECT (self,
          "interval since last SPS/PPS %" GST_TIME_FORMAT,
          GST_TIME_ARGS (diff));

      if (GST_TIME_AS_SECONDS (diff) >= self->interval ||
          initial_frame || self->push_codec) {
        GstClockTime new_ts;

        /* avoid overwriting a perfectly fine timestamp */
        new_ts = GST_CLOCK_TIME_IS_VALID (timestamp) ? timestamp :
            self->last_report;

        if (gst_h26x_base_parse_handle_vps_sps_pps_nals (self, buffer, frame)) {
          self->last_report = new_ts;
        }
      }
      /* we pushed whatever we had */
      self->push_codec = FALSE;
      self->have_vps = FALSE;
      self->have_sps = FALSE;
      self->have_pps = FALSE;
      self->state &= GST_H26X_BASE_PARSE_STATE_VALID_PICTURE_HEADERS;
    }
  } else if (self->interval == -1) {
    if (self->idr_pos >= 0) {
      GST_LOG_OBJECT (self, "IDR nal at offset %d", self->idr_pos);

      gst_h26x_base_parse_handle_vps_sps_pps_nals (self, buffer, frame);

      /* we pushed whatever we had */
      self->push_codec = FALSE;
      self->have_vps = FALSE;
      self->have_sps = FALSE;
      self->have_pps = FALSE;
      self->state &= GST_H26X_BASE_PARSE_STATE_VALID_PICTURE_HEADERS;
    }
  }

  /* Fixme: setting passthrough mode casuing multiple issues:
   * For nal aligned multiresoluton streams, passthrough mode make self
   * unable to advertise the new resoultions. Also causing issues while
   * parsing MVC streams when it has two layers.
   * Disabing passthourgh mode for now */
#if 0
  /* If SPS/PPS and a keyframe have been parsed, and we're not converting,
   * we might switch to passthrough mode now on the basis that we've seen
   * the SEI packets and know optional caps params (such as multiview).
   * This is an efficiency optimisation that relies on stream properties
   * remaining uniform in practice. */
  if (self->can_passthrough) {
    if (self->keyframe && self->have_sps && self->have_pps) {
      GST_LOG_OBJECT (parse, "Switching to passthrough mode");
      gst_base_parse_set_passthrough (parse, TRUE);
    }
  }
#endif

  gst_h26x_base_parse_reset_frame (self);

  return GST_FLOW_OK;
}

static gboolean
gst_h26x_base_parse_set_caps (GstBaseParse * parse, GstCaps * caps)
{
  GstH26XBaseParse *self;
  GstStructure *str;
  const GValue *codec_data_value;
  GstBuffer *codec_data = NULL;
  guint format, align;
  GstCaps *old_caps;
  GstH26XBaseParseClass *klass;

  self = GST_H26X_BASE_PARSE (parse);
  klass = GST_H26X_BASE_PARSE_GET_CLASS (self);

  /* reset */
  self->push_codec = FALSE;

  old_caps = gst_pad_get_current_caps (GST_BASE_PARSE_SINK_PAD (parse));
  if (old_caps) {
    if (!gst_caps_is_equal (old_caps, caps))
      gst_h26x_base_parse_reset_stream_info (self);
    gst_caps_unref (old_caps);
  }

  str = gst_caps_get_structure (caps, 0);

  /* accept upstream info if provided */
  gst_structure_get_int (str, "width", &self->width);
  gst_structure_get_int (str, "height", &self->height);
  gst_structure_get_fraction (str, "framerate", &self->fps_num, &self->fps_den);
  gst_structure_get_fraction (str, "pixel-aspect-ratio",
      &self->upstream_par_n, &self->upstream_par_d);

  /* get upstream format and align from caps */
  gst_h26x_base_parse_format_from_caps (self, caps, &format, &align);

  codec_data_value = gst_structure_get_value (str, "codec_data");

  if (klass->fixate_format) {
    if (!klass->fixate_format (self, &format, &align, codec_data_value))
      goto refuse_caps;
  } else {
    GST_FIXME_OBJECT (self, "Implement fixate format");
  }

  /* packetized video has codec_data (required for AVC, optional for AVC3) */
  if (codec_data_value != NULL) {
    GstMapInfo map;
    gboolean ret;

    GST_DEBUG_OBJECT (self, "have packetized h264");
    /* make note for optional split processing */
    self->packetized = TRUE;

    /* codec_data field should hold a buffer */
    if (!GST_VALUE_HOLDS_BUFFER (codec_data_value))
      goto packetized_caps_codec_data_wrong_type;

    codec_data = gst_value_get_buffer (codec_data_value);
    if (!codec_data)
      goto packetized_caps_codec_data_missing;

    gst_buffer_map (codec_data, &map, GST_MAP_READ);
    ret = klass->handle_codec_data (self, &map);
    gst_buffer_unmap (codec_data, &map);

    if (!ret)
      goto refuse_caps;

    gst_buffer_replace (&self->codec_data_in, codec_data);
  } else if (format == GST_H26X_BASE_PARSE_FORMAT_BYTE) {
    GST_DEBUG_OBJECT (self, "have bytestream h264");
    /* nothing to pre-process */
    self->packetized = FALSE;
    /* we have 4 sync bytes */
    self->nal_length_size = 4;
  } else {
    /* probably AVC3 without codec_data field, anything to do here? */
  }

  {
    GstCaps *in_caps;

    /* prefer input type determined above */
    in_caps = klass->get_default_caps (self);
    gst_caps_set_simple (in_caps,
        "parsed", G_TYPE_BOOLEAN, TRUE,
        "stream-format", G_TYPE_STRING,
        gst_h26x_base_parse_get_string (self, TRUE, format),
        "alignment", G_TYPE_STRING,
        gst_h26x_base_parse_get_string (self, FALSE, align), NULL);
    /* negotiate with downstream, sets ->format and ->align */
    gst_h26x_base_parse_negotiate (self, format, in_caps);
    gst_caps_unref (in_caps);
  }

  if (format == self->format && align == self->align) {
    /* we did parse codec-data and might supplement src caps */
    gst_h26x_base_parse_update_src_caps (self, caps);
  } else if (format != GST_H26X_BASE_PARSE_FORMAT_BYTE) {
    /* if input != output, and input is avc, must split before anything else */
    /* arrange to insert codec-data in-stream if needed.
     * src caps are only arranged for later on */
    self->push_codec = TRUE;
    self->have_sps = FALSE;
    self->have_pps = FALSE;
    if (self->align == GST_H26X_BASE_PARSE_ALIGN_NAL)
      self->split_packetized = TRUE;
    self->packetized = TRUE;
  }

  self->in_align = align;

  return TRUE;

  /* ERRORS */
packetized_caps_codec_data_wrong_type:
  {
    GST_WARNING_OBJECT (parse, "H.264 AVC caps, codec_data field not a buffer");
    goto refuse_caps;
  }
packetized_caps_codec_data_missing:
  {
    GST_WARNING_OBJECT (parse, "H.264 AVC caps, but no codec_data");
    goto refuse_caps;
  }
refuse_caps:
  {
    GST_WARNING_OBJECT (self, "refused caps %" GST_PTR_FORMAT, caps);
    return FALSE;
  }
}

static void
remove_fields (GstCaps * caps, gboolean all)
{
  guint i, n;

  n = gst_caps_get_size (caps);
  for (i = 0; i < n; i++) {
    GstStructure *s = gst_caps_get_structure (caps, i);

    if (all) {
      gst_structure_remove_field (s, "alignment");
      gst_structure_remove_field (s, "stream-format");
    }
    gst_structure_remove_field (s, "parsed");
  }
}

static GstCaps *
gst_h26x_base_parse_get_caps (GstBaseParse * parse, GstCaps * filter)
{
  GstCaps *peercaps, *templ;
  GstCaps *res, *tmp, *pcopy;

  templ = gst_pad_get_pad_template_caps (GST_BASE_PARSE_SINK_PAD (parse));
  if (filter) {
    GstCaps *fcopy = gst_caps_copy (filter);
    /* Remove the fields we convert */
    remove_fields (fcopy, TRUE);
    peercaps = gst_pad_peer_query_caps (GST_BASE_PARSE_SRC_PAD (parse), fcopy);
    gst_caps_unref (fcopy);
  } else
    peercaps = gst_pad_peer_query_caps (GST_BASE_PARSE_SRC_PAD (parse), NULL);

  pcopy = gst_caps_copy (peercaps);
  remove_fields (pcopy, TRUE);

  res = gst_caps_intersect_full (pcopy, templ, GST_CAPS_INTERSECT_FIRST);
  gst_caps_unref (pcopy);
  gst_caps_unref (templ);

  if (filter) {
    GstCaps *tmp = gst_caps_intersect_full (res, filter,
        GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref (res);
    res = tmp;
  }

  /* Try if we can put the downstream caps first */
  pcopy = gst_caps_copy (peercaps);
  remove_fields (pcopy, FALSE);
  tmp = gst_caps_intersect_full (pcopy, res, GST_CAPS_INTERSECT_FIRST);
  gst_caps_unref (pcopy);
  if (!gst_caps_is_empty (tmp))
    res = gst_caps_merge (tmp, res);
  else
    gst_caps_unref (tmp);

  gst_caps_unref (peercaps);
  return res;
}

static gboolean
gst_h26x_base_parse_sink_event (GstBaseParse * parse, GstEvent * event)
{
  gboolean res;
  GstH26XBaseParse *self = GST_H26X_BASE_PARSE (parse);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CUSTOM_DOWNSTREAM:
    {
      GstClockTime timestamp, stream_time, running_time;
      gboolean all_headers;
      guint count;

      if (gst_video_event_is_force_key_unit (event)) {
        gst_video_event_parse_downstream_force_key_unit (event,
            &timestamp, &stream_time, &running_time, &all_headers, &count);

        GST_INFO_OBJECT (self,
            "received downstream force key unit event, "
            "seqnum %d running_time %" GST_TIME_FORMAT
            " all_headers %d count %d", gst_event_get_seqnum (event),
            GST_TIME_ARGS (running_time), all_headers, count);
        if (self->force_key_unit_event) {
          GST_INFO_OBJECT (self, "ignoring force key unit event "
              "as one is already queued");
        } else {
          self->pending_key_unit_ts = running_time;
          gst_event_replace (&self->force_key_unit_event, event);
        }
        gst_event_unref (event);
        res = TRUE;
      } else {
        res = GST_BASE_PARSE_CLASS (parent_class)->sink_event (parse, event);
        break;
      }
      break;
    }
    case GST_EVENT_FLUSH_STOP:
      self->dts = GST_CLOCK_TIME_NONE;
      self->ts_trn_nb = GST_CLOCK_TIME_NONE;
      self->push_codec = TRUE;

      res = GST_BASE_PARSE_CLASS (parent_class)->sink_event (parse, event);
      break;
    case GST_EVENT_SEGMENT:
    {
      const GstSegment *segment;

      gst_event_parse_segment (event, &segment);
      /* don't try to mess with more subtle cases (e.g. seek) */
      if (segment->format == GST_FORMAT_TIME &&
          (segment->start != 0 || segment->rate != 1.0
              || segment->applied_rate != 1.0))
        self->do_ts = FALSE;

      self->last_report = GST_CLOCK_TIME_NONE;

      res = GST_BASE_PARSE_CLASS (parent_class)->sink_event (parse, event);
      break;
    }
    default:
      res = GST_BASE_PARSE_CLASS (parent_class)->sink_event (parse, event);
      break;
  }
  return res;
}

static gboolean
gst_h26x_base_parse_src_event (GstBaseParse * parse, GstEvent * event)
{
  gboolean res;
  GstH26XBaseParse *self = GST_H26X_BASE_PARSE (parse);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CUSTOM_UPSTREAM:
    {
      GstClockTime running_time;
      gboolean all_headers;
      guint count;

      if (gst_video_event_is_force_key_unit (event)) {
        gst_video_event_parse_upstream_force_key_unit (event,
            &running_time, &all_headers, &count);

        GST_INFO_OBJECT (self, "received upstream force-key-unit event, "
            "seqnum %d running_time %" GST_TIME_FORMAT
            " all_headers %d count %d", gst_event_get_seqnum (event),
            GST_TIME_ARGS (running_time), all_headers, count);

        if (all_headers) {
          self->pending_key_unit_ts = running_time;
          gst_event_replace (&self->force_key_unit_event, event);
        }
      }
      res = GST_BASE_PARSE_CLASS (parent_class)->src_event (parse, event);
      break;
    }
    default:
      res = GST_BASE_PARSE_CLASS (parent_class)->src_event (parse, event);
      break;
  }

  return res;
}

static void
gst_h26x_base_parse_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstH26XBaseParse *parse;

  parse = GST_H26X_BASE_PARSE (object);

  switch (prop_id) {
    case PROP_CONFIG_INTERVAL:
      parse->interval = g_value_get_int (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_h26x_base_parse_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstH26XBaseParse *parse;

  parse = GST_H26X_BASE_PARSE (object);

  switch (prop_id) {
    case PROP_CONFIG_INTERVAL:
      g_value_set_int (value, parse->interval);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}
