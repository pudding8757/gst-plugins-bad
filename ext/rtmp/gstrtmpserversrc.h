/* GStreamer
 * Copyright (C) 2018 Seungha Yang <seungha.yang@navercorp.com>
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

#ifndef __GST_RTMP_SERVER_SRC_H__
#define __GST_RTMP_SERVER_SRC_H__

#include <gst/base/gstbasesrc.h>
#include <gst/base/gstpushsrc.h>

#include <librtmp/rtmp.h>
#include <librtmp/log.h>
#include <librtmp/amf.h>

G_BEGIN_DECLS

#define GST_TYPE_RTMP_SERVER_SRC \
  (gst_rtmp_server_src_get_type())
#define GST_RTMP_SERVER_SRC(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_RTMP_SERVER_SRC,GstRTMPServerSrc))
#define GST_RTMP_SERVER_SRC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_RTMP_SERVER_SRC,GstRTMPServerSrcClass))
#define GST_IS_RTMP_SERVER_SRC(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_RTMP_SERVER_SRC))
#define GST_IS_RTMP_SERVER_SRC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_RTMP_SERVER_SRC))

typedef struct _GstRTMPServerSrc      GstRTMPServerSrc;
typedef struct _GstRTMPServerSrcClass GstRTMPServerSrcClass;
typedef struct _GstRTMPServerSrcPrivate GstRTMPServerSrcPrivate;

/**
 * GstRTMPServerSrc:
 *
 * Opaque data structure.
 */
struct _GstRTMPServerSrc
{
  GstPushSrc parent;

  /* < private > */
  GstUri *uri;
  gchar *swf_url;
  gchar *page_url;

  RTMP *rtmp;
  int timeout;
  gint64 cur_offset;
  GstClockTime last_timestamp;
  gboolean seekable;
  gboolean discont;

  GstRTMPServerSrcPrivate *priv;
};

struct _GstRTMPServerSrcClass
{
  GstPushSrcClass  parent;
};

GType gst_rtmp_server_src_get_type (void);

G_END_DECLS

#endif /* __GST_RTMP_SERVER_SRC_H__ */

