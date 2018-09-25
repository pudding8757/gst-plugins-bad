/* GStreamer H.265 Parser
 * Copyright (C) 2013 Intel Corporation
 *  Contact:Sreerenj Balachandran <sreerenj.balachandran@intel.com>
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
#include "gsth265parse.h"

#include <string.h>

GST_DEBUG_CATEGORY (h265_parse_debug);
#define GST_CAT_DEFAULT h265_parse_debug

/* expect at least 3 bytes startcode == sc, and 3 bytes NALU payload */
#define GST_H265_PARSE_MIN_NALU_SIZE 6

enum
{
  GST_H265_PARSE_FORMAT_NONE = GST_H26X_BASE_PARSE_FORMAT_NONE,
  GST_H265_PARSE_FORMAT_BYTE = GST_H26X_BASE_PARSE_FORMAT_BYTE,
  GST_H265_PARSE_FORMAT_HVC1,
  GST_H265_PARSE_FORMAT_HEV1,
};

static GstStaticPadTemplate sinktemplate = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-h265"));

static GstStaticPadTemplate srctemplate = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-h265, parsed = (boolean) true, "
        "stream-format=(string) { hvc1, hev1, byte-stream }, "
        "alignment=(string) { au, nal }"));

#define parent_class gst_h265_parse_parent_class
G_DEFINE_TYPE (GstH265Parse, gst_h265_parse, GST_TYPE_H26X_BASE_PARSE);

static gboolean gst_h265_parse_start (GstBaseParse * parse);
static gboolean gst_h265_parse_stop (GstBaseParse * parse);

static void gst_h265_parse_get_max_vps_sps_pps_count (GstH26XBaseParse * parse,
    guint * max_vps_count, guint * max_sps_count, guint * max_pps_count);
static guint gst_h265_parse_get_min_nalu_size (GstH26XBaseParse * parse);
static const gchar *gst_h265_parse_format_to_string (GstH26XBaseParse * parse,
    guint format);
static guint gst_h265_parse_format_from_string (GstH26XBaseParse * parse,
    const gchar * format);
static GstCaps *gst_h265_parse_new_empty_caps (GstH26XBaseParse * parse);
static gboolean gst_h265_parse_fill_sps_info (GstH26XBaseParse * parse,
    GstH26XBaseParseSPSInfo * info);
static GstCaps *gst_h265_parse_get_compatible_profile_caps (GstH26XBaseParse *
    parse);
static GstBuffer *gst_h265_parse_make_codec_data (GstH26XBaseParse * parse);
static GstFlowReturn
gst_h265_parse_handle_frame_packetized (GstH26XBaseParse * parse,
    GstBaseParseFrame * frame, gboolean split);
static GstH26XBaseParseHandleFrameReturn
gst_h265_parse_handle_frame_check_initial_skip (GstH26XBaseParse * parse,
    gint * skipsize, gint * dropsize, GstMapInfo * map);
static GstH26XBaseParseHandleFrameReturn
gst_h265_parse_handle_frame_bytestream (GstH26XBaseParse * parse,
    gint * skipsize, gint * framesize, gint * current_offset,
    gboolean * aud_complete, GstMapInfo * map, gboolean drain);
static gboolean
gst_h265_parse_fixate_format (GstH26XBaseParse * parse, guint * format,
    guint * align, const GValue * codec_data_value);
static gboolean
gst_h265_parse_handle_codec_data (GstH26XBaseParse * parse, GstMapInfo * map);

static void
gst_h265_parse_class_init (GstH265ParseClass * klass)
{
  GstBaseParseClass *parse_class = GST_BASE_PARSE_CLASS (klass);
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS (klass);
  GstH26XBaseParseClass *h26xbase_class = GST_H26X_BASE_PARSE_CLASS (klass);

  GST_DEBUG_CATEGORY_INIT (h265_parse_debug, "h265parse", 0, "h265 parser");

  /* Override BaseParse vfuncs */
  parse_class->start = GST_DEBUG_FUNCPTR (gst_h265_parse_start);
  parse_class->stop = GST_DEBUG_FUNCPTR (gst_h265_parse_stop);

  h26xbase_class->get_max_vps_sps_pps_count =
      GST_DEBUG_FUNCPTR (gst_h265_parse_get_max_vps_sps_pps_count);
  h26xbase_class->get_min_nalu_size =
      GST_DEBUG_FUNCPTR (gst_h265_parse_get_min_nalu_size);
  h26xbase_class->format_to_string =
      GST_DEBUG_FUNCPTR (gst_h265_parse_format_to_string);
  h26xbase_class->format_from_string =
      GST_DEBUG_FUNCPTR (gst_h265_parse_format_from_string);
  h26xbase_class->new_empty_caps =
      GST_DEBUG_FUNCPTR (gst_h265_parse_new_empty_caps);
  h26xbase_class->fill_sps_info =
      GST_DEBUG_FUNCPTR (gst_h265_parse_fill_sps_info);
  h26xbase_class->get_compatible_profile_caps =
      GST_DEBUG_FUNCPTR (gst_h265_parse_get_compatible_profile_caps);
#if 0
  /* TODO: add code for AUD inserting */
  h26xbase_class->make_aud_memory =
      GST_DEBUG_FUNCPTR (gst_h265_parse_make_aud_memory);
#endif
  h26xbase_class->make_codec_data =
      GST_DEBUG_FUNCPTR (gst_h265_parse_make_codec_data);
  h26xbase_class->handle_frame_packetized =
      GST_DEBUG_FUNCPTR (gst_h265_parse_handle_frame_packetized);
  h26xbase_class->handle_frame_check_initial_skip =
      GST_DEBUG_FUNCPTR (gst_h265_parse_handle_frame_check_initial_skip);
  h26xbase_class->handle_frame_bytestream =
      GST_DEBUG_FUNCPTR (gst_h265_parse_handle_frame_bytestream);
  h26xbase_class->fixate_format =
      GST_DEBUG_FUNCPTR (gst_h265_parse_fixate_format);
  h26xbase_class->handle_codec_data =
      GST_DEBUG_FUNCPTR (gst_h265_parse_handle_codec_data);


  gst_element_class_add_static_pad_template (gstelement_class, &srctemplate);
  gst_element_class_add_static_pad_template (gstelement_class, &sinktemplate);

  gst_element_class_set_static_metadata (gstelement_class, "H.265 parser",
      "Codec/Parser/Converter/Video",
      "Parses H.265 streams",
      "Sreerenj Balachandran <sreerenj.balachandran@intel.com>");
}

