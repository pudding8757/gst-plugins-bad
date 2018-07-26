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

#include "gstsrtbasesrc.h"
#include "gstsrt.h"

#define GST_CAT_DEFAULT gst_debug_srt_base_src
GST_DEBUG_CATEGORY (GST_CAT_DEFAULT);

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

enum
{
  PROP_URI = 1,
  PROP_CAPS,
  PROP_LATENCY,
  PROP_PASSPHRASE,
  PROP_KEY_LENGTH,

  /*< private > */
  PROP_LAST
};

static GParamSpec *properties[PROP_LAST];

static void gst_srt_base_src_uri_handler_init (gpointer g_iface,
    gpointer iface_data);
static gchar *gst_srt_base_src_uri_get_uri (GstURIHandler * handler);
static gboolean gst_srt_base_src_uri_set_uri (GstURIHandler * handler,
    const gchar * uri, GError ** error);

#define gst_srt_base_src_parent_class parent_class
G_DEFINE_ABSTRACT_TYPE_WITH_CODE (GstSRTBaseSrc, gst_srt_base_src,
    GST_TYPE_PUSH_SRC, G_IMPLEMENT_INTERFACE (GST_TYPE_URI_HANDLER,
        gst_srt_base_src_uri_handler_init)
    GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, "srtbasesrc", 0,
        "SRT Base Source"));

