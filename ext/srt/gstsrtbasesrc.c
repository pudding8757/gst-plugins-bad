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

struct _GstSRTBaseSrcPrivate
{
  GstUri *uri;
  GstCaps *caps;
  gint latency;
  gchar *passphrase;
  gint key_length;

  GCancellable *cancellable;
  int cancellable_fd;

  GstClockTime start_timestamp;
};

#define GST_SRT_BASE_SRC_GET_PRIVATE(obj)  \
       (G_TYPE_INSTANCE_GET_PRIVATE ((obj), GST_TYPE_SRT_BASE_SRC, GstSRTBaseSrcPrivate))

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

#define _do_init \
  GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, "srtbasesrc", 0, "SRT Base Source")

#define gst_srt_base_src_parent_class parent_class
G_DEFINE_ABSTRACT_TYPE_WITH_CODE (GstSRTBaseSrc, gst_srt_base_src,
    GST_TYPE_PUSH_SRC, G_ADD_PRIVATE (GstSRTBaseSrc)
    G_IMPLEMENT_INTERFACE (GST_TYPE_URI_HANDLER,
        gst_srt_base_src_uri_handler_init) _do_init);

static void
gst_srt_base_src_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstSRTBaseSrc *self = GST_SRT_BASE_SRC (object);
  GstSRTBaseSrcPrivate *priv = self->priv;

  switch (prop_id) {
    case PROP_URI:
      if (priv->uri != NULL) {
        gchar *uri_str = gst_srt_base_src_uri_get_uri (GST_URI_HANDLER (self));
        g_value_take_string (value, uri_str);
      }
      break;
    case PROP_CAPS:
      GST_OBJECT_LOCK (self);
      gst_value_set_caps (value, priv->caps);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_LATENCY:
      g_value_set_int (value, priv->latency);
      break;
    case PROP_PASSPHRASE:
      g_value_set_string (value, priv->passphrase);
      break;
    case PROP_KEY_LENGTH:
      g_value_set_int (value, priv->key_length);
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
  GstSRTBaseSrcPrivate *priv = self->priv;

  switch (prop_id) {
    case PROP_URI:
      gst_srt_base_src_uri_set_uri (GST_URI_HANDLER (self),
          g_value_get_string (value), NULL);
      break;
    case PROP_CAPS:
      GST_OBJECT_LOCK (self);
      g_clear_pointer (&priv->caps, gst_caps_unref);
      priv->caps = gst_caps_copy (gst_value_get_caps (value));
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_LATENCY:
      priv->latency = g_value_get_int (value);
      break;
    case PROP_PASSPHRASE:
      g_free (priv->passphrase);
      priv->passphrase = g_value_dup_string (value);
      break;
    case PROP_KEY_LENGTH:
    {
      gint key_length = g_value_get_int (value);
      g_return_if_fail (key_length == 16 || key_length == 24
          || key_length == 32);
      priv->key_length = key_length;
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
  GstSRTBaseSrcPrivate *priv = self->priv;

  g_clear_pointer (&priv->uri, gst_uri_unref);
  g_clear_pointer (&priv->caps, gst_caps_unref);
  g_clear_pointer (&priv->passphrase, g_free);

  if (self->poll_id != SRT_ERROR) {
    srt_epoll_release (self->poll_id);
    self->poll_id = SRT_ERROR;
  }

  if (self->sock != SRT_INVALID_SOCK) {
    srt_close (self->sock);
    self->sock = SRT_INVALID_SOCK;
  }

  g_object_unref (priv->cancellable);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static GstCaps *
gst_srt_base_src_get_caps (GstBaseSrc * src, GstCaps * filter)
{
  GstSRTBaseSrc *self = GST_SRT_BASE_SRC (src);
  GstSRTBaseSrcPrivate *priv = self->priv;
  GstCaps *result, *caps = NULL;

  GST_OBJECT_LOCK (self);
  if (priv->caps != NULL) {
    caps = gst_caps_ref (priv->caps);
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
  GstSRTBaseSrcPrivate *priv = self->priv;
  GstSRTBaseSrcClass *bclass = GST_SRT_BASE_SRC_GET_CLASS (self);
  SRTSOCKET sock = SRT_INVALID_SOCK;
  gint poll_id = SRT_ERROR;
  GstUri *uri = NULL;
  gboolean ret;

  if (G_UNLIKELY (priv->uri == NULL)) {
    GST_ERROR_OBJECT (src, "NULL uri");
    return FALSE;
  }

  if (G_UNLIKELY (!bclass->open)) {
    GST_ERROR_OBJECT (src, "Implement open vfunc");
    return FALSE;
  }

  uri = gst_uri_ref (priv->uri);
  ret = bclass->open (self, priv->cancellable,
      gst_uri_get_host (uri), gst_uri_get_port (uri), &sock, &poll_id);

  if (!ret)
    goto failed;

  self->sock = sock;
  self->poll_id = poll_id;

  gst_uri_unref (uri);
  /* HACK: Since srt_epoll_wait() is not cancellable, install our event fd to
   * epoll */
  priv->cancellable_fd = g_cancellable_get_fd (priv->cancellable);
  srt_epoll_add_ssock (poll_id, priv->cancellable_fd, NULL);

  return TRUE;

failed:
  if (poll_id != SRT_ERROR)
    srt_epoll_release (poll_id);

  if (sock != SRT_ERROR)
    srt_close (sock);

  if (uri)
    gst_uri_unref (uri);

  return FALSE;
}

static gboolean
gst_srt_base_src_stop (GstBaseSrc * src)
{
  GstSRTBaseSrc *self = GST_SRT_BASE_SRC (src);
  GstSRTBaseSrcPrivate *priv = self->priv;
  GstSRTBaseSrcClass *bclass = GST_SRT_BASE_SRC_GET_CLASS (self);

  if (bclass->close)
    bclass->close (self);

  if (self->poll_id != SRT_ERROR) {
    if (self->sock != SRT_INVALID_SOCK)
      srt_epoll_remove_usock (self->poll_id, self->sock);
    if (priv->cancellable_fd != -1)
      srt_epoll_remove_ssock (self->poll_id, priv->cancellable_fd);
    srt_epoll_release (self->poll_id);
  }
  self->poll_id = SRT_ERROR;

  GST_DEBUG_OBJECT (self, "closing SRT connection");
  if (self->sock != SRT_INVALID_SOCK)
    srt_close (self->sock);
  self->sock = SRT_INVALID_SOCK;

  return TRUE;
}

static gboolean
gst_srt_base_src_unlock (GstBaseSrc * src)
{
  GstSRTBaseSrc *self = GST_SRT_BASE_SRC (src);
  GstSRTBaseSrcPrivate *priv = self->priv;

  GST_DEBUG_OBJECT (src, "Unlock");
  g_cancellable_cancel (priv->cancellable);

  return TRUE;
}

static gboolean
gst_srt_base_src_unlock_stop (GstBaseSrc * src)
{
  GstSRTBaseSrc *self = GST_SRT_BASE_SRC (src);
  GstSRTBaseSrcPrivate *priv = self->priv;

  GST_DEBUG_OBJECT (src, "Unlock stop");
  g_cancellable_reset (priv->cancellable);

  return TRUE;
}

static void
gst_srt_base_src_class_init (GstSRTBaseSrcClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS (klass);
  GstBaseSrcClass *gstbasesrc_class = GST_BASE_SRC_CLASS (klass);

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
}

static void
gst_srt_base_src_init (GstSRTBaseSrc * self)
{
  GstSRTBaseSrcPrivate *priv = GST_SRT_BASE_SRC_GET_PRIVATE (self);

  gst_srt_base_src_uri_set_uri (GST_URI_HANDLER (self), SRT_DEFAULT_URI, NULL);
  gst_base_src_set_format (GST_BASE_SRC (self), GST_FORMAT_TIME);
  gst_base_src_set_live (GST_BASE_SRC (self), TRUE);

  priv->latency = SRT_DEFAULT_LATENCY;
  priv->key_length = SRT_DEFAULT_KEY_LENGTH;
  priv->cancellable = g_cancellable_new ();
  priv->cancellable_fd = -1;
  priv->start_timestamp = GST_CLOCK_TIME_NONE;

  self->priv = priv;

  self->sock = SRT_INVALID_SOCK;
  self->poll_id = SRT_ERROR;
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
  GstSRTBaseSrcPrivate *priv = GST_SRT_BASE_SRC_GET_PRIVATE (self);

  GST_OBJECT_LOCK (self);
  uri_str = gst_uri_to_string (priv->uri);
  GST_OBJECT_UNLOCK (self);

  return uri_str;
}

static gboolean
gst_srt_base_src_uri_set_uri (GstURIHandler * handler,
    const gchar * uri, GError ** error)
{
  GstSRTBaseSrc *self = GST_SRT_BASE_SRC (handler);
  GstSRTBaseSrcPrivate *priv = GST_SRT_BASE_SRC_GET_PRIVATE (self);
  gboolean ret = TRUE;
  GstUri *parsed_uri = gst_uri_from_string (uri);

  if (g_strcmp0 (gst_uri_get_scheme (parsed_uri), SRT_URI_SCHEME) != 0) {
    g_set_error (error, GST_URI_ERROR, GST_URI_ERROR_BAD_URI,
        "Invalid SRT URI scheme");
    ret = FALSE;
    goto out;
  }

  GST_OBJECT_LOCK (self);

  g_clear_pointer (&priv->uri, gst_uri_unref);
  priv->uri = gst_uri_ref (parsed_uri);

  GST_OBJECT_UNLOCK (self);

out:
  g_clear_pointer (&parsed_uri, gst_uri_unref);
  return ret;
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

gint
gst_srt_base_src_get_latency (GstSRTBaseSrc * src)
{
  GstSRTBaseSrcPrivate *priv = src->priv;

  return priv->latency;
}

const gchar *
gst_srt_base_src_get_passphrase (GstSRTBaseSrc * src)
{
  GstSRTBaseSrcPrivate *priv = src->priv;

  return priv->passphrase;
}

gint
gst_srt_base_src_get_key_length (GstSRTBaseSrc * src)
{
  GstSRTBaseSrcPrivate *priv = src->priv;

  return priv->key_length;
}

gboolean
gst_srt_base_src_is_cancelled (GstSRTBaseSrc * src)
{
  GstSRTBaseSrcPrivate *priv = src->priv;

  return g_cancellable_is_cancelled (priv->cancellable);
}

void
gst_srt_base_src_do_timestamp (GstSRTBaseSrc * src, GstBuffer * buffer,
    SRT_MSGCTRL * mc)
{
  GstSRTBaseSrcPrivate *priv;
  GstClockTime srttime_to_gst;
  GstClockTime now;
  static GstStaticCaps src_reference =
      GST_STATIC_CAPS ("timestamp/x-srt-srctime");

  g_return_if_fail (src != NULL);
  g_return_if_fail (buffer != NULL);
  g_return_if_fail (mc != NULL);

  priv = src->priv;

#ifndef GST_DISABLE_GST_DEBUG
  /* The SRT srctime parameter is the number of usec (since epoch) in local time */
  {
    GstDateTime *src_datatime =
        gst_date_time_new_from_unix_epoch_local_time (mc->srctime / 1000000);
    if (src_datatime) {
      gchar *readable_src_datatime =
          gst_date_time_to_iso8601_string (src_datatime);

      GST_LOG_OBJECT (src, "SRT srctime : %s", readable_src_datatime);
      gst_date_time_unref (src_datatime);
      g_free (readable_src_datatime);
    }
  }
#endif

  srttime_to_gst = mc->srctime * GST_USECOND;

  if (G_UNLIKELY (priv->start_timestamp == GST_CLOCK_TIME_NONE))
    priv->start_timestamp = srttime_to_gst;

  /* Add GstReferenceTimestampMeta for application (only change timescale to nanosec) */
  gst_buffer_add_reference_timestamp_meta (buffer,
      gst_static_caps_get (&src_reference), srttime_to_gst,
      GST_CLOCK_TIME_NONE);

  if (srttime_to_gst != 0) {
    if (srttime_to_gst > priv->start_timestamp)
      now = srttime_to_gst - priv->start_timestamp;
    else
      now = 0;
  } else {
    GstClock *clock;

    clock = gst_element_get_clock (GST_ELEMENT_CAST (src));

    if (clock) {
      GstClockTime base_time =
          gst_element_get_base_time (GST_ELEMENT_CAST (src));

      now = gst_clock_get_time (clock);
      if (now > base_time)
        now -= base_time;
      else
        now = 0;
      gst_object_unref (clock);
    } else {
      GST_WARNING_OBJECT (src, "No available clock source for timestamp");
      now = GST_CLOCK_TIME_NONE;
    }
  }

  GST_BUFFER_PTS (buffer) = now;

  return;
}
