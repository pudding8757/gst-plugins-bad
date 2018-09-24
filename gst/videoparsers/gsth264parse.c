/* GStreamer H.264 Parser
 * Copyright (C) <2010> Collabora ltd
 * Copyright (C) <2010> Nokia Corporation
 * Copyright (C) <2011> Intel Corporation
 *
 * Copyright (C) <2010> Mark Nauwelaerts <mark.nauwelaerts@collabora.co.uk>
 * Copyright (C) <2011> Thibault Saunier <thibault.saunier@collabora.com>
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
#include "gsth264parse.h"

#include <string.h>

GST_DEBUG_CATEGORY (h264_parse_debug);
#define GST_CAT_DEFAULT h264_parse_debug

#define GST_H264_PARSE_MIN_NALU_SIZE 5

enum
{
  GST_H264_PARSE_FORMAT_NONE = GST_H26X_BASE_PARSE_FORMAT_NONE,
  GST_H264_PARSE_FORMAT_BYTE = GST_H26X_BASE_PARSE_FORMAT_BYTE,
  GST_H264_PARSE_FORMAT_AVC,
  GST_H264_PARSE_FORMAT_AVC3
};

static GstStaticPadTemplate sinktemplate = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-h264"));

static GstStaticPadTemplate srctemplate = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-h264, parsed = (boolean) true, "
        "stream-format=(string) { avc, avc3, byte-stream }, "
        "alignment=(string) { au, nal }"));

#define parent_class gst_h264_parse_parent_class
G_DEFINE_TYPE (GstH264Parse, gst_h264_parse, GST_TYPE_H26X_BASE_PARSE);

static gboolean gst_h264_parse_start (GstBaseParse * parse);
static gboolean gst_h264_parse_stop (GstBaseParse * parse);

static void gst_h264_parse_get_max_vps_sps_pps_count (GstH26XBaseParse * parse,
    guint * max_vps_count, guint * max_sps_count, guint * max_pps_count);
static guint gst_h264_parse_get_min_nalu_size (GstH26XBaseParse * parse);
static const gchar *gst_h264_parse_format_to_string (GstH26XBaseParse * parse,
    guint format);
static guint gst_h264_parse_format_from_string (GstH26XBaseParse * parse,
    const gchar * format);
static GstCaps *gst_h264_parse_get_default_caps (GstH26XBaseParse * parse);
static gboolean gst_h264_parse_has_last_sps (GstH26XBaseParse * parse);
static gboolean gst_h264_parse_fill_sps_info (GstH26XBaseParse * parse,
    GstH26XBaseParseSPSInfo * info);
static gboolean
gst_h264_parse_fill_profile_tier_level (GstH26XBaseParse * parse,
    GstH26XBaseParseProfileTierLevel * ptl);
static GstCaps *gst_h264_parse_get_compatible_profile_caps_from_last_sps
    (GstH26XBaseParse * parse);
static GstBuffer *gst_h264_parse_prepare_pre_push_frame (GstH26XBaseParse *
    parse, GstBaseParseFrame * frame);
static GstBuffer *gst_h264_parse_make_codec_data (GstH26XBaseParse * parse);
static GstFlowReturn
gst_h264_parse_handle_frame_packetized (GstH26XBaseParse * parse,
    GstBaseParseFrame * frame);
static GstH26XBaseParseHandleFrameReturn
gst_h264_parse_handle_frame_check_initial_skip (GstH26XBaseParse * parse,
    gint * skipsize, gint * dropsize, GstMapInfo * map);
static GstH26XBaseParseHandleFrameReturn
gst_h264_parse_handle_frame_bytestream (GstH26XBaseParse * parse,
    gint * skipsize, gint * framesize, gint * current_offset,
    GstMapInfo * map, gboolean drain);
static gboolean
gst_h264_parse_fixate_format (GstH26XBaseParse * parse, guint * format,
    guint * align, const GValue * codec_data_value);
static gboolean
gst_h264_parse_handle_codec_data (GstH26XBaseParse * parse, GstMapInfo * map);
static void
gst_h264_parse_get_timestamp (GstH26XBaseParse * parse,
    GstClockTime * out_ts, GstClockTime * out_dur);

static void
gst_h264_parse_class_init (GstH264ParseClass * klass)
{
  GstBaseParseClass *parse_class = GST_BASE_PARSE_CLASS (klass);
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS (klass);
  GstH26XBaseParseClass *h26xbase_class = GST_H26X_BASE_PARSE_CLASS (klass);

  GST_DEBUG_CATEGORY_INIT (h264_parse_debug, "h264parse", 0, "h264 parser");

  /* Override BaseParse vfuncs */
  parse_class->start = GST_DEBUG_FUNCPTR (gst_h264_parse_start);
  parse_class->stop = GST_DEBUG_FUNCPTR (gst_h264_parse_stop);

  h26xbase_class->get_max_vps_sps_pps_count =
      GST_DEBUG_FUNCPTR (gst_h264_parse_get_max_vps_sps_pps_count);
  h26xbase_class->get_min_nalu_size =
      GST_DEBUG_FUNCPTR (gst_h264_parse_get_min_nalu_size);
  h26xbase_class->format_to_string =
      GST_DEBUG_FUNCPTR (gst_h264_parse_format_to_string);
  h26xbase_class->format_from_string =
      GST_DEBUG_FUNCPTR (gst_h264_parse_format_from_string);
  h26xbase_class->get_default_caps =
      GST_DEBUG_FUNCPTR (gst_h264_parse_get_default_caps);
  h26xbase_class->has_last_sps =
      GST_DEBUG_FUNCPTR (gst_h264_parse_has_last_sps);
  h26xbase_class->fill_sps_info =
      GST_DEBUG_FUNCPTR (gst_h264_parse_fill_sps_info);
  h26xbase_class->fill_profile_tier_level =
      GST_DEBUG_FUNCPTR (gst_h264_parse_fill_profile_tier_level);
  h26xbase_class->get_compatible_profile_caps_from_last_sps =
      GST_DEBUG_FUNCPTR
      (gst_h264_parse_get_compatible_profile_caps_from_last_sps);
  h26xbase_class->prepare_pre_push_frame =
      GST_DEBUG_FUNCPTR (gst_h264_parse_prepare_pre_push_frame);
  h26xbase_class->make_codec_data =
      GST_DEBUG_FUNCPTR (gst_h264_parse_make_codec_data);
  h26xbase_class->handle_frame_packetized =
      GST_DEBUG_FUNCPTR (gst_h264_parse_handle_frame_packetized);
  h26xbase_class->handle_frame_check_initial_skip =
      GST_DEBUG_FUNCPTR (gst_h264_parse_handle_frame_check_initial_skip);
  h26xbase_class->handle_frame_bytestream =
      GST_DEBUG_FUNCPTR (gst_h264_parse_handle_frame_bytestream);
  h26xbase_class->fixate_format =
      GST_DEBUG_FUNCPTR (gst_h264_parse_fixate_format);
  h26xbase_class->handle_codec_data =
      GST_DEBUG_FUNCPTR (gst_h264_parse_handle_codec_data);
  h26xbase_class->get_timestamp =
      GST_DEBUG_FUNCPTR (gst_h264_parse_get_timestamp);

  gst_element_class_add_static_pad_template (gstelement_class, &srctemplate);
  gst_element_class_add_static_pad_template (gstelement_class, &sinktemplate);

  gst_element_class_set_static_metadata (gstelement_class, "H.264 parser",
      "Codec/Parser/Converter/Video",
      "Parses H.264 streams",
      "Mark Nauwelaerts <mark.nauwelaerts@collabora.co.uk>");
}

static void
gst_h264_parse_init (GstH264Parse * h264parse)
{
}

static gboolean
gst_h264_parse_start (GstBaseParse * parse)
{
  GstH264Parse *h264parse = GST_H264_PARSE (parse);

  GST_DEBUG_OBJECT (parse, "start");

  h264parse->nalparser = gst_h264_nal_parser_new ();
  h264parse->sei_pic_struct_pres_flag = FALSE;
  h264parse->sei_pic_struct = 0;
  h264parse->field_pic_flag = 0;

  return GST_BASE_PARSE_CLASS (parent_class)->start (parse);
}

