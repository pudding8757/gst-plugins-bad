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

typedef struct
{
  SRTSOCKET sock;
  GSocketAddress *sockaddr;
  gboolean sent_headers;
  GPtrArray *bufqueue;

  gint ref_count;
} SRTClient;

static SRTClient *
srt_client_new (void)
{
  SRTClient *client = g_new0 (SRTClient, 1);
  client->sock = SRT_INVALID_SOCK;
  client->bufqueue =
      g_ptr_array_new_with_free_func ((GDestroyNotify) gst_buffer_unref);

  client->ref_count = 1;

  return client;
}

static SRTClient *
srt_client_ref (SRTClient * client)
{
  g_return_val_if_fail (client != NULL && client->ref_count > 0, NULL);

  g_atomic_int_add (&client->ref_count, 1);

  return client;
}

static void
srt_client_unref (SRTClient * client)
{
  g_return_if_fail (client != NULL && client->ref_count > 0);

  if (g_atomic_int_dec_and_test (&client->ref_count)) {
    g_clear_object (&client->sockaddr);

    if (client->sock != SRT_INVALID_SOCK) {
      srt_close (client->sock);
    }

    g_ptr_array_free (client->bufqueue, TRUE);
    g_free (client);
  }
}

static void
gst_srt_server_sink_add_client (GstSRTServerSink * self, SRTClient * client)
{
  int epoll_event = SRT_EPOLL_ERR;
  int poll_id = GST_SRT_BASE_SINK (self)->poll_id;

  GST_DEBUG_OBJECT (self, "Client added");

  CLIENTS_LOCK (self);
  if (poll_id != SRT_ERROR)
    srt_epoll_add_usock (poll_id, client->sock, &epoll_event);

  g_hash_table_insert (self->client_hash, &client->sock, client);
  self->need_data = TRUE;
  g_cond_broadcast (&self->clients_cond);
  CLIENTS_UNLOCK (self);

  g_signal_emit (self, signals[SIG_CLIENT_ADDED], 0, client->sock,
      client->sockaddr);
}

static void
gst_srt_server_sink_remove_client (GstSRTServerSink * self, SRTClient * client,
    gboolean remove_from_table)
{
  int poll_id = GST_SRT_BASE_SINK (self)->poll_id;

  GST_DEBUG_OBJECT (self, "Client removed");

  client = srt_client_ref (client);

  CLIENTS_LOCK (self);
  if (poll_id != SRT_ERROR)
    srt_epoll_remove_usock (poll_id, client->sock);

  if (remove_from_table) {
    g_hash_table_remove (self->client_hash, &client->sock);
  }
  CLIENTS_UNLOCK (self);

  g_signal_emit (self, signals[SIG_CLIENT_REMOVED], 0, client->sock,
      client->sockaddr);
  srt_client_unref (client);
}

static gboolean
_client_hash_foreach_remove (gpointer key, gpointer value, gpointer user_data)
{
  GstSRTServerSink *self = GST_SRT_SERVER_SINK (user_data);
  SRTClient *client = (SRTClient *) value;

  gst_srt_server_sink_remove_client (self, client, FALSE);

  return TRUE;
}