static void
gst_h265_parse_init (GstH265Parse * h265parse)
{
}

static gboolean
gst_h265_parse_start (GstBaseParse * parse)
{
  GstH265Parse *h265parse = GST_H265_PARSE (parse);

  GST_DEBUG_OBJECT (parse, "start");

  h265parse->nalparser = gst_h265_parser_new ();

  return GST_BASE_PARSE_CLASS (parent_class)->start (parse);
}

static gboolean
gst_h265_parse_stop (GstBaseParse * parse)
{
  GstH265Parse *h265parse = GST_H265_PARSE (parse);

  GST_DEBUG_OBJECT (parse, "stop");

  gst_h265_parser_free (h265parse->nalparser);

  return GST_BASE_PARSE_CLASS (parent_class)->stop (parse);
}

static void
gst_h265_parse_get_max_vps_sps_pps_count (GstH26XBaseParse * parse,
    guint * max_vps_count, guint * max_sps_count, guint * max_pps_count)
{
  *max_vps_count = GST_H265_MAX_VPS_COUNT;
  *max_sps_count = GST_H265_MAX_SPS_COUNT;
  *max_pps_count = GST_H265_MAX_PPS_COUNT;
}

static guint
gst_h265_parse_get_min_nalu_size (GstH26XBaseParse * parse)
{
  return GST_H265_PARSE_MIN_NALU_SIZE;
}

static const gchar *
gst_h265_parse_format_to_string (GstH26XBaseParse * parse, guint format)
{
  switch (format) {
    case GST_H265_PARSE_FORMAT_HVC1:
      return "hvc1";
    case GST_H265_PARSE_FORMAT_HEV1:
      return "hev1";
    case GST_H265_PARSE_FORMAT_BYTE:
      return "byte-stream";
    default:
      return "none";
  }
}

static guint
gst_h265_parse_format_from_string (GstH26XBaseParse * parse,
    const gchar * format)
{
  guint ret = GST_H265_PARSE_FORMAT_NONE;

  if (g_strcmp0 (format, "hvc1") == 0)
    ret = GST_H265_PARSE_FORMAT_HVC1;
  else if (g_strcmp0 (format, "hev1") == 0)
    ret = GST_H265_PARSE_FORMAT_HEV1;
  else if (g_strcmp0 (format, "byte-stream") == 0)
    ret = GST_H265_PARSE_FORMAT_BYTE;

  return ret;
}

static GstCaps *
gst_h265_parse_new_empty_caps (GstH26XBaseParse * parse)
{
  return gst_caps_new_empty_simple ("video/x-h265");
}

#ifndef GST_DISABLE_GST_DEBUG
static const gchar *nal_names[] = {
  "Slice_TRAIL_N",
  "Slice_TRAIL_R",
  "Slice_TSA_N",
  "Slice_TSA_R",
  "Slice_STSA_N",
  "Slice_STSA_R",
  "Slice_RADL_N",
  "Slice_RADL_R",
  "SLICE_RASL_N",
  "SLICE_RASL_R",
  "Invalid (10)",
  "Invalid (11)",
  "Invalid (12)",
  "Invalid (13)",
  "Invalid (14)",
  "Invalid (15)",
  "SLICE_BLA_W_LP",
  "SLICE_BLA_W_RADL",
  "SLICE_BLA_N_LP",
  "SLICE_IDR_W_RADL",
  "SLICE_IDR_N_LP",
  "SLICE_CRA_NUT",
  "Invalid (22)",
  "Invalid (23)",
  "Invalid (24)",
  "Invalid (25)",
  "Invalid (26)",
  "Invalid (27)",
  "Invalid (28)",
  "Invalid (29)",
  "Invalid (30)",
  "Invalid (31)",
  "VPS",
  "SPS",
  "PPS",
  "AUD",
  "EOS",
  "EOB",
  "FD",
  "PREFIX_SEI",
  "SUFFIX_SEI"
};

static const gchar *
_nal_name (GstH265NalUnitType nal_type)
{
  if (nal_type <= GST_H265_NAL_SUFFIX_SEI)
    return nal_names[nal_type];
  return "Invalid";
}
#endif

