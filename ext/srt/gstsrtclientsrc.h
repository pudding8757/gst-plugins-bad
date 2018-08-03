/* GStreamer
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

#ifndef __GST_SRT_CLIENT_SRC_H__
#define __GST_SRT_CLIENT_SRC_H__

#include "gstsrtbasesrc.h"

G_BEGIN_DECLS

#define GST_TYPE_SRT_CLIENT_SRC              (gst_srt_client_src_get_type ())
#define GST_IS_SRT_CLIENT_SRC(obj)           (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GST_TYPE_SRT_CLIENT_SRC))
#define GST_IS_SRT_CLIENT_SRC_CLASS(klass)   (G_TYPE_CHECK_CLASS_TYPE ((klass), GST_TYPE_SRT_CLIENT_SRC))
#define GST_SRT_CLIENT_SRC_GET_CLASS(obj)    (G_TYPE_INSTANCE_GET_CLASS ((obj), GST_TYPE_SRT_CLIENT_SRC, GstSRTClientSrcClass))
#define GST_SRT_CLIENT_SRC(obj)              (G_TYPE_CHECK_INSTANCE_CAST ((obj), GST_TYPE_SRT_CLIENT_SRC, GstSRTClientSrc))
#define GST_SRT_CLIENT_SRC_CLASS(klass)      (G_TYPE_CHECK_CLASS_CAST ((klass), GST_TYPE_SRT_CLIENT_SRC, GstSRTClientSrcClass))
#define GST_SRT_CLIENT_SRC_CAST(obj)         ((GstSRTClientSrc*)(obj))
#define GST_SRT_CLIENT_SRC_CLASS_CAST(klass) ((GstSRTClientSrcClass*)(klass))

typedef struct _GstSRTClientSrc GstSRTClientSrc;
typedef struct _GstSRTClientSrcClass GstSRTClientSrcClass;

struct _GstSRTClientSrc {
  GstSRTBaseSrc parent;

  SRTSOCKET sock;
  gint poll_id;

  gboolean rendez_vous;
  gchar *bind_address;
  guint16 bind_port;
};

struct _GstSRTClientSrcClass {
  GstSRTBaseSrcClass parent_class;
};

GST_EXPORT
GType gst_srt_client_src_get_type (void);

G_END_DECLS

#endif /* __GST_SRT_CLIENT_SRC_H__ */
