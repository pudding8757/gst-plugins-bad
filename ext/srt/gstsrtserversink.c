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

#define SRT_DEFAULT_POLL_TIMEOUT -1

#define GST_CAT_DEFAULT gst_debug_srt_server_sink
GST_DEBUG_CATEGORY (GST_CAT_DEFAULT);

#define CLIENT_GET_LOCK(d) (&(GST_SRT_SERVER_SINK_CAST(d)->priv->client_lock))
#define CLIENT_LOCK(d) g_mutex_lock (CLIENT_GET_LOCK(d))
#define CLIENT_UNLOCK(d) g_mutex_unlock (CLIENT_GET_LOCK(d))

struct _GstSRTServerSinkPrivate
{
  gboolean cancelled;

  gint poll_timeout;

  GstTask *accept_task;
  GRecMutex accept_lock;
  GMutex client_lock;
  GCancellable *cancellable;
  int cancellable_fd;

  GList *clients;
};

#define GST_SRT_SERVER_SINK_GET_PRIVATE(obj)  \
       (G_TYPE_INSTANCE_GET_PRIVATE ((obj), GST_TYPE_SRT_SERVER_SINK, GstSRTServerSinkPrivate))

enum
{
  PROP_POLL_TIMEOUT = 1,
  PROP_STATS,
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
    GST_TYPE_SRT_BASE_SINK, G_ADD_PRIVATE (GstSRTServerSink)
    GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, "srtserversink", 0,
        "SRT Server Sink"));