/* caller guarantees 2 bytes of nal payload */
static gboolean
gst_h265_parse_process_nal (GstH265Parse * h265parse, GstH265NalUnit * nalu)
{
  GstH26XBaseParse *parse = GST_H26X_BASE_PARSE (h265parse);
  GstH265PPS pps = { 0, };
  GstH265SPS sps = { 0, };
  GstH265VPS vps = { 0, };
  gboolean is_irap;
  guint nal_type;
  GstH265Parser *nalparser = h265parse->nalparser;
  GstH265ParserResult pres = GST_H265_PARSER_ERROR;

  /* nothing to do for broken input */
  if (G_UNLIKELY (nalu->size < 2)) {
    GST_DEBUG_OBJECT (h265parse, "not processing nal size %u", nalu->size);
    return TRUE;
  }

  /* we have a peek as well */
  nal_type = nalu->type;

  GST_DEBUG_OBJECT (h265parse, "processing nal of type %u %s, size %u",
      nal_type, _nal_name (nal_type), nalu->size);
  switch (nal_type) {
    case GST_H265_NAL_VPS:
      /* It is not mandatory to have VPS in the stream. But it might
       * be needed for other extensions like svc */
      pres = gst_h265_parser_parse_vps (nalparser, nalu, &vps);
      if (pres != GST_H265_PARSER_OK) {
        GST_WARNING_OBJECT (h265parse, "failed to parse VPS");
        return FALSE;
      }

      gst_h26x_base_parse_store_header_nal (parse, vps.id,
          GST_H26X_BASE_PARSE_STORE_NAL_TYPE_VPS, nalu->data + nalu->offset,
          nalu->size);
      gst_h26x_base_parse_vps_parsed (parse);
      break;
    case GST_H265_NAL_SPS:
      /* reset state, everything else is obsolete */
      gst_h26x_base_parse_clear_state (parse, GST_H26X_BASE_PARSE_STATE_INIT);
      pres = gst_h265_parser_parse_sps (nalparser, nalu, &sps, TRUE);

      /* arranged for a fallback sps.id, so use that one and only warn */
      if (pres != GST_H265_PARSER_OK) {
        GST_WARNING_OBJECT (h265parse, "failed to parse SPS:");
        return FALSE;
      }

      gst_h26x_base_parse_store_header_nal (parse, sps.id,
          GST_H26X_BASE_PARSE_STORE_NAL_TYPE_SPS, nalu->data + nalu->offset,
          nalu->size);
      gst_h26x_base_parse_sps_parsed (parse);
      break;
    case GST_H265_NAL_PPS:
      /* expected state: got-sps */
      gst_h26x_base_parse_clear_state (parse,
          GST_H26X_BASE_PARSE_STATE_GOT_SPS);
      if (!gst_h26x_base_parse_is_valid_state (parse,
              GST_H26X_BASE_PARSE_STATE_GOT_SPS))
        return FALSE;

      pres = gst_h265_parser_parse_pps (nalparser, nalu, &pps);
      /* arranged for a fallback pps.id, so use that one and only warn */
      if (pres != GST_H265_PARSER_OK) {
        GST_WARNING_OBJECT (h265parse, "failed to parse PPS:");
        if (pres != GST_H265_PARSER_BROKEN_LINK)
          return FALSE;
      }

      gst_h26x_base_parse_store_header_nal (parse, pps.id,
          GST_H26X_BASE_PARSE_STORE_NAL_TYPE_PPS, nalu->data + nalu->offset,
          nalu->size);
      gst_h26x_base_parse_pps_parsed (parse);
      break;
    case GST_H265_NAL_PREFIX_SEI:
    case GST_H265_NAL_SUFFIX_SEI:
      /* expected state: got-sps */
      if (!gst_h26x_base_parse_is_valid_state (parse,
              GST_H26X_BASE_PARSE_STATE_GOT_SPS))
        return FALSE;

      /*Fixme: parse sei messages */
      gst_h26x_base_parse_sei_parsed (parse, nalu->sc_offset);
      break;
    case GST_H265_NAL_SLICE_TRAIL_N:
    case GST_H265_NAL_SLICE_TRAIL_R:
    case GST_H265_NAL_SLICE_TSA_N:
    case GST_H265_NAL_SLICE_TSA_R:
    case GST_H265_NAL_SLICE_STSA_N:
    case GST_H265_NAL_SLICE_STSA_R:
    case GST_H265_NAL_SLICE_RADL_N:
    case GST_H265_NAL_SLICE_RADL_R:
    case GST_H265_NAL_SLICE_RASL_N:
    case GST_H265_NAL_SLICE_RASL_R:
    case GST_H265_NAL_SLICE_BLA_W_LP:
    case GST_H265_NAL_SLICE_BLA_W_RADL:
    case GST_H265_NAL_SLICE_BLA_N_LP:
    case GST_H265_NAL_SLICE_IDR_W_RADL:
    case GST_H265_NAL_SLICE_IDR_N_LP:
    case GST_H265_NAL_SLICE_CRA_NUT:
    {
      GstH265SliceHdr slice;

      /* expected state: got-sps|got-pps (valid picture headers) */
      gst_h26x_base_parse_clear_state (parse,
          GST_H26X_BASE_PARSE_STATE_VALID_PICTURE_HEADERS);
      if (!gst_h26x_base_parse_is_valid_state (parse,
              GST_H26X_BASE_PARSE_STATE_VALID_PICTURE_HEADERS))
        return FALSE;

      pres = gst_h265_parser_parse_slice_hdr (nalparser, nalu, &slice);

      if (pres == GST_H265_PARSER_OK) {
        gst_h26x_base_parse_slice_hdr_parsed (parse,
            GST_H265_IS_I_SLICE (&slice));
      }

      if (slice.first_slice_segment_in_pic_flag == 1) {
        GST_DEBUG_OBJECT (h265parse,
            "frame start, first_slice_segment_in_pic_flag = 1");
        gst_h26x_base_parse_frame_started (parse);
      }

      GST_DEBUG_OBJECT (h265parse,
          "parse result %d, first slice_segment: %u, slice type: %u",
          pres, slice.first_slice_segment_in_pic_flag, slice.type);

      gst_h265_slice_hdr_free (&slice);
    }

      is_irap = ((nal_type >= GST_H265_NAL_SLICE_BLA_W_LP)
          && (nal_type <= GST_H265_NAL_SLICE_CRA_NUT)) ? TRUE : FALSE;

      gst_h26x_base_parse_update_idr_pos (parse, nalu->sc_offset, is_irap);
      break;
    case GST_H265_NAL_AUD:
      /* Just accumulate AU Delimiter, whether it's before SPS or not */
      pres = gst_h265_parser_parse_nal (nalparser, nalu);
      if (pres != GST_H265_PARSER_OK)
        return FALSE;
      gst_h26x_base_parse_aud_parsed (parse);
      break;
    default:
      /* drop anything before the initial SPS */
      if (!gst_h26x_base_parse_is_valid_state (parse,
              GST_H26X_BASE_PARSE_STATE_GOT_SPS))
        return FALSE;

      pres = gst_h265_parser_parse_nal (nalparser, nalu);
      if (pres != GST_H265_PARSER_OK)
        return FALSE;
      break;
  }

  gst_h26x_base_pares_finish_process_nal (parse,
      nalu->data + nalu->offset, nalu->size);

  return TRUE;
}

/* caller guarantees at least 3 bytes of nal payload for each nal
 * returns TRUE if next_nal indicates that nal terminates an AU */
