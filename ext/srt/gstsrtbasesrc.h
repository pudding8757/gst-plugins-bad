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

#ifndef __GST_SRT_BASE_SRC_H__
#define __GST_SRT_BASE_SRC_H__

#include <gst/gst.h>
#include <gst/base/gstpushsrc.h>
#include <srt/srt.h>

G_BEGIN_DECLS

#define GST_TYPE_SRT_BASE_SRC              (gst_srt_base_src_get_type ())
#define GST_IS_SRT_BASE_SRC(obj)           (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GST_TYPE_SRT_BASE_SRC))
#define GST_IS_SRT_BASE_SRC_CLASS(klass)   (G_TYPE_CHECK_CLASS_TYPE ((klass), GST_TYPE_SRT_BASE_SRC))
#define GST_SRT_BASE_SRC_GET_CLASS(obj)    (G_TYPE_INSTANCE_GET_CLASS ((obj), GST_TYPE_SRT_BASE_SRC, GstSRTBaseSrcClass))
#define GST_SRT_BASE_SRC(obj)              (G_TYPE_CHECK_INSTANCE_CAST ((obj), GST_TYPE_SRT_BASE_SRC, GstSRTBaseSrc))
#define GST_SRT_BASE_SRC_CLASS(klass)      (G_TYPE_CHECK_CLASS_CAST ((klass), GST_TYPE_SRT_BASE_SRC, GstSRTBaseSrcClass))
#define GST_SRT_BASE_SRC_CAST(obj)         ((GstSRTBaseSrc*)(obj))
#define GST_SRT_BASE_SRC_CLASS_CAST(klass) ((GstSRTBaseSrcClass*)(klass))

typedef struct _GstSRTBaseSrc GstSRTBaseSrc;
typedef struct _GstSRTBaseSrcClass GstSRTBaseSrcClass;

struct _GstSRTBaseSrc {
  GstPushSrc parent;

  GstUri *uri;
  GstCaps *caps;
  gint latency;
  gchar *passphrase;
  gint key_length;


  /*< private >*/
  GstClockTime start_timestamp;

  gpointer _gst_reserved[GST_PADDING];
};

struct _GstSRTBaseSrcClass {
  GstPushSrcClass parent_class;

  gpointer _gst_reserved[GST_PADDING_LARGE];
};

GST_EXPORT
GType gst_srt_base_src_get_type (void);

void          gst_srt_base_src_do_timestamp (GstSRTBaseSrc * src,
                                             GstBuffer * buffer,
                                             SRT_MSGCTRL * mc);

G_END_DECLS

#endif /* __GST_SRT_BASE_SRC_H__ */