typedef struct
{
  int sock;
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
  GstSRTServerSinkPrivate *priv = self->priv;

  switch (prop_id) {
    case PROP_POLL_TIMEOUT:
      g_value_set_int (value, priv->poll_timeout);
      break;
    case PROP_STATS:
    {
      GList *item;

      GST_OBJECT_LOCK (self);
      for (item = priv->clients; item; item = item->next) {
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
  GstSRTServerSink *self = GST_SRT_SERVER_SINK (object);
  GstSRTServerSinkPrivate *priv = self->priv;

  switch (prop_id) {
    case PROP_POLL_TIMEOUT:
      priv->poll_timeout = g_value_get_int (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_srt_server_sink_finalize (GObject * object)
{
  GstSRTServerSink *self = GST_SRT_SERVER_SINK (object);
  GstSRTServerSinkPrivate *priv = self->priv;

  if (priv->accept_task) {
    g_cancellable_cancel (priv->cancellable);

    gst_task_join (priv->accept_task);
    gst_object_unref (priv->accept_task);
    priv->accept_task = NULL;
  }

  g_object_unref (priv->cancellable);

  g_rec_mutex_clear (&priv->accept_lock);
  g_mutex_clear (&priv->client_lock);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_srt_server_sink_accept_loop (GstSRTServerSink * self)
{
  GstSRTServerSinkPrivate *priv = self->priv;

  SRTClient *client;
  SRTSOCKET ready[2];
  SYSSOCKET cancellable[2];
  struct sockaddr sa;
  int sa_len;

  if (g_cancellable_is_cancelled (priv->cancellable))
    goto cancelled;

  /* Set timeout infinite, since we can interrupt */
  if (srt_epoll_wait (GST_SRT_BASE_SINK_POLL_ID (self), ready, &(int) {
          2}, 0, 0, priv->poll_timeout, cancellable, &(int) {
          2}, 0, 0) == -1) {
    if (g_cancellable_is_cancelled (priv->cancellable))
      goto cancelled;

    if (srt_getlasterror (NULL) != SRT_ETIMEOUT) {
      GST_ELEMENT_ERROR (self, RESOURCE, FAILED,
          ("SRT error: %s", srt_getlasterror_str ()), (NULL));
      gst_task_stop (priv->accept_task);
      return;
    }
  }

  if (g_cancellable_is_cancelled (priv->cancellable))
    goto cancelled;

  client = srt_client_new ();
  client->sock = srt_accept (GST_SRT_BASE_SINK_SOCKET (self), &sa, &sa_len);

  if (client->sock == SRT_INVALID_SOCK) {
    GST_WARNING_OBJECT (self, "detected invalid SRT client socket (reason: %s)",
        srt_getlasterror_str ());
    srt_clearlasterror ();
    srt_client_free (client);
    return;
  }

  client->sockaddr = g_socket_address_new_from_native (&sa, sa_len);

  CLIENT_LOCK (self);
  priv->clients = g_list_append (priv->clients, client);
  CLIENT_UNLOCK (self);

  g_signal_emit (self, signals[SIG_CLIENT_ADDED], 0, client->sock,
      client->sockaddr);
  GST_DEBUG_OBJECT (self, "client added");

  return;

cancelled:
  GST_LOG_OBJECT (self, "Cancelled");
  return;
}

static gboolean
gst_srt_server_sink_open (GstSRTBaseSink * sink, const gchar * host, guint port,
    SRTSOCKET * sock, gint * poll_id)
{
  GstSRTServerSink *self = GST_SRT_SERVER_SINK (sink);
  GstSRTServerSinkPrivate *priv = self->priv;
  gint latency = gst_srt_base_sink_get_latency (sink);
  const gchar *passphrase = gst_srt_base_sink_get_passphrase (sink);
  gint key_len = gst_srt_base_sink_get_key_length (sink);

  if (!port) {
    GST_ELEMENT_ERROR (sink, RESOURCE, OPEN_WRITE, NULL, (("Invalid port")));
    return FALSE;
  }

  *sock = gst_srt_server_listen (GST_ELEMENT (sink), NULL,
      TRUE, host, port, latency, poll_id, passphrase, key_len);

  if (*sock == SRT_INVALID_SOCK) {
    GST_ERROR_OBJECT (sink, "Failed to create srt socket");
    goto failed;
  }

  /* HACK: Since srt_epoll_wait() is not cancellable, install our event fd to
   * epoll */
  priv->cancellable_fd = g_cancellable_get_fd (priv->cancellable);
  srt_epoll_add_ssock (*poll_id, priv->cancellable_fd, NULL);

  priv->accept_task =
      gst_task_new ((GstTaskFunction) gst_srt_server_sink_accept_loop, self,
      NULL);
  gst_task_set_lock (priv->accept_task, &priv->accept_lock);
  gst_task_start (priv->accept_task);

  return TRUE;

failed:
  if (*poll_id != SRT_ERROR) {
    srt_epoll_release (*poll_id);
    *poll_id = SRT_ERROR;
  }

  if (*sock != SRT_INVALID_SOCK) {
    srt_close (*sock);
    *sock = SRT_INVALID_SOCK;
  }

  return FALSE;
}

static gboolean
send_buffer_internal (GstSRTBaseSink * sink,
    const GstMapInfo * mapinfo, gpointer user_data)
{
  SRTClient *client = user_data;

  if (srt_sendmsg2 (client->sock, (char *) mapinfo->data, mapinfo->size,
          0) == SRT_ERROR) {
    GST_WARNING_OBJECT (sink, "%s", srt_getlasterror_str ());
    return FALSE;
  }

  return TRUE;
}

static gboolean
gst_srt_server_sink_send_buffer (GstSRTBaseSink * sink,
    const GstMapInfo * mapinfo)
{
  GstSRTServerSink *self = GST_SRT_SERVER_SINK (sink);
  GstSRTServerSinkPrivate *priv = self->priv;
  GList *clients = priv->clients;

  CLIENT_LOCK (sink);
  while (clients != NULL) {
    SRTClient *client = clients->data;
    clients = clients->next;

    if (!client->sent_headers) {
      if (!gst_srt_base_sink_send_headers (sink, send_buffer_internal, client))
        goto err;

      client->sent_headers = TRUE;
    }

    if (!send_buffer_internal (sink, mapinfo, client))
      goto err;

    continue;

  err:
    priv->clients = g_list_remove (priv->clients, client);
    CLIENT_UNLOCK (sink);
    g_signal_emit (self, signals[SIG_CLIENT_REMOVED], 0, client->sock,
        client->sockaddr);
    srt_client_free (client);
    GST_OBJECT_LOCK (sink);
  }
  CLIENT_UNLOCK (sink);

  return TRUE;
}

static gboolean
gst_srt_server_sink_close (GstSRTBaseSink * sink)
{
  GstSRTServerSink *self = GST_SRT_SERVER_SINK (sink);
  GstSRTServerSinkPrivate *priv = self->priv;
  GList *clients;

  GST_DEBUG_OBJECT (self, "closing client sockets");

  if (priv->accept_task) {
    gst_task_stop (priv->accept_task);
    g_cancellable_cancel (priv->cancellable);
  }

  CLIENT_LOCK (sink);
  clients = priv->clients;
  priv->clients = NULL;
  CLIENT_UNLOCK (sink);

  g_list_foreach (clients, (GFunc) srt_emit_client_removed, self);
  g_list_free_full (clients, (GDestroyNotify) srt_client_free);

  return TRUE;
}

static gboolean
gst_srt_server_sink_unlock (GstBaseSink * sink)
{
  GstSRTServerSink *self = GST_SRT_SERVER_SINK (sink);
  GstSRTServerSinkPrivate *priv = GST_SRT_SERVER_SINK_GET_PRIVATE (self);

  if (priv->accept_task) {
    GST_LOG_OBJECT (sink, "Pause accept task");
    gst_task_pause (priv->accept_task);
  }

  GST_LOG_OBJECT (sink, "Unlock");
  g_cancellable_cancel (priv->cancellable);

  return TRUE;
}

static gboolean
gst_srt_server_sink_unlock_stop (GstBaseSink * sink)
{
  GstSRTServerSink *self = GST_SRT_SERVER_SINK (sink);
  GstSRTServerSinkPrivate *priv = GST_SRT_SERVER_SINK_GET_PRIVATE (self);

  GST_DEBUG_OBJECT (sink, "Unlock stop");
  g_cancellable_reset (priv->cancellable);

  if (priv->accept_task) {
    GST_DEBUG_OBJECT (sink, "Start accept task");
    gst_task_start (priv->accept_task);
  }

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
  gobject_class->finalize = gst_srt_server_sink_finalize;

  properties[PROP_POLL_TIMEOUT] =
      g_param_spec_int ("poll-timeout", "Poll Timeout",
      "Return poll wait after timeout miliseconds (-1 = infinite)", -1,
      G_MAXINT32, SRT_DEFAULT_POLL_TIMEOUT,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

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

  gstbasesink_class->unlock = GST_DEBUG_FUNCPTR (gst_srt_server_sink_unlock);
  gstbasesink_class->unlock_stop =
      GST_DEBUG_FUNCPTR (gst_srt_server_sink_unlock_stop);

  gstsrtbasesink_class->open = GST_DEBUG_FUNCPTR (gst_srt_server_sink_open);
  gstsrtbasesink_class->close = GST_DEBUG_FUNCPTR (gst_srt_server_sink_close);
  gstsrtbasesink_class->send_buffer =
      GST_DEBUG_FUNCPTR (gst_srt_server_sink_send_buffer);
}

static void
gst_srt_server_sink_init (GstSRTServerSink * self)
{
  GstSRTServerSinkPrivate *priv = GST_SRT_SERVER_SINK_GET_PRIVATE (self);
  priv->poll_timeout = SRT_DEFAULT_POLL_TIMEOUT;
  priv->cancellable = g_cancellable_new ();
  priv->cancellable_fd = -1;

  g_rec_mutex_init (&priv->accept_lock);
  g_mutex_init (&priv->client_lock);

  self->priv = priv;
}
