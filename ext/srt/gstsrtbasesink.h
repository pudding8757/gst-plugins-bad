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

#ifndef __GST_SRT_BASE_SINK_H__
#define __GST_SRT_BASE_SINK_H__

#include <gst/gst.h>
#include <gst/base/gstbasesink.h>
#include <gio/gio.h>

#include <srt/srt.h>

G_BEGIN_DECLS

#define GST_TYPE_SRT_BASE_SINK              (gst_srt_base_sink_get_type ())
#define GST_IS_SRT_BASE_SINK(obj)           (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GST_TYPE_SRT_BASE_SINK))
#define GST_IS_SRT_BASE_SINK_CLASS(klass)   (G_TYPE_CHECK_CLASS_TYPE ((klass), GST_TYPE_SRT_BASE_SINK))
#define GST_SRT_BASE_SINK_GET_CLASS(obj)    (G_TYPE_INSTANCE_GET_CLASS ((obj), GST_TYPE_SRT_BASE_SINK, GstSRTBaseSinkClass))
#define GST_SRT_BASE_SINK(obj)              (G_TYPE_CHECK_INSTANCE_CAST ((obj), GST_TYPE_SRT_BASE_SINK, GstSRTBaseSink))
#define GST_SRT_BASE_SINK_CLASS(klass)      (G_TYPE_CHECK_CLASS_CAST ((klass), GST_TYPE_SRT_BASE_SINK, GstSRTBaseSinkClass))
#define GST_SRT_BASE_SINK_CAST(obj)         ((GstSRTBaseSink*)(obj))
#define GST_SRT_BASE_SINK_CLASS_CAST(klass) ((GstSRTBaseSinkClass*)(klass))

#define GST_SRT_FLOW_SEND_AGAIN GST_FLOW_CUSTOM_ERROR
#define GST_SRT_FLOW_SEND_ERROR GST_FLOW_CUSTOM_ERROR_1

typedef struct _GstSRTBaseSink GstSRTBaseSink;
typedef struct _GstSRTBaseSinkClass GstSRTBaseSinkClass;
typedef struct _GstSRTClientHandle GstSRTClientHandle;

typedef enum {
  SRT_CLIENT_INIT = 0,
  SRT_CLIENT_STARTED,
  SRT_CLIENT_STOPPED
} GstSRTClientState;

struct _GstSRTClientHandle {
  GstSRTBaseSink *sink;

  SRTSOCKET sock;
  GSocketAddress *sockaddr;

  GSList *queue;
  GstCaps *caps;

  gint ref_count;

  GstSRTClientState state;
  guint32 retry_count;
};

struct _GstSRTBaseSink {
  GstBaseSink parent;

  GstUri *uri;
  gint latency;
  gchar *passphrase;
  gint key_length;
  gint sndbuf_size;

  /*< private >*/
  gpointer _gst_reserved[GST_PADDING];
};

struct _GstSRTBaseSinkClass {
  GstBaseSinkClass parent_class;

  /* ask the subclass to send a buffer */
  GstFlowReturn (*send_buffer)       (GstSRTBaseSink *self, GstBuffer *buf);

  gpointer _gst_reserved[GST_PADDING_LARGE];
};

GST_EXPORT
GType gst_srt_base_sink_get_type (void);

GstStructure * gst_srt_base_sink_get_stats (GstSRTClientHandle *handle);

gboolean gst_srt_base_sink_client_queue_buffer (GstSRTBaseSink *sink,
    GstSRTClientHandle *handle, GstBuffer *buffer);

GstFlowReturn gst_srt_base_sink_client_send_message (GstSRTBaseSink *sink,
    GstSRTClientHandle *handle);

GstSRTClientHandle * gst_srt_client_handle_new (GstSRTBaseSink *sink);

GstSRTClientHandle * gst_srt_client_handle_ref (GstSRTClientHandle *handle);
void gst_srt_client_handle_unref (GstSRTClientHandle *handle);


G_END_DECLS

#endif /* __GST_SRT_BASE_SINK_H__ */