static gboolean
gst_h264_parse_stop (GstBaseParse * parse)
{
  GstH264Parse *h264parse = GST_H264_PARSE (parse);

  GST_DEBUG_OBJECT (parse, "stop");

  gst_h264_nal_parser_free (h264parse->nalparser);

  return GST_BASE_PARSE_CLASS (parent_class)->stop (parse);
}

static void
gst_h264_parse_get_max_vps_sps_pps_count (GstH26XBaseParse * parse,
    guint * max_vps_count, guint * max_sps_count, guint * max_pps_count)
{
  *max_vps_count = 0;
  *max_sps_count = GST_H264_MAX_SPS_COUNT;
  *max_pps_count = GST_H264_MAX_PPS_COUNT;
}

static guint
gst_h264_parse_get_min_nalu_size (GstH26XBaseParse * parse)
{
  return GST_H264_PARSE_MIN_NALU_SIZE;
}

static const gchar *
gst_h264_parse_format_to_string (GstH26XBaseParse * parse, guint format)
{
  switch (format) {
    case GST_H264_PARSE_FORMAT_AVC:
      return "avc";
    case GST_H264_PARSE_FORMAT_BYTE:
      return "byte-stream";
    case GST_H264_PARSE_FORMAT_AVC3:
      return "avc3";
    default:
      return "none";
  }
}

static guint
gst_h264_parse_format_from_string (GstH26XBaseParse * parse,
    const gchar * format)
{
  guint ret = GST_H264_PARSE_FORMAT_NONE;

  g_return_val_if_fail (format, GST_H264_PARSE_FORMAT_NONE);

  if (g_strcmp0 (format, "avc") == 0)
    ret = GST_H264_PARSE_FORMAT_AVC;
  else if (g_strcmp0 (format, "byte-stream") == 0)
    ret = GST_H264_PARSE_FORMAT_BYTE;
  else if (g_strcmp0 (format, "avc3") == 0)
    ret = GST_H264_PARSE_FORMAT_AVC3;

  return ret;
}

static GstCaps *
gst_h264_parse_get_default_caps (GstH26XBaseParse * parse)
{
  return gst_caps_new_empty_simple ("video/x-h264");
}

static void
gst_h264_parser_store_nal (GstH264Parse * h264parse, guint id,
    GstH264NalUnitType naltype, GstH264NalUnit * nalu)
{
  GstH26XBaseParse *parse = GST_H26X_BASE_PARSE (h264parse);
  GstBuffer *buf, **store;
  guint size = nalu->size, store_size;

  if (naltype == GST_H264_NAL_SPS || naltype == GST_H264_NAL_SUBSET_SPS) {
    store_size = GST_H264_MAX_SPS_COUNT;
    store = parse->sps_nals;
    GST_DEBUG_OBJECT (h264parse, "storing sps %u", id);
  } else if (naltype == GST_H264_NAL_PPS) {
    store_size = GST_H264_MAX_PPS_COUNT;
    store = parse->pps_nals;
    GST_DEBUG_OBJECT (h264parse, "storing pps %u", id);
  } else
    return;

  if (id >= store_size) {
    GST_DEBUG_OBJECT (h264parse, "unable to store nal, id out-of-range %d", id);
    return;
  }

  buf = gst_buffer_new_allocate (NULL, size, NULL);
  gst_buffer_fill (buf, 0, nalu->data + nalu->offset, size);

  /* Indicate that buffer contain a header needed for decoding */
  if (naltype == GST_H264_NAL_SPS || naltype == GST_H264_NAL_PPS)
    GST_BUFFER_FLAG_SET (buf, GST_BUFFER_FLAG_HEADER);

  if (store[id])
    gst_buffer_unref (store[id]);

  store[id] = buf;
}

#ifndef GST_DISABLE_GST_DEBUG
static const gchar *nal_names[] = {
  "Unknown",
  "Slice",
  "Slice DPA",
  "Slice DPB",
  "Slice DPC",
  "Slice IDR",
  "SEI",
  "SPS",
  "PPS",
  "AU delimiter",
  "Sequence End",
  "Stream End",
  "Filler Data",
  "SPS extension",
  "Prefix",
  "SPS Subset",
  "Depth Parameter Set",
  "Reserved", "Reserved",
  "Slice Aux Unpartitioned",
  "Slice Extension",
  "Slice Depth/3D-AVC Extension"
};

static const gchar *
_nal_name (GstH264NalUnitType nal_type)
{
  if (nal_type <= GST_H264_NAL_SLICE_DEPTH)
    return nal_names[nal_type];
  return "Invalid";
}
#endif

