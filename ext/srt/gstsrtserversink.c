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

#define CLIENTS_GET_LOCK(s) (&(GST_SRT_SERVER_SINK_CAST(s)->clients_lock))
#define CLIENTS_LOCK(d) G_STMT_START { \
    GST_TRACE_OBJECT(d, "Locking from thread %p", g_thread_self()); \
    g_mutex_lock (CLIENTS_GET_LOCK (d)); \
    GST_TRACE_OBJECT(d, "Locked from thread %p", g_thread_self()); \
 } G_STMT_END
#define CLIENTS_UNLOCK(d) G_STMT_START { \
    GST_TRACE_OBJECT(d, "Unlocking from thread %p", g_thread_self()); \
    g_mutex_unlock (CLIENTS_GET_LOCK (d)); \
 } G_STMT_END


/* Must be called with clients lock taken */
static void
gst_srt_server_sink_add_client (GstSRTServerSink * self,
    GstSRTClientHandle * client)
{
  int epoll_event = SRT_EPOLL_ERR;

  GST_DEBUG_OBJECT (self, "Client added");

  srt_epoll_add_usock (self->poll_id, client->sock, &epoll_event);
  g_hash_table_insert (self->clients_hash, &client->sock, client);

  self->need_data = TRUE;
  g_cond_signal (&self->clients_cond);
}

/* Must be called with clients lock taken */
static void
gst_srt_server_sink_remove_client (GstSRTServerSink * self,
    GstSRTClientHandle * client)
{
  GST_DEBUG_OBJECT (self, "Client removed");

  srt_epoll_remove_usock (self->poll_id, client->sock);
  g_hash_table_remove (self->clients_hash, &client->sock);
}

static void
hash_foreach_get_stats (gpointer key, gpointer value, gpointer user_data)
{
  GstSRTClientHandle *client = (GstSRTClientHandle *) value;
  GValue *prop_value = (GValue *) user_data;
  GValue stat = G_VALUE_INIT;

  g_value_init (&stat, GST_TYPE_STRUCTURE);
  g_value_take_boxed (&stat, gst_srt_base_sink_get_stats (client));
  gst_value_array_append_and_take_value (prop_value, &stat);
}

