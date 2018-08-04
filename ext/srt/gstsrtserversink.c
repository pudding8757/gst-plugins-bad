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
 * </refsect2>
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstsrtserversink.h"
#include "gstsrt.h"
#include <srt/srt.h>
#include <gio/gio.h>

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

#define GST_CAT_DEFAULT gst_debug_srt_server_sink
GST_DEBUG_CATEGORY (GST_CAT_DEFAULT);

enum
{
  PROP_STATS = 1,
  /*< private > */
  PROP_LAST
};

static GParamSpec *properties[PROP_LAST];

enum
{
  SIG_CLIENT_ADDED,
  SIG_CLIENT_REMOVED,

  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

#define gst_srt_server_sink_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstSRTServerSink, gst_srt_server_sink,
    GST_TYPE_SRT_BASE_SINK,
    GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, "srtserversink", 0,
        "SRT Server Sink"));

static void
srt_emit_client_removed (GstSRTClientHandle * client, gpointer user_data)
{
  GstSRTServerSink *self = GST_SRT_SERVER_SINK (user_data);
  g_return_if_fail (client != NULL && GST_IS_SRT_SERVER_SINK (self));

  g_signal_emit (self, signals[SIG_CLIENT_REMOVED], 0, client->sock,
      client->sockaddr);
}