static void
gst_h264_parse_process_sei (GstH264Parse * h264parse, GstH264NalUnit * nalu)
{
  GstH26XBaseParse *parse = GST_H26X_BASE_PARSE (h264parse);
  GstH264SEIMessage sei;
  GstH264NalParser *nalparser = h264parse->nalparser;
  GstH264ParserResult pres;
  GArray *messages;
  guint i;

  pres = gst_h264_parser_parse_sei (nalparser, nalu, &messages);
  if (pres != GST_H264_PARSER_OK)
    GST_WARNING_OBJECT (h264parse, "failed to parse one or more SEI message");

  /* Even if pres != GST_H264_PARSER_OK, some message could have been parsed and
   * stored in messages.
   */
  for (i = 0; i < messages->len; i++) {
    sei = g_array_index (messages, GstH264SEIMessage, i);
    switch (sei.payloadType) {
      case GST_H264_SEI_PIC_TIMING:
        h264parse->sei_pic_struct_pres_flag =
            sei.payload.pic_timing.pic_struct_present_flag;
        h264parse->sei_cpb_removal_delay =
            sei.payload.pic_timing.cpb_removal_delay;
        if (h264parse->sei_pic_struct_pres_flag)
          h264parse->sei_pic_struct = sei.payload.pic_timing.pic_struct;
        GST_LOG_OBJECT (h264parse, "pic timing updated");
        break;
      case GST_H264_SEI_BUF_PERIOD:
        if (parse->ts_trn_nb == GST_CLOCK_TIME_NONE ||
            parse->dts == GST_CLOCK_TIME_NONE)
          parse->ts_trn_nb = 0;
        else
          parse->ts_trn_nb = parse->dts;

        GST_LOG_OBJECT (h264parse,
            "new buffering period; ts_trn_nb updated: %" GST_TIME_FORMAT,
            GST_TIME_ARGS (parse->ts_trn_nb));
        break;

        /* Additional messages that are not innerly useful to the
         * element but for debugging purposes */
      case GST_H264_SEI_RECOVERY_POINT:
        GST_LOG_OBJECT (h264parse, "recovery point found: %u %u %u %u",
            sei.payload.recovery_point.recovery_frame_cnt,
            sei.payload.recovery_point.exact_match_flag,
            sei.payload.recovery_point.broken_link_flag,
            sei.payload.recovery_point.changing_slice_group_idc);
        break;

        /* Additional messages that are not innerly useful to the
         * element but for debugging purposes */
      case GST_H264_SEI_STEREO_VIDEO_INFO:{
        GstVideoMultiviewMode mview_mode = GST_VIDEO_MULTIVIEW_MODE_NONE;
        GstVideoMultiviewFlags mview_flags = GST_VIDEO_MULTIVIEW_FLAGS_NONE;

        GST_LOG_OBJECT (h264parse, "Stereo video information %u %u %u %u %u %u",
            sei.payload.stereo_video_info.field_views_flag,
            sei.payload.stereo_video_info.top_field_is_left_view_flag,
            sei.payload.stereo_video_info.current_frame_is_left_view_flag,
            sei.payload.stereo_video_info.next_frame_is_second_view_flag,
            sei.payload.stereo_video_info.left_view_self_contained_flag,
            sei.payload.stereo_video_info.right_view_self_contained_flag);

        if (sei.payload.stereo_video_info.field_views_flag) {
          mview_mode = GST_VIDEO_MULTIVIEW_MODE_ROW_INTERLEAVED;
          if (!sei.payload.stereo_video_info.top_field_is_left_view_flag)
            mview_mode |= GST_VIDEO_MULTIVIEW_FLAGS_RIGHT_VIEW_FIRST;
        } else {
          mview_mode = GST_VIDEO_MULTIVIEW_MODE_FRAME_BY_FRAME;
          if (sei.payload.stereo_video_info.next_frame_is_second_view_flag) {
            /* Mark current frame as first in bundle */
            parse->first_in_bundle = TRUE;
            if (!sei.payload.stereo_video_info.current_frame_is_left_view_flag)
              mview_flags |= GST_VIDEO_MULTIVIEW_FLAGS_RIGHT_VIEW_FIRST;
          }
        }
        if (mview_mode != parse->multiview_mode ||
            mview_flags != parse->multiview_flags) {
          parse->multiview_mode = mview_mode;
          parse->multiview_flags = mview_flags;
          /* output caps need to be changed */
          gst_h26x_base_parse_update_src_caps (parse, NULL);
        }
        break;
      }
      case GST_H264_SEI_FRAME_PACKING:{
        GstVideoMultiviewMode mview_mode = GST_VIDEO_MULTIVIEW_MODE_NONE;
        GstVideoMultiviewFlags mview_flags = GST_VIDEO_MULTIVIEW_FLAGS_NONE;

        GST_LOG_OBJECT (h264parse,
            "frame packing arrangement message: id %u cancelled %u "
            "type %u quincunx %u content_interpretation %d flip %u "
            "right_first %u field_views %u is_frame0 %u",
            sei.payload.frame_packing.frame_packing_id,
            sei.payload.frame_packing.frame_packing_cancel_flag,
            sei.payload.frame_packing.frame_packing_type,
            sei.payload.frame_packing.quincunx_sampling_flag,
            sei.payload.frame_packing.content_interpretation_type,
            sei.payload.frame_packing.spatial_flipping_flag,
            sei.payload.frame_packing.frame0_flipped_flag,
            sei.payload.frame_packing.field_views_flag,
            sei.payload.frame_packing.current_frame_is_frame0_flag);

        /* Only IDs from 0->255 and 512->2^31-1 are valid. Ignore others */
        if ((sei.payload.frame_packing.frame_packing_id >= 256 &&
                sei.payload.frame_packing.frame_packing_id < 512) ||
            (sei.payload.frame_packing.frame_packing_id >= (1U << 31)))
          break;                /* ignore */

        if (!sei.payload.frame_packing.frame_packing_cancel_flag) {
          /* Cancel flag sets things back to no-info */

          if (sei.payload.frame_packing.content_interpretation_type == 2)
            mview_flags |= GST_VIDEO_MULTIVIEW_FLAGS_RIGHT_VIEW_FIRST;

          switch (sei.payload.frame_packing.frame_packing_type) {
            case 0:
              mview_mode = GST_VIDEO_MULTIVIEW_MODE_CHECKERBOARD;
              break;
            case 1:
              mview_mode = GST_VIDEO_MULTIVIEW_MODE_COLUMN_INTERLEAVED;
              break;
            case 2:
              mview_mode = GST_VIDEO_MULTIVIEW_MODE_ROW_INTERLEAVED;
              break;
            case 3:
              if (sei.payload.frame_packing.quincunx_sampling_flag)
                mview_mode = GST_VIDEO_MULTIVIEW_MODE_SIDE_BY_SIDE_QUINCUNX;
              else
                mview_mode = GST_VIDEO_MULTIVIEW_MODE_SIDE_BY_SIDE;
              if (sei.payload.frame_packing.spatial_flipping_flag) {
                /* One of the views is flopped. */
                if (sei.payload.frame_packing.frame0_flipped_flag !=
                    ! !(mview_flags &
                        GST_VIDEO_MULTIVIEW_FLAGS_RIGHT_VIEW_FIRST))
                  /* the left view is flopped */
                  mview_flags |= GST_VIDEO_MULTIVIEW_FLAGS_LEFT_FLOPPED;
                else
                  mview_flags |= GST_VIDEO_MULTIVIEW_FLAGS_RIGHT_FLOPPED;
              }
              break;
            case 4:
              mview_mode = GST_VIDEO_MULTIVIEW_MODE_TOP_BOTTOM;
              if (sei.payload.frame_packing.spatial_flipping_flag) {
                /* One of the views is flipped, */
                if (sei.payload.frame_packing.frame0_flipped_flag !=
                    ! !(mview_flags &
                        GST_VIDEO_MULTIVIEW_FLAGS_RIGHT_VIEW_FIRST))
                  /* the left view is flipped */
                  mview_flags |= GST_VIDEO_MULTIVIEW_FLAGS_LEFT_FLIPPED;
                else
                  mview_flags |= GST_VIDEO_MULTIVIEW_FLAGS_RIGHT_FLIPPED;
              }
              break;
            case 5:
              if (sei.payload.frame_packing.content_interpretation_type == 0)
                mview_mode = GST_VIDEO_MULTIVIEW_MODE_MULTIVIEW_FRAME_BY_FRAME;
              else
                mview_mode = GST_VIDEO_MULTIVIEW_MODE_FRAME_BY_FRAME;
              break;
            default:
              GST_DEBUG_OBJECT (h264parse, "Invalid frame packing type %u",
                  sei.payload.frame_packing.frame_packing_type);
              break;
          }
        }

        if (mview_mode != parse->multiview_mode ||
            mview_flags != parse->multiview_flags) {
          parse->multiview_mode = mview_mode;
          parse->multiview_flags = mview_flags;
          /* output caps need to be changed */
          gst_h26x_base_parse_update_src_caps (parse, NULL);
        }
        break;
      }
    }
  }
  g_array_free (messages, TRUE);
}