static inline gboolean
gst_h265_parse_collect_nal (GstH265Parse * h265parse, const guint8 * data,
    guint size, GstH265NalUnit * nalu)
{
  GstH26XBaseParse *parse = GST_H26X_BASE_PARSE (h265parse);
  gboolean complete;
  GstH265ParserResult parse_res;
  GstH265NalUnitType nal_type = nalu->type;
  GstH265NalUnit nnalu;

  GST_DEBUG_OBJECT (h265parse, "parsing collected nal");
  parse_res = gst_h265_parser_identify_nalu_unchecked (h265parse->nalparser,
      data, nalu->offset + nalu->size, size, &nnalu);

  if (parse_res != GST_H265_PARSER_OK)
    return FALSE;

  /* determine if AU complete */
  GST_LOG_OBJECT (h265parse, "nal type: %d %s", nal_type, _nal_name (nal_type));
  /* coded slice NAL starts a picture,
   * i.e. other types become aggregated in front of it */
  parse->picture_start |= ((nal_type >= GST_H265_NAL_SLICE_TRAIL_N
          && nal_type <= GST_H265_NAL_SLICE_RASL_R)
      || (nal_type >= GST_H265_NAL_SLICE_BLA_W_LP
          && nal_type <= RESERVED_IRAP_NAL_TYPE_MAX));

  /* consider a coded slices (IRAP or not) to start a picture,
   * (so ending the previous one) if first_slice_segment_in_pic_flag == 1*/
  nal_type = nnalu.type;
  complete = parse->picture_start && ((nal_type >= GST_H265_NAL_VPS
          && nal_type <= GST_H265_NAL_AUD)
      || nal_type == GST_H265_NAL_PREFIX_SEI || (nal_type >= 41
          && nal_type <= 44) || (nal_type >= 48 && nal_type <= 55));

  GST_LOG_OBJECT (h265parse, "next nal type: %d %s", nal_type,
      _nal_name (nal_type));

  /* Any VCL Nal unit with first_slice_segment_in_pic_flag == 1 considered start of frame */
  complete |= parse->picture_start
      && (((nal_type >= GST_H265_NAL_SLICE_TRAIL_N
              && nal_type <= GST_H265_NAL_SLICE_RASL_R)
          || (nal_type >= GST_H265_NAL_SLICE_BLA_W_LP
              && nal_type <= RESERVED_IRAP_NAL_TYPE_MAX))
      && (nnalu.data[nnalu.offset + 2] & 0x80));

  GST_LOG_OBJECT (h265parse, "au complete: %d", complete);
  return complete;
}

static GstFlowReturn
gst_h265_parse_handle_frame_packetized (GstH26XBaseParse * parse,
    GstBaseParseFrame * frame, gboolean split)
{
  GstH265Parse *h265parse = GST_H265_PARSE (parse);
  GstBaseParse *baseparse = GST_BASE_PARSE (parse);
  GstBuffer *buffer = frame->buffer;
  GstFlowReturn ret = GST_FLOW_OK;
  GstH265ParserResult parse_res;
  GstH265NalUnit nalu;
  const guint nl = parse->nal_length_size;
  GstMapInfo map;
  gint left;

  /* need to save buffer from invalidation upon _finish_frame */
  if (split)
    buffer = gst_buffer_copy (frame->buffer);

  gst_buffer_map (buffer, &map, GST_MAP_READ);

  left = map.size;

  GST_LOG_OBJECT (h265parse,
      "processing packet buffer of size %" G_GSIZE_FORMAT, map.size);

  parse_res = gst_h265_parser_identify_nalu_hevc (h265parse->nalparser,
      map.data, 0, map.size, nl, &nalu);

  while (parse_res == GST_H265_PARSER_OK) {
    GST_DEBUG_OBJECT (h265parse, "HEVC nal offset %d", nalu.offset + nalu.size);

    /* either way, have a look at it */
    gst_h265_parse_process_nal (h265parse, &nalu);

    /* dispatch per NALU if needed */
    if (split) {
      GstBaseParseFrame tmp_frame;

      gst_base_parse_frame_init (&tmp_frame);
      tmp_frame.flags |= frame->flags;
      tmp_frame.offset = frame->offset;
      tmp_frame.overhead = frame->overhead;
      tmp_frame.buffer = gst_buffer_copy_region (buffer, GST_BUFFER_COPY_ALL,
          nalu.offset, nalu.size);

      /* note we don't need to come up with a sub-buffer, since
       * subsequent code only considers input buffer's metadata.
       * Real data is either taken from input by baseclass or
       * a replacement output buffer is provided anyway. */
      gst_h26x_base_parse_parse_frame (parse, &tmp_frame);
      ret = gst_base_parse_finish_frame (baseparse, &tmp_frame, nl + nalu.size);
      left -= nl + nalu.size;
    }

    parse_res = gst_h265_parser_identify_nalu_hevc (h265parse->nalparser,
        map.data, nalu.offset + nalu.size, map.size, nl, &nalu);
  }

  gst_buffer_unmap (buffer, &map);

  if (!split) {
    gst_h26x_base_parse_parse_frame (parse, frame);
    ret = gst_base_parse_finish_frame (baseparse, frame, map.size);
  } else {
    gst_buffer_unref (buffer);
    if (G_UNLIKELY (left)) {
      /* should not be happening for nice HEVC */
      GST_WARNING_OBJECT (parse, "skipping leftover HEVC data %d", left);
      frame->flags |= GST_BASE_PARSE_FRAME_FLAG_DROP;
      ret = gst_base_parse_finish_frame (baseparse, frame, map.size);
    }
  }

  if (parse_res == GST_H265_PARSER_NO_NAL_END ||
      parse_res == GST_H265_PARSER_BROKEN_DATA) {

    if (split) {
      GST_ELEMENT_ERROR (h265parse, STREAM, FAILED, (NULL),
          ("invalid HEVC input data"));

      return GST_FLOW_ERROR;
    } else {
      /* do not meddle to much in this case */
      GST_DEBUG_OBJECT (h265parse, "parsing packet failed");
    }
  }

  return ret;
}

