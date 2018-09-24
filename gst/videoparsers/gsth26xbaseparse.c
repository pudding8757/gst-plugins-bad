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

#define DEFAULT_CONFIG_INTERVAL 0

enum
{
  PROP_0,
  PROP_CONFIG_INTERVAL
};

struct _GstH26XBaseParsePrivate
{
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
  gboolean packetized;
  gboolean split_packetized;
  gboolean transform;

  /* state */
  guint state;
  GstH26XBaseParseAlign in_align;
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

  /* props */
  gint interval;

  GstClockTime pending_key_unit_ts;
  GstEvent *force_key_unit_event;

  gboolean discont;

  /* frame parsing */
  gint idr_pos, sei_pos;
  gboolean update_caps;
  GstAdapter *frame_out;
  gboolean keyframe;
  gboolean header;
  gboolean frame_start;

  gboolean do_ts;

  /* For insertion of AU Delimiter */
  gboolean aud_needed;
  gboolean aud_insert;
};

#define GST_H26X_BASE_PARSE_GET_PRIVATE(obj)  \
    (G_TYPE_INSTANCE_GET_PRIVATE ((obj), GST_TYPE_H26X_BASE_PARSE, GstH26XBaseParsePrivate))

#define parent_class gst_h26x_base_parse_parent_class
G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE (GstH26XBaseParse, gst_h26x_base_parse,
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

static gboolean
gst_h26x_base_parse_allow_passthrough_default (GstH26XBaseParse * parse);

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

  klass->allow_passthrough =
      GST_DEBUG_FUNCPTR (gst_h26x_base_parse_allow_passthrough_default);
}

static void
gst_h26x_base_parse_init (GstH26XBaseParse * self)
{
  GstH26XBaseParsePrivate *priv = GST_H26X_BASE_PARSE_GET_PRIVATE (self);
  priv->frame_out = gst_adapter_new ();
  gst_base_parse_set_pts_interpolation (GST_BASE_PARSE (self), FALSE);
  GST_PAD_SET_ACCEPT_INTERSECT (GST_BASE_PARSE_SINK_PAD (self));
  GST_PAD_SET_ACCEPT_TEMPLATE (GST_BASE_PARSE_SINK_PAD (self));

  self->priv = priv;
  priv->aud_needed = TRUE;
  priv->aud_insert = TRUE;
}