static void
gst_srt_base_src_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstSRTBaseSrc *self = GST_SRT_BASE_SRC (object);

  switch (prop_id) {
    case PROP_URI:
      if (self->uri != NULL) {
        gchar *uri_str = gst_srt_base_src_uri_get_uri (GST_URI_HANDLER (self));
        g_value_take_string (value, uri_str);
      }
      break;
    case PROP_CAPS:
      GST_OBJECT_LOCK (self);
      gst_value_set_caps (value, self->caps);
      GST_OBJECT_UNLOCK (self);
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
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_srt_base_src_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec)
{
  GstSRTBaseSrc *self = GST_SRT_BASE_SRC (object);

  switch (prop_id) {
    case PROP_URI:
      gst_srt_base_src_uri_set_uri (GST_URI_HANDLER (self),
          g_value_get_string (value), NULL);
      break;
    case PROP_CAPS:
      GST_OBJECT_LOCK (self);
      g_clear_pointer (&self->caps, gst_caps_unref);
      self->caps = gst_caps_copy (gst_value_get_caps (value));
      GST_OBJECT_UNLOCK (self);
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
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_srt_base_src_finalize (GObject * object)
{
  GstSRTBaseSrc *self = GST_SRT_BASE_SRC (object);

  g_clear_pointer (&self->uri, gst_uri_unref);
  g_clear_pointer (&self->caps, gst_caps_unref);
  g_clear_pointer (&self->passphrase, g_free);

  g_cancellable_release_fd (self->cancellable);
  g_object_unref (self->cancellable);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static GstCaps *
gst_srt_base_src_get_caps (GstBaseSrc * src, GstCaps * filter)
{
  GstSRTBaseSrc *self = GST_SRT_BASE_SRC (src);
  GstCaps *result, *caps = NULL;

  GST_OBJECT_LOCK (self);
  if (self->caps != NULL) {
    caps = gst_caps_ref (self->caps);
  }
  GST_OBJECT_UNLOCK (self);

  if (caps) {
    if (filter) {
      result = gst_caps_intersect_full (filter, caps, GST_CAPS_INTERSECT_FIRST);
      gst_caps_unref (caps);
    } else {
      result = caps;
    }
  } else {
    result = (filter) ? gst_caps_ref (filter) : gst_caps_new_any ();
  }

  return result;
}

static gboolean
gst_srt_base_src_start (GstBaseSrc * src)
{
  GstSRTBaseSrc *self = GST_SRT_BASE_SRC (src);
  GstSRTBaseSrcClass *bclass;
  gchar *host = NULL;
  guint port;

  GST_OBJECT_LOCK (self);
  if (G_UNLIKELY (self->uri == NULL ||
          gst_uri_get_port (self->uri) == GST_URI_NO_PORT)) {
    GST_OBJECT_UNLOCK (self);
    GST_ELEMENT_ERROR (src, RESOURCE, OPEN_WRITE, NULL, (("Invalid port")));
    return FALSE;
  }

  host = g_strdup (gst_uri_get_host (self->uri));
  port = gst_uri_get_port (self->uri);
  GST_OBJECT_UNLOCK (self);

  bclass = GST_SRT_BASE_SRC_GET_CLASS (src);
  if (!bclass->open (self, host, port, &self->poll_id, &self->sock)) {
    GST_ERROR_OBJECT (src, "Failed to create srt socket");
    goto failed;
  }

  /* HACK: Since srt_epoll_wait() is not cancellable, install our event fd to
   * epoll */
  srt_epoll_add_ssock (self->poll_id, self->event_fd, NULL);

  g_free (host);

  return TRUE;

failed:
  if (self->poll_id != SRT_ERROR) {
    srt_epoll_release (self->poll_id);
    self->poll_id = SRT_ERROR;
  }

  if (self->sock != SRT_ERROR) {
    srt_close (self->sock);
    self->sock = SRT_ERROR;
  }

  g_free (host);

  return FALSE;
}

static gboolean
gst_srt_base_src_stop (GstBaseSrc * src)
{
  GstSRTBaseSrc *self = GST_SRT_BASE_SRC (src);

  GST_DEBUG_OBJECT (self, "release SRT epoll");
  if (self->poll_id != SRT_ERROR) {
    if (self->sock != SRT_INVALID_SOCK)
      srt_epoll_remove_usock (self->poll_id, self->sock);
    srt_epoll_remove_ssock (self->poll_id, self->event_fd);
    srt_epoll_release (self->poll_id);
  }
  self->poll_id = SRT_ERROR;

  GST_DEBUG_OBJECT (self, "close SRT socket");
  if (self->sock != SRT_INVALID_SOCK)
    srt_close (self->sock);
  self->sock = SRT_INVALID_SOCK;

  return TRUE;
}

static gboolean
gst_srt_base_src_unlock (GstBaseSrc * src)
{
  GstSRTBaseSrc *self = GST_SRT_BASE_SRC (src);

  GST_DEBUG_OBJECT (self, "Unlock");
  g_cancellable_cancel (self->cancellable);

  return TRUE;
}

static gboolean
gst_srt_base_src_unlock_stop (GstBaseSrc * src)
{
  GstSRTBaseSrc *self = GST_SRT_BASE_SRC (src);

  GST_DEBUG_OBJECT (self, "Unlock stop");
  g_cancellable_reset (self->cancellable);

  return TRUE;
}

static GstFlowReturn
gst_srt_base_src_fill (GstPushSrc * src, GstBuffer * outbuf)
{
  GstSRTBaseSrc *self = GST_SRT_BASE_SRC (src);
  GstSRTBaseSrcClass *bclass = GST_SRT_BASE_SRC_GET_CLASS (self);
  SRTSOCKET ready[2];
  int rnum = 2;
  SYSSOCKET cancellable[2];
  gint i;

retry:
  /* Wake up only if there are socket event including cancelled event */
  if (srt_epoll_wait (self->poll_id, ready, &rnum, 0, 0, -1, cancellable, &(int) {
          2}, 0, 0) == SRT_ERROR) {
    if (g_cancellable_is_cancelled (self->cancellable))
      goto cancelled;

    GST_ELEMENT_ERROR (src, RESOURCE, FAILED,
        ("SRT error: %s", srt_getlasterror_str ()), (NULL));
    return GST_FLOW_ERROR;
  }

  if (g_cancellable_is_cancelled (self->cancellable))
    goto cancelled;

  for (i = 0; i < rnum; i++) {
    GstFlowReturn recv_ret;

    recv_ret = bclass->receive_message (self, ready[i], outbuf);

    if (recv_ret == GST_SRT_FLOW_AGAIN) {
      GST_LOG_OBJECT (self, "Do wait again");
      goto retry;
    } else if (recv_ret != GST_FLOW_OK) {
      GST_DEBUG_OBJECT (self, "Receive message return %s",
          gst_flow_get_name (recv_ret));
      return recv_ret;
    }
  }

  return GST_FLOW_OK;

cancelled:
  GST_DEBUG_OBJECT (src, "Cancelled");
  return GST_FLOW_FLUSHING;
}

static void
gst_srt_base_src_class_init (GstSRTBaseSrcClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS (klass);
  GstBaseSrcClass *gstbasesrc_class = GST_BASE_SRC_CLASS (klass);
  GstPushSrcClass *gstpushsrc_class = GST_PUSH_SRC_CLASS (klass);

  gobject_class->set_property = gst_srt_base_src_set_property;
  gobject_class->get_property = gst_srt_base_src_get_property;
  gobject_class->finalize = gst_srt_base_src_finalize;

  /**
   * GstSRTBaseSrc:uri:
   *
   * The URI used by SRT Connection.
   */
  properties[PROP_URI] = g_param_spec_string ("uri", "URI",
      "URI in the form of srt://address:port", SRT_DEFAULT_URI,
      G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY | G_PARAM_STATIC_STRINGS);

  /**
   * GstSRTBaseSrc:caps:
   *
   * The Caps used by the source pad.
   */
  properties[PROP_CAPS] =
      g_param_spec_boxed ("caps", "Caps", "The caps of the source pad",
      GST_TYPE_CAPS, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

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

  g_object_class_install_properties (gobject_class, PROP_LAST, properties);

  gst_element_class_add_static_pad_template (gstelement_class, &src_template);

  gstbasesrc_class->get_caps = GST_DEBUG_FUNCPTR (gst_srt_base_src_get_caps);
  gstbasesrc_class->start = GST_DEBUG_FUNCPTR (gst_srt_base_src_start);
  gstbasesrc_class->stop = GST_DEBUG_FUNCPTR (gst_srt_base_src_stop);
  gstbasesrc_class->unlock = GST_DEBUG_FUNCPTR (gst_srt_base_src_unlock);
  gstbasesrc_class->unlock_stop =
      GST_DEBUG_FUNCPTR (gst_srt_base_src_unlock_stop);

  gstpushsrc_class->fill = GST_DEBUG_FUNCPTR (gst_srt_base_src_fill);
}

static void
gst_srt_base_src_init (GstSRTBaseSrc * self)
{
  gst_srt_base_src_uri_set_uri (GST_URI_HANDLER (self), SRT_DEFAULT_URI, NULL);
  gst_base_src_set_format (GST_BASE_SRC (self), GST_FORMAT_TIME);
  gst_base_src_set_live (GST_BASE_SRC (self), TRUE);
  gst_base_src_set_do_timestamp (GST_BASE_SRC (self), TRUE);

#ifndef GST_DISABLE_GST_DEBUG
  gst_srt_debug_init ();
#endif

  self->latency = SRT_DEFAULT_LATENCY;
  self->key_length = SRT_DEFAULT_KEY_LENGTH;
  self->cancellable = g_cancellable_new ();
  self->event_fd = g_cancellable_get_fd (self->cancellable);
}

static GstURIType
gst_srt_base_src_uri_get_type (GType type)
{
  return GST_URI_SRC;
}

static const gchar *const *
gst_srt_base_src_uri_get_protocols (GType type)
{
  static const gchar *protocols[] = { SRT_URI_SCHEME, NULL };

  return protocols;
}

static gchar *
gst_srt_base_src_uri_get_uri (GstURIHandler * handler)
{
  gchar *uri_str;
  GstSRTBaseSrc *self = GST_SRT_BASE_SRC (handler);

  GST_OBJECT_LOCK (self);
  uri_str = gst_uri_to_string (self->uri);
  GST_OBJECT_UNLOCK (self);

  return uri_str;
}

static gboolean
gst_srt_base_src_uri_set_uri (GstURIHandler * handler,
    const gchar * uri, GError ** error)
{
  GstSRTBaseSrc *self = GST_SRT_BASE_SRC (handler);
  GstUri *parsed_uri = gst_uri_from_string (uri);

  if (g_strcmp0 (gst_uri_get_scheme (parsed_uri), SRT_URI_SCHEME) != 0) {
    g_set_error (error, GST_URI_ERROR, GST_URI_ERROR_BAD_URI,
        "Invalid SRT URI scheme");
    return FALSE;
  }

  GST_OBJECT_LOCK (self);

  g_clear_pointer (&self->uri, gst_uri_unref);
  self->uri = gst_uri_ref (parsed_uri);

  GST_OBJECT_UNLOCK (self);

  g_clear_pointer (&parsed_uri, gst_uri_unref);
  return TRUE;
}

static void
gst_srt_base_src_uri_handler_init (gpointer g_iface, gpointer iface_data)
{
  GstURIHandlerInterface *iface = (GstURIHandlerInterface *) g_iface;

  iface->get_type = gst_srt_base_src_uri_get_type;
  iface->get_protocols = gst_srt_base_src_uri_get_protocols;
  iface->get_uri = gst_srt_base_src_uri_get_uri;
  iface->set_uri = gst_srt_base_src_uri_set_uri;
}