static GstH26XBaseParseHandleFrameReturn
gst_h265_parse_handle_frame_check_initial_skip (GstH26XBaseParse * parse,
    gint * skipsize, gint * dropsize, GstMapInfo * map)
{
  GstH265Parse *h265parse = GST_H265_PARSE (parse);
  GstH265Parser *nalparser = h265parse->nalparser;
  guint8 *data;
  gsize size;
  GstH265NalUnit nalu;
  GstH265ParserResult pres;

  data = map->data;
  size = map->size;

  pres =
      gst_h265_parser_identify_nalu_unchecked (nalparser, data, 0, size, &nalu);

  switch (pres) {
    case GST_H265_PARSER_OK:
      if (nalu.sc_offset > 0) {
        *skipsize = nalu.sc_offset;
        return GST_H26X_BASE_PARSE_HANDLE_FRAME_SKIP;
      }
      break;
    case GST_H265_PARSER_NO_NAL:
      *skipsize = size - 3;
      return GST_H26X_BASE_PARSE_HANDLE_FRAME_SKIP;
      break;
    default:
      /* should not really occur either */
      GST_ELEMENT_ERROR (parse, STREAM, FORMAT,
          ("Error parsing H.264 stream"), ("Invalid H.264 stream"));
      return GST_H26X_BASE_PARSE_HANDLE_FRAME_INVALID_STREAM;
  }

  return GST_H26X_BASE_PARSE_HANDLE_FRAME_OK;
}

static GstH26XBaseParseHandleFrameReturn
gst_h265_parse_handle_frame_bytestream (GstH26XBaseParse * parse,
    gint * skipsize, gint * framesize, gint * current_offset,
    gboolean * au_complete, GstMapInfo * map, gboolean drain)
{
  GstH265Parse *h265parse = GST_H265_PARSE (parse);
  GstH265Parser *nalparser = h265parse->nalparser;
  guint8 *data;
  gsize size;
  GstH265NalUnit nalu;
  GstH265ParserResult pres;
  gboolean nonext;

  data = map->data;
  size = map->size;
  nonext = FALSE;

  while (TRUE) {
    pres =
        gst_h265_parser_identify_nalu (nalparser, data, *current_offset, size,
        &nalu);

    switch (pres) {
      case GST_H265_PARSER_OK:
        GST_DEBUG_OBJECT (h265parse, "complete nal (offset, size): (%u, %u) ",
            nalu.offset, nalu.size);
        break;
      case GST_H265_PARSER_NO_NAL_END:
        GST_DEBUG_OBJECT (h265parse, "not a complete nal found at offset %u",
            nalu.offset);
        /* if draining, accept it as complete nal */
        if (drain) {
          nonext = TRUE;
          nalu.size = size - nalu.offset;
          GST_DEBUG_OBJECT (h265parse, "draining, accepting with size %u",
              nalu.size);
          /* if it's not too short at least */
          if (nalu.size < 3)
            goto broken;
          break;
        }
        /* otherwise need more */
        return GST_H26X_BASE_PARSE_HANDLE_FRAME_MORE;
      case GST_H265_PARSER_BROKEN_LINK:
        GST_ELEMENT_ERROR (h265parse, STREAM, FORMAT,
            ("Error parsing H.265 stream"),
            ("The link to structure needed for the parsing couldn't be found"));
        return GST_H26X_BASE_PARSE_HANDLE_FRAME_INVALID_STREAM;
      case GST_H265_PARSER_ERROR:
        /* should not really occur either */
        GST_ELEMENT_ERROR (h265parse, STREAM, FORMAT,
            ("Error parsing H.265 stream"), ("Invalid H.265 stream"));
        return GST_H26X_BASE_PARSE_HANDLE_FRAME_INVALID_STREAM;
      case GST_H265_PARSER_NO_NAL:
        GST_ELEMENT_ERROR (h265parse, STREAM, FORMAT,
            ("Error parsing H.265 stream"), ("No H.265 NAL unit found"));
        return GST_H26X_BASE_PARSE_HANDLE_FRAME_INVALID_STREAM;
      case GST_H265_PARSER_BROKEN_DATA:
        GST_WARNING_OBJECT (h265parse, "input stream is corrupt; "
            "it contains a NAL unit of length %u", nalu.size);
      broken:
        /* broken nal at start -> arrange to skip it,
         * otherwise have it terminate current au
         * (and so it will be skipped on next frame round) */
        if (*current_offset == 0) {
          GST_DEBUG_OBJECT (h265parse, "skipping broken nal");
          *skipsize = nalu.offset;
          return GST_H26X_BASE_PARSE_HANDLE_FRAME_SKIP;
        } else {
          GST_DEBUG_OBJECT (h265parse, "terminating au");
          nalu.size = 0;
          nalu.offset = nalu.sc_offset;
          goto end;
        }
      default:
        g_assert_not_reached ();
        break;
    }

    GST_DEBUG_OBJECT (h265parse, "%p complete nal found. Off: %u, Size: %u",
        data, nalu.offset, nalu.size);

    if (!nonext) {
      if (nalu.offset + nalu.size + 5 + 2 > size) {
        GST_DEBUG_OBJECT (h265parse, "not enough data for next NALU");
        if (drain) {
          GST_DEBUG_OBJECT (h265parse, "but draining anyway");
          nonext = TRUE;
        } else {
          return GST_H26X_BASE_PARSE_HANDLE_FRAME_MORE;
        }
      }
    }

    if (!gst_h265_parse_process_nal (h265parse, &nalu)) {
      GST_WARNING_OBJECT (h265parse,
          "broken/invalid nal Type: %d %s, Size: %u will be dropped",
          nalu.type, _nal_name (nalu.type), nalu.size);
      *skipsize = nalu.size;
      return GST_H26X_BASE_PARSE_HANDLE_FRAME_SKIP;
    }

    if (nonext)
      break;

    /* if no next nal, we know it's complete here */
    *au_complete = gst_h265_parse_collect_nal (h265parse, data, size, &nalu);

    if (parse->align == GST_H26X_BASE_PARSE_ALIGN_NAL)
      break;

    if (*au_complete)
      break;

    GST_DEBUG_OBJECT (h265parse, "Looking for more");
    *current_offset = nalu.offset + nalu.size;
  }

end:
  *framesize = nalu.offset + nalu.size;

  return GST_H26X_BASE_PARSE_HANDLE_FRAME_OK;
}

