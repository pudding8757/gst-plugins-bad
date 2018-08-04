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

/**
 * SECTION:element-srtserversink
 * @title: srtserversink
 *
 * srtserversink is a network sink that sends <ulink url="http://www.srtalliance.org/">SRT</ulink>
 * packets to the network. Although SRT is an UDP-based protocol, srtserversink works like
 * a server socket of connection-oriented protocol.
 *
 * <refsect2>
 * <title>Examples</title>
 * |[
 * gst-launch-1.0 -v audiotestsrc ! srtserversink
 * ]| This pipeline shows how to serve SRT packets through the default port.

 * |[
 * gst-launch-1.0 -v audiotestsrc ! srtserversink uri=srt://192.168.1.10:8888/ rendez-vous=1
 * ]| This pipeline shows how to serve SRT packets to 192.168.1.10 port 8888 using the rendez-vous mode.
 * </refsect2>
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstsrtclientsink.h"
#include "gstsrt.h"
#include <srt/srt.h>
#include <gio/gio.h>

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

#define GST_CAT_DEFAULT gst_debug_srt_client_sink
GST_DEBUG_CATEGORY (GST_CAT_DEFAULT);

enum
{
  PROP_BIND_ADDRESS = 1,
  PROP_BIND_PORT,
  PROP_RENDEZ_VOUS,
  PROP_STATS,
  /*< private > */
  PROP_LAST
};

static GParamSpec *properties[PROP_LAST];

#define DEFAULT_BIND_ADDRESS  NULL
#define DEFAULT_BIND_PORT     0
#define DEFAULT_RENDEZ_VOUS   FALSE

#define gst_srt_client_sink_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstSRTClientSink, gst_srt_client_sink,
    GST_TYPE_SRT_BASE_SINK,
    GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, "srtclientsink", 0,
        "SRT Client Sink"));

