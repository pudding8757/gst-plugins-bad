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
    GST_TYPE_SRT_BASE_SINK, GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT,
        "srtserversink", 0, "SRT Server Sink"));

typedef struct
{
  SRTSOCKET sock;
  GSocketAddress *sockaddr;
  gboolean sent_headers;
} SRTClient;

static SRTClient *
srt_client_new (void)
{
  SRTClient *client = g_new0 (SRTClient, 1);
  client->sock = SRT_INVALID_SOCK;
  return client;
}

static void
srt_client_free (SRTClient * client)
{
  g_return_if_fail (client != NULL);

  g_clear_object (&client->sockaddr);

  if (client->sock != SRT_INVALID_SOCK) {
    srt_close (client->sock);
  }

  g_free (client);
}

static void
srt_emit_client_removed (SRTClient * client, gpointer user_data)
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
        SRTClient *client = item->data;
        GValue tmp = G_VALUE_INIT;

        g_value_init (&tmp, GST_TYPE_STRUCTURE);
        g_value_take_boxed (&tmp, gst_srt_base_sink_get_stats (client->sockaddr,
                client->sock));
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

/* listen loop for accept client socket */
static gboolean
idle_listen_callback (gpointer data)
{
  GstSRTBaseSink *sink = GST_SRT_BASE_SINK (data);
  GstSRTServerSink *self = GST_SRT_SERVER_SINK (data);
  SRTClient *client;
  SRTSOCKET ready[2];
  SYSSOCKET cancellable[2];
  struct sockaddr sa;
  int sa_len;

  if (srt_epoll_wait (sink->poll_id, ready, &(int) {
          2}, 0, 0, -1, cancellable, &(int) {
          2}, 0, 0) == SRT_ERROR) {
    int srt_errno;

    if (g_cancellable_is_cancelled (sink->cancellable))
      goto cancelled;

    srt_errno = srt_getlasterror (NULL);

    if (srt_errno != SRT_ETIMEOUT) {
      GST_ELEMENT_ERROR (self, RESOURCE, FAILED,
          ("SRT error: %s", srt_getlasterror_str ()), (NULL));
      return FALSE;
    }
  }

  if (g_cancellable_is_cancelled (sink->cancellable))
    goto cancelled;

  client = srt_client_new ();
  client->sock = srt_accept (sink->sock, &sa, &sa_len);

  if (client->sock == SRT_INVALID_SOCK) {
    GST_WARNING_OBJECT (self, "detected invalid SRT client socket (reason: %s)",
        srt_getlasterror_str ());
    srt_clearlasterror ();
    srt_client_free (client);
    return TRUE;
  }

  client->sockaddr = g_socket_address_new_from_native (&sa, sa_len);

  GST_OBJECT_LOCK (self);
  self->clients = g_list_append (self->clients, client);
  GST_OBJECT_UNLOCK (self);

  g_signal_emit (self, signals[SIG_CLIENT_ADDED], 0, client->sock,
      client->sockaddr);
  GST_DEBUG_OBJECT (self, "client added");

  return TRUE;

cancelled:
  GST_DEBUG_OBJECT (self, "Cancelled");
  return TRUE;
}

static gpointer
thread_func (gpointer data)
{
  GstSRTServerSink *self = GST_SRT_SERVER_SINK (data);

  g_main_loop_run (self->loop);

  return NULL;
}

static gboolean
gst_srt_server_sink_open (GstSRTBaseSink * sink, const gchar * host, guint port,
    gint * poll_id, SRTSOCKET * socket)
{
  GstSRTServerSink *self = GST_SRT_SERVER_SINK (sink);
  gint latency;
  gchar *passphrase = NULL;
  gint key_length;
  GError *error = NULL;

  GST_OBJECT_LOCK (sink);
  latency = sink->latency;
  passphrase = g_strdup (sink->passphrase);
  key_length = sink->key_length;
  GST_OBJECT_UNLOCK (sink);

  *socket = gst_srt_server_listen (GST_ELEMENT (self),
      TRUE, host, port, latency, poll_id, passphrase, key_length);

  g_free (passphrase);

  if (*socket == SRT_INVALID_SOCK) {
    GST_ERROR_OBJECT (sink, "Failed to create srt socket");
    return FALSE;
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
    return FALSE;
  }

  return TRUE;
}