/* caller guarantees 2 bytes of nal payload */
static gboolean
gst_h264_parse_process_nal (GstH264Parse * h264parse, GstH264NalUnit * nalu)
{
  GstH26XBaseParse *parse = GST_H26X_BASE_PARSE (h264parse);
  guint nal_type;
  GstH264PPS pps = { 0, };
  GstH264SPS sps = { 0, };
  GstH264NalParser *nalparser = h264parse->nalparser;
  GstH264ParserResult pres;

  /* nothing to do for broken input */
  if (G_UNLIKELY (nalu->size < 2)) {
    GST_DEBUG_OBJECT (h264parse, "not processing nal size %u", nalu->size);
    return TRUE;
  }

  /* we have a peek as well */
  nal_type = nalu->type;

  GST_DEBUG_OBJECT (h264parse, "processing nal of type %u %s, size %u",
      nal_type, _nal_name (nal_type), nalu->size);

  switch (nal_type) {
    case GST_H264_NAL_SUBSET_SPS:
      if (!GST_H26X_BASE_PARSE_STATE_VALID (h264parse,
              GST_H26X_BASE_PARSE_STATE_GOT_SPS))
        return FALSE;
      pres = gst_h264_parser_parse_subset_sps (nalparser, nalu, &sps, TRUE);
      goto process_sps;

    case GST_H264_NAL_SPS:
      /* reset state, everything else is obsolete */
      parse->state = 0;
      pres = gst_h264_parser_parse_sps (nalparser, nalu, &sps, TRUE);

    process_sps:
      /* arranged for a fallback sps.id, so use that one and only warn */
      if (pres != GST_H264_PARSER_OK) {
        GST_WARNING_OBJECT (h264parse, "failed to parse SPS:");
        return FALSE;
      }

      gst_h264_parser_store_nal (h264parse, sps.id, nal_type, nalu);
      gst_h264_sps_clear (&sps);
      gst_h26x_base_parse_sps_parsed (parse);
      break;
    case GST_H264_NAL_PPS:
      /* expected state: got-sps */
      parse->state &= GST_H26X_BASE_PARSE_STATE_GOT_SPS;
      if (!GST_H26X_BASE_PARSE_STATE_VALID (h264parse,
              GST_H26X_BASE_PARSE_STATE_GOT_SPS))
        return FALSE;

      pres = gst_h264_parser_parse_pps (nalparser, nalu, &pps);
      /* arranged for a fallback pps.id, so use that one and only warn */
      if (pres != GST_H264_PARSER_OK) {
        GST_WARNING_OBJECT (h264parse, "failed to parse PPS:");
        if (pres != GST_H264_PARSER_BROKEN_LINK)
          return FALSE;
      }

      gst_h264_parser_store_nal (h264parse, pps.id, nal_type, nalu);
      gst_h264_pps_clear (&pps);
      gst_h26x_base_parse_pps_parsed (parse);
      break;
    case GST_H264_NAL_SEI:
      /* expected state: got-sps */
      if (!GST_H26X_BASE_PARSE_STATE_VALID (h264parse,
              GST_H26X_BASE_PARSE_STATE_GOT_SPS))
        return FALSE;

      gst_h264_parse_process_sei (h264parse, nalu);
      gst_h26x_base_parse_sei_parsed (parse, nalu->sc_offset);
      break;
    case GST_H264_NAL_SLICE:
    case GST_H264_NAL_SLICE_DPA:
    case GST_H264_NAL_SLICE_DPB:
    case GST_H264_NAL_SLICE_DPC:
    case GST_H264_NAL_SLICE_IDR:
    case GST_H264_NAL_SLICE_EXT:
      /* expected state: got-sps|got-pps (valid picture headers) */
      parse->state &= GST_H26X_BASE_PARSE_STATE_VALID_PICTURE_HEADERS;
      if (!GST_H26X_BASE_PARSE_STATE_VALID (h264parse,
              GST_H26X_BASE_PARSE_STATE_VALID_PICTURE_HEADERS))
        return FALSE;

      /* don't need to parse the whole slice (header) here */
      if (*(nalu->data + nalu->offset + nalu->header_bytes) & 0x80) {
        /* means first_mb_in_slice == 0 */
        /* real frame data */
        GST_DEBUG_OBJECT (h264parse, "first_mb_in_slice = 0");
        gst_h26x_base_parse_frame_started (parse);
      }
      if (nal_type == GST_H264_NAL_SLICE_EXT && !GST_H264_IS_MVC_NALU (nalu))
        break;
      {
        GstH264SliceHdr slice;

        pres = gst_h264_parser_parse_slice_hdr (nalparser, nalu, &slice,
            FALSE, FALSE);
        GST_DEBUG_OBJECT (h264parse,
            "parse result %d, first MB: %u, slice type: %u",
            pres, slice.first_mb_in_slice, slice.type);
        if (pres == GST_H264_PARSER_OK) {
          gboolean keyframe = FALSE;

          if (GST_H264_IS_I_SLICE (&slice) || GST_H264_IS_SI_SLICE (&slice))
            keyframe = TRUE;

          gst_h26x_base_parse_slice_hdr_parsed (parse, keyframe);
          h264parse->field_pic_flag = slice.field_pic_flag;
        }
      }
      if (G_LIKELY (nal_type != GST_H264_NAL_SLICE_IDR && !parse->push_codec))
        break;
      /* if we need to sneak codec NALs into the stream,
       * this is a good place, so fake it as IDR
       * (which should be at start anyway) */
      /* mark where config needs to go if interval expired */
      /* mind replacement buffer if applicable */
      gst_h26x_base_parse_update_idr_pos (parse, nalu->sc_offset);
      break;
    case GST_H264_NAL_AU_DELIMITER:
      /* Just accumulate AU Delimiter, whether it's before SPS or not */
      pres = gst_h264_parser_parse_nal (nalparser, nalu);
      if (pres != GST_H264_PARSER_OK)
        return FALSE;
      parse->aud_insert = FALSE;
      break;
    default:
      /* drop anything before the initial SPS */
      if (!GST_H26X_BASE_PARSE_STATE_VALID (h264parse,
              GST_H26X_BASE_PARSE_STATE_GOT_SPS))
        return FALSE;

      pres = gst_h264_parser_parse_nal (nalparser, nalu);
      if (pres != GST_H264_PARSER_OK)
        return FALSE;
      break;
  }

  gst_h26x_base_pares_finish_process_nal (parse,
      nalu->data + nalu->offset, nalu->size);

  return TRUE;
}

/* caller guarantees at least 2 bytes of nal payload for each nal
 * returns TRUE if next_nal indicates that nal terminates an AU */
static inline gboolean
gst_h264_parse_collect_nal (GstH264Parse * h264parse, const guint8 * data,
    guint size, GstH264NalUnit * nalu)
{
  GstH26XBaseParse *parse = GST_H26X_BASE_PARSE (h264parse);
  gboolean complete;
  GstH264ParserResult parse_res;
  GstH264NalUnitType nal_type = nalu->type;
  GstH264NalUnit nnalu;

  GST_DEBUG_OBJECT (h264parse, "parsing collected nal");
  parse_res = gst_h264_parser_identify_nalu_unchecked (h264parse->nalparser,
      data, nalu->offset + nalu->size, size, &nnalu);

  if (parse_res != GST_H264_PARSER_OK)
    return FALSE;

  /* determine if AU complete */
  GST_LOG_OBJECT (h264parse, "nal type: %d %s", nal_type, _nal_name (nal_type));
  /* coded slice NAL starts a picture,
   * i.e. other types become aggregated in front of it */
  parse->picture_start |= (nal_type == GST_H264_NAL_SLICE ||
      nal_type == GST_H264_NAL_SLICE_DPA || nal_type == GST_H264_NAL_SLICE_IDR);

  /* consider a coded slices (IDR or not) to start a picture,
   * (so ending the previous one) if first_mb_in_slice == 0
   * (non-0 is part of previous one) */
  /* NOTE this is not entirely according to Access Unit specs in 7.4.1.2.4,
   * but in practice it works in sane cases, needs not much parsing,
   * and also works with broken frame_num in NAL
   * (where spec-wise would fail) */
  nal_type = nnalu.type;
  complete = parse->picture_start && ((nal_type >= GST_H264_NAL_SEI &&
          nal_type <= GST_H264_NAL_AU_DELIMITER) ||
      (nal_type >= 14 && nal_type <= 18));

  GST_LOG_OBJECT (h264parse, "next nal type: %d %s", nal_type,
      _nal_name (nal_type));
  complete |= parse->picture_start && (nal_type == GST_H264_NAL_SLICE
      || nal_type == GST_H264_NAL_SLICE_DPA
      || nal_type == GST_H264_NAL_SLICE_IDR) &&
      /* first_mb_in_slice == 0 considered start of frame */
      (nnalu.data[nnalu.offset + nnalu.header_bytes] & 0x80);

  GST_LOG_OBJECT (h264parse, "au complete: %d", complete);

  return complete;
}

static guint8 au_delim[6] = {
  0x00, 0x00, 0x00, 0x01,       /* nal prefix */
  0x09,                         /* nal unit type = access unit delimiter */
  0xf0                          /* allow any slice type */
};