/* byte together hevc codec data based on collected pps and sps so far */
static GstBuffer *
gst_h265_parse_make_codec_data (GstH26XBaseParse * parse)
{
  GstH265Parse *h265parse = GST_H265_PARSE (parse);
  GstBuffer *buf, *nal;
  gint i, j, k = 0;
  guint vps_size = 0, sps_size = 0, pps_size = 0;
  guint num_vps = 0, num_sps = 0, num_pps = 0;
  gboolean found = FALSE;
  GstMapInfo map;
  guint8 *data;
  gint nl;
  guint8 num_arrays = 0;
  GstH265SPS *sps = NULL;
  guint16 min_spatial_segmentation_idc = 0;
  GstH265ProfileTierLevel *pft;

  /* only nal payload in stored nals */
  /* Fixme: Current implementation is not embedding SEI in codec_data */
  for (i = 0; i < GST_H265_MAX_VPS_COUNT; i++) {
    if ((nal = parse->vps_nals[i])) {
      num_vps++;
      /* size bytes also count */
      vps_size += gst_buffer_get_size (nal) + 2;
    }
  }
  if (num_vps > 0)
    num_arrays++;

  for (i = 0; i < GST_H265_MAX_SPS_COUNT; i++) {
    if ((nal = parse->sps_nals[i])) {
      num_sps++;
      /* size bytes also count */
      sps_size += gst_buffer_get_size (nal) + 2;
      found = TRUE;
    }
  }
  if (num_sps > 0)
    num_arrays++;

  for (i = 0; i < GST_H265_MAX_PPS_COUNT; i++) {
    if ((nal = parse->pps_nals[i])) {
      num_pps++;
      /* size bytes also count */
      pps_size += gst_buffer_get_size (nal) + 2;
    }
  }
  if (num_pps > 0)
    num_arrays++;

  GST_DEBUG_OBJECT (h265parse,
      "constructing codec_data: num_vps =%d num_sps=%d, num_pps=%d", num_vps,
      num_sps, num_pps);

  if (!found)
    return NULL;

  sps = h265parse->nalparser->last_sps;
  if (!sps)
    return NULL;

  buf =
      gst_buffer_new_allocate (NULL,
      23 + (3 * num_arrays) + vps_size + sps_size + pps_size, NULL);
  gst_buffer_map (buf, &map, GST_MAP_WRITE);
  data = map.data;
  memset (data, 0, map.size);
  nl = parse->nal_length_size;

  pft = &sps->profile_tier_level;
  if (sps->vui_parameters_present_flag)
    min_spatial_segmentation_idc = sps->vui_params.min_spatial_segmentation_idc;

  /* HEVCDecoderConfigurationVersion = 1
   * profile_space | tier_flat | profile_idc |
   * profile_compatibility_flags | constraint_indicator_flags |
   * level_idc */
  data[0] = 1;
  data[1] =
      (pft->profile_space << 5) | (pft->tier_flag << 5) | pft->profile_idc;
  for (i = 2; i < 6; i++) {
    for (j = 7; j >= 0; j--) {
      data[i] |= (pft->profile_compatibility_flag[k] << j);
      k++;
    }
  }
  data[6] |=
      (pft->progressive_source_flag << 7) | (pft->interlaced_source_flag << 6) |
      (pft->non_packed_constraint_flag << 5) | (pft->
      frame_only_constraint_flag << 4);
  data[12] = pft->level_idc;
  /* min_spatial_segmentation_idc */
  GST_WRITE_UINT16_BE (data + 13, min_spatial_segmentation_idc);
  data[13] |= 0xf0;
  data[15] = 0xfc;              /* keeping parrallelismType as zero (unknown) */
  data[16] = 0xfc | sps->chroma_format_idc;
  data[17] = 0xf8 | sps->bit_depth_luma_minus8;
  data[18] = 0xf8 | sps->bit_depth_chroma_minus8;
  data[19] = 0x00;              /* keep avgFrameRate as unspecified */
  data[20] = 0x00;              /* keep avgFrameRate as unspecified */
  /* constFrameRate(2 bits): 0, stream may or may not be of constant framerate
   * numTemporalLayers (3 bits): number of temporal layers, value from SPS
   * TemporalIdNested (1 bit): sps_temporal_id_nesting_flag from SPS
   * lengthSizeMinusOne (2 bits): plus 1 indicates the length of the NALUnitLength */
  data[21] =
      0x00 | ((sps->max_sub_layers_minus1 +
          1) << 3) | (sps->temporal_id_nesting_flag << 2) | (nl - 1);
  GST_WRITE_UINT8 (data + 22, num_arrays);      /* numOfArrays */

  data += 23;

  /* VPS */
  if (num_vps > 0) {
    /* array_completeness | reserved_zero bit | nal_unit_type */
    data[0] = 0x00 | 0x20;
    data++;

    GST_WRITE_UINT16_BE (data, num_vps);
    data += 2;

    for (i = 0; i < GST_H265_MAX_VPS_COUNT; i++) {
      if ((nal = parse->vps_nals[i])) {
        gsize nal_size = gst_buffer_get_size (nal);
        GST_WRITE_UINT16_BE (data, nal_size);
        gst_buffer_extract (nal, 0, data + 2, nal_size);
        data += 2 + nal_size;
      }
    }
  }

  /* SPS */
  if (num_sps > 0) {
    /* array_completeness | reserved_zero bit | nal_unit_type */
    data[0] = 0x00 | 0x21;
    data++;

    GST_WRITE_UINT16_BE (data, num_sps);
    data += 2;

    for (i = 0; i < GST_H265_MAX_SPS_COUNT; i++) {
      if ((nal = parse->sps_nals[i])) {
        gsize nal_size = gst_buffer_get_size (nal);
        GST_WRITE_UINT16_BE (data, nal_size);
        gst_buffer_extract (nal, 0, data + 2, nal_size);
        data += 2 + nal_size;
      }
    }
  }

  /* PPS */
  if (num_pps > 0) {
    /* array_completeness | reserved_zero bit | nal_unit_type */
    data[0] = 0x00 | 0x22;
    data++;

    GST_WRITE_UINT16_BE (data, num_pps);
    data += 2;

    for (i = 0; i < GST_H265_MAX_PPS_COUNT; i++) {
      if ((nal = parse->pps_nals[i])) {
        gsize nal_size = gst_buffer_get_size (nal);
        GST_WRITE_UINT16_BE (data, nal_size);
        gst_buffer_extract (nal, 0, data + 2, nal_size);
        data += 2 + nal_size;
      }
    }
  }
  gst_buffer_unmap (buf, &map);

  return buf;
}