static GstFlowReturn
send_mapped_buffer_internal (GstSRTBaseSink * sink,
    const GstMapInfo * mapinfo, SRTClient * client)
{
  if (srt_sendmsg2 (client->sock, (char *) mapinfo->data, mapinfo->size,
          0) == SRT_ERROR) {
    GST_WARNING_OBJECT (sink, "%s", srt_getlasterror_str ());
    return GST_FLOW_ERROR;
  }

  return GST_FLOW_OK;
}

static GstFlowReturn
send_buffer_internal (GstSRTBaseSink * sink,
    GstBuffer * buf, gpointer user_data)
{
  GstMapInfo info;
  SRTClient *client = user_data;
  GstFlowReturn ret;

  if (!gst_buffer_map (buf, &info, GST_MAP_READ)) {
    GST_ELEMENT_ERROR (sink, RESOURCE, READ,
        ("Could not map the input stream"), (NULL));
    return GST_FLOW_ERROR;
  }

  ret = send_mapped_buffer_internal (sink, &info, client);

  gst_buffer_unmap (buf, &info);

  return ret;
}

static GstFlowReturn
gst_srt_server_sink_send_buffer (GstSRTBaseSink * sink, GstBuffer * buf)
{
  GstSRTServerSink *self = GST_SRT_SERVER_SINK (sink);
  GList *clients = self->clients;
  GstFlowReturn ret;
  GstMapInfo info;

  if (!gst_buffer_map (buf, &info, GST_MAP_READ)) {
    GST_ELEMENT_ERROR (self, RESOURCE, READ,
        ("Could not map the input stream"), (NULL));
    return GST_FLOW_ERROR;
  }

  GST_OBJECT_LOCK (sink);
  while (clients != NULL) {
    SRTClient *client = clients->data;
    clients = clients->next;

    if (!client->sent_headers) {
      ret = gst_srt_base_sink_send_headers (sink, send_buffer_internal, client);
      if (ret != GST_FLOW_OK)
        goto err;

      client->sent_headers = TRUE;
    }

    ret = send_mapped_buffer_internal (sink, &info, client);
    if (ret != GST_FLOW_OK)
      goto err;

    continue;

  err:
    self->clients = g_list_remove (self->clients, client);
    GST_OBJECT_UNLOCK (sink);
    g_signal_emit (self, signals[SIG_CLIENT_REMOVED], 0, client->sock,
        client->sockaddr);
    srt_client_free (client);
    GST_OBJECT_LOCK (sink);
  }
  GST_OBJECT_UNLOCK (sink);

  gst_buffer_unmap (buf, &info);

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
  g_list_free_full (clients, (GDestroyNotify) srt_client_free);

  /* Set cancelled to terminate listen thread */
  g_cancellable_cancel (GST_SRT_BASE_SINK (self)->cancellable);

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
  g_cancellable_reset (GST_SRT_BASE_SINK (self)->cancellable);

  return GST_BASE_SINK_CLASS (parent_class)->stop (sink);
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

  gst_element_class_set_metadata (gstelement_class,
      "SRT server sink", "Sink/Network",
      "Send data over the network via SRT",
      "Justin Kim <justin.kim@collabora.com>");

  gstbasesink_class->stop = GST_DEBUG_FUNCPTR (gst_srt_server_sink_stop);

  gstsrtbasesink_class->open = GST_DEBUG_FUNCPTR (gst_srt_server_sink_open);
  gstsrtbasesink_class->send_buffer =
      GST_DEBUG_FUNCPTR (gst_srt_server_sink_send_buffer);
}

static void
gst_srt_server_sink_init (GstSRTServerSink * self)
{
}