static GstFlowReturn
gst_h264_parse_handle_frame_packetized (GstH26XBaseParse * parse,
    GstBaseParseFrame * frame)
{
  GstH264Parse *h264parse = GST_H264_PARSE (parse);
  GstBaseParse *baseparse = GST_BASE_PARSE (parse);
  GstBuffer *buffer = frame->buffer;
  GstFlowReturn ret = GST_FLOW_OK;
  GstH264ParserResult parse_res;
  GstH264NalUnit nalu;
  const guint nl = parse->nal_length_size;
  GstMapInfo map;
  gint left;

  if (nl < 1 || nl > 4) {
    GST_DEBUG_OBJECT (parse, "insufficient data to split input");
    return GST_FLOW_NOT_NEGOTIATED;
  }

  /* need to save buffer from invalidation upon _finish_frame */
  if (parse->split_packetized)
    buffer = gst_buffer_copy (frame->buffer);

  gst_buffer_map (buffer, &map, GST_MAP_READ);

  left = map.size;

  GST_LOG_OBJECT (parse,
      "processing packet buffer of size %" G_GSIZE_FORMAT, map.size);

  parse_res = gst_h264_parser_identify_nalu_avc (h264parse->nalparser,
      map.data, 0, map.size, nl, &nalu);

  while (parse_res == GST_H264_PARSER_OK) {
    GST_DEBUG_OBJECT (parse, "AVC nal offset %d", nalu.offset + nalu.size);

    /* either way, have a look at it */
    gst_h264_parse_process_nal (h264parse, &nalu);

    /* dispatch per NALU if needed */
    if (parse->split_packetized) {
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

    parse_res = gst_h264_parser_identify_nalu_avc (h264parse->nalparser,
        map.data, nalu.offset + nalu.size, map.size, nl, &nalu);
  }

  gst_buffer_unmap (buffer, &map);

  if (!parse->split_packetized) {
    gst_h26x_base_parse_parse_frame (parse, frame);
    ret = gst_base_parse_finish_frame (baseparse, frame, map.size);
  } else {
    gst_buffer_unref (buffer);
    if (G_UNLIKELY (left)) {
      /* should not be happening for nice AVC */
      GST_WARNING_OBJECT (parse, "skipping leftover AVC data %d", left);
      frame->flags |= GST_BASE_PARSE_FRAME_FLAG_DROP;
      ret = gst_base_parse_finish_frame (baseparse, frame, map.size);
    }
  }

  if (parse_res == GST_H264_PARSER_NO_NAL_END ||
      parse_res == GST_H264_PARSER_BROKEN_DATA) {

    if (parse->split_packetized) {
      GST_ELEMENT_ERROR (parse, STREAM, FAILED, (NULL),
          ("invalid AVC input data"));

      return GST_FLOW_ERROR;
    } else {
      /* do not meddle to much in this case */
      GST_DEBUG_OBJECT (parse, "parsing packet failed");
    }
  }

  return ret;
}

static GstH26XBaseParseHandleFrameReturn
gst_h264_parse_handle_frame_check_initial_skip (GstH26XBaseParse * parse,
    gint * skipsize, gint * dropsize, GstMapInfo * map)
{
  GstH264Parse *h264parse = GST_H264_PARSE (parse);
  GstH264NalParser *nalparser = h264parse->nalparser;
  guint8 *data;
  gsize size;
  GstH264NalUnit nalu;
  GstH264ParserResult pres;

  data = map->data;
  size = map->size;

  pres =
      gst_h264_parser_identify_nalu_unchecked (nalparser, data, 0, size, &nalu);

  switch (pres) {
    case GST_H264_PARSER_OK:
      if (nalu.sc_offset > 0) {
        gint i;
        gboolean is_filler_data = TRUE;
        /* Handle filler data */
        for (i = 0; i < nalu.sc_offset; i++) {
          if (data[i] != 0x00) {
            is_filler_data = FALSE;
            break;
          }
        }
        if (is_filler_data) {
          GST_DEBUG_OBJECT (parse, "Dropping filler data %d", nalu.sc_offset);
          *dropsize = nalu.sc_offset;
          return GST_H26X_BASE_PARSE_HANDLE_FRAME_DROP;
        }
        *skipsize = nalu.sc_offset;
        return GST_H26X_BASE_PARSE_HANDLE_FRAME_SKIP;
      }
      break;
    case GST_H264_PARSER_NO_NAL:
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
gst_h264_parse_handle_frame_bytestream (GstH26XBaseParse * parse,
    gint * skipsize, gint * framesize, gint * current_offset,
    GstMapInfo * map, gboolean drain)
{
  GstH264Parse *h264parse = GST_H264_PARSE (parse);
  GstH264NalParser *nalparser = h264parse->nalparser;
  guint8 *data;
  gsize size;
  GstH264NalUnit nalu;
  GstH264ParserResult pres;
  gboolean nonext;
  gboolean au_complete;

  data = map->data;
  size = map->size;
  nonext = FALSE;

  while (TRUE) {
    pres =
        gst_h264_parser_identify_nalu (nalparser, data, *current_offset, size,
        &nalu);

    switch (pres) {
      case GST_H264_PARSER_OK:
        GST_DEBUG_OBJECT (parse, "complete nal (offset, size): (%u, %u) ",
            nalu.offset, nalu.size);
        break;
      case GST_H264_PARSER_NO_NAL_END:
        GST_DEBUG_OBJECT (parse, "not a complete nal found at offset %u",
            nalu.offset);
        /* if draining, accept it as complete nal */
        if (drain) {
          nonext = TRUE;
          nalu.size = size - nalu.offset;
          GST_DEBUG_OBJECT (parse, "draining, accepting with size %u",
              nalu.size);
          /* if it's not too short at least */
          if (nalu.size < 2)
            goto broken;
          break;
        }
        /* otherwise need more */
        return GST_H26X_BASE_PARSE_HANDLE_FRAME_MORE;
      case GST_H264_PARSER_BROKEN_LINK:
        GST_ELEMENT_ERROR (parse, STREAM, FORMAT,
            ("Error parsing H.264 stream"),
            ("The link to structure needed for the parsing couldn't be found"));
        return GST_H26X_BASE_PARSE_HANDLE_FRAME_INVALID_STREAM;
      case GST_H264_PARSER_ERROR:
        /* should not really occur either */
        GST_ELEMENT_ERROR (parse, STREAM, FORMAT,
            ("Error parsing H.264 stream"), ("Invalid H.264 stream"));
        return GST_H26X_BASE_PARSE_HANDLE_FRAME_INVALID_STREAM;
      case GST_H264_PARSER_NO_NAL:
        GST_ELEMENT_ERROR (parse, STREAM, FORMAT,
            ("Error parsing H.264 stream"), ("No H.264 NAL unit found"));
        return GST_H26X_BASE_PARSE_HANDLE_FRAME_INVALID_STREAM;
      case GST_H264_PARSER_BROKEN_DATA:
        GST_WARNING_OBJECT (parse, "input stream is corrupt; "
            "it contains a NAL unit of length %u", nalu.size);
      broken:
        /* broken nal at start -> arrange to skip it,
         * otherwise have it terminate current au
         * (and so it will be skipped on next frame round) */
        if (*current_offset == 0) {
          GST_DEBUG_OBJECT (parse, "skipping broken nal");
          *skipsize = nalu.offset;
          parse->aud_needed = TRUE;
          return GST_H26X_BASE_PARSE_HANDLE_FRAME_SKIP;
        } else {
          GST_DEBUG_OBJECT (parse, "terminating au");
          nalu.size = 0;
          nalu.offset = nalu.sc_offset;
          goto end;
        }
        break;
      default:
        g_assert_not_reached ();
        break;
    }

    GST_DEBUG_OBJECT (parse, "%p complete nal found. Off: %u, Size: %u",
        data, nalu.offset, nalu.size);

    if (!nonext) {
      if (nalu.offset + nalu.size + 4 + 2 > size) {
        GST_DEBUG_OBJECT (parse, "not enough data for next NALU");
        if (drain) {
          GST_DEBUG_OBJECT (parse, "but draining anyway");
          nonext = TRUE;
        } else {
          return GST_H26X_BASE_PARSE_HANDLE_FRAME_MORE;
        }
      }
    }

    if (!gst_h264_parse_process_nal (h264parse, &nalu)) {
      GST_WARNING_OBJECT (parse,
          "broken/invalid nal Type: %d %s, Size: %u will be dropped",
          nalu.type, _nal_name (nalu.type), nalu.size);
      *skipsize = nalu.size;
      parse->aud_needed = TRUE;
      return GST_H26X_BASE_PARSE_HANDLE_FRAME_SKIP;
    }

    /* Judge whether or not to insert AU Delimiter in case of byte-stream
     * If we're in the middle of au, we don't need to insert aud.
     * Otherwise, we honor the result in gst_h264_parse_process_nal.
     * Note that this should be done until draining if it's happening.
     */
    if (parse->align == GST_H26X_BASE_PARSE_ALIGN_NAL && !parse->aud_needed)
      parse->aud_insert = FALSE;

    if (nonext)
      break;

    /* if no next nal, we know it's complete here */
    au_complete = gst_h264_parse_collect_nal (h264parse, data, size, &nalu);

    if (parse->align == GST_H26X_BASE_PARSE_ALIGN_NAL) {
      parse->aud_needed = au_complete;
      break;
    }

    if (au_complete)
      break;

    GST_DEBUG_OBJECT (parse, "Looking for more");
    *current_offset = nalu.offset + nalu.size;
  }

end:
  *framesize = nalu.offset + nalu.size;

  return GST_H26X_BASE_PARSE_HANDLE_FRAME_OK;
}

/* byte together avc codec data based on collected pps and sps so far */
static GstBuffer *
gst_h264_parse_make_codec_data (GstH26XBaseParse * parse)
{
  GstBuffer *buf, *nal;
  gint i, sps_size = 0, pps_size = 0, num_sps = 0, num_pps = 0;
  guint8 profile_idc = 0, profile_comp = 0, level_idc = 0;
  gboolean found = FALSE;
  GstMapInfo map;
  guint8 *data;
  gint nl;

  /* only nal payload in stored nals */

  for (i = 0; i < GST_H264_MAX_SPS_COUNT; i++) {
    if ((nal = parse->sps_nals[i])) {
      gsize size = gst_buffer_get_size (nal);
      num_sps++;
      /* size bytes also count */
      sps_size += size + 2;
      if (size >= 4) {
        guint8 tmp[3];
        found = TRUE;
        gst_buffer_extract (nal, 1, tmp, 3);
        profile_idc = tmp[0];
        profile_comp = tmp[1];
        level_idc = tmp[2];
      }
    }
  }
  for (i = 0; i < GST_H264_MAX_PPS_COUNT; i++) {
    if ((nal = parse->pps_nals[i])) {
      num_pps++;
      /* size bytes also count */
      pps_size += gst_buffer_get_size (nal) + 2;
    }
  }

  /* AVC3 has SPS/PPS inside the stream, not in the codec_data */
  if (parse->format == GST_H264_PARSE_FORMAT_AVC3) {
    num_sps = sps_size = 0;
    num_pps = pps_size = 0;
  }

  GST_DEBUG_OBJECT (parse,
      "constructing codec_data: num_sps=%d, num_pps=%d", num_sps, num_pps);

  if (!found || (0 == num_pps && GST_H264_PARSE_FORMAT_AVC3 != parse->format))
    return NULL;

  buf = gst_buffer_new_allocate (NULL, 5 + 1 + sps_size + 1 + pps_size, NULL);
  gst_buffer_map (buf, &map, GST_MAP_WRITE);
  data = map.data;
  nl = parse->nal_length_size;

  data[0] = 1;                  /* AVC Decoder Configuration Record ver. 1 */
  data[1] = profile_idc;        /* profile_idc                             */
  data[2] = profile_comp;       /* profile_compability                     */
  data[3] = level_idc;          /* level_idc                               */
  data[4] = 0xfc | (nl - 1);    /* nal_length_size_minus1                  */
  data[5] = 0xe0 | num_sps;     /* number of SPSs */

  data += 6;
  if (parse->format != GST_H264_PARSE_FORMAT_AVC3) {
    for (i = 0; i < GST_H264_MAX_SPS_COUNT; i++) {
      if ((nal = parse->sps_nals[i])) {
        gsize nal_size = gst_buffer_get_size (nal);
        GST_WRITE_UINT16_BE (data, nal_size);
        gst_buffer_extract (nal, 0, data + 2, nal_size);
        data += 2 + nal_size;
      }
    }
  }

  data[0] = num_pps;
  data++;
  if (parse->format != GST_H264_PARSE_FORMAT_AVC3) {
    for (i = 0; i < GST_H264_MAX_PPS_COUNT; i++) {
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

static GstCaps *
gst_h264_parse_get_compatible_profile_caps_from_last_sps (GstH26XBaseParse *
    parse)
{
  GstH264Parse *h264parse = GST_H264_PARSE (parse);
  GstCaps *caps = NULL;
  const gchar **profiles = NULL;
  gint i;
  GstH264SPS *sps;
  GValue compat_profiles = G_VALUE_INIT;
  g_value_init (&compat_profiles, GST_TYPE_LIST);

  sps = h264parse->nalparser->last_sps;

  if (G_UNLIKELY (!sps))
    return NULL;

  switch (sps->profile_idc) {
    case GST_H264_PROFILE_EXTENDED:
      if (sps->constraint_set0_flag) {  /* A.2.1 */
        if (sps->constraint_set1_flag) {
          static const gchar *profile_array[] =
              { "constrained-baseline", "baseline", "main", "high",
            "high-10", "high-4:2:2", "high-4:4:4", NULL
          };
          profiles = profile_array;
        } else {
          static const gchar *profile_array[] = { "baseline", NULL };
          profiles = profile_array;
        }
      } else if (sps->constraint_set1_flag) {   /* A.2.2 */
        static const gchar *profile_array[] =
            { "main", "high", "high-10", "high-4:2:2", "high-4:4:4", NULL };
        profiles = profile_array;
      }
      break;
    case GST_H264_PROFILE_BASELINE:
      if (sps->constraint_set1_flag) {  /* A.2.1 */
        static const gchar *profile_array[] =
            { "baseline", "main", "high", "high-10", "high-4:2:2",
          "high-4:4:4", NULL
        };
        profiles = profile_array;
      } else {
        static const gchar *profile_array[] = { "extended", NULL };
        profiles = profile_array;
      }
      break;
    case GST_H264_PROFILE_MAIN:
    {
      static const gchar *profile_array[] =
          { "high", "high-10", "high-4:2:2", "high-4:4:4", NULL };
      profiles = profile_array;
    }
      break;
    case GST_H264_PROFILE_HIGH:
      if (sps->constraint_set1_flag) {
        static const gchar *profile_array[] =
            { "main", "high-10", "high-4:2:2", "high-4:4:4", NULL };
        profiles = profile_array;
      } else {
        static const gchar *profile_array[] =
            { "high-10", "high-4:2:2", "high-4:4:4", NULL };
        profiles = profile_array;
      }
      break;
    case GST_H264_PROFILE_HIGH10:
      if (sps->constraint_set1_flag) {
        static const gchar *profile_array[] =
            { "main", "high", "high-4:2:2", "high-4:4:4", NULL };
        profiles = profile_array;
      } else {
        if (sps->constraint_set3_flag) {        /* A.2.8 */
          static const gchar *profile_array[] =
              { "high-10", "high-4:2:2", "high-4:4:4", "high-4:2:2-intra",
            "high-4:4:4-intra", NULL
          };
          profiles = profile_array;
        } else {
          static const gchar *profile_array[] =
              { "high-4:2:2", "high-4:4:4", NULL };
          profiles = profile_array;
        }
      }
      break;
    case GST_H264_PROFILE_HIGH_422:
      if (sps->constraint_set1_flag) {
        static const gchar *profile_array[] =
            { "main", "high", "high-10", "high-4:4:4", NULL };
        profiles = profile_array;
      } else {
        if (sps->constraint_set3_flag) {        /* A.2.9 */
          static const gchar *profile_array[] =
              { "high-4:2:2", "high-4:4:4", "high-4:4:4-intra", NULL };
          profiles = profile_array;
        } else {
          static const gchar *profile_array[] = { "high-4:4:4", NULL };
          profiles = profile_array;
        }
      }
      break;
    case GST_H264_PROFILE_HIGH_444:
      if (sps->constraint_set1_flag) {
        static const gchar *profile_array[] =
            { "main", "high", "high-10", "high-4:2:2", NULL };
        profiles = profile_array;
      } else if (sps->constraint_set3_flag) {   /* A.2.10 */
        static const gchar *profile_array[] = { "high-4:4:4", NULL };
        profiles = profile_array;
      }
      break;
    case GST_H264_PROFILE_MULTIVIEW_HIGH:
      if (sps->extension_type == GST_H264_NAL_EXTENSION_MVC
          && sps->extension.mvc.num_views_minus1 == 1) {
        static const gchar *profile_array[] =
            { "stereo-high", "multiview-high", NULL };
        profiles = profile_array;
      } else {
        static const gchar *profile_array[] = { "multiview-high", NULL };
        profiles = profile_array;
      }
      break;
    default:
      break;
  }

  if (profiles) {
    GValue value = G_VALUE_INIT;
    caps = gst_caps_new_empty_simple ("video/x-h264");
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
get_profile_string (GstH264SPS * sps)
{
  const gchar *profile = NULL;

  switch (sps->profile_idc) {
    case 66:
      if (sps->constraint_set1_flag)
        profile = "constrained-baseline";
      else
        profile = "baseline";
      break;
    case 77:
      profile = "main";
      break;
    case 88:
      profile = "extended";
      break;
    case 100:
      if (sps->constraint_set4_flag) {
        if (sps->constraint_set5_flag)
          profile = "constrained-high";
        else
          profile = "progressive-high";
      } else
        profile = "high";
      break;
    case 110:
      if (sps->constraint_set3_flag)
        profile = "high-10-intra";
      else if (sps->constraint_set4_flag)
        profile = "progressive-high-10";
      else
        profile = "high-10";
      break;
    case 122:
      if (sps->constraint_set3_flag)
        profile = "high-4:2:2-intra";
      else
        profile = "high-4:2:2";
      break;
    case 244:
      if (sps->constraint_set3_flag)
        profile = "high-4:4:4-intra";
      else
        profile = "high-4:4:4";
      break;
    case 44:
      profile = "cavlc-4:4:4-intra";
      break;
    case 118:
      profile = "multiview-high";
      break;
    case 128:
      profile = "stereo-high";
      break;
    case 83:
      if (sps->constraint_set5_flag)
        profile = "scalable-constrained-baseline";
      else
        profile = "scalable-baseline";
      break;
    case 86:
      if (sps->constraint_set3_flag)
        profile = "scalable-high-intra";
      else if (sps->constraint_set5_flag)
        profile = "scalable-constrained-high";
      else
        profile = "scalable-high";
      break;
    default:
      return NULL;
  }

  return profile;
}

static const gchar *
get_level_string (GstH264SPS * sps)
{
  if (sps->level_idc == 0)
    return NULL;
  else if ((sps->level_idc == 11 && sps->constraint_set3_flag)
      || sps->level_idc == 9)
    return "1b";
  else if (sps->level_idc % 10 == 0)
    return digit_to_string (sps->level_idc / 10);
  else {
    switch (sps->level_idc) {
      case 11:
        return "1.1";
      case 12:
        return "1.2";
      case 13:
        return "1.3";
      case 21:
        return "2.1";
      case 22:
        return "2.2";
      case 31:
        return "3.1";
      case 32:
        return "3.2";
      case 41:
        return "4.1";
      case 42:
        return "4.2";
      case 51:
        return "5.1";
      case 52:
        return "5.2";
      default:
        return NULL;
    }
  }
}

static gboolean
gst_h264_parse_has_last_sps (GstH26XBaseParse * parse)
{
  GstH264Parse *h264parse = GST_H264_PARSE (parse);
  return (h264parse->nalparser->last_sps != NULL) ? TRUE : FALSE;
}

static gboolean
gst_h264_parse_fill_sps_info (GstH26XBaseParse * parse,
    GstH26XBaseParseSPSInfo * info)
{
  GstH264Parse *h264parse = GST_H264_PARSE (parse);
  GstH264SPS *sps;
  gint fps_num, fps_den;

  g_return_val_if_fail (h264parse->nalparser != NULL, FALSE);

  sps = h264parse->nalparser->last_sps;

  if (G_UNLIKELY (!sps))
    return FALSE;

  if (sps->frame_cropping_flag) {
    info->width = sps->crop_rect_width;
    info->height = sps->crop_rect_height;
  } else {
    info->width = sps->width;
    info->height = sps->height;
  }

  /* 0/1 is set as the default in the codec parser, we will set
   * it in case we have no info */
  gst_h264_video_calculate_framerate (sps, h264parse->field_pic_flag,
      h264parse->sei_pic_struct, &fps_num, &fps_den);

  info->fps_num = fps_num;
  info->fps_den = fps_den;

  if (sps->vui_parameters.aspect_ratio_info_present_flag) {
    info->par_num = sps->vui_parameters.par_n;
    info->par_den = sps->vui_parameters.par_d;
  }

  if (sps->frame_mbs_only_flag == 0)
    info->interlace_mode = GST_VIDEO_INTERLACE_MODE_MIXED;
  else
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

  return TRUE;
}

static gboolean
gst_h264_parse_fill_profile_tier_level (GstH26XBaseParse * parse,
    GstH26XBaseParseProfileTierLevel * ptl)
{
  GstH264Parse *h264parse = GST_H264_PARSE (parse);
  GstH264SPS *sps;

  g_return_val_if_fail (h264parse->nalparser != NULL, FALSE);

  sps = h264parse->nalparser->last_sps;

  if (G_UNLIKELY (!sps))
    return FALSE;

  ptl->profile = get_profile_string (sps);
  ptl->level = get_level_string (sps);
  ptl->tier = NULL;

  return TRUE;
}

static void
gst_h264_parse_get_timestamp (GstH26XBaseParse * parse,
    GstClockTime * out_ts, GstClockTime * out_dur)
{
  GstH264Parse *h264parse = GST_H264_PARSE (parse);
  GstH264SPS *sps = h264parse->nalparser->last_sps;
  GstClockTime upstream;
  gint duration = 1;

  g_return_if_fail (out_dur != NULL);
  g_return_if_fail (out_ts != NULL);

  upstream = *out_ts;
  GST_LOG_OBJECT (h264parse, "Upstream ts %" GST_TIME_FORMAT,
      GST_TIME_ARGS (upstream));

  if (!parse->frame_start) {
    GST_LOG_OBJECT (h264parse, "no frame data ->  0 duration");
    *out_dur = 0;
    goto exit;
  } else {
    *out_ts = upstream;
  }

  if (!sps) {
    GST_DEBUG_OBJECT (h264parse, "referred SPS invalid");
    goto exit;
  } else if (!sps->vui_parameters_present_flag) {
    GST_DEBUG_OBJECT (h264parse,
        "unable to compute timestamp: VUI not present");
    goto exit;
  } else if (!sps->vui_parameters.timing_info_present_flag) {
    GST_DEBUG_OBJECT (h264parse,
        "unable to compute timestamp: timing info not present");
    goto exit;
  } else if (sps->vui_parameters.time_scale == 0) {
    GST_DEBUG_OBJECT (h264parse,
        "unable to compute timestamp: time_scale = 0 "
        "(this is forbidden in spec; bitstream probably contains error)");
    goto exit;
  }

  if (h264parse->sei_pic_struct_pres_flag &&
      h264parse->sei_pic_struct != (guint8) - 1) {
    /* Note that when h264parse->sei_pic_struct == -1 (unspecified), there
     * are ways to infer its value. This is related to computing the
     * TopFieldOrderCnt and BottomFieldOrderCnt, which looks
     * complicated and thus not implemented for the time being. Yet
     * the value we have here is correct for many applications
     */
    switch (h264parse->sei_pic_struct) {
      case GST_H264_SEI_PIC_STRUCT_TOP_FIELD:
      case GST_H264_SEI_PIC_STRUCT_BOTTOM_FIELD:
        duration = 1;
        break;
      case GST_H264_SEI_PIC_STRUCT_FRAME:
      case GST_H264_SEI_PIC_STRUCT_TOP_BOTTOM:
      case GST_H264_SEI_PIC_STRUCT_BOTTOM_TOP:
        duration = 2;
        break;
      case GST_H264_SEI_PIC_STRUCT_TOP_BOTTOM_TOP:
      case GST_H264_SEI_PIC_STRUCT_BOTTOM_TOP_BOTTOM:
        duration = 3;
        break;
      case GST_H264_SEI_PIC_STRUCT_FRAME_DOUBLING:
        duration = 4;
        break;
      case GST_H264_SEI_PIC_STRUCT_FRAME_TRIPLING:
        duration = 6;
        break;
      default:
        GST_DEBUG_OBJECT (h264parse,
            "h264parse->sei_pic_struct of unknown value %d. Not parsed",
            h264parse->sei_pic_struct);
        break;
    }
  } else {
    duration = h264parse->field_pic_flag ? 1 : 2;
  }

  GST_LOG_OBJECT (h264parse, "frame tick duration %d", duration);

  /*
   * h264parse.264 C.1.2 Timing of coded picture removal (equivalent to DTS):
   * Tr,n(0) = initial_cpb_removal_delay[ SchedSelIdx ] / 90000
   * Tr,n(n) = Tr,n(nb) + Tc * cpb_removal_delay(n)
   * where
   * Tc = num_units_in_tick / time_scale
   */

  if (parse->ts_trn_nb != GST_CLOCK_TIME_NONE) {
    GST_LOG_OBJECT (h264parse, "buffering based ts");
    /* buffering period is present */
    if (upstream != GST_CLOCK_TIME_NONE) {
      /* If upstream timestamp is valid, we respect it and adjust current
       * reference point */
      parse->ts_trn_nb = upstream -
          (GstClockTime) gst_util_uint64_scale_int
          (h264parse->sei_cpb_removal_delay * GST_SECOND,
          sps->vui_parameters.num_units_in_tick,
          sps->vui_parameters.time_scale);
    } else {
      /* If no upstream timestamp is given, we write in new timestamp */
      upstream = parse->dts = parse->ts_trn_nb +
          (GstClockTime) gst_util_uint64_scale_int
          (h264parse->sei_cpb_removal_delay * GST_SECOND,
          sps->vui_parameters.num_units_in_tick,
          sps->vui_parameters.time_scale);
    }
  } else {
    GstClockTime dur;

    GST_LOG_OBJECT (h264parse, "duration based ts");
    /* naive method: no removal delay specified
     * track upstream timestamp and provide best guess frame duration */
    dur = gst_util_uint64_scale_int (duration * GST_SECOND,
        sps->vui_parameters.num_units_in_tick, sps->vui_parameters.time_scale);
    /* sanity check */
    if (dur < GST_MSECOND) {
      GST_DEBUG_OBJECT (h264parse, "discarding dur %" GST_TIME_FORMAT,
          GST_TIME_ARGS (dur));
    } else {
      *out_dur = dur;
    }
  }

exit:
  if (GST_CLOCK_TIME_IS_VALID (upstream))
    *out_ts = parse->dts = upstream;

  if (GST_CLOCK_TIME_IS_VALID (*out_dur) &&
      GST_CLOCK_TIME_IS_VALID (parse->dts))
    parse->dts += *out_dur;
}

static GstBuffer *
gst_h264_parse_prepare_pre_push_frame (GstH26XBaseParse * parse,
    GstBaseParseFrame * frame)
{
  GstBuffer *buffer;

  /* In case of byte-stream, insert au delimeter by default
   * if it doesn't exist */
  if (parse->aud_insert && parse->format == GST_H264_PARSE_FORMAT_BYTE) {
    if (parse->align == GST_H26X_BASE_PARSE_ALIGN_AU) {
      GstMemory *mem =
          gst_memory_new_wrapped (GST_MEMORY_FLAG_READONLY, (guint8 *) au_delim,
          sizeof (au_delim), 0, sizeof (au_delim), NULL, NULL);

      frame->out_buffer = gst_buffer_copy (frame->buffer);
      gst_buffer_prepend_memory (frame->out_buffer, mem);
      if (parse->idr_pos >= 0)
        parse->idr_pos += sizeof (au_delim);

      buffer = frame->out_buffer;
    } else {
      GstBuffer *aud_buffer = gst_buffer_new_allocate (NULL, 2, NULL);
      gst_buffer_fill (aud_buffer, 0, (guint8 *) (au_delim + 4), 2);

      buffer = frame->buffer;
      gst_h26x_base_parse_push_codec_buffer (parse, aud_buffer,
          GST_BUFFER_TIMESTAMP (buffer));
      gst_buffer_unref (aud_buffer);
    }
  } else {
    buffer = frame->buffer;
  }

  return buffer;
}

static gboolean
gst_h264_parse_fixate_format (GstH26XBaseParse * parse, guint * format,
    guint * align, const GValue * codec_data_value)
{
  if (*format == GST_H264_PARSE_FORMAT_NONE) {
    /* codec_data implies avc */
    if (codec_data_value != NULL) {
      GST_ERROR ("video/x-h264 caps with codec_data but no stream-format=avc");
      *format = GST_H264_PARSE_FORMAT_AVC;
    } else {
      /* otherwise assume bytestream input */
      GST_ERROR ("video/x-h264 caps without codec_data or stream-format");
      *format = GST_H264_PARSE_FORMAT_BYTE;
    }
  }

  /* avc caps sanity checks */
  if (*format == GST_H264_PARSE_FORMAT_AVC) {
    /* AVC requires codec_data, AVC3 might have one and/or SPS/PPS inline */
    if (codec_data_value == NULL)
      goto avc_format_codec_data_missing;

    /* AVC implies alignment=au, everything else is not allowed */
    if (*align == GST_H26X_BASE_PARSE_ALIGN_NONE)
      *align = GST_H26X_BASE_PARSE_ALIGN_AU;
    else if (*align != GST_H26X_BASE_PARSE_ALIGN_AU)
      goto avc_format_wrong_alignment;
  }

  /* bytestream caps sanity checks */
  if (*format == GST_H264_PARSE_FORMAT_BYTE) {
    /* should have SPS/PSS in-band (and/or oob in streamheader field) */
    if (codec_data_value != NULL)
      goto bytestream_format_with_codec_data;
  }

  return TRUE;

  /* ERRORS */
avc_format_codec_data_missing:
  {
    GST_WARNING_OBJECT (parse, "H.264 AVC format, but no codec_data");
    return FALSE;
  }
avc_format_wrong_alignment:
  {
    GST_WARNING_OBJECT (parse,
        "H.264 AVC format with NAL alignment, must be AU");
    return FALSE;
  }
bytestream_format_with_codec_data:
  {
    GST_WARNING_OBJECT (parse, "H.264 bytestream format with codec_data is not "
        "expected, send SPS/PPS in-band with data or in streamheader field");
    return FALSE;
  }
}

static gboolean
gst_h264_parse_handle_codec_data (GstH26XBaseParse * parse, GstMapInfo * map)
{
  GstH264Parse *h264parse = GST_H264_PARSE (parse);
  gsize size;
  guint8 *data;
  guint num_sps, num_pps;
#ifndef GST_DISABLE_GST_DEBUG
  guint profile;
#endif
  gint i;
  guint off;
  GstH264ParserResult parseres;
  GstH264NalUnit nalu;

  data = map->data;
  size = map->size;

  /* parse the avcC data */
  if (size < 7) {               /* when numSPS==0 and numPPS==0, length is 7 bytes */
    goto avcc_too_small;
  }
  /* parse the version, this must be 1 */
  if (data[0] != 1) {
    goto wrong_version;
  }
#ifndef GST_DISABLE_GST_DEBUG
  /* AVCProfileIndication */
  /* profile_compat */
  /* AVCLevelIndication */
  profile = (data[1] << 16) | (data[2] << 8) | data[3];
  GST_DEBUG_OBJECT (parse, "profile %06x", profile);
#endif

  /* 6 bits reserved | 2 bits lengthSizeMinusOne */
  /* this is the number of bytes in front of the NAL units to mark their
   * length */
  parse->nal_length_size = (data[4] & 0x03) + 1;
  GST_DEBUG_OBJECT (parse, "nal length size %u", parse->nal_length_size);

  num_sps = data[5] & 0x1f;
  off = 6;
  for (i = 0; i < num_sps; i++) {
    parseres = gst_h264_parser_identify_nalu_avc (h264parse->nalparser,
        data, off, size, 2, &nalu);
    if (parseres != GST_H264_PARSER_OK) {
      goto avcc_too_small;
    }

    gst_h264_parse_process_nal (h264parse, &nalu);
    off = nalu.offset + nalu.size;
  }

  if (off >= size) {
    goto avcc_too_small;
  }
  num_pps = data[off];
  off++;

  for (i = 0; i < num_pps; i++) {
    parseres = gst_h264_parser_identify_nalu_avc (h264parse->nalparser,
        data, off, size, 2, &nalu);
    if (parseres != GST_H264_PARSER_OK) {
      goto avcc_too_small;
    }

    gst_h264_parse_process_nal (h264parse, &nalu);
    off = nalu.offset + nalu.size;
  }

  return TRUE;

  /* ERRORS */
avcc_too_small:
  {
    GST_DEBUG_OBJECT (parse, "avcC size %" G_GSIZE_FORMAT " < 8", size);
    return FALSE;
  }
wrong_version:
  {
    GST_DEBUG_OBJECT (parse, "wrong avcC version");
    return FALSE;
  }
}