static void
gst_h26x_base_parse_finalize (GObject * object)
{
  GstH26XBaseParse *self = GST_H26X_BASE_PARSE (object);
  GstH26XBaseParsePrivate *priv = self->priv;

  g_object_unref (priv->frame_out);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_h26x_base_parse_reset_frame (GstH26XBaseParse * self)
{
  GstH26XBaseParsePrivate *priv = self->priv;

  GST_DEBUG_OBJECT (self, "reset frame");

  /* done parsing; reset state */
  priv->current_off = -1;

  self->picture_start = FALSE;
  priv->update_caps = FALSE;
  priv->idr_pos = -1;
  priv->sei_pos = -1;
  priv->keyframe = FALSE;
  priv->header = FALSE;
  priv->frame_start = FALSE;
  priv->aud_insert = TRUE;
  gst_adapter_clear (priv->frame_out);
}

static void
gst_h26x_base_parse_reset_stream_info (GstH26XBaseParse * self)
{
  GstH26XBaseParsePrivate *priv = self->priv;
  gint i;

  priv->width = 0;
  priv->height = 0;
  priv->fps_num = 0;
  priv->fps_den = 0;
  priv->upstream_par_n = -1;
  priv->upstream_par_d = -1;
  priv->parsed_par_n = 0;
  priv->parsed_par_d = 0;
  priv->have_vps = FALSE;
  priv->have_sps = FALSE;
  priv->have_pps = FALSE;

  self->multiview_mode = GST_VIDEO_MULTIVIEW_MODE_NONE;
  self->multiview_flags = GST_VIDEO_MULTIVIEW_FLAGS_NONE;

  self->align = GST_H26X_BASE_PARSE_ALIGN_NONE;
  self->format = GST_H26X_BASE_PARSE_FORMAT_NONE;
  self->nal_length_size = 4;

  priv->transform = FALSE;
  priv->packetized = FALSE;
  priv->push_codec = FALSE;

  gst_buffer_replace (&priv->codec_data, NULL);
  gst_buffer_replace (&priv->codec_data_in, NULL);

  gst_h26x_base_parse_reset_frame (self);

  for (i = 0; i < priv->max_vps_count; i++)
    gst_buffer_replace (&self->vps_nals[i], NULL);
  for (i = 0; i < priv->max_sps_count; i++)
    gst_buffer_replace (&self->sps_nals[i], NULL);
  for (i = 0; i < priv->max_pps_count; i++)
    gst_buffer_replace (&self->pps_nals[i], NULL);
}

static void
gst_h26x_base_parse_reset (GstH26XBaseParse * self)
{
  GstH26XBaseParsePrivate *priv = self->priv;

  priv->last_report = GST_CLOCK_TIME_NONE;

  priv->do_ts = TRUE;

  priv->sent_codec_tag = FALSE;

  priv->pending_key_unit_ts = GST_CLOCK_TIME_NONE;
  gst_event_replace (&priv->force_key_unit_event, NULL);

  priv->discont = FALSE;

  gst_h26x_base_parse_reset_stream_info (self);
}

static gboolean
gst_h26x_base_parse_start (GstBaseParse * parse)
{
  GstH26XBaseParse *self = GST_H26X_BASE_PARSE (parse);
  GstH26XBaseParseClass *klass = GST_H26X_BASE_PARSE_GET_CLASS (self);
  GstH26XBaseParsePrivate *priv = self->priv;
  guint max_vps_count, max_sps_count, max_pps_count;

  GST_DEBUG_OBJECT (parse, "start");

  klass->get_max_vps_sps_pps_count (self,
      &max_vps_count, &max_sps_count, &max_pps_count);
  priv->min_nalu_size = klass->get_min_nalu_size (self);

  if (max_vps_count)
    self->vps_nals = g_new0 (GstBuffer *, max_vps_count);
  self->sps_nals = g_new0 (GstBuffer *, max_sps_count);
  self->pps_nals = g_new0 (GstBuffer *, max_pps_count);

  priv->max_vps_count = max_vps_count;
  priv->max_sps_count = max_sps_count;
  priv->max_pps_count = max_pps_count;

  priv->state = GST_H26X_BASE_PARSE_STATE_INIT;
  gst_h26x_base_parse_reset (self);

  gst_base_parse_set_min_frame_size (parse, priv->min_nalu_size + 1);

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
  GstH26XBaseParsePrivate *priv = self->priv;

  g_return_if_fail ((in_caps == NULL) || gst_caps_is_fixed (in_caps));

  caps = gst_pad_get_allowed_caps (GST_BASE_PARSE_SRC_PAD (self));
  GST_DEBUG_OBJECT (self, "allowed caps: %" GST_PTR_FORMAT, caps);

  /* concentrate on leading structure, since decodebin parser
   * capsfilter always includes parser template caps */
  if (caps) {
    caps = gst_caps_truncate (caps);
    GST_DEBUG_OBJECT (self, "negotiating with caps: %" GST_PTR_FORMAT, caps);
  }

  priv->can_passthrough = FALSE;

  if (in_caps && caps) {
    if (gst_caps_can_intersect (in_caps, caps)) {
      GST_DEBUG_OBJECT (self, "downstream accepts upstream caps");
      gst_h26x_base_parse_format_from_caps (self, in_caps, &format, &align);
      gst_caps_unref (caps);
      caps = NULL;
      priv->can_passthrough = TRUE;
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

  priv->transform = in_format != self->format ||
      align == GST_H26X_BASE_PARSE_ALIGN_AU;

  if (caps)
    gst_caps_unref (caps);
}

void
gst_h26x_base_parse_clear_state (GstH26XBaseParse * self,
    GstH26XBaseParseState at_most)
{
  GstH26XBaseParsePrivate *priv = self->priv;

  priv->state &= at_most;
}

gboolean
gst_h26x_base_parse_is_valid_state (GstH26XBaseParse * self,
    GstH26XBaseParseState expected)
{
  GstH26XBaseParsePrivate *priv = self->priv;

  return ((priv->state & expected) == expected);
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
gst_h26x_base_parse_vps_parsed (GstH26XBaseParse * self)
{
  GstH26XBaseParsePrivate *priv = self->priv;

  GST_DEBUG_OBJECT (self, "VPS parsed, triggering src caps check");
  priv->update_caps = TRUE;
  priv->have_vps = TRUE;
  if (priv->push_codec && priv->have_pps) {
    /* VPS/SPS/PPS found in stream before the first pre_push_frame, no need
     * to forcibly push at start */
    GST_INFO_OBJECT (self, "have VPS/SPS/PPS in stream");
    priv->push_codec = FALSE;
    priv->have_vps = FALSE;
    priv->have_sps = FALSE;
    priv->have_pps = FALSE;
  }

  priv->header = TRUE;
}

void
gst_h26x_base_parse_sps_parsed (GstH26XBaseParse * self)
{
  GstH26XBaseParsePrivate *priv = self->priv;

  GST_DEBUG_OBJECT (self, "SPS parsed, triggering src caps check");
  priv->update_caps = TRUE;
  priv->have_sps = TRUE;
  if (priv->push_codec && priv->have_pps) {
    /* SPS and PPS found in stream before the first pre_push_frame, no need
     * to forcibly push at start */
    GST_INFO_OBJECT (self, "have SPS/PPS in stream");
    priv->push_codec = FALSE;
    priv->have_sps = FALSE;
    priv->have_pps = FALSE;
  }

  priv->state |= GST_H26X_BASE_PARSE_STATE_GOT_SPS;
  priv->header = TRUE;
}

void
gst_h26x_base_parse_pps_parsed (GstH26XBaseParse * self)
{
  GstH26XBaseParsePrivate *priv = self->priv;

  /* parameters might have changed, force caps check */
  if (!priv->have_pps) {
    GST_DEBUG_OBJECT (self, "PPS parsed, triggering src caps check");
    priv->update_caps = TRUE;
  }
  priv->have_pps = TRUE;
  if (priv->push_codec && priv->have_sps) {
    /* SPS and PPS found in stream before the first pre_push_frame, no need
     * to forcibly push at start */
    GST_INFO_OBJECT (self, "have SPS/PPS in stream");
    priv->push_codec = FALSE;
    priv->have_sps = FALSE;
    priv->have_pps = FALSE;
  }

  priv->state |= GST_H26X_BASE_PARSE_STATE_GOT_PPS;
  priv->header = TRUE;
}

void
gst_h26x_base_parse_sei_parsed (GstH26XBaseParse * self, guint nalu_offset)
{
  GstH26XBaseParsePrivate *priv = self->priv;

  /* mark SEI pos */
  if (priv->sei_pos == -1) {
    if (priv->transform)
      priv->sei_pos = gst_adapter_available (priv->frame_out);
    else
      priv->sei_pos = nalu_offset;
    GST_DEBUG_OBJECT (self, "marking SEI in frame at offset %d", priv->sei_pos);
  }
  priv->header = TRUE;
}

void
gst_h26x_base_parse_aud_parsed (GstH26XBaseParse * self)
{
  GstH26XBaseParsePrivate *priv = self->priv;
  priv->aud_insert = FALSE;
}

void
gst_h26x_base_parse_frame_started (GstH26XBaseParse * self)
{
  GstH26XBaseParsePrivate *priv = self->priv;

  GST_DEBUG_OBJECT (self, "frame start");
  priv->frame_start = TRUE;
}

void
gst_h26x_base_parse_slice_hdr_parsed (GstH26XBaseParse * self,
    gboolean keyframe)
{
  GstH26XBaseParsePrivate *priv = self->priv;

  if (keyframe)
    priv->keyframe = TRUE;
  priv->state |= GST_H26X_BASE_PARSE_STATE_GOT_SLICE;
}

void
gst_h26x_base_parse_update_idr_pos (GstH26XBaseParse * self, guint nalu_offset,
    gboolean is_idr)
{
  GstH26XBaseParsePrivate *priv = self->priv;

  if (!is_idr && !priv->push_codec)
    return;

  /* if we need to sneak codec NALs into the stream,
   * this is a good place, so fake it as IDR
   * (which should be at start anyway) */
  /* mark where config needs to go if interval expired */
  /* mind replacement buffer if applicable */
  if (priv->idr_pos == -1) {
    if (priv->transform)
      priv->idr_pos = gst_adapter_available (priv->frame_out);
    else
      priv->idr_pos = nalu_offset;
    GST_DEBUG_OBJECT (self, "marking IDR in frame at offset %d", priv->idr_pos);
  }
  /* if SEI preceeds (faked) IDR, then we have to insert config there */
  if (priv->sei_pos >= 0 && priv->idr_pos > priv->sei_pos) {
    priv->idr_pos = priv->sei_pos;
    GST_DEBUG_OBJECT (self, "moved IDR mark to SEI position %d", priv->idr_pos);
  }
}

void
gst_h26x_base_pares_finish_process_nal (GstH26XBaseParse * self, guint8 * data,
    guint size)
{
  GstH26XBaseParsePrivate *priv = self->priv;

  /* if packetized format output needed, collect properly prefixed nal in adapter,
   * and use that to replace outgoing buffer data later on */
  if (priv->transform) {
    GstBuffer *buf;

    GST_LOG_OBJECT (self, "collecting NAL in frame");
    buf = gst_h26x_base_parse_wrap_nal (self, self->format, data, size);
    gst_adapter_push (priv->frame_out, buf);
  }
}

void
gst_h26x_base_parse_store_header_nal (GstH26XBaseParse * parse, guint id,
    GstH26XBaseParseStoreNalType naltype, const guint8 * data, guint size)
{
  GstBuffer *buf, **store;
  guint store_size;
  GstH26XBaseParsePrivate *priv = parse->priv;

  switch (naltype) {
    case GST_H26X_BASE_PARSE_STORE_NAL_TYPE_VPS:
      GST_DEBUG_OBJECT (parse, "storing vps %u", id);
      store_size = priv->max_vps_count;
      store = parse->vps_nals;
      break;
    case GST_H26X_BASE_PARSE_STORE_NAL_TYPE_SPS:
      GST_DEBUG_OBJECT (parse, "storing sps %u", id);
      store_size = priv->max_sps_count;
      store = parse->sps_nals;
      break;
    case GST_H26X_BASE_PARSE_STORE_NAL_TYPE_PPS:
      GST_DEBUG_OBJECT (parse, "storing pps %u", id);
      store_size = priv->max_pps_count;
      store = parse->pps_nals;
      break;
    default:
      return;
  }

  if (id >= store_size) {
    GST_DEBUG_OBJECT (parse, "unable to store nal, id out-of-range %d", id);
    return;
  }

  buf = gst_buffer_new_allocate (NULL, size, NULL);
  gst_buffer_fill (buf, 0, data, size);

  /* Indicate that buffer contain a header needed for decoding */
  GST_BUFFER_FLAG_SET (buf, GST_BUFFER_FLAG_HEADER);

  if (store[id])
    gst_buffer_unref (store[id]);

  store[id] = buf;
}

static GstFlowReturn
gst_h26x_base_parse_handle_frame (GstBaseParse * parse,
    GstBaseParseFrame * frame, gint * skipsize)
{
  GstH26XBaseParse *self = GST_H26X_BASE_PARSE (parse);
  GstH26XBaseParseClass *klass = GST_H26X_BASE_PARSE_GET_CLASS (self);
  GstH26XBaseParsePrivate *priv = self->priv;
  GstBuffer *buffer = frame->buffer;
  GstMapInfo map;
  gsize size;
  gint current_off = 0;
  gboolean drain;
  gint framesize;
  GstFlowReturn ret;
  gboolean au_complete = FALSE;
  GstH26XBaseParseHandleFrameReturn handle_ret;

  if (G_UNLIKELY (GST_BUFFER_FLAG_IS_SET (frame->buffer,
              GST_BUFFER_FLAG_DISCONT))) {
    priv->discont = TRUE;
  }

  /* delegate in packetized case, no skipping should be needed */
  if (priv->packetized) {
    guint nl = self->nal_length_size;

    if (G_UNLIKELY (nl < 1 || nl > 4)) {
      GST_DEBUG_OBJECT (self, "insufficient data to split input");
      return GST_FLOW_NOT_NEGOTIATED;
    }

    return klass->handle_frame_packetized (self, frame, priv->split_packetized);
  }

  gst_buffer_map (buffer, &map, GST_MAP_READ);
  size = map.size;

  if (G_UNLIKELY (size < priv->min_nalu_size)) {
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
      || priv->in_align == GST_H26X_BASE_PARSE_ALIGN_AU;

  current_off = priv->current_off;
  if (current_off < 0)
    current_off = 0;
  g_assert (current_off < size);
  GST_DEBUG_OBJECT (self, "last parse position %d", current_off);

  /* check for initial skip */
  if (priv->current_off == -1) {
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

  /* Judge whether or not to insert AU Delimiter in case of byte-stream
   * If we're in the middle of au, we don't need to insert aud.
   * Otherwise, we honor the result in gst_h264_parse_process_nal.
   * Note that this should be done until draining if it's happening.
   */
  if (self->align == GST_H26X_BASE_PARSE_ALIGN_NAL && !priv->aud_needed)
    priv->aud_insert = FALSE;

  handle_ret = klass->handle_frame_bytestream (self, skipsize, &framesize,
      &current_off, &au_complete, &map, drain);

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

  /* we don't need aud per nal if it's incomplete au */
  if (self->align == GST_H26X_BASE_PARSE_ALIGN_NAL) {
    priv->aud_needed = au_complete;
  }

  gst_buffer_unmap (buffer, &map);

  gst_h26x_base_parse_parse_frame (self, frame);

  return gst_base_parse_finish_frame (parse, frame, framesize);

more:
  *skipsize = 0;

  /* Restart parsing from here next time */
  if (current_off > 0)
    priv->current_off = current_off;

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
      !(priv->state & GST_H26X_BASE_PARSE_STATE_VALID_PICTURE_HEADERS) ||
      (priv->state & GST_H26X_BASE_PARSE_STATE_GOT_SLICE))
    gst_h26x_base_parse_reset_frame (self);
  goto out;

invalid_stream:
  gst_buffer_unmap (buffer, &map);
  return GST_FLOW_ERROR;
}

static void
gst_h26x_base_parse_get_par (GstH26XBaseParse * self, gint * num, gint * den)
{
  GstH26XBaseParsePrivate *priv = self->priv;

  if (priv->upstream_par_n != -1 && priv->upstream_par_d != -1) {
    *num = priv->upstream_par_n;
    *den = priv->upstream_par_d;
  } else {
    *num = priv->parsed_par_n;
    *den = priv->parsed_par_d;
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
    GstCaps *filter_caps = klass->new_empty_caps (self);

    if (peer_caps)
      gst_caps_unref (peer_caps);
    peer_caps =
        gst_pad_peer_query_caps (GST_BASE_PARSE_SRC_PAD (self), filter_caps);

    gst_caps_unref (filter_caps);
  }

  if (peer_caps && !gst_caps_can_intersect (caps, peer_caps)) {
    GstStructure *structure;

    compat_caps = klass->get_compatible_profile_caps (self);
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
  GstH26XBaseParseSPSInfo info = { 0 };
  gboolean have_sps;
  GstH26XBaseParsePrivate *priv = self->priv;

  if (G_UNLIKELY (!gst_pad_has_current_caps (GST_BASE_PARSE_SRC_PAD (self))))
    modified = TRUE;
  else if (G_UNLIKELY (!priv->update_caps))
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
    sink_caps = klass->new_empty_caps (self);
  else
    s = gst_caps_get_structure (sink_caps, 0);

  have_sps = klass->fill_sps_info (self, &info);
  GST_DEBUG_OBJECT (self, "have sps: %d", have_sps);

  /* only codec-data for nice-and-clean au aligned packetized avc format */
  if ((self->format != GST_H26X_BASE_PARSE_FORMAT_BYTE)
      && self->align == GST_H26X_BASE_PARSE_ALIGN_AU) {
    buf = klass->make_codec_data (self);
    if (buf && priv->codec_data) {
      GstMapInfo map;

      gst_buffer_map (buf, &map, GST_MAP_READ);
      if (map.size != gst_buffer_get_size (priv->codec_data) ||
          gst_buffer_memcmp (priv->codec_data, 0, map.data, map.size))
        modified = TRUE;

      gst_buffer_unmap (buf, &map);
    } else {
      if (!buf && priv->codec_data_in)
        buf = gst_buffer_ref (priv->codec_data_in);
      modified = TRUE;
    }
  }

  caps = NULL;
  if (G_UNLIKELY (!have_sps)) {
    caps = gst_caps_copy (sink_caps);
  } else {
    if (G_UNLIKELY (priv->width != info.width || priv->height != info.height)) {
      GST_INFO_OBJECT (self, "resolution changed %dx%d",
          info.width, info.height);
      priv->width = info.width;
      priv->height = info.height;
      modified = TRUE;
    }

    if (info.fps_num > 0 && info.fps_den > 0) {
      if (G_UNLIKELY (priv->fps_num != info.fps_num
              || priv->fps_den != info.fps_den)) {
        GST_DEBUG_OBJECT (self, "framerate changed %d/%d", info.fps_num,
            info.fps_den);
        priv->fps_num = info.fps_num;
        priv->fps_den = info.fps_den;
        modified = TRUE;
      }
    }

    if (info.par_num > 0 && info.par_den > 0) {
      if (G_UNLIKELY ((priv->parsed_par_n != info.par_num)
              || (priv->parsed_par_d != info.par_den))) {
        priv->parsed_par_n = info.par_num;
        priv->parsed_par_d = info.par_den;
        GST_INFO_OBJECT (self, "pixel aspect ratio has been changed %d/%d",
            priv->parsed_par_n, priv->parsed_par_d);
      }
    }

    if (G_UNLIKELY (modified || priv->update_caps)) {
      gint width, height;
      GstClockTime latency;
      gint fps_num, fps_den;
      gint par_n, par_d;
      const gchar *caps_mview_mode = NULL;
      GstVideoMultiviewMode mview_mode = self->multiview_mode;
      GstVideoMultiviewFlags mview_flags = self->multiview_flags;

      fps_num = priv->fps_num;
      fps_den = priv->fps_den;

      caps = gst_caps_copy (sink_caps);

      /* sps should give this but upstream overrides */
      if (s && gst_structure_has_field (s, "width"))
        gst_structure_get_int (s, "width", &width);
      else
        width = priv->width;

      if (s && gst_structure_has_field (s, "height"))
        gst_structure_get_int (s, "height", &height);
      else
        height = priv->height;

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
    if (have_sps) {
      if (info.profile != NULL) {
        gst_caps_set_simple (caps,
            "profile", G_TYPE_STRING, info.profile, NULL);
      }

      if (info.tier != NULL)
        gst_caps_set_simple (caps, "tier", G_TYPE_STRING, info.tier, NULL);

      if (info.level != NULL)
        gst_caps_set_simple (caps, "level", G_TYPE_STRING, info.level, NULL);

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
        gst_buffer_replace (&priv->codec_data, buf);
        gst_buffer_unref (buf);
        buf = NULL;
      } else {
        GstStructure *s;
        /* remove any left-over codec-data hanging around */
        s = gst_caps_get_structure (caps, 0);
        gst_structure_remove_field (s, "codec_data");
        gst_buffer_replace (&priv->codec_data, NULL);
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
  GstH26XBaseParseClass *klass = GST_H26X_BASE_PARSE_GET_CLASS (self);
  GstH26XBaseParsePrivate *priv = self->priv;

  buffer = frame->buffer;

  gst_h26x_base_parse_update_src_caps (self, NULL);

  /* don't mess with timestamps if provided by upstream,
   * particularly since our ts not that good they handle seeking etc */
  if (priv->do_ts) {
    if (klass->get_timestamp) {
      klass->get_timestamp (self,
          &GST_BUFFER_TIMESTAMP (buffer), &GST_BUFFER_DURATION (buffer),
          priv->frame_start);
    } else {
      GST_FIXME_OBJECT (self,
          "Implement timestamp/duration interpolation based on SEI message");
    }
  }

  if (priv->keyframe)
    GST_BUFFER_FLAG_UNSET (buffer, GST_BUFFER_FLAG_DELTA_UNIT);
  else
    GST_BUFFER_FLAG_SET (buffer, GST_BUFFER_FLAG_DELTA_UNIT);

  if (priv->header)
    GST_BUFFER_FLAG_SET (buffer, GST_BUFFER_FLAG_HEADER);
  else
    GST_BUFFER_FLAG_UNSET (buffer, GST_BUFFER_FLAG_HEADER);

  if (priv->discont) {
    GST_BUFFER_FLAG_SET (buffer, GST_BUFFER_FLAG_DISCONT);
    priv->discont = FALSE;
  }

  /* replace with transformed output if applicable */
  av = gst_adapter_available (priv->frame_out);
  if (av) {
    GstBuffer *buf;

    buf = gst_adapter_take_buffer (priv->frame_out, av);
    gst_buffer_copy_into (buf, buffer, GST_BUFFER_COPY_METADATA, 0, -1);
    gst_buffer_replace (&frame->out_buffer, buf);
    gst_buffer_unref (buf);
  }

  return GST_FLOW_OK;
}

/* sends a codec NAL downstream, decorating and transforming as needed.
 * No ownership is taken of @nal */
static GstFlowReturn
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
  GstH26XBaseParsePrivate *priv = self->priv;
  GstClockTime running_time;
  guint count;
#ifndef GST_DISABLE_GST_DEBUG
  gboolean have_vps, have_sps, have_pps;
  gint i;
#endif

  priv->pending_key_unit_ts = GST_CLOCK_TIME_NONE;
  gst_event_replace (&priv->force_key_unit_event, NULL);

  gst_video_event_parse_downstream_force_key_unit (event,
      NULL, NULL, &running_time, NULL, &count);

  GST_INFO_OBJECT (self, "pushing downstream force-key-unit event %d "
      "%" GST_TIME_FORMAT " count %d", gst_event_get_seqnum (event),
      GST_TIME_ARGS (running_time), count);
  gst_pad_push_event (GST_BASE_PARSE_SRC_PAD (self), event);

#ifndef GST_DISABLE_GST_DEBUG
  have_vps = have_sps = have_pps = FALSE;
  for (i = 0; i < priv->max_vps_count; i++) {
    if (self->vps_nals[i] != NULL) {
      have_vps = TRUE;
      break;
    }
  }
  for (i = 0; i < priv->max_sps_count; i++) {
    if (self->sps_nals[i] != NULL) {
      have_sps = TRUE;
      break;
    }
  }
  for (i = 0; i < priv->max_pps_count; i++) {
    if (self->pps_nals[i] != NULL) {
      have_pps = TRUE;
      break;
    }
  }

  if (priv->max_vps_count) {
    GST_INFO_OBJECT (self,
        "preparing key unit, have vps %d have sps %d have pps %d", have_vps,
        have_sps, have_pps);
  } else {
    GST_INFO_OBJECT (self, "preparing key unit, have sps %d have pps %d",
        have_sps, have_pps);
  }
#endif

  /* set push_codec to TRUE so that pre_push_frame sends SPS/PPS again */
  priv->push_codec = TRUE;
}

static gboolean
gst_h26x_base_parse_handle_vps_sps_pps_nals (GstH26XBaseParse * self,
    GstBuffer * buffer, GstBaseParseFrame * frame)
{
  GstBuffer *codec_nal;
  gint i;
  gboolean send_done = FALSE;
  GstClockTime timestamp = GST_BUFFER_TIMESTAMP (buffer);
  GstH26XBaseParsePrivate *priv = self->priv;

  if (self->align == GST_H26X_BASE_PARSE_ALIGN_NAL) {
    /* send separate config NAL buffers */
    GST_DEBUG_OBJECT (self, "- sending %sSPS/PPS",
        priv->max_vps_count ? "VPS/" : "");
    for (i = 0; i < priv->max_vps_count; i++) {
      if ((codec_nal = self->vps_nals[i])) {
        GST_DEBUG_OBJECT (self, "sending VPS nal");
        gst_h26x_base_parse_push_codec_buffer (self, codec_nal, timestamp);
        send_done = TRUE;
      }
    }
    for (i = 0; i < priv->max_sps_count; i++) {
      if ((codec_nal = self->sps_nals[i])) {
        GST_DEBUG_OBJECT (self, "sending SPS nal");
        gst_h26x_base_parse_push_codec_buffer (self, codec_nal, timestamp);
        send_done = TRUE;
      }
    }
    for (i = 0; i < priv->max_pps_count; i++) {
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
    ok = gst_byte_writer_put_buffer (&bw, buffer, 0, priv->idr_pos);
    GST_DEBUG_OBJECT (self, "- inserting %sSPS/PPS",
        priv->max_vps_count ? "VPS/" : "");
    for (i = 0; i < priv->max_vps_count; i++) {
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
    for (i = 0; i < priv->max_sps_count; i++) {
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
    for (i = 0; i < priv->max_pps_count; i++) {
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
    ok &= gst_byte_writer_put_buffer (&bw, buffer, priv->idr_pos, -1);
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
          priv->max_vps_count ? "VPS/" : "");
    }
  }

  return send_done;
}

static GstFlowReturn
gst_h26x_base_parse_pre_push_frame (GstBaseParse * parse,
    GstBaseParseFrame * frame)
{
  GstH26XBaseParse *self = GST_H26X_BASE_PARSE (parse);
  GstH26XBaseParseClass *klass = GST_H26X_BASE_PARSE_GET_CLASS (self);
  GstH26XBaseParsePrivate *priv = self->priv;
  GstBuffer *buffer;
  GstEvent *event;

  if (!priv->sent_codec_tag) {
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
    priv->sent_codec_tag = TRUE;
  }

  if (klass->make_aud_memory && priv->aud_insert &&
      self->format == GST_H26X_BASE_PARSE_FORMAT_BYTE) {
    GstMemory *mem;
    guint size;

    /* In case of byte-stream, insert au delimeter by default
     * if it doesn't exist */
    if (self->align == GST_H26X_BASE_PARSE_ALIGN_AU) {
      mem = klass->make_aud_memory (self, TRUE, &size);

      frame->out_buffer = gst_buffer_copy (frame->buffer);
      gst_buffer_prepend_memory (frame->out_buffer, mem);
      if (priv->idr_pos >= 0)
        priv->idr_pos += size;

      buffer = frame->out_buffer;
    } else {
      GstBuffer *aud_buffer = gst_buffer_new ();

      mem = klass->make_aud_memory (self, FALSE, NULL);
      gst_buffer_prepend_memory (aud_buffer, mem);

      buffer = frame->buffer;
      gst_h26x_base_parse_push_codec_buffer (self, aud_buffer,
          GST_BUFFER_TIMESTAMP (buffer));
      gst_buffer_unref (aud_buffer);
    }
  } else {
    buffer = frame->buffer;
  }

  if ((event = check_pending_key_unit_event (priv->force_key_unit_event,
              &parse->segment, GST_BUFFER_TIMESTAMP (buffer),
              GST_BUFFER_FLAGS (buffer), priv->pending_key_unit_ts))) {
    gst_h26x_base_parse_prepare_key_unit (self, event);
  }

  /* periodic SPS/PPS sending */
  if (priv->interval > 0 || priv->push_codec) {
    GstClockTime timestamp = GST_BUFFER_TIMESTAMP (buffer);
    guint64 diff;
    gboolean initial_frame = FALSE;

    /* init */
    if (!GST_CLOCK_TIME_IS_VALID (priv->last_report)) {
      priv->last_report = timestamp;
      initial_frame = TRUE;
    }

    if (priv->idr_pos >= 0) {
      GST_LOG_OBJECT (self, "IDR nal at offset %d", priv->idr_pos);

      if (timestamp > priv->last_report)
        diff = timestamp - priv->last_report;
      else
        diff = 0;

      GST_LOG_OBJECT (self,
          "now %" GST_TIME_FORMAT ", last SPS/PPS %" GST_TIME_FORMAT,
          GST_TIME_ARGS (timestamp), GST_TIME_ARGS (priv->last_report));

      GST_DEBUG_OBJECT (self,
          "interval since last SPS/PPS %" GST_TIME_FORMAT,
          GST_TIME_ARGS (diff));

      if (GST_TIME_AS_SECONDS (diff) >= priv->interval ||
          initial_frame || priv->push_codec) {
        GstClockTime new_ts;

        /* avoid overwriting a perfectly fine timestamp */
        new_ts = GST_CLOCK_TIME_IS_VALID (timestamp) ? timestamp :
            priv->last_report;

        if (gst_h26x_base_parse_handle_vps_sps_pps_nals (self, buffer, frame)) {
          priv->last_report = new_ts;
        }
      }
      /* we pushed whatever we had */
      priv->push_codec = FALSE;
      priv->have_vps = FALSE;
      priv->have_sps = FALSE;
      priv->have_pps = FALSE;
      priv->state &= GST_H26X_BASE_PARSE_STATE_VALID_PICTURE_HEADERS;
    }
  } else if (priv->interval == -1) {
    if (priv->idr_pos >= 0) {
      GST_LOG_OBJECT (self, "IDR nal at offset %d", priv->idr_pos);

      gst_h26x_base_parse_handle_vps_sps_pps_nals (self, buffer, frame);

      /* we pushed whatever we had */
      priv->push_codec = FALSE;
      priv->have_vps = FALSE;
      priv->have_sps = FALSE;
      priv->have_pps = FALSE;
      priv->state &= GST_H26X_BASE_PARSE_STATE_VALID_PICTURE_HEADERS;
    }
  }

  /* If SPS/PPS and a keyframe have been parsed, and we're not converting,
   * we might switch to passthrough mode now on the basis that we've seen
   * the SEI packets and know optional caps params (such as multiview).
   * This is an efficiency optimisation that relies on stream properties
   * remaining uniform in practice. */
  if (priv->can_passthrough && klass->allow_passthrough (self)) {
    if (priv->keyframe && priv->have_sps && priv->have_pps) {
      GST_LOG_OBJECT (parse, "Switching to passthrough mode");
      gst_base_parse_set_passthrough (parse, TRUE);
    }
  }

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
  GstH26XBaseParsePrivate *priv;

  self = GST_H26X_BASE_PARSE (parse);
  klass = GST_H26X_BASE_PARSE_GET_CLASS (self);
  priv = self->priv;

  /* reset */
  priv->push_codec = FALSE;

  old_caps = gst_pad_get_current_caps (GST_BASE_PARSE_SINK_PAD (parse));
  if (old_caps) {
    if (!gst_caps_is_equal (old_caps, caps))
      gst_h26x_base_parse_reset_stream_info (self);
    gst_caps_unref (old_caps);
  }

  str = gst_caps_get_structure (caps, 0);

  /* accept upstream info if provided */
  gst_structure_get_int (str, "width", &priv->width);
  gst_structure_get_int (str, "height", &priv->height);
  gst_structure_get_fraction (str, "framerate", &priv->fps_num, &priv->fps_den);
  gst_structure_get_fraction (str, "pixel-aspect-ratio",
      &priv->upstream_par_n, &priv->upstream_par_d);

  /* get upstream format and align from caps */
  gst_h26x_base_parse_format_from_caps (self, caps, &format, &align);

  codec_data_value = gst_structure_get_value (str, "codec_data");

  if (!klass->fixate_format (self, &format, &align, codec_data_value))
    goto refuse_caps;

  /* packetized video has codec_data (required for AVC, optional for AVC3) */
  if (codec_data_value != NULL) {
    GstMapInfo map;
    gboolean ret;

    GST_DEBUG_OBJECT (self, "have packetized format");
    /* make note for optional split processing */
    priv->packetized = TRUE;

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

    gst_buffer_replace (&priv->codec_data_in, codec_data);
  } else if (format == GST_H26X_BASE_PARSE_FORMAT_BYTE) {
    GST_DEBUG_OBJECT (self, "have bytestream");
    /* nothing to pre-process */
    priv->packetized = FALSE;
    /* we have 4 sync bytes */
    self->nal_length_size = 4;
  } else {
    /* probably AVC3 without codec_data field, anything to do here? */
  }

  {
    GstCaps *in_caps;

    /* prefer input type determined above */
    in_caps = klass->new_empty_caps (self);
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
    priv->push_codec = TRUE;
    priv->have_vps = FALSE;
    priv->have_sps = FALSE;
    priv->have_pps = FALSE;
    if (self->align == GST_H26X_BASE_PARSE_ALIGN_NAL)
      priv->split_packetized = TRUE;
    priv->packetized = TRUE;
  }

  priv->in_align = align;

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
  GstH26XBaseParsePrivate *priv = self->priv;

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
        if (priv->force_key_unit_event) {
          GST_INFO_OBJECT (self, "ignoring force key unit event "
              "as one is already queued");
        } else {
          priv->pending_key_unit_ts = running_time;
          gst_event_replace (&priv->force_key_unit_event, event);
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
      priv->push_codec = TRUE;

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
        priv->do_ts = FALSE;

      priv->last_report = GST_CLOCK_TIME_NONE;

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
  GstH26XBaseParsePrivate *priv = self->priv;

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
          priv->pending_key_unit_ts = running_time;
          gst_event_replace (&priv->force_key_unit_event, event);
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
  GstH26XBaseParse *self = GST_H26X_BASE_PARSE (object);
  GstH26XBaseParsePrivate *priv = self->priv;

  switch (prop_id) {
    case PROP_CONFIG_INTERVAL:
      priv->interval = g_value_get_int (value);
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
  GstH26XBaseParse *self = GST_H26X_BASE_PARSE (object);
  GstH26XBaseParsePrivate *priv = self->priv;

  switch (prop_id) {
    case PROP_CONFIG_INTERVAL:
      g_value_set_int (value, priv->interval);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static gboolean
gst_h26x_base_parse_allow_passthrough_default (GstH26XBaseParse * parse)
{
  return TRUE;
}
