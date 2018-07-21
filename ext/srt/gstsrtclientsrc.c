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
 * SECTION:element-srtclientsrc
 * @title: srtclientsrc
 *
 * srtclientsrc is a network source that reads <ulink url="http://www.srtalliance.org/">SRT</ulink>
 * packets from the network. Although SRT is a protocol based on UDP, srtclientsrc works like
 * a client socket of connection-oriented protocol.
 *
 * <refsect2>
 * <title>Examples</title>
 * |[
 * gst-launch-1.0 -v srtclientsrc uri="srt://127.0.0.1:7001" ! fakesink
 * ]| This pipeline shows how to connect SRT server by setting #GstSRTClientSrc:uri property.
 *
 * |[
 * gst-launch-1.0 -v srtclientsrc uri="srt://192.168.1.10:7001" rendez-vous ! fakesink
 * ]| This pipeline shows how to connect SRT server by setting #GstSRTClientSrc:uri property and using the rendez-vous mode.
 * </refsect2>
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstsrtclientsrc.h"
#include <srt/srt.h>
#include <gio/gio.h>

#include "gstsrt.h"

#define GST_CAT_DEFAULT gst_debug_srt_client_src
GST_DEBUG_CATEGORY (GST_CAT_DEFAULT);

enum
{
  PROP_BIND_ADDRESS = 1,
  PROP_BIND_PORT,
  PROP_RENDEZ_VOUS,

  /*< private > */
  PROP_LAST
};

static GParamSpec *properties[PROP_LAST];

#define DEFAULT_BIND_ADDRESS  NULL
#define DEFAULT_BIND_PORT     0
#define DEFAULT_RENDEZ_VOUS   FALSE

#define gst_srt_client_src_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstSRTClientSrc, gst_srt_client_src,
    GST_TYPE_SRT_BASE_SRC,
    GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, "srtclientsrc", 0,
        "SRT Client Source"));

static void
gst_srt_client_src_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstSRTClientSrc *self = GST_SRT_CLIENT_SRC (object);

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
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_srt_client_src_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec)
{
  GstSRTClientSrc *self = GST_SRT_CLIENT_SRC (object);

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
gst_srt_client_src_finalize (GObject * object)
{
  GstSRTClientSrc *self = GST_SRT_CLIENT_SRC (object);

  g_free (self->bind_address);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static GstFlowReturn
gst_srt_client_src_receive_message (GstSRTBaseSrc * src, SRTSOCKET socket,
    GstBuffer * outbuf)
{
  GstMapInfo info;
  gint recv_len;
  SRT_SOCKSTATUS status = srt_getsockstate (socket);

  if (G_UNLIKELY (status != SRTS_CONNECTED)) {
    GST_ERROR_OBJECT (src, "Connection closed");
    return GST_FLOW_ERROR;
  }

  if (!gst_buffer_map (outbuf, &info, GST_MAP_WRITE)) {
    GST_ELEMENT_ERROR (src, RESOURCE, READ,
        ("Could not map the buffer for writing "), (NULL));
    return GST_FLOW_ERROR;
  }

  recv_len = srt_recvmsg (socket, (char *) info.data,
      gst_buffer_get_size (outbuf));

  gst_buffer_unmap (outbuf, &info);

  if (recv_len == SRT_ERROR) {
    GST_ELEMENT_ERROR (src, RESOURCE, READ,
        (NULL), ("srt_recvmsg error: %s", srt_getlasterror_str ()));
    return GST_FLOW_ERROR;
  } else if (recv_len == 0) {
    return GST_FLOW_EOS;
  }

  gst_buffer_resize (outbuf, 0, recv_len);

  GST_LOG_OBJECT (src, "filled buffer from _get of size %" G_GSIZE_FORMAT,
      gst_buffer_get_size (outbuf));

  return GST_FLOW_OK;
}

static gboolean
gst_srt_client_src_open (GstSRTBaseSrc * src, const gchar * host, guint port,
    gint * poll_id, SRTSOCKET * socket)
{
  GstSRTClientSrc *self = GST_SRT_CLIENT_SRC (src);
  GSocketAddress *socket_address = NULL;

  *socket = gst_srt_client_connect (GST_ELEMENT (src), FALSE, host, port,
      self->rendez_vous, self->bind_address, self->bind_port, src->latency,
      &socket_address, poll_id, src->passphrase, src->key_length);

  g_clear_object (&socket_address);

  if (*socket == SRT_INVALID_SOCK)
    return FALSE;

  /* Make non-blocking */
  srt_setsockopt (*socket, 0, SRTO_RCVSYN, &(int) {
      0}, sizeof (int));

  return TRUE;
}

static void
gst_srt_client_src_class_init (GstSRTClientSrcClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS (klass);
  GstSRTBaseSrcClass *gstsrtbasesrc_class = GST_SRT_BASE_SRC_CLASS (klass);

  gobject_class->set_property = gst_srt_client_src_set_property;
  gobject_class->get_property = gst_srt_client_src_get_property;
  gobject_class->finalize = gst_srt_client_src_finalize;

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

  g_object_class_install_properties (gobject_class, PROP_LAST, properties);

  gst_element_class_set_metadata (gstelement_class,
      "SRT client source", "Source/Network",
      "Receive data over the network via SRT",
      "Justin Kim <justin.kim@collabora.com>");

  gstsrtbasesrc_class->open = GST_DEBUG_FUNCPTR (gst_srt_client_src_open);
  gstsrtbasesrc_class->receive_message =
      GST_DEBUG_FUNCPTR (gst_srt_client_src_receive_message);
}

static void
gst_srt_client_src_init (GstSRTClientSrc * self)
{
  self->bind_address = DEFAULT_BIND_ADDRESS;
  self->bind_port = DEFAULT_BIND_PORT;
  self->rendez_vous = DEFAULT_RENDEZ_VOUS;
}