static void
gst_srt_server_sink_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstSRTServerSink *self = GST_SRT_SERVER_SINK (object);

  switch (prop_id) {
    case PROP_STATS:
    {
      GList *item;

      GST_OBJECT_LOCK (self);
      for (item = self->clients; item; item = item->next) {
        GstSRTClientHandle *client = item->data;
        GValue tmp = G_VALUE_INIT;

        g_value_init (&tmp, GST_TYPE_STRUCTURE);
        g_value_take_boxed (&tmp, gst_srt_base_sink_get_stats (client));
        gst_value_array_append_and_take_value (value, &tmp);
      }
      GST_OBJECT_UNLOCK (self);
      break;
    }
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_srt_server_sink_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec)
{
  switch (prop_id) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static gboolean
idle_listen_callback (gpointer data)
{
  GstSRTServerSink *self = GST_SRT_SERVER_SINK (data);
  gboolean ret = TRUE;

  GstSRTClientHandle *client;
  SRTSOCKET ready[2];
  struct sockaddr sa;
  int sa_len;

  if (srt_epoll_wait (self->poll_id, ready, &(int) {
          2}, 0, 0, -1, 0, 0, 0, 0) == SRT_ERROR) {
    GST_ELEMENT_ERROR (self, RESOURCE, FAILED,
        ("SRT error: %s", srt_getlasterror_str ()), (NULL));
    ret = FALSE;
    goto out;
  }

  client = gst_srt_client_handle_new (GST_SRT_BASE_SINK (self));
  client->sock = srt_accept (self->sock, &sa, &sa_len);

  if (client->sock == SRT_INVALID_SOCK) {
    GST_WARNING_OBJECT (self, "detected invalid SRT client socket (reason: %s)",
        srt_getlasterror_str ());
    srt_clearlasterror ();
    gst_srt_client_handle_unref (client);
    ret = FALSE;
    goto out;
  }

  client->sockaddr = g_socket_address_new_from_native (&sa, sa_len);

  GST_OBJECT_LOCK (self);
  self->clients = g_list_append (self->clients, client);
  GST_OBJECT_UNLOCK (self);

  g_signal_emit (self, signals[SIG_CLIENT_ADDED], 0, client->sock,
      client->sockaddr);
  GST_DEBUG_OBJECT (self, "client added");

out:
  return ret;
}

static gpointer
thread_func (gpointer data)
{
  GstSRTServerSink *self = GST_SRT_SERVER_SINK (data);

  g_main_loop_run (self->loop);

  return NULL;
}

static gboolean
gst_srt_server_sink_start (GstBaseSink * sink)
{
  GstSRTServerSink *self = GST_SRT_SERVER_SINK (sink);
  GstSRTBaseSink *base = GST_SRT_BASE_SINK (sink);
  GstUri *uri = gst_uri_ref (GST_SRT_BASE_SINK (self)->uri);
  GError *error = NULL;
  gboolean ret = TRUE;
  const gchar *host;

  if (gst_uri_get_port (uri) == GST_URI_NO_PORT) {
    GST_ELEMENT_ERROR (sink, RESOURCE, OPEN_WRITE, NULL, (("Invalid port")));
    return FALSE;
  }

  host = gst_uri_get_host (uri);

  self->sock = gst_srt_server_listen (GST_ELEMENT (self),
      TRUE, host, gst_uri_get_port (uri),
      base->latency, &self->poll_id, base->passphrase, base->key_length);

  if (self->sock == SRT_INVALID_SOCK) {
    GST_ERROR_OBJECT (sink, "Failed to create srt socket");
    goto failed;
  }

  self->context = g_main_context_new ();

  self->server_source = g_idle_source_new ();
  g_source_set_callback (self->server_source,
      (GSourceFunc) idle_listen_callback, gst_object_ref (self),
      (GDestroyNotify) gst_object_unref);

  g_source_attach (self->server_source, self->context);
  self->loop = g_main_loop_new (self->context, TRUE);

  self->thread = g_thread_try_new ("srtserversink", thread_func, self, &error);
  if (error != NULL) {
    GST_WARNING_OBJECT (self, "failed to create thread (reason: %s)",
        error->message);
    ret = FALSE;
  }

  g_clear_pointer (&uri, gst_uri_unref);

  return ret;

failed:
  if (self->poll_id != SRT_ERROR) {
    srt_epoll_release (self->poll_id);
    self->poll_id = SRT_ERROR;
  }

  if (self->sock != SRT_INVALID_SOCK) {
    srt_close (self->sock);
    self->sock = SRT_INVALID_SOCK;
  }

  g_clear_error (&error);
  g_clear_pointer (&uri, gst_uri_unref);

  return FALSE;
}

static GstFlowReturn
gst_srt_server_sink_send_buffer (GstSRTBaseSink * sink, GstBuffer * buffer)
{
  GstSRTServerSink *self = GST_SRT_SERVER_SINK (sink);
  GList *clients = self->clients;

  GST_OBJECT_LOCK (sink);
  while (clients != NULL) {
    GstSRTClientHandle *client = clients->data;
    GstFlowReturn ret = GST_FLOW_OK;
    clients = clients->next;

    if (!gst_srt_base_sink_client_queue_buffer (sink, client, buffer))
      return GST_FLOW_ERROR;

    while (client->queue && (ret == GST_FLOW_OK
            || ret == GST_SRT_FLOW_SEND_AGAIN)) {
      ret = gst_srt_base_sink_client_send_message (sink, client);
    }

    if (ret != GST_FLOW_OK) {
      if (ret == GST_SRT_FLOW_SEND_ERROR) {
        GST_DEBUG_OBJECT (self,
            "Failed to send message, remove client %p", client);
        goto err;
      }

      GST_OBJECT_UNLOCK (sink);
      return ret;
    }

    continue;

  err:
    self->clients = g_list_remove (self->clients, client);
    GST_OBJECT_UNLOCK (sink);
    g_signal_emit (self, signals[SIG_CLIENT_REMOVED], 0, client->sock,
        client->sockaddr);
    gst_srt_client_handle_unref (client);
    GST_OBJECT_LOCK (sink);
  }
  GST_OBJECT_UNLOCK (sink);

  return GST_FLOW_OK;
}

static gboolean
gst_srt_server_sink_stop (GstBaseSink * sink)
{
  GstSRTServerSink *self = GST_SRT_SERVER_SINK (sink);
  GList *clients;

  GST_DEBUG_OBJECT (self, "closing client sockets");

  GST_OBJECT_LOCK (sink);
  clients = self->clients;
  self->clients = NULL;
  GST_OBJECT_UNLOCK (sink);

  g_list_foreach (clients, (GFunc) srt_emit_client_removed, self);
  g_list_free_full (clients, (GDestroyNotify) gst_srt_client_handle_unref);

  GST_DEBUG_OBJECT (self, "closing SRT connection");
  srt_epoll_remove_usock (self->poll_id, self->sock);
  srt_epoll_release (self->poll_id);
  srt_close (self->sock);

  if (self->loop) {
    g_main_loop_quit (self->loop);
    g_thread_join (self->thread);
    g_clear_pointer (&self->loop, g_main_loop_unref);
    g_clear_pointer (&self->thread, g_thread_unref);
  }

  if (self->server_source) {
    g_source_destroy (self->server_source);
    g_clear_pointer (&self->server_source, g_source_unref);
  }

  g_clear_pointer (&self->context, g_main_context_unref);

  return TRUE;
}

static gboolean
gst_srt_server_sink_unlock (GstBaseSink * sink)
{
  GstSRTServerSink *self = GST_SRT_SERVER_SINK (sink);

  self->cancelled = TRUE;

  return TRUE;
}

static gboolean
gst_srt_server_sink_unlock_stop (GstBaseSink * sink)
{
  GstSRTServerSink *self = GST_SRT_SERVER_SINK (sink);

  self->cancelled = FALSE;

  return TRUE;
}

static void
gst_srt_server_sink_class_init (GstSRTServerSinkClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS (klass);
  GstBaseSinkClass *gstbasesink_class = GST_BASE_SINK_CLASS (klass);
  GstSRTBaseSinkClass *gstsrtbasesink_class = GST_SRT_BASE_SINK_CLASS (klass);

  gobject_class->set_property = gst_srt_server_sink_set_property;
  gobject_class->get_property = gst_srt_server_sink_get_property;

  properties[PROP_STATS] = gst_param_spec_array ("stats", "Statistics",
      "Array of GstStructures containing SRT statistics",
      g_param_spec_boxed ("stats", "Statistics",
          "Statistics for one client", GST_TYPE_STRUCTURE,
          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS),
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (gobject_class, PROP_LAST, properties);

  /**
   * GstSRTServerSink::client-added:
   * @gstsrtserversink: the srtserversink element that emitted this signal
   * @sock: the client socket descriptor that was added to srtserversink
   * @addr: the pointer of "struct sockaddr" that describes the @sock
   * @addr_len: the length of @addr
   *
   * The given socket descriptor was added to srtserversink.
   */
  signals[SIG_CLIENT_ADDED] =
      g_signal_new ("client-added", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET (GstSRTServerSinkClass, client_added),
      NULL, NULL, g_cclosure_marshal_generic, G_TYPE_NONE,
      2, G_TYPE_INT, G_TYPE_SOCKET_ADDRESS);

  /**
   * GstSRTServerSink::client-removed:
   * @gstsrtserversink: the srtserversink element that emitted this signal
   * @sock: the client socket descriptor that was added to srtserversink
   * @addr: the pointer of "struct sockaddr" that describes the @sock
   * @addr_len: the length of @addr
   *
   * The given socket descriptor was removed from srtserversink.
   */
  signals[SIG_CLIENT_REMOVED] =
      g_signal_new ("client-removed", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET (GstSRTServerSinkClass,
          client_removed), NULL, NULL, g_cclosure_marshal_generic, G_TYPE_NONE,
      2, G_TYPE_INT, G_TYPE_SOCKET_ADDRESS);

  gst_element_class_add_static_pad_template (gstelement_class, &sink_template);
  gst_element_class_set_metadata (gstelement_class,
      "SRT server sink", "Sink/Network",
      "Send data over the network via SRT",
      "Justin Kim <justin.kim@collabora.com>");

  gstbasesink_class->start = GST_DEBUG_FUNCPTR (gst_srt_server_sink_start);
  gstbasesink_class->stop = GST_DEBUG_FUNCPTR (gst_srt_server_sink_stop);
  gstbasesink_class->unlock = GST_DEBUG_FUNCPTR (gst_srt_server_sink_unlock);
  gstbasesink_class->unlock_stop =
      GST_DEBUG_FUNCPTR (gst_srt_server_sink_unlock_stop);

  gstsrtbasesink_class->send_buffer =
      GST_DEBUG_FUNCPTR (gst_srt_server_sink_send_buffer);
}

static void
gst_srt_server_sink_init (GstSRTServerSink * self)
{
}
