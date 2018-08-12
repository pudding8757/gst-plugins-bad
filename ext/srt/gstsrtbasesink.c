/* GStreamer SRT plugin based on libsrt
 * Copyright (C) 2017, Collabora Ltd.
 *   Author:Justin Kim <justin.kim@collabora.com>
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
#include "config.h"
#endif

#include "gstsrtbasesink.h"
#include "gstsrt.h"
#include <srt/srt.h>

#define GST_CAT_DEFAULT gst_debug_srt_base_sink
GST_DEBUG_CATEGORY (GST_CAT_DEFAULT);

enum
{
  PROP_URI = 1,
  PROP_LATENCY,
  PROP_PASSPHRASE,
  PROP_KEY_LENGTH,
  PROP_SNDBUF_SIZE,

  /*< private > */
  PROP_LAST
};

static GParamSpec *properties[PROP_LAST];

static void gst_srt_base_sink_uri_handler_init (gpointer g_iface,
    gpointer iface_data);
static gchar *gst_srt_base_sink_uri_get_uri (GstURIHandler * handler);
static gboolean gst_srt_base_sink_uri_set_uri (GstURIHandler * handler,
    const gchar * uri, GError ** error);

#define gst_srt_base_sink_parent_class parent_class
G_DEFINE_ABSTRACT_TYPE_WITH_CODE (GstSRTBaseSink, gst_srt_base_sink,
    GST_TYPE_BASE_SINK,
    G_IMPLEMENT_INTERFACE (GST_TYPE_URI_HANDLER,
        gst_srt_base_sink_uri_handler_init)
    GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, "srtbasesink", 0,
        "SRT Base Sink"));

