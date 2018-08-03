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

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

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

  if (self->poll_id != SRT_ERROR) {
    srt_epoll_release (self->poll_id);
    self->poll_id = SRT_ERROR;
  }

  if (self->sock != SRT_INVALID_SOCK) {
    srt_close (self->sock);
    self->sock = SRT_INVALID_SOCK;
  }

  g_free (self->bind_address);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static GstFlowReturn
gst_srt_client_src_fill (GstPushSrc * src, GstBuffer * outbuf)
{
  GstSRTClientSrc *self = GST_SRT_CLIENT_SRC (src);
  GstFlowReturn ret = GST_FLOW_OK;
  GstMapInfo info;
  SRTSOCKET ready[2];
  gint recv_len;

  if (srt_epoll_wait (self->poll_id, ready, &(int) {
          2}, 0, 0, -1, 0, 0, 0, 0) == SRT_ERROR) {
    GST_ELEMENT_ERROR (src, RESOURCE, READ,
        (NULL), ("srt_epoll_wait error: %s", srt_getlasterror_str ()));
    ret = GST_FLOW_ERROR;
    srt_clearlasterror ();
    goto out;
  }

  if (!gst_buffer_map (outbuf, &info, GST_MAP_WRITE)) {
    GST_ELEMENT_ERROR (src, RESOURCE, READ,
        ("Could not map the buffer for writing "), (NULL));
    ret = GST_FLOW_ERROR;
    goto out;
  }

  recv_len = srt_recvmsg (self->sock, (char *) info.data,
      gst_buffer_get_size (outbuf));

  gst_buffer_unmap (outbuf, &info);

  if (recv_len == SRT_ERROR) {
    GST_ELEMENT_ERROR (src, RESOURCE, READ,
        (NULL), ("srt_recvmsg error: %s", srt_getlasterror_str ()));
    ret = GST_FLOW_ERROR;
    goto out;
  } else if (recv_len == 0) {
    ret = GST_FLOW_EOS;
    goto out;
  }

  gst_buffer_resize (outbuf, 0, recv_len);

  GST_LOG_OBJECT (src, "filled buffer from _get of size %" G_GSIZE_FORMAT,
      gst_buffer_get_size (outbuf));

out:
  return ret;
}

static gboolean
gst_srt_client_src_start (GstBaseSrc * src)
{
  GstSRTClientSrc *self = GST_SRT_CLIENT_SRC (src);
  GstSRTBaseSrc *base = GST_SRT_BASE_SRC (src);
  GstUri *uri = gst_uri_ref (base->uri);
  GSocketAddress *socket_address = NULL;

  self->sock = gst_srt_client_connect (GST_ELEMENT (src), FALSE,
      gst_uri_get_host (uri), gst_uri_get_port (uri), self->rendez_vous,
      self->bind_address, self->bind_port, base->latency,
      &socket_address, &self->poll_id, base->passphrase, base->key_length);

  g_clear_object (&socket_address);
  g_clear_pointer (&uri, gst_uri_unref);

  return (self->sock != SRT_INVALID_SOCK);
}

static gboolean
gst_srt_client_src_stop (GstBaseSrc * src)
{
  GstSRTClientSrc *self = GST_SRT_CLIENT_SRC (src);

  if (self->poll_id != SRT_ERROR) {
    if (self->sock != SRT_INVALID_SOCK)
      srt_epoll_remove_usock (self->poll_id, self->sock);
    srt_epoll_release (self->poll_id);
  }
  self->poll_id = SRT_ERROR;

  GST_DEBUG_OBJECT (self, "closing SRT connection");
  if (self->sock != SRT_INVALID_SOCK)
    srt_close (self->sock);
  self->sock = SRT_INVALID_SOCK;

  return TRUE;
}

static void
gst_srt_client_src_class_init (GstSRTClientSrcClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS (klass);
  GstBaseSrcClass *gstbasesrc_class = GST_BASE_SRC_CLASS (klass);
  GstPushSrcClass *gstpushsrc_class = GST_PUSH_SRC_CLASS (klass);

  gobject_class->set_property = gst_srt_client_src_set_property;
  gobject_class->get_property = gst_srt_client_src_get_property;
  gobject_class->finalize = gst_srt_client_src_finalize;

  properties[PROP_BIND_ADDRESS] =
      g_param_spec_string ("bind-address", "Bind Address",
      "Address to bind socket to (required for rendez-vous mode) ", NULL,
      G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY | G_PARAM_STATIC_STRINGS);

  properties[PROP_BIND_PORT] =
      g_param_spec_int ("bind-port", "Bind Port",
      "Port to bind socket to (Ignored in rendez-vous mode)", 0,
      G_MAXUINT16, 0,
      G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY | G_PARAM_STATIC_STRINGS);

  properties[PROP_RENDEZ_VOUS] =
      g_param_spec_boolean ("rendez-vous", "Rendez Vous",
      "Work in Rendez-Vous mode instead of client/caller mode", FALSE,
      G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (gobject_class, PROP_LAST, properties);

  gst_element_class_add_static_pad_template (gstelement_class, &src_template);
  gst_element_class_set_metadata (gstelement_class,
      "SRT client source", "Source/Network",
      "Receive data over the network via SRT",
      "Justin Kim <justin.kim@collabora.com>");

  gstbasesrc_class->start = GST_DEBUG_FUNCPTR (gst_srt_client_src_start);
  gstbasesrc_class->stop = GST_DEBUG_FUNCPTR (gst_srt_client_src_stop);

  gstpushsrc_class->fill = GST_DEBUG_FUNCPTR (gst_srt_client_src_fill);
}

static void
gst_srt_client_src_init (GstSRTClientSrc * self)
{
  self->sock = SRT_INVALID_SOCK;
  self->poll_id = SRT_ERROR;
  self->rendez_vous = FALSE;
  self->bind_address = NULL;
  self->bind_port = 0;
}