static const gchar *
digit_to_string (guint digit)
{
  static const char itoa[][2] = {
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9"
  };

  if (G_LIKELY (digit < 10))
    return itoa[digit];
  else
    return NULL;
}

static const gchar *
get_profile_string (GstH265Profile profile)
{
  switch (profile) {
    case GST_H265_PROFILE_MAIN:
      return "main";
    case GST_H265_PROFILE_MAIN_10:
      return "main-10";
    case GST_H265_PROFILE_MAIN_STILL_PICTURE:
      return "main-still-picture";
    case GST_H265_PROFILE_MONOCHROME:
      return "monochrome";
    case GST_H265_PROFILE_MONOCHROME_12:
      return "monochrome-12";
    case GST_H265_PROFILE_MONOCHROME_16:
      return "monochrome-16";
    case GST_H265_PROFILE_MAIN_12:
      return "main-12";
    case GST_H265_PROFILE_MAIN_422_10:
      return "main-422-10";
    case GST_H265_PROFILE_MAIN_422_12:
      return "main-422-12";
    case GST_H265_PROFILE_MAIN_444:
      return "main-444";
    case GST_H265_PROFILE_MAIN_444_10:
      return "main-444-10";
    case GST_H265_PROFILE_MAIN_444_12:
      return "main-444-12";
    case GST_H265_PROFILE_MAIN_INTRA:
      return "main-intra";
    case GST_H265_PROFILE_MAIN_10_INTRA:
      return "main-10-intra";
    case GST_H265_PROFILE_MAIN_12_INTRA:
      return "main-12-intra";
    case GST_H265_PROFILE_MAIN_422_10_INTRA:
      return "main-422-10-intra";
    case GST_H265_PROFILE_MAIN_422_12_INTRA:
      return "main-422-12-intra";
    case GST_H265_PROFILE_MAIN_444_INTRA:
      return "main-444-intra";
    case GST_H265_PROFILE_MAIN_444_10_INTRA:
      return "main-444-10-intra";
    case GST_H265_PROFILE_MAIN_444_12_INTRA:
      return "main-444-12-intra";
    case GST_H265_PROFILE_MAIN_444_16_INTRA:
      return "main-444-16-intra";
    case GST_H265_PROFILE_MAIN_444_STILL_PICTURE:
      return "main-444-still-picture";
    case GST_H265_PROFILE_MAIN_444_16_STILL_PICTURE:
      return "main-444-16-still-picture";
    default:
      break;
  }

  return NULL;
}

static const gchar *
get_tier_string (guint8 tier_flag)
{
  const gchar *tier = NULL;

  if (tier_flag)
    tier = "high";
  else
    tier = "main";

  return tier;
}

static const gchar *
get_level_string (guint8 level_idc)
{
  if (level_idc == 0)
    return NULL;
  else if (level_idc % 30 == 0)
    return digit_to_string (level_idc / 30);
  else {
    switch (level_idc) {
      case 63:
        return "2.1";
        break;
      case 93:
        return "3.1";
        break;
      case 123:
        return "4.1";
        break;
      case 153:
        return "5.1";
        break;
      case 156:
        return "5.2";
        break;
      case 183:
        return "6.1";
        break;
      case 186:
        return "6.2";
        break;
      default:
        return NULL;
    }
  }
}

static GstCaps *
gst_h265_parse_get_compatible_profile_caps (GstH26XBaseParse * parse)
{
  GstH265Parse *h265parse = GST_H265_PARSE (parse);
  GstCaps *caps = NULL;
  const gchar **profiles = NULL;
  gint i;
  GstH265SPS *sps;
  GValue compat_profiles = G_VALUE_INIT;
  g_value_init (&compat_profiles, GST_TYPE_LIST);

  sps = h265parse->nalparser->last_sps;

  if (G_UNLIKELY (!sps))
    return NULL;

  switch (sps->profile_tier_level.profile_idc) {
    case GST_H265_PROFILE_IDC_MAIN_10:
      if (sps->profile_tier_level.profile_compatibility_flag[1]) {
        if (sps->profile_tier_level.profile_compatibility_flag[3]) {
          static const gchar *profile_array[] =
              { "main", "main-still-picture", NULL };
          profiles = profile_array;
        } else {
          static const gchar *profile_array[] = { "main", NULL };
          profiles = profile_array;
        }
      }
      break;
    case GST_H265_PROFILE_IDC_MAIN:
      if (sps->profile_tier_level.profile_compatibility_flag[3]) {
        static const gchar *profile_array[] =
            { "main-still-picture", "main-10", NULL
        };
        profiles = profile_array;
      } else {
        static const gchar *profile_array[] = { "main-10", NULL };
        profiles = profile_array;
      }
      break;
    case GST_H265_PROFILE_IDC_MAIN_STILL_PICTURE:
    {
      static const gchar *profile_array[] = { "main", "main-10", NULL
      };
      profiles = profile_array;
    }
      break;
    default:
      break;
  }

  if (profiles) {
    GValue value = G_VALUE_INIT;
    caps = gst_caps_new_empty_simple ("video/x-h265");
    for (i = 0; profiles[i]; i++) {
      g_value_init (&value, G_TYPE_STRING);
      g_value_set_string (&value, profiles[i]);
      gst_value_list_append_value (&compat_profiles, &value);
      g_value_unset (&value);
    }
    gst_caps_set_value (caps, "profile", &compat_profiles);
    g_value_unset (&compat_profiles);
  }

  return caps;
}