static void
gst_srt_base_sink_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstSRTBaseSink *self = GST_SRT_BASE_SINK (object);

  switch (prop_id) {
    case PROP_URI:
      if (self->uri != NULL) {
        gchar *uri_str = gst_srt_base_sink_uri_get_uri (GST_URI_HANDLER (self));
        g_value_take_string (value, uri_str);
      }
      break;
    case PROP_LATENCY:
      g_value_set_int (value, self->latency);
      break;
    case PROP_PASSPHRASE:
      g_value_set_string (value, self->passphrase);
      break;
    case PROP_KEY_LENGTH:
      g_value_set_int (value, self->key_length);
      break;
    case PROP_SNDBUF_SIZE:
      g_value_set_int (value, self->sndbuf_size);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_srt_base_sink_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec)
{
  GstSRTBaseSink *self = GST_SRT_BASE_SINK (object);

  switch (prop_id) {
    case PROP_URI:
      gst_srt_base_sink_uri_set_uri (GST_URI_HANDLER (self),
          g_value_get_string (value), NULL);
      break;
    case PROP_LATENCY:
      self->latency = g_value_get_int (value);
      break;
    case PROP_PASSPHRASE:
      g_free (self->passphrase);
      self->passphrase = g_value_dup_string (value);
      break;
    case PROP_KEY_LENGTH:
    {
      gint key_length = g_value_get_int (value);
      g_return_if_fail (key_length == 16 || key_length == 24
          || key_length == 32);
      self->key_length = key_length;
      break;
    }
    case PROP_SNDBUF_SIZE:
      self->sndbuf_size = g_value_get_int (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_srt_base_sink_finalize (GObject * object)
{
  GstSRTBaseSink *self = GST_SRT_BASE_SINK (object);

  g_clear_pointer (&self->uri, gst_uri_unref);
  g_clear_pointer (&self->passphrase, g_free);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

/* From gstmultihandlesink.c in -base */
static gboolean
buffer_is_in_caps (GstSRTBaseSink * sink, GstBuffer * buf)
{
  GstCaps *caps;
  GstStructure *s;
  const GValue *v;

  caps = gst_pad_get_current_caps (GST_BASE_SINK_PAD (sink));
  if (!caps)
    return FALSE;
  s = gst_caps_get_structure (caps, 0);
  if (!gst_structure_has_field (s, "streamheader")) {
    gst_caps_unref (caps);
    return FALSE;
  }

  v = gst_structure_get_value (s, "streamheader");
  if (GST_VALUE_HOLDS_ARRAY (v)) {
    guint n = gst_value_array_get_size (v);
    guint i;
    GstMapInfo map;

    gst_buffer_map (buf, &map, GST_MAP_READ);

    for (i = 0; i < n; i++) {
      const GValue *v2 = gst_value_array_get_value (v, i);
      GstBuffer *buf2;
      GstMapInfo map2;

      if (!GST_VALUE_HOLDS_BUFFER (v2))
        continue;

      buf2 = gst_value_get_buffer (v2);
      if (buf == buf2) {
        gst_caps_unref (caps);
        return TRUE;
      }
      gst_buffer_map (buf2, &map2, GST_MAP_READ);
      if (map.size == map2.size && memcmp (map.data, map2.data, map.size) == 0) {
        gst_buffer_unmap (buf2, &map2);
        gst_buffer_unmap (buf, &map);
        gst_caps_unref (caps);
        return TRUE;
      }
      gst_buffer_unmap (buf2, &map2);
    }
    gst_buffer_unmap (buf, &map);
  }

  gst_caps_unref (caps);

  return FALSE;
}

static GstFlowReturn
gst_srt_base_sink_render (GstBaseSink * sink, GstBuffer * buffer)
{
  GstSRTBaseSink *self = GST_SRT_BASE_SINK (sink);
  GstSRTBaseSinkClass *bclass = GST_SRT_BASE_SINK_GET_CLASS (sink);
  gboolean in_caps = FALSE;

  if (GST_BUFFER_FLAG_IS_SET (buffer, GST_BUFFER_FLAG_HEADER)) {
    in_caps = buffer_is_in_caps (self, buffer);
  }

  GST_TRACE_OBJECT (self, "received buffer %p, in_caps: %s, offset %"
      G_GINT64_FORMAT ", offset_end %" G_GINT64_FORMAT
      ", timestamp %" GST_TIME_FORMAT ", duration %" GST_TIME_FORMAT,
      buffer, in_caps ? "yes" : "no", GST_BUFFER_OFFSET (buffer),
      GST_BUFFER_OFFSET_END (buffer),
      GST_TIME_ARGS (GST_BUFFER_TIMESTAMP (buffer)),
      GST_TIME_ARGS (GST_BUFFER_DURATION (buffer)));

  if (G_UNLIKELY (in_caps)) {
    GST_DEBUG_OBJECT (self, "ignoring HEADER buffer with length %"
        G_GSIZE_FORMAT, gst_buffer_get_size (buffer));
    return GST_FLOW_OK;
  }

  return bclass->send_buffer (self, buffer);
}

static void
gst_srt_base_sink_class_init (GstSRTBaseSinkClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstBaseSinkClass *gstbasesink_class = GST_BASE_SINK_CLASS (klass);

  gobject_class->set_property = gst_srt_base_sink_set_property;
  gobject_class->get_property = gst_srt_base_sink_get_property;
  gobject_class->finalize = gst_srt_base_sink_finalize;

  /**
   * GstSRTBaseSink:uri:
   *
   * The URI used by SRT Connection.
   */
  properties[PROP_URI] = g_param_spec_string ("uri", "URI",
      "URI in the form of srt://address:port", SRT_DEFAULT_URI,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  properties[PROP_LATENCY] =
      g_param_spec_int ("latency", "latency",
      "Minimum latency (milliseconds)", 0,
      G_MAXINT32, SRT_DEFAULT_LATENCY,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  properties[PROP_PASSPHRASE] = g_param_spec_string ("passphrase", "Passphrase",
      "The password for the encrypted transmission", NULL,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  properties[PROP_KEY_LENGTH] =
      g_param_spec_int ("key-length", "key length",
      "Crypto key length in bytes{16,24,32}", 16,
      32, SRT_DEFAULT_KEY_LENGTH, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  properties[PROP_SNDBUF_SIZE] =
      g_param_spec_int ("send-buffer-size", "send buffer size",
      "SRT send buffer size in srt packet unit (1500 - 28 bytes)",
      SRT_MIN_BUFFER_SIZE, SRT_MAX_BUFFER_SIZE, SRT_DEFAULT_BUFFER_SIZE,
      GST_PARAM_MUTABLE_READY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (gobject_class, PROP_LAST, properties);

  gstbasesink_class->render = GST_DEBUG_FUNCPTR (gst_srt_base_sink_render);
}

static void
gst_srt_base_sink_init (GstSRTBaseSink * self)
{
  self->uri = gst_uri_from_string (SRT_DEFAULT_URI);
  self->latency = SRT_DEFAULT_LATENCY;
  self->passphrase = NULL;
  self->key_length = SRT_DEFAULT_KEY_LENGTH;
  self->sndbuf_size = SRT_DEFAULT_BUFFER_SIZE;

#ifndef GST_DISABLE_GST_DEBUG
  gst_srt_debug_init ();
#endif
}

static GstURIType
gst_srt_base_sink_uri_get_type (GType type)
{
  return GST_URI_SINK;
}

static const gchar *const *
gst_srt_base_sink_uri_get_protocols (GType type)
{
  static const gchar *protocols[] = { SRT_URI_SCHEME, NULL };

  return protocols;
}

static gchar *
gst_srt_base_sink_uri_get_uri (GstURIHandler * handler)
{
  gchar *uri_str;
  GstSRTBaseSink *self = GST_SRT_BASE_SINK (handler);

  GST_OBJECT_LOCK (self);
  uri_str = gst_uri_to_string (self->uri);
  GST_OBJECT_UNLOCK (self);

  return uri_str;
}

static gboolean
gst_srt_base_sink_uri_set_uri (GstURIHandler * handler,
    const gchar * uri, GError ** error)
{
  GstSRTBaseSink *self = GST_SRT_BASE_SINK (handler);
  gboolean ret = TRUE;
  GstUri *parsed_uri = gst_uri_from_string (uri);

  GST_TRACE_OBJECT (self, "Requested URI=%s", uri);

  if (g_strcmp0 (gst_uri_get_scheme (parsed_uri), SRT_URI_SCHEME) != 0) {
    g_set_error (error, GST_URI_ERROR, GST_URI_ERROR_BAD_URI,
        "Invalid SRT URI scheme");
    ret = FALSE;
    goto out;
  }

  GST_OBJECT_LOCK (self);

  g_clear_pointer (&self->uri, gst_uri_unref);
  self->uri = gst_uri_ref (parsed_uri);

  GST_OBJECT_UNLOCK (self);

out:
  g_clear_pointer (&parsed_uri, gst_uri_unref);
  return ret;
}

static void
gst_srt_base_sink_uri_handler_init (gpointer g_iface, gpointer iface_data)
{
  GstURIHandlerInterface *iface = (GstURIHandlerInterface *) g_iface;

  iface->get_type = gst_srt_base_sink_uri_get_type;
  iface->get_protocols = gst_srt_base_sink_uri_get_protocols;
  iface->get_uri = gst_srt_base_sink_uri_get_uri;
  iface->set_uri = gst_srt_base_sink_uri_set_uri;
}

GstStructure *
gst_srt_base_sink_get_stats (GstSRTClientHandle * handle)
{
  SRT_TRACEBSTATS stats;
  int ret;
  GValue v = G_VALUE_INIT;
  GstStructure *s;

  if (handle->sock == SRT_INVALID_SOCK || handle->sockaddr == NULL)
    return gst_structure_new_empty ("application/x-srt-statistics");

  s = gst_structure_new ("application/x-srt-statistics",
      "sockaddr", G_TYPE_SOCKET_ADDRESS, handle->sockaddr, NULL);

  ret = srt_bstats (handle->sock, &stats, 0);
  if (ret >= 0) {
    gst_structure_set (s,
        /* number of sent data packets, including retransmissions */
        "packets-sent", G_TYPE_INT64, stats.pktSent,
        /* number of lost packets (sender side) */
        "packets-sent-lost", G_TYPE_INT, stats.pktSndLoss,
        /* number of retransmitted packets */
        "packets-retransmitted", G_TYPE_INT, stats.pktRetrans,
        /* number of received ACK packets */
        "packet-ack-received", G_TYPE_INT, stats.pktRecvACK,
        /* number of received NAK packets */
        "packet-nack-received", G_TYPE_INT, stats.pktRecvNAK,
        /* time duration when UDT is sending data (idle time exclusive) */
        "send-duration-us", G_TYPE_INT64, stats.usSndDuration,
        /* number of sent data bytes, including retransmissions */
        "bytes-sent", G_TYPE_UINT64, stats.byteSent,
        /* number of retransmitted bytes */
        "bytes-retransmitted", G_TYPE_UINT64, stats.byteRetrans,
        /* number of too-late-to-send dropped bytes */
        "bytes-sent-dropped", G_TYPE_UINT64, stats.byteSndDrop,
        /* number of too-late-to-send dropped packets */
        "packets-sent-dropped", G_TYPE_INT, stats.pktSndDrop,
        /* sending rate in Mb/s */
        "send-rate-mbps", G_TYPE_DOUBLE, stats.msRTT,
        /* estimated bandwidth, in Mb/s */
        "bandwidth-mbps", G_TYPE_DOUBLE, stats.mbpsBandwidth,
        /* busy sending time (i.e., idle time exclusive) */
        "send-duration-us", G_TYPE_UINT64, stats.usSndDuration,
        "rtt-ms", G_TYPE_DOUBLE, stats.msRTT,
        "negotiated-latency-ms", G_TYPE_INT, stats.msSndTsbPdDelay, NULL);
  }

  g_value_init (&v, G_TYPE_STRING);
  g_value_take_string (&v,
      g_socket_connectable_to_string (G_SOCKET_CONNECTABLE (handle->sockaddr)));
  gst_structure_take_value (s, "sockaddr-str", &v);

  return s;
}

/* From gstmultihandlesink.c in -base */
gboolean
gst_srt_base_sink_client_queue_buffer (GstSRTBaseSink * sink,
    GstSRTClientHandle * handle, GstBuffer * buffer)
{
  GstCaps *caps;

  /* TRUE: send them if the new caps have them */
  gboolean send_streamheader = FALSE;

  /* before we queue the buffer, we check if we need to queue streamheader
   * buffers (because it's a new client, or because they changed) */
  caps = gst_pad_get_current_caps (GST_BASE_SINK_PAD (sink));

  if (!handle->caps) {
    if (caps) {
      GST_DEBUG_OBJECT (sink,
          "%p no previous caps for this client, send streamheader", handle);
      send_streamheader = TRUE;
      handle->caps = gst_caps_ref (caps);
    }
  } else {
    /* there were previous caps recorded, so compare */
    if (!gst_caps_is_equal (caps, handle->caps)) {
      GstStructure *s;
      /* caps are not equal, but could still have the same streamheader */
      s = gst_caps_get_structure (caps, 0);
      if (!gst_structure_has_field (s, "streamheader")) {
        /* no new streamheader, so nothing new to send */
        GST_DEBUG_OBJECT (sink,
            "%p new caps do not have streamheader, not sending", handle);
      } else {
        /* there is a new streamheader */
        s = gst_caps_get_structure (handle->caps, 0);
        if (!gst_structure_has_field (s, "streamheader")) {
          /* no previous streamheader, so send the new one */
          GST_DEBUG_OBJECT (sink,
              "%p previous caps did not have streamheader, sending", handle);
          send_streamheader = TRUE;
        } else {
          const GValue *sh1, *sh2;
          sh1 = gst_structure_get_value (s, "streamheader");
          s = gst_caps_get_structure (caps, 0);
          sh2 = gst_structure_get_value (s, "streamheader");
          if (gst_value_compare (sh1, sh2) != GST_VALUE_EQUAL) {
            GST_DEBUG_OBJECT (sink,
                "%p new streamheader different from old, sending", handle);
            send_streamheader = TRUE;
          }
        }
      }
    }
    /* Replace the old caps */
    gst_caps_replace (&handle->caps, caps);
  }

  if (G_UNLIKELY (send_streamheader)) {
    const GValue *sh;
    GstStructure *s;
    GArray *buffers;
    gint i;

    GST_LOG_OBJECT (sink,
        "%p sending streamheader from caps %" GST_PTR_FORMAT, handle, caps);
    s = gst_caps_get_structure (caps, 0);
    if (!gst_structure_has_field (s, "streamheader")) {
      GST_DEBUG_OBJECT (sink,
          "%p no new streamheader, so nothing to send", handle);
    } else {
      GST_LOG_OBJECT (sink,
          "%p sending streamheader from caps %" GST_PTR_FORMAT, handle, caps);
      sh = gst_structure_get_value (s, "streamheader");
      g_assert (G_VALUE_TYPE (sh) == GST_TYPE_ARRAY);
      buffers = g_value_peek_pointer (sh);
      GST_DEBUG_OBJECT (sink, "%d streamheader buffers", buffers->len);
      for (i = 0; i < buffers->len; ++i) {
        GValue *bufval;
        GstBuffer *buffer;

        bufval = &g_array_index (buffers, GValue, i);
        g_assert (G_VALUE_TYPE (bufval) == GST_TYPE_BUFFER);
        buffer = g_value_peek_pointer (bufval);
        GST_DEBUG_OBJECT (sink,
            "%p queueing streamheader buffer of length %" G_GSIZE_FORMAT,
            handle, gst_buffer_get_size (buffer));
        gst_buffer_ref (buffer);

        handle->queue = g_slist_append (handle->queue, buffer);
      }
    }
  }

  if (caps)
    gst_caps_unref (caps);

  GST_LOG_OBJECT (sink, "%p queueing buffer of length %" G_GSIZE_FORMAT,
      handle, gst_buffer_get_size (buffer));

  gst_buffer_ref (buffer);
  handle->queue = g_slist_append (handle->queue, buffer);

  return TRUE;
}

GstFlowReturn
gst_srt_base_sink_client_send_message (GstSRTBaseSink * sink,
    GstSRTClientHandle * handle)
{
  GstBuffer *head;
  GstMapInfo info;
  gint32 snddata;
  int dummy;

  g_return_val_if_fail (handle != NULL, GST_FLOW_ERROR);

  if (!handle->queue) {
    GST_DEBUG_OBJECT (sink, "Client queue is empty");
    return GST_FLOW_OK;
  }

  srt_getsockopt (handle->sock, 0, SRTO_SNDDATA, &snddata, &dummy);

  GST_LOG_OBJECT (sink, "Num unacknowledged packet %" G_GINT32_FORMAT "/%"
      G_GINT32_FORMAT, snddata, sink->sndbuf_size);

  if (sink->sndbuf_size - 2 <= snddata) {
    handle->retry_count++;
    GST_DEBUG_OBJECT (sink,
        "Send message would block, retry count %" G_GUINT32_FORMAT,
        handle->retry_count);
    return GST_SRT_FLOW_SEND_AGAIN;
  }

  head = GST_BUFFER (handle->queue->data);

  if (!gst_buffer_map (head, &info, GST_MAP_READ)) {
    GST_ELEMENT_ERROR (sink, RESOURCE, READ,
        ("Could not map the buffer for reading"), (NULL));
    return GST_FLOW_ERROR;
  }

  if (srt_sendmsg2 (handle->sock, (char *) info.data, info.size, NULL)
      == SRT_ERROR) {
    int err = srt_getlasterror (NULL);
    GstFlowReturn ret;

    if (err == SRT_EASYNCSND) {
      handle->retry_count++;
      GST_DEBUG_OBJECT (sink, "EAGAIN, need to send again, retry count %"
          G_GUINT32_FORMAT, handle->retry_count);
      ret = GST_SRT_FLOW_SEND_AGAIN;
    } else {
      GST_ERROR_OBJECT (sink,
          "Failed to send message (%s)", srt_getlasterror_str ());
      ret = GST_SRT_FLOW_SEND_ERROR;
    }
    gst_buffer_unmap (head, &info);

    return ret;
  }

  handle->retry_count = 0;

  gst_buffer_unmap (head, &info);
  handle->queue = g_slist_remove (handle->queue, head);
  gst_buffer_unref (head);

  return GST_FLOW_OK;
}

GstSRTClientHandle *
gst_srt_client_handle_new (GstSRTBaseSink * sink)
{
  GstSRTClientHandle *handle = g_new0 (GstSRTClientHandle, 1);

  handle->sink = sink;
  handle->sock = SRT_INVALID_SOCK;
  handle->ref_count = 1;

  return handle;
}

GstSRTClientHandle *
gst_srt_client_handle_ref (GstSRTClientHandle * handle)
{
  g_return_val_if_fail (handle != NULL && handle->ref_count > 0, NULL);

  g_atomic_int_add (&handle->ref_count, 1);
  return handle;
}

void
gst_srt_client_handle_unref (GstSRTClientHandle * handle)
{
  g_return_if_fail (handle != NULL && handle->ref_count > 0);

  if (g_atomic_int_dec_and_test (&handle->ref_count)) {
    GST_LOG_OBJECT (handle->sink, "free client handle");
    if (handle->sock != SRT_INVALID_SOCK) {
      srt_close (handle->sock);
    }
    g_clear_object (&handle->sockaddr);
    g_clear_pointer (&handle->caps, gst_caps_unref);
    if (handle->queue)
      g_slist_free_full (handle->queue, (GDestroyNotify) gst_buffer_unref);
    g_free (handle);
  }
}
