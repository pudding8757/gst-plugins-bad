/* GStreamer H.265 Parser
 * Copyright (C) 2013 Intel Corporation
 *   Contact: Sreerenj Balachandran <sreerenj.balachandran@intel.com>
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

#ifndef __GST_H265_PARSE_H__
#define __GST_H265_PARSE_H__

#include <gst/gst.h>
#include <gst/base/gstbaseparse.h>
#include <gst/codecparsers/gsth265parser.h>
#include "gsth26xbaseparse.h"

G_BEGIN_DECLS

#define GST_TYPE_H265_PARSE \
  (gst_h265_parse_get_type())
#define GST_H265_PARSE(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_H265_PARSE,GstH265Parse))
#define GST_H265_PARSE_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_H265_PARSE,GstH265ParseClass))
#define GST_IS_H265_PARSE(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_H265_PARSE))
#define GST_IS_H265_PARSE_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_H265_PARSE))

GType gst_h265_parse_get_type (void);

typedef struct _GstH265Parse GstH265Parse;
typedef struct _GstH265ParseClass GstH265ParseClass;

struct _GstH265Parse
{
  GstH26XBaseParse baseparse;

  GstH265Parser *nalparser;
};

struct _GstH265ParseClass
{
  GstH26XBaseParseClass parent_class;
};

G_END_DECLS
#endif
