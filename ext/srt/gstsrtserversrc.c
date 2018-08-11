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
 * SECTION:element-srtserversrc
 * @title: srtserversrc
 *
 * srtserversrc is a network source that reads <ulink url="http://www.srtalliance.org/">SRT</ulink>
 * packets from the network. Although SRT is a protocol based on UDP, srtserversrc works like
 * a server socket of connection-oriented protocol, but it accepts to only one client connection.
 *
 * <refsect2>
 * <title>Examples</title>
 * |[
 * gst-launch-1.0 -v srtserversrc uri="srt://:7001" ! fakesink
 * ]| This pipeline shows how to bind SRT server by setting #GstSRTServerSrc:uri property.
 * </refsect2>
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstsrtserversrc.h"
#include "gstsrt.h"
#include <gio/gio.h>

#define GST_CAT_DEFAULT gst_debug_srt_server_src
GST_DEBUG_CATEGORY (GST_CAT_DEFAULT);

enum
{
  SIG_CLIENT_ADDED,
  SIG_CLIENT_CLOSED,

  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

#define gst_srt_server_src_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstSRTServerSrc, gst_srt_server_src,
    GST_TYPE_SRT_BASE_SRC,
    GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, "srtserversrc", 0,
        "SRT Server Source"));

static void
gst_srt_server_src_finalize (GObject * object)
{
  GstSRTServerSrc *self = GST_SRT_SERVER_SRC (object);

  g_clear_object (&self->client_sockaddr);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static GstFlowReturn
gst_srt_server_src_receive_message (GstSRTBaseSrc * src, SRTSOCKET socket,
    GstBuffer * outbuf)
{
  GstSRTServerSrc *self = GST_SRT_SERVER_SRC (src);
  GstMapInfo info;
  SRTSOCKET client_sock;
  gint recv_len;
  struct sockaddr client_sa = { 0, };
  size_t client_sa_len = sizeof (struct sockaddr_in);
  SRT_SOCKSTATUS status;

  if (G_UNLIKELY (socket == src->sock)) {
    client_sock = srt_accept (socket, &client_sa, (int *) &client_sa_len);

    GST_DEBUG_OBJECT (self, "checking client sock");
    if (client_sock == SRT_INVALID_SOCK) {
      GST_WARNING_OBJECT (self,
          "detected invalid SRT client socket (reason: %s)",
          srt_getlasterror_str ());
      srt_clearlasterror ();

      return GST_SRT_FLOW_AGAIN;
    }

    if (self->has_client) {
      GST_DEBUG_OBJECT (src, "We have client already, close new client");
      srt_close (client_sock);

      return GST_SRT_FLOW_AGAIN;
    }

    self->has_client = TRUE;
    self->client_sock = client_sock;
    g_clear_object (&self->client_sockaddr);
    self->client_sockaddr = g_socket_address_new_from_native (&client_sa,
        client_sa_len);
    g_signal_emit (self, signals[SIG_CLIENT_ADDED], 0,
        self->client_sock, self->client_sockaddr);

    srt_epoll_add_usock (src->poll_id, self->client_sock, &(int) {
        SRT_EPOLL_IN | SRT_EPOLL_ERR});

    return GST_SRT_FLOW_AGAIN;
  }

  status = srt_getsockstate (socket);

  if (G_UNLIKELY (status != SRTS_CONNECTED))
    goto closed;

  if (!gst_buffer_map (outbuf, &info, GST_MAP_WRITE)) {
    GST_ELEMENT_ERROR (src, RESOURCE, WRITE,
        ("Could not map the output stream"), (NULL));
    return GST_FLOW_ERROR;
  }

  recv_len = srt_recvmsg (self->client_sock, (char *) info.data,
      gst_buffer_get_size (outbuf));

  gst_buffer_unmap (outbuf, &info);

  if (recv_len == SRT_ERROR) {
    goto closed;
  } else if (recv_len == 0) {
    return GST_FLOW_EOS;
  }

  gst_buffer_resize (outbuf, 0, recv_len);

  GST_LOG_OBJECT (src, "filled buffer from _get of size %" G_GSIZE_FORMAT,
      gst_buffer_get_size (outbuf));

  return GST_FLOW_OK;

closed:
  GST_DEBUG_OBJECT (self, "Client connection closed");
  g_signal_emit (self, signals[SIG_CLIENT_CLOSED], 0,
      self->client_sock, self->client_sockaddr);

  srt_epoll_remove_usock (src->poll_id, self->client_sock);

  srt_close (self->client_sock);
  self->client_sock = SRT_INVALID_SOCK;
  g_clear_object (&self->client_sockaddr);
  self->has_client = FALSE;
  return GST_SRT_FLOW_AGAIN;
}

static gboolean
gst_srt_server_src_open (GstSRTBaseSrc * src, const gchar * host, guint port,
    gint * poll_id, SRTSOCKET * socket)
{
  *socket = gst_srt_server_listen (GST_ELEMENT (src),
      FALSE, host, port, src->latency, poll_id, src->passphrase,
      src->key_length, src->rcvbuf_size);

  return (*socket != SRT_INVALID_SOCK);
}

static gboolean
gst_srt_server_src_stop (GstBaseSrc * src)
{
  GstSRTServerSrc *self = GST_SRT_SERVER_SRC (src);

  if (self->client_sock != SRT_INVALID_SOCK) {
    g_signal_emit (self, signals[SIG_CLIENT_CLOSED], 0,
        self->client_sock, self->client_sockaddr);
    srt_close (self->client_sock);
    g_clear_object (&self->client_sockaddr);
    self->client_sock = SRT_INVALID_SOCK;
    self->has_client = FALSE;
  }

  return GST_BASE_SRC_CLASS (parent_class)->stop (src);
}

static void
gst_srt_server_src_class_init (GstSRTServerSrcClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS (klass);
  GstBaseSrcClass *gstbasesrc_class = GST_BASE_SRC_CLASS (klass);
  GstSRTBaseSrcClass *gstsrtbasesrc_class = GST_SRT_BASE_SRC_CLASS (klass);

  gobject_class->finalize = gst_srt_server_src_finalize;

  /**
   * GstSRTServerSrc::client-added:
   * @gstsrtserversrc: the srtserversrc element that emitted this signal
   * @sock: the client socket descriptor that was added to srtserversrc
   * @addr: the pointer of "struct sockaddr" that describes the @sock
   * @addr_len: the length of @addr
   *
   * The given socket descriptor was added to srtserversrc.
   */
  signals[SIG_CLIENT_ADDED] =
      g_signal_new ("client-added", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET (GstSRTServerSrcClass, client_added),
      NULL, NULL, g_cclosure_marshal_generic, G_TYPE_NONE,
      2, G_TYPE_INT, G_TYPE_SOCKET_ADDRESS);

  /**
   * GstSRTServerSrc::client-closed:
   * @gstsrtserversrc: the srtserversrc element that emitted this signal
   * @sock: the client socket descriptor that was added to srtserversrc
   * @addr: the pointer of "struct sockaddr" that describes the @sock
   * @addr_len: the length of @addr
   *
   * The given socket descriptor was closed.
   */
  signals[SIG_CLIENT_CLOSED] =
      g_signal_new ("client-closed", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET (GstSRTServerSrcClass, client_closed),
      NULL, NULL, g_cclosure_marshal_generic, G_TYPE_NONE,
      2, G_TYPE_INT, G_TYPE_SOCKET_ADDRESS);

  gst_element_class_set_metadata (gstelement_class,
      "SRT Server source", "Source/Network",
      "Receive data over the network via SRT",
      "Justin Kim <justin.kim@collabora.com>");

  gstbasesrc_class->stop = GST_DEBUG_FUNCPTR (gst_srt_server_src_stop);
  gstsrtbasesrc_class->open = GST_DEBUG_FUNCPTR (gst_srt_server_src_open);
  gstsrtbasesrc_class->receive_message =
      GST_DEBUG_FUNCPTR (gst_srt_server_src_receive_message);
}

static void
gst_srt_server_src_init (GstSRTServerSrc * self)
{
  self->client_sock = SRT_INVALID_SOCK;
}