static void
gst_srt_client_sink_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstSRTClientSink *self = GST_SRT_CLIENT_SINK (object);

  switch (prop_id) {
    case PROP_BIND_PORT:
      g_value_set_int (value, self->bind_port);
      break;
    case PROP_BIND_ADDRESS:
      g_value_set_string (value, self->bind_address);
      break;
    case PROP_RENDEZ_VOUS:
      g_value_set_boolean (value, self->rendez_vous);
      break;
    case PROP_STATS:
      if (self->handle)
        g_value_take_boxed (value, gst_srt_base_sink_get_stats (self->handle));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_srt_client_sink_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec)
{
  GstSRTClientSink *self = GST_SRT_CLIENT_SINK (object);

  switch (prop_id) {
    case PROP_BIND_ADDRESS:
      g_free (self->bind_address);
      self->bind_address = g_value_dup_string (value);
      break;
    case PROP_BIND_PORT:
      self->bind_port = g_value_get_int (value);
      break;
    case PROP_RENDEZ_VOUS:
      self->rendez_vous = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_srt_client_sink_finalize (GObject * object)
{
  GstSRTClientSink *self = GST_SRT_CLIENT_SINK (object);

  g_free (self->bind_address);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static gboolean
gst_srt_client_sink_start (GstBaseSink * sink)
{
  GstSRTClientSink *self = GST_SRT_CLIENT_SINK (sink);
  GstSRTBaseSink *base = GST_SRT_BASE_SINK (sink);
  GstUri *uri = gst_uri_ref (GST_SRT_BASE_SINK (self)->uri);
  GstSRTClientHandle *handle;

  handle = gst_srt_client_handle_new (base);

  handle->sock = gst_srt_client_connect (GST_ELEMENT (sink), TRUE,
      gst_uri_get_host (uri), gst_uri_get_port (uri), self->rendez_vous,
      self->bind_address, self->bind_port, base->latency,
      &handle->sockaddr, &self->poll_id, base->passphrase, base->key_length);

  g_clear_pointer (&uri, gst_uri_unref);

  if (handle->sock == SRT_INVALID_SOCK) {
    gst_srt_client_handle_unref (handle);
    return FALSE;
  }

  self->handle = handle;

  return TRUE;
}

static GstFlowReturn
gst_srt_client_sink_send_buffer (GstSRTBaseSink * sink, GstBuffer * buffer)
{
  GstSRTClientSink *self = GST_SRT_CLIENT_SINK (sink);
  GstSRTClientHandle *handle = self->handle;
  GstFlowReturn ret = GST_FLOW_OK;

  if (!gst_srt_base_sink_client_queue_buffer (sink, handle, buffer))
    return GST_FLOW_ERROR;

  while (handle->queue && (ret == GST_FLOW_OK
          || ret == GST_SRT_FLOW_SEND_AGAIN)) {
    ret = gst_srt_base_sink_client_send_message (sink, handle);
  }

  if (ret == GST_SRT_FLOW_SEND_ERROR)
    ret = GST_FLOW_ERROR;

  return ret;
}

static gboolean
gst_srt_client_sink_stop (GstBaseSink * sink)
{
  GstSRTClientSink *self = GST_SRT_CLIENT_SINK (sink);
  GstSRTClientHandle *handle = self->handle;

  GST_DEBUG_OBJECT (self, "closing SRT connection");

  self->handle = NULL;

  if (self->poll_id != SRT_ERROR) {
    if (handle && handle->sock != SRT_INVALID_SOCK)
      srt_epoll_remove_usock (self->poll_id, handle->sock);
    srt_epoll_release (self->poll_id);
    self->poll_id = SRT_ERROR;
  }

  if (handle)
    gst_srt_client_handle_unref (handle);

  return TRUE;
}

static void
gst_srt_client_sink_class_init (GstSRTClientSinkClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS (klass);
  GstBaseSinkClass *gstbasesink_class = GST_BASE_SINK_CLASS (klass);
  GstSRTBaseSinkClass *gstsrtbasesink_class = GST_SRT_BASE_SINK_CLASS (klass);

  gobject_class->set_property = gst_srt_client_sink_set_property;
  gobject_class->get_property = gst_srt_client_sink_get_property;
  gobject_class->finalize = gst_srt_client_sink_finalize;

  properties[PROP_BIND_ADDRESS] =
      g_param_spec_string ("bind-address", "Bind Address",
      "Address to bind socket to (required for rendez-vous mode) ",
      DEFAULT_BIND_ADDRESS,
      G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY | G_PARAM_STATIC_STRINGS);

  properties[PROP_BIND_PORT] =
      g_param_spec_int ("bind-port", "Bind Port",
      "Port to bind socket to (Ignored in rendez-vous mode)", 0,
      G_MAXUINT16, DEFAULT_BIND_PORT,
      G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY | G_PARAM_STATIC_STRINGS);

  properties[PROP_RENDEZ_VOUS] =
      g_param_spec_boolean ("rendez-vous", "Rendez Vous",
      "Work in Rendez-Vous mode instead of client/caller mode",
      DEFAULT_RENDEZ_VOUS,
      G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY | G_PARAM_STATIC_STRINGS);

  properties[PROP_STATS] = g_param_spec_boxed ("stats", "Statistics",
      "SRT Statistics", GST_TYPE_STRUCTURE,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (gobject_class, PROP_LAST, properties);

  gst_element_class_add_static_pad_template (gstelement_class, &sink_template);
  gst_element_class_set_metadata (gstelement_class,
      "SRT client sink", "Sink/Network",
      "Send data over the network via SRT",
      "Justin Kim <justin.kim@collabora.com>");

  gstbasesink_class->start = GST_DEBUG_FUNCPTR (gst_srt_client_sink_start);
  gstbasesink_class->stop = GST_DEBUG_FUNCPTR (gst_srt_client_sink_stop);

  gstsrtbasesink_class->send_buffer =
      GST_DEBUG_FUNCPTR (gst_srt_client_sink_send_buffer);
}

static void
gst_srt_client_sink_init (GstSRTClientSink * self)
{
  self->bind_address = DEFAULT_BIND_ADDRESS;
  self->bind_port = DEFAULT_BIND_PORT;
  self->rendez_vous = DEFAULT_RENDEZ_VOUS;
}