static gboolean
gst_h265_parse_fill_sps_info (GstH26XBaseParse * parse,
    GstH26XBaseParseSPSInfo * info)
{
  GstH265Parse *h265parse = GST_H265_PARSE (parse);
  GstH265SPS *sps;
  GstH265Profile ptl;

  g_return_val_if_fail (h265parse->nalparser != NULL, FALSE);

  sps = h265parse->nalparser->last_sps;

  if (G_UNLIKELY (!sps))
    return FALSE;

  if (sps->conformance_window_flag) {
    info->width = sps->crop_rect_width;
    info->height = sps->crop_rect_height;
  } else {
    info->width = sps->width;
    info->height = sps->height;
  }

  /* 0/1 is set as the default in the codec parser */
  if (sps->vui_params.timing_info_present_flag &&
      !(sps->fps_num == 0 && sps->fps_den == 1)) {
    info->fps_num = sps->fps_num;
    info->fps_den = sps->fps_den;
  }

  if (sps->vui_params.aspect_ratio_info_present_flag) {
    info->par_num = sps->vui_params.par_n;
    info->par_den = sps->vui_params.par_d;
  }

  /* FIXME: update interlace mode */
  info->interlace_mode = GST_VIDEO_INTERLACE_MODE_PROGRESSIVE;

  info->chroma_format = NULL;
  info->bit_depth_chroma = sps->bit_depth_chroma_minus8 + 8;
  info->bit_depth_luma = sps->bit_depth_luma_minus8 + 8;

  switch (sps->chroma_format_idc) {
    case 0:
      info->chroma_format = "4:0:0";
      info->bit_depth_chroma = 0;
      break;
    case 1:
      info->chroma_format = "4:2:0";
      break;
    case 2:
      info->chroma_format = "4:2:2";
      break;
    case 3:
      info->chroma_format = "4:4:4";
      break;
    default:
      break;
  }

  ptl = gst_h265_profile_tier_level_get_profile (&sps->profile_tier_level);
  info->profile = get_profile_string (ptl);
  info->level = get_level_string (sps->profile_tier_level.level_idc);
  info->tier = get_tier_string (sps->profile_tier_level.tier_flag);

  return TRUE;
}

static gboolean
gst_h265_parse_fixate_format (GstH26XBaseParse * parse, guint * format,
    guint * align, const GValue * codec_data_value)
{
  if (*format == GST_H265_PARSE_FORMAT_NONE) {
    /* codec_data implies packetized format */
    if (codec_data_value != NULL) {
      GST_ERROR ("video/x-h265 caps with codec_data but no stream-format=avc");
      *format = GST_H265_PARSE_FORMAT_HVC1;
    } else {
      /* otherwise assume bytestream input */
      GST_ERROR ("video/x-h265 caps without codec_data or stream-format");
      *format = GST_H265_PARSE_FORMAT_BYTE;
    }
  }

  if (*format != GST_H265_PARSE_FORMAT_BYTE) {
    /* packetized format implies alignment=au */
    if (*align == GST_H26X_BASE_PARSE_ALIGN_NONE)
      *align = GST_H26X_BASE_PARSE_ALIGN_AU;
  }

  /* bytestream caps sanity checks */
  if (*format == GST_H265_PARSE_FORMAT_BYTE) {
    /* should have SPS/PSS in-band (and/or oob in streamheader field) */
    if (codec_data_value != NULL)
      goto bytestream_format_with_codec_data;
  }

  return TRUE;

bytestream_format_with_codec_data:
  {
    GST_WARNING_OBJECT (parse, "HEVC bytestream format with codec_data is not "
        "expected, send SPS/PPS in-band with data or in streamheader field");
    return FALSE;
  }
}

static gboolean
gst_h265_parse_handle_codec_data (GstH26XBaseParse * parse, GstMapInfo * map)
{
  GstH265Parse *h265parse = GST_H265_PARSE (parse);
  gsize off, size;
  guint8 *data;
  guint num_nals, i, j;
  GstH265NalUnit nalu;
  GstH265ParserResult parseres;
  guint num_nal_arrays;

  data = map->data;
  size = map->size;

  /* parse the hvcC data */
  if (size < 23) {
    goto hvcc_too_small;
  }
  /* parse the version, this must be one but
   * is zero until the spec is finalized */
  if (data[0] != 0 && data[0] != 1) {
    goto wrong_version;
  }

  parse->nal_length_size = (data[21] & 0x03) + 1;
  GST_DEBUG_OBJECT (h265parse, "nal length size %u", parse->nal_length_size);

  num_nal_arrays = data[22];
  off = 23;

  for (i = 0; i < num_nal_arrays; i++) {
    if (off + 3 >= size) {
      goto hvcc_too_small;
    }

    num_nals = GST_READ_UINT16_BE (data + off + 1);
    off += 3;
    for (j = 0; j < num_nals; j++) {
      parseres = gst_h265_parser_identify_nalu_hevc (h265parse->nalparser,
          data, off, size, 2, &nalu);

      if (parseres != GST_H265_PARSER_OK) {
        goto hvcc_too_small;
      }

      gst_h265_parse_process_nal (h265parse, &nalu);
      off = nalu.offset + nalu.size;
    }
  }

  return TRUE;

  /* ERRORS */
hvcc_too_small:
  {
    GST_DEBUG_OBJECT (h265parse, "hvcC size %" G_GSIZE_FORMAT " < 23", size);
    return FALSE;
  }
wrong_version:
  {
    GST_DEBUG_OBJECT (h265parse, "wrong hvcC version");
    return FALSE;
  }
}