static void
_client_hash_foreach_get_stats (gpointer key, gpointer value,
    gpointer user_data)
{
  SRTClient *client = (SRTClient *) value;
  GValue *prop_value = (GValue *) user_data;
  GValue stat = G_VALUE_INIT;

  g_value_init (&stat, GST_TYPE_STRUCTURE);
  g_value_take_boxed (&stat, gst_srt_base_sink_get_stats (client->sockaddr,
          client->sock));
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
      g_hash_table_foreach (self->client_hash,
          (GHFunc) _client_hash_foreach_get_stats, value);
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

  if (self->client_hash)
    g_hash_table_destroy (self->client_hash);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static gboolean
gst_srt_server_sink_start_sending (GstSRTServerSink * sink, SRTClient * client)
{
  int epoll_event = SRT_EPOLL_OUT | SRT_EPOLL_ERR;
  int ret;

  ret = srt_epoll_update_usock (GST_SRT_BASE_SINK (sink)->poll_id, client->sock,
      &epoll_event);

  return (ret != SRT_ERROR);
}

static gboolean
gst_srt_server_sink_stop_sending (GstSRTServerSink * sink, SRTClient * client)
{
  int epoll_event = SRT_EPOLL_ERR;
  int ret;

  ret = srt_epoll_update_usock (GST_SRT_BASE_SINK (sink)->poll_id, client->sock,
      &epoll_event);

  sink->need_data = TRUE;
  g_cond_broadcast (&sink->clients_cond);

  return (ret != SRT_ERROR);
}

static void
gst_srt_server_sink_epoll_loop (GstSRTServerSink * self)
{
  GstSRTBaseSink *sink = GST_SRT_BASE_SINK (self);
  SRTSOCKET ready[2];
  int rnum = 2;
  gpointer writefds;
  SYSSOCKET cancellable[2];
  struct sockaddr sa;
  int sa_len;
  int wnum;
  gint i;

  if (g_cancellable_is_cancelled (sink->cancellable))
    goto cancelled;

  CLIENTS_LOCK (self);
  wnum = g_hash_table_size (self->client_hash) + 1;
  writefds = g_alloca (sizeof (SRTSOCKET) * wnum);
  CLIENTS_UNLOCK (self);

  if (srt_epoll_wait (sink->poll_id, ready, &rnum, (SRTSOCKET *) writefds,
          &wnum, -1, cancellable, &(int) {
          2}, 0, 0) == SRT_ERROR) {
    int srt_errno;

    if (g_cancellable_is_cancelled (sink->cancellable))
      goto cancelled;

    srt_errno = srt_getlasterror (NULL);

    if (srt_errno != SRT_ETIMEOUT) {
      GST_ELEMENT_ERROR (self, RESOURCE, FAILED,
          ("SRT error: %s", srt_getlasterror_str ()), (NULL));
      gst_task_stop (self->event_task);
      return;
    }
  }

  if (g_cancellable_is_cancelled (sink->cancellable))
    goto cancelled;

  GST_TRACE_OBJECT (sink,
      "num readable sockets: %d, num writable sockets: %d", rnum, wnum);

  for (i = 0; i < rnum; i++) {
    SRTClient *client;
    SRT_SOCKSTATUS status;

    /* Ignore non-server socket */
    if (ready[i] != sink->sock) {
      continue;
    }

    status = srt_getsockstate (ready[i]);
    GST_TRACE_OBJECT (sink, "server socket status %d", status);

    if (G_UNLIKELY (status != SRTS_LISTENING)) {
      GST_ELEMENT_ERROR (self, RESOURCE, FAILED,
          ("Server socket is not listening"), (NULL));
      gst_task_stop (self->event_task);
      return;
    }

    client = srt_client_new ();
    client->sock = srt_accept (sink->sock, &sa, &sa_len);

    if (client->sock == SRT_INVALID_SOCK) {
      GST_WARNING_OBJECT (self,
          "detected invalid SRT client socket (reason: %s)",
          srt_getlasterror_str ());
      srt_clearlasterror ();
      srt_client_unref (client);
      continue;
    }

    client->sockaddr = g_socket_address_new_from_native (&sa, sa_len);

    srt_setsockopt (client->sock, 0, SRTO_SNDSYN, &(int) {
        0}, sizeof (int));

    gst_srt_server_sink_add_client (self, client);
  }

  CLIENTS_LOCK (sink);
  for (i = 0; i < wnum; i++) {
    SRTSOCKET sock;
    SRTClient *client;
    gboolean remove_client = FALSE;
    SRT_SOCKSTATUS status;

    if (g_cancellable_is_cancelled (sink->cancellable)) {
      CLIENTS_UNLOCK (sink);
      goto cancelled;
    }

    sock = ((SRTSOCKET *) writefds)[i];
    client = g_hash_table_lookup (self->client_hash, &sock);

    if (client == NULL) {
      GST_WARNING_OBJECT (self, "Failed to lookup client");
      continue;
    }

    status = srt_getsockstate (sock);

    if (G_UNLIKELY (status != SRTS_CONNECTED)) {
      GST_DEBUG_OBJECT (self, "Client disconected, status %d", status);
      remove_client = TRUE;
      goto next;
    }

    if (client->bufqueue->len > 0) {
      GstMapInfo info;
      GstBuffer *buf;

      buf = g_ptr_array_index (client->bufqueue, 0);
      if (!gst_buffer_map (buf, &info, GST_MAP_READ)) {
        GST_ELEMENT_ERROR (self, RESOURCE, READ,
            ("Could not map the input stream"), (NULL));
        gst_task_stop (self->event_task);
        return;
      }

      if (srt_sendmsg2 (sock, (char *) info.data, info.size, NULL) == SRT_ERROR) {
        GST_WARNING_OBJECT (self, "Failed to send buffer to client");
        remove_client = TRUE;
      }

      gst_buffer_unmap (buf, &info);
      g_ptr_array_remove_index (client->bufqueue, 0);
    }

  next:
    if (G_UNLIKELY (remove_client)) {
      CLIENTS_UNLOCK (sink);
      gst_srt_server_sink_remove_client (self, client, TRUE);
      CLIENTS_LOCK (sink);
    } else if (client->bufqueue->len == 0) {
      gst_srt_server_sink_stop_sending (self, client);
    }
  }

  CLIENTS_UNLOCK (sink);

  return;

cancelled:
  GST_DEBUG_OBJECT (self, "Cancelled");
  return;
}

static gboolean
gst_srt_server_sink_open (GstSRTBaseSink * sink, const gchar * host, guint port,
    gint * poll_id, SRTSOCKET * socket)
{
  GstSRTServerSink *self = GST_SRT_SERVER_SINK (sink);
  gint latency;
  gchar *passphrase = NULL;
  gint key_length;

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

  self->client_hash =
      g_hash_table_new_full (g_int_hash, g_int_equal, NULL,
      (GDestroyNotify) srt_client_unref);

  self->event_task =
      gst_task_new ((GstTaskFunction) gst_srt_server_sink_epoll_loop, self,
      NULL);
  gst_task_set_lock (self->event_task, &self->event_lock);

  gst_task_start (self->event_task);

  return TRUE;
}

/* Must be called with client lock taken */
static GstFlowReturn
gst_srt_server_client_enqueue (GstSRTBaseSink * sink,
    GstBuffer * buf, gpointer user_data)
{
  SRTClient *client = user_data;

  if (g_cancellable_is_cancelled (sink->cancellable))
    return GST_FLOW_FLUSHING;

  gst_buffer_ref (buf);
  g_ptr_array_add (client->bufqueue, buf);
  gst_srt_server_sink_start_sending (GST_SRT_SERVER_SINK (sink), client);

  return GST_FLOW_OK;
}

static GstFlowReturn
gst_srt_server_sink_send_buffer (GstSRTBaseSink * sink, GstBuffer * buf)
{
  GstSRTServerSink *self = GST_SRT_SERVER_SINK (sink);
  GstFlowReturn ret;
  GHashTableIter iter;
  gpointer value;

  CLIENTS_LOCK (sink);
  if (g_hash_table_size (self->client_hash) == 0) {
    CLIENTS_UNLOCK (sink);
    return GST_FLOW_OK;
  }

  while (!self->need_data) {
    g_cond_wait (&self->clients_cond, &self->clients_lock);
    if (g_cancellable_is_cancelled (sink->cancellable)) {
      CLIENTS_UNLOCK (sink);
      return GST_FLOW_FLUSHING;
    }
  }

  g_hash_table_iter_init (&iter, self->client_hash);
  while (g_hash_table_iter_next (&iter, NULL, &value)) {
    SRTClient *client = (SRTClient *) value;
    if (!client->sent_headers) {
      ret =
          gst_srt_base_sink_send_headers (sink, gst_srt_server_client_enqueue,
          client);
      if (ret != GST_FLOW_OK) {
        CLIENTS_UNLOCK (sink);
        return ret;
      }

      client->sent_headers = TRUE;
    }

    ret = gst_srt_server_client_enqueue (sink, buf, client);
    if (ret != GST_FLOW_OK) {
      CLIENTS_UNLOCK (sink);
      return ret;
    }
  }

  CLIENTS_UNLOCK (sink);

  return GST_FLOW_OK;
}

static gboolean
gst_srt_server_sink_stop (GstBaseSink * sink)
{
  GstSRTServerSink *self = GST_SRT_SERVER_SINK (sink);
  GHashTable *client_hash;

  GST_DEBUG_OBJECT (self, "closing client sockets");

  CLIENTS_LOCK (sink);
  client_hash = self->client_hash;
  self->client_hash = NULL;
  CLIENTS_UNLOCK (sink);

  g_hash_table_foreach_remove (client_hash, _client_hash_foreach_remove, self);
  g_hash_table_destroy (client_hash);

  /* Set cancelled to terminate listen thread */
  gst_task_stop (self->event_task);
  g_cancellable_cancel (GST_SRT_BASE_SINK (self)->cancellable);
  gst_task_join (self->event_task);
  gst_object_unref (self->event_task);
  self->event_task = NULL;
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
  g_rec_mutex_init (&self->event_lock);
  g_mutex_init (&self->clients_lock);
  g_cond_init (&self->clients_cond);
}