static void
gst_srt_server_sink_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstSRTServerSink *self = GST_SRT_SERVER_SINK (object);

  switch (prop_id) {
    case PROP_STATS:
    {
      CLIENTS_LOCK (self);
      if (self->clients_hash) {
        g_hash_table_foreach (self->clients_hash,
            (GHFunc) hash_foreach_get_stats, value);
      }
      CLIENTS_UNLOCK (self);
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

static void
gst_srt_server_sink_finalize (GObject * object)
{
  GstSRTServerSink *self = GST_SRT_SERVER_SINK (object);

  g_rec_mutex_clear (&self->event_lock);
  g_mutex_clear (&self->clients_lock);
  g_cond_clear (&self->clients_cond);

  g_cancellable_release_fd (self->cancellable);
  g_object_unref (self->cancellable);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static gboolean
gst_srt_server_sink_start_sending (GstSRTServerSink * sink,
    GstSRTClientHandle * client)
{
  int epoll_event = SRT_EPOLL_OUT | SRT_EPOLL_ERR;
  int ret;

  /* Already started, do not need to update epoll */
  if (client->state == SRT_CLIENT_STARTED)
    return TRUE;

  ret = srt_epoll_update_usock (sink->poll_id, client->sock, &epoll_event);
  if (ret == SRT_ERROR) {
    GST_ERROR_OBJECT (sink, "SRT epoll update error");
    return FALSE;
  }

  client->state = SRT_CLIENT_STARTED;

  return TRUE;
}

static gboolean
gst_srt_server_sink_stop_sending (GstSRTServerSink * sink,
    GstSRTClientHandle * client)
{
  int epoll_event = SRT_EPOLL_ERR;
  int ret;

  /* Already stopped, do not need to update epoll */
  if (client->state == SRT_CLIENT_STOPPED)
    return TRUE;

  ret = srt_epoll_update_usock (sink->poll_id, client->sock, &epoll_event);
  if (ret == SRT_ERROR) {
    GST_ERROR_OBJECT (sink, "SRT epoll update error");
    return FALSE;
  }

  client->state = SRT_CLIENT_STOPPED;

  return TRUE;
}

static void
gst_srt_server_sink_emit_client_added (GstSRTClientHandle * client)
{
  GST_DEBUG_OBJECT (client->sink, "Emit client-added");
  g_signal_emit (client->sink, signals[SIG_CLIENT_ADDED], 0, client->sock,
      client->sockaddr);
}

static void
gst_srt_server_sink_emit_client_removed (GstSRTClientHandle * client)
{
  GST_DEBUG_OBJECT (client->sink, "Emit client-removed");
  g_signal_emit (client->sink, signals[SIG_CLIENT_REMOVED], 0, client->sock,
      client->sockaddr);
}

static void
gst_srt_server_sink_client_unref (GstSRTClientHandle * client)
{
  gst_srt_server_sink_emit_client_removed (client);
  gst_srt_client_handle_unref (client);
}

static void
gst_srt_server_sink_epoll_loop (GstSRTServerSink * self)
{
  SRTSOCKET ready[2];
  int rnum = 2;
  gpointer writefds;
  SYSSOCKET cancellable[2];
  struct sockaddr sa;
  int sa_len;
  int wnum;
  gint i;
  GList *to_remove = NULL;
  GList *to_stop = NULL;
  GList *iter;

  if (g_cancellable_is_cancelled (self->cancellable))
    goto cancelled;

  CLIENTS_LOCK (self);
  wnum = g_hash_table_size (self->clients_hash) + 1;
  writefds = g_alloca (sizeof (SRTSOCKET) * wnum);
  CLIENTS_UNLOCK (self);

  if (srt_epoll_wait (self->poll_id, ready, &rnum, (SRTSOCKET *) writefds,
          &wnum, -1, cancellable, &(int) {
          2}, 0, 0) == SRT_ERROR) {
    if (g_cancellable_is_cancelled (self->cancellable))
      goto cancelled;

    GST_ELEMENT_ERROR (self, RESOURCE, FAILED,
        ("SRT error: %s", srt_getlasterror_str ()), (NULL));
    gst_task_stop (self->event_task);
    return;
  }

  if (g_cancellable_is_cancelled (self->cancellable))
    goto cancelled;

  GST_TRACE_OBJECT (self,
      "num readable sockets: %d, num writable sockets: %d", rnum, wnum);

  for (i = 0; i < rnum; i++) {
    GstSRTClientHandle *client;
    SRT_SOCKSTATUS status;

    /* Ignore non-server socket */
    if (ready[i] != self->sock) {
      continue;
    }

    status = srt_getsockstate (ready[i]);
    GST_TRACE_OBJECT (self, "server socket status %d", status);

    if (G_UNLIKELY (status != SRTS_LISTENING)) {
      GST_ELEMENT_ERROR (self, RESOURCE, FAILED,
          ("Server socket is not listening"), (NULL));
      gst_task_stop (self->event_task);
      return;
    }

    client = gst_srt_client_handle_new (GST_SRT_BASE_SINK (self));
    client->sock = srt_accept (self->sock, &sa, &sa_len);

    if (client->sock == SRT_INVALID_SOCK) {
      GST_WARNING_OBJECT (self,
          "detected invalid SRT client socket (reason: %s)",
          srt_getlasterror_str ());
      srt_clearlasterror ();
      gst_srt_client_handle_unref (client);
      continue;
    }

    client->sockaddr = g_socket_address_new_from_native (&sa, sa_len);

    /* Make non-blocking */
    srt_setsockopt (client->sock, 0, SRTO_SNDSYN, &(int) {
        0}, sizeof (int));

    CLIENTS_LOCK (self);
    gst_srt_server_sink_add_client (self, client);
    CLIENTS_UNLOCK (self);
    gst_srt_server_sink_emit_client_added (client);
  }

  CLIENTS_LOCK (self);
  for (i = 0; i < wnum; i++) {
    SRTSOCKET sock;
    GstSRTClientHandle *client;
    gboolean remove_client = FALSE;
    SRT_SOCKSTATUS status;

    sock = ((SRTSOCKET *) writefds)[i];
    client = g_hash_table_lookup (self->clients_hash, &sock);

    if (client == NULL) {
      GST_WARNING_OBJECT (self, "Failed to lookup client");
      continue;
    }

    status = srt_getsockstate (sock);

    if (G_UNLIKELY (status != SRTS_CONNECTED)) {
      GST_DEBUG_OBJECT (self, "Client disconected, status %d", status);
      to_remove = g_list_append (to_remove, gst_srt_client_handle_ref (client));
      continue;
    }

    if (client->queue) {
      GstFlowReturn ret;

      ret =
          gst_srt_base_sink_client_send_message (GST_SRT_BASE_SINK (self),
          client);

      switch (ret) {
        case GST_SRT_FLOW_SEND_ERROR:
          GST_WARNING_OBJECT (self, "Failed to send buffer to peer");
          to_remove = g_list_append (to_remove,
              gst_srt_client_handle_ref (client));
          remove_client = TRUE;
          break;
        case GST_FLOW_ERROR:
          /* ERROR message was posted in _send_message() above */
          gst_task_stop (self->event_task);
          break;
        case GST_SRT_FLOW_SEND_AGAIN:
          /* fallthrough */
        default:
          break;
      }
    }

    /* Stop watching writable event for empty client */
    if (!remove_client && !client->queue) {
      to_stop = g_list_append (to_stop, client);
    }
  }

  for (iter = to_stop; iter; iter = g_list_next (iter)) {
    GstSRTClientHandle *client = (GstSRTClientHandle *) iter->data;
    gst_srt_server_sink_stop_sending (self, client);
  }

  if (to_stop) {
    g_list_free (to_stop);
    GST_TRACE_OBJECT (self, "Need more data");
    self->need_data = TRUE;
    g_cond_signal (&self->clients_cond);
  }

  /* to_remove still holds ref in order to emit client-removed signal
   * after unlock */
  for (iter = to_remove; iter; iter = g_list_next (iter)) {
    GstSRTClientHandle *client = (GstSRTClientHandle *) iter->data;
    gst_srt_server_sink_remove_client (self, client);
  }

  CLIENTS_UNLOCK (self);

  if (to_remove) {
    /* Emit client-removed signal here */
    g_list_free_full (to_remove,
        (GDestroyNotify) gst_srt_server_sink_client_unref);
  }

  return;

cancelled:
  GST_DEBUG_OBJECT (self, "Cancelled");
  return;
}

static gboolean
gst_srt_server_sink_start (GstBaseSink * sink)
{
  GstSRTServerSink *self = GST_SRT_SERVER_SINK (sink);
  GstSRTBaseSink *base = GST_SRT_BASE_SINK (sink);
  GstUri *uri = gst_uri_ref (GST_SRT_BASE_SINK (self)->uri);
  const gchar *host;

  if (gst_uri_get_port (uri) == GST_URI_NO_PORT) {
    GST_ELEMENT_ERROR (sink, RESOURCE, OPEN_WRITE, NULL, (("Invalid port")));
    return FALSE;
  }

  host = gst_uri_get_host (uri);

  self->sock = gst_srt_server_listen (GST_ELEMENT (self),
      TRUE, host, gst_uri_get_port (uri),
      base->latency, &self->poll_id, base->passphrase, base->key_length,
      base->sndbuf_size);

  g_clear_pointer (&uri, gst_uri_unref);

  if (self->sock == SRT_INVALID_SOCK) {
    GST_ERROR_OBJECT (sink, "Failed to create srt socket");
    goto failed;
  }

  /* Add our event fd to cancel srt_epoll_wait */
  srt_epoll_add_ssock (self->poll_id, self->event_fd, NULL);

  self->clients_hash =
      g_hash_table_new_full (g_int_hash, g_int_equal, NULL,
      (GDestroyNotify) gst_srt_client_handle_unref);

  self->event_task =
      gst_task_new ((GstTaskFunction) gst_srt_server_sink_epoll_loop, self,
      NULL);
  gst_task_set_lock (self->event_task, &self->event_lock);

  gst_task_start (self->event_task);

  return TRUE;

failed:
  if (self->poll_id != SRT_ERROR) {
    srt_epoll_release (self->poll_id);
    self->poll_id = SRT_ERROR;
  }

  if (self->sock != SRT_INVALID_SOCK) {
    srt_close (self->sock);
    self->sock = SRT_INVALID_SOCK;
  }

  return FALSE;
}

static GstFlowReturn
gst_srt_server_sink_send_buffer (GstSRTBaseSink * sink, GstBuffer * buffer)
{
  GstSRTServerSink *self = GST_SRT_SERVER_SINK (sink);
  GHashTableIter iter;
  gpointer value;

  CLIENTS_LOCK (self);
  /* no client yet, drop buffer */
  if (g_hash_table_size (self->clients_hash) == 0) {
    CLIENTS_UNLOCK (self);
    return GST_FLOW_OK;
  }

  /* all client queues are filled, wait empty queue */
  while (!self->need_data) {
    GST_LOG_OBJECT (self, "Wait empty client queue");
    g_cond_wait (&self->clients_cond, &self->clients_lock);
    GST_LOG_OBJECT (self, "Wakeup");
    if (g_cancellable_is_cancelled (self->cancellable)) {
      GST_LOG_OBJECT (self, "Flushing");
      CLIENTS_UNLOCK (self);
      return GST_FLOW_FLUSHING;
    }
  }

  g_hash_table_iter_init (&iter, self->clients_hash);
  while (g_hash_table_iter_next (&iter, NULL, &value)) {
    GstSRTClientHandle *client = (GstSRTClientHandle *) value;

    if (!gst_srt_base_sink_client_queue_buffer (sink, client, buffer))
      goto error;

    if (!gst_srt_server_sink_start_sending (self, client))
      goto error;
  }

  CLIENTS_UNLOCK (self);

  return GST_FLOW_OK;

error:
  CLIENTS_UNLOCK (self);
  GST_ERROR_OBJECT (sink, "Failed to send buffer");
  return GST_FLOW_ERROR;
}

static gboolean
hash_foreach_remove (gpointer key, gpointer value, gpointer user_data)
{
  GstSRTServerSink *self = GST_SRT_SERVER_SINK (user_data);
  GstSRTClientHandle *client = (GstSRTClientHandle *) value;

  srt_epoll_remove_usock (self->poll_id, client->sock);
  gst_srt_server_sink_emit_client_removed (client);

  return TRUE;
}

static gboolean
gst_srt_server_sink_stop (GstBaseSink * sink)
{
  GstSRTServerSink *self = GST_SRT_SERVER_SINK (sink);

  GST_DEBUG_OBJECT (self, "closing client sockets");

  /* Set cancelled to terminate listen thread */
  gst_task_stop (self->event_task);
  g_cancellable_cancel (self->cancellable);
  g_cond_signal (&self->clients_cond);
  gst_task_join (self->event_task);
  gst_object_unref (self->event_task);
  self->event_task = NULL;
  g_cancellable_reset (self->cancellable);

  g_hash_table_foreach_remove (self->clients_hash, hash_foreach_remove, self);
  g_clear_pointer (&self->clients_hash, g_hash_table_destroy);

  GST_DEBUG_OBJECT (self, "closing SRT connection");
  srt_epoll_remove_usock (self->poll_id, self->sock);
  srt_epoll_release (self->poll_id);
  srt_close (self->sock);

  return TRUE;
}

static gboolean
gst_srt_server_sink_unlock (GstBaseSink * sink)
{
  GstSRTServerSink *self = GST_SRT_SERVER_SINK (sink);

  GST_DEBUG_OBJECT (self, "Unlock");
  g_cancellable_cancel (self->cancellable);
  g_cond_signal (&self->clients_cond);

  return TRUE;
}

static gboolean
gst_srt_server_sink_unlock_stop (GstBaseSink * sink)
{
  GstSRTServerSink *self = GST_SRT_SERVER_SINK (sink);

  GST_DEBUG_OBJECT (self, "Unlock stop");
  g_cancellable_reset (self->cancellable);

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
  g_rec_mutex_init (&self->event_lock);
  g_mutex_init (&self->clients_lock);
  g_cond_init (&self->clients_cond);

  self->cancellable = g_cancellable_new ();
  self->event_fd = g_cancellable_get_fd (self->cancellable);
}
