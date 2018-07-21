/* GStreamer
 * Copyright (C) <2018> Seungha Yang <seungha.yang@navercorp.com>
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

#include <gst/check/gstcheck.h>
#include <srt/srt.h>

#include <gio/gio.h>
#include <unistd.h>

#if 0
static SRTSOCKET
_srt_server_listen (int sender, int blocking,
    const gchar * host, guint16 port, int latency, gint * poll_id,
    const gchar * passphrase, int key_length)
{
  SRTSOCKET sock = SRT_INVALID_SOCK;
  GError *error = NULL;
  struct sockaddr sa;
  size_t sa_len;
  GSocketAddress *addr = NULL;

  if (host == NULL) {
    GInetAddress *any = g_inet_address_new_any (G_SOCKET_FAMILY_IPV4);

    addr = g_inet_socket_address_new (any, port);
    g_object_unref (any);
  } else {
    addr = g_inet_socket_address_new_from_string (host, port);
  }

  if (addr == NULL) {
    GST_WARNING ("failed to extract host or port from the given URI");
    goto failed;
  }

  sa_len = g_socket_address_get_native_size (addr);
  if (!g_socket_address_to_native (addr, &sa, sa_len, &error)) {
    goto failed;
  }

  sock = srt_socket (sa.sa_family, SOCK_DGRAM, 0);
  if (sock == SRT_INVALID_SOCK) {
    GST_WARNING ("failed to create SRT socket (reason: %s)",
        srt_getlasterror_str ());
    goto failed;
  }

  srt_setsockopt (sock, 0, SRTO_SNDSYN, &blocking, sizeof (int));

  /* Make sure TSBPD mode is enable (SRT mode) */
  srt_setsockopt (sock, 0, SRTO_TSBPDMODE, &(int) {
      1}, sizeof (int));

  srt_setsockopt (sock, 0, SRTO_SENDER, &sender, sizeof (int));
  srt_setsockopt (sock, 0, SRTO_TSBPDDELAY, &latency, sizeof (int));

  if (passphrase != NULL && passphrase[0] != '\0') {
    srt_setsockopt (sock, 0, SRTO_PASSPHRASE, passphrase, strlen (passphrase));
    srt_setsockopt (sock, 0, SRTO_PBKEYLEN, &key_length, sizeof (int));
  }

  *poll_id = srt_epoll_create ();
  if (*poll_id == -1) {
    goto failed;
  }

  srt_epoll_add_usock (*poll_id, sock, &(int) {
      SRT_EPOLL_IN});

  if (srt_bind (sock, &sa, sa_len) == SRT_ERROR) {
    GST_WARNING ("failed to bind SRT server socket (reason: %s)",
        srt_getlasterror_str ());
    goto failed;
  }

  if (srt_listen (sock, 1) == SRT_ERROR) {
    GST_WARNING ("failed to listen SRT socket (reason: %s)",
        srt_getlasterror_str ());
    goto failed;
  }

  return sock;

failed:
  if (*poll_id != SRT_ERROR) {
    srt_epoll_release (*poll_id);
    *poll_id = SRT_ERROR;
  }

  if (sock != SRT_INVALID_SOCK) {
    srt_close (sock);
    sock = SRT_INVALID_SOCK;
  }

  g_clear_error (&error);
  g_clear_object (&addr);

  return SRT_INVALID_SOCK;
}

static gpointer
thread_func (gpointer data)
{
  GCancellable *cancell = (GCancellable *) data;

  sleep (1);
  GST_INFO ("do cancell");
  g_cancellable_cancel (cancell);

  return NULL;
}


GST_START_TEST (test_src_properties)
{
  GstElement *serversrc;
  GstElement *clientsrc;

  serversrc = gst_check_setup_element ("srtserversrc");
  clientsrc = gst_check_setup_element ("srtclientsrc");

  gst_check_teardown_element (serversrc);
  gst_check_teardown_element (clientsrc);

  SRTSOCKET socket = SRT_ERROR;
  gint poll_id = SRT_ERROR;
  SRTSOCKET ready[2] = { 0, };
  SYSSOCKET cancellable[2] = { 0, };
  SYSSOCKET cancellable2[2] = { 0, };
  GThread *thread;
  GCancellable *cancell = g_cancellable_new ();
  int can_fd = g_cancellable_get_fd (cancell);
  int add_ret;

  socket =
      _srt_server_listen (FALSE, TRUE, "127.0.0.1", 9999, 2147483647, &poll_id,
      NULL, 0);

  g_cancellable_reset (cancell);
  add_ret = srt_epoll_add_ssock (poll_id, can_fd, NULL);
  GST_INFO ("can fd: %d, add ret %d", can_fd, add_ret);

  thread = g_thread_new ("mythread", thread_func, cancell);

  if (thread == NULL) {
    GST_ERROR ("ERROR");
  }

  GST_INFO ("socket %d, pollid %d", socket, poll_id);

  if (srt_epoll_wait (poll_id, ready, &(int) {
          2}, 0, 0, 2147483647, cancellable, &(int) {
          2}, 0, 0) == -1) {

  } else {
    GST_INFO ("OK");
    GST_INFO ("%d %d %d %d %d %d", ready[0], ready[1], cancellable[0],
        cancellable[1], cancellable2[0], cancellable2[1]);
  }

  if (poll_id != SRT_ERROR)
    srt_epoll_release (poll_id);

  if (socket != SRT_ERROR)
    srt_close (socket);
}

GST_END_TEST;

#endif

#define PROP_DUMMY_PASSPHRASE "foobartempdummy!"
#define PROP_TEST_URI "srt://123.456.789.012:9999"
#define PROP_TEST_BIND_ADDRESS "srt://123.456.789.012:10000"
#define PROP_TEST_BIND_PORT 10000

GST_START_TEST (test_properties)
{
  gint i, j;
  gint valid_keylen[] = { 16, 24, 32 };
  const gchar *elems[] =
      { "srtserversrc", "srtclientsrc", "srtserversink", "srtclientsink" };

  for (i = 0; i < G_N_ELEMENTS (elems); i++) {
    GstElement *elem;
    GstCaps *set_caps, *get_caps = NULL;
    gint default_key_len = -1;
    gchar *passphrase = NULL;
    gint poll_timeout = 0;
    gchar *uri = NULL;

    elem = gst_check_setup_element (elems[i]);

    fail_unless (elem != NULL);

    if (i < 2) {
      set_caps = gst_caps_new_simple ("video/mpegts",
          "systemstream", G_TYPE_BOOLEAN, TRUE, NULL);

      g_object_set (G_OBJECT (elem), "caps", set_caps, NULL);
      g_object_get (G_OBJECT (elem), "caps", &get_caps, NULL);
      fail_unless (get_caps != NULL);
      fail_unless (gst_caps_is_equal (set_caps, get_caps));

      gst_caps_unref (set_caps);
      gst_caps_unref (get_caps);
      set_caps = NULL;
      get_caps = NULL;
    }

    /* valid key-length {16,24,32} */
    g_object_get (G_OBJECT (elem), "key-length", &default_key_len, NULL);
    fail_unless (default_key_len == 16 || default_key_len == 24
        || default_key_len == 32);

    /* Set invalid value */
    for (j = 0; j < G_N_ELEMENTS (valid_keylen); j++) {
      gint key_len;

      g_object_set (G_OBJECT (elem), "key-length", valid_keylen[j], NULL);
      g_object_get (G_OBJECT (elem), "key-length", &key_len, NULL);
      fail_unless_equals_int (key_len, valid_keylen[j]);
    }

    g_object_set (G_OBJECT (elem), "key-length", strlen (PROP_DUMMY_PASSPHRASE),
        NULL);

    /* We didn't set passphrase yet, must be null */
    g_object_get (G_OBJECT (elem), "passphrase", &passphrase, NULL);
    fail_unless (passphrase == NULL);

    g_object_set (G_OBJECT (elem), "passphrase", PROP_DUMMY_PASSPHRASE, NULL);
    g_object_get (G_OBJECT (elem), "passphrase", &passphrase, NULL);
    fail_unless_equals_string (passphrase, PROP_DUMMY_PASSPHRASE);
    g_free (passphrase);
    passphrase = NULL;

    g_object_set (G_OBJECT (elem), "poll-timeout", 200, NULL);
    g_object_get (G_OBJECT (elem), "poll-timeout", &poll_timeout, NULL);
    fail_unless_equals_int (poll_timeout, 200);

    g_object_set (G_OBJECT (elem), "uri", PROP_TEST_URI, NULL);
    g_object_get (G_OBJECT (elem), "uri", &uri, NULL);
    fail_unless_equals_string (uri, PROP_TEST_URI);
    g_free (uri);
    uri = NULL;

    if (i % 2) {
      gboolean rendez_vous;
      gchar *bind_addr = NULL;
      gint bind_port;

      g_object_set (G_OBJECT (elem), "rendez-vous", TRUE, NULL);
      g_object_get (G_OBJECT (elem), "rendez-vous", &rendez_vous, NULL);
      fail_unless_equals_int (rendez_vous, TRUE);

      g_object_set (G_OBJECT (elem), "rendez-vous", FALSE, NULL);
      g_object_get (G_OBJECT (elem), "rendez-vous", &rendez_vous, NULL);
      fail_unless_equals_int (rendez_vous, FALSE);

      g_object_set (G_OBJECT (elem), "bind-address", PROP_TEST_BIND_ADDRESS,
          NULL);
      g_object_get (G_OBJECT (elem), "bind-address", &bind_addr, NULL);
      fail_unless_equals_string (bind_addr, PROP_TEST_BIND_ADDRESS);
      g_free (bind_addr);

      g_object_set (G_OBJECT (elem), "bind-port", PROP_TEST_BIND_PORT, NULL);
      g_object_get (G_OBJECT (elem), "bind-port", &bind_port, NULL);
      fail_unless_equals_int (bind_port, PROP_TEST_BIND_PORT);
    }

    gst_check_teardown_element (elem);
  }
}

GST_END_TEST;

static GstStaticPadTemplate sinktemplate = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

static GstStaticPadTemplate srctemplate = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

static gboolean
srtsrc_setup (GstElement ** srtsrc, GstPad ** sinkpad, const gchar * uri,
    gint poll_timeout, gboolean is_server)
{
  *srtsrc =
      is_server ? gst_check_setup_element ("srtserversrc") :
      gst_check_setup_element ("srtclientsrc");
  fail_unless (*srtsrc != NULL);

  g_object_set (G_OBJECT (*srtsrc), "uri", uri, "poll-timeout", poll_timeout,
      NULL);

  *sinkpad = gst_check_setup_sink_pad_by_name (*srtsrc, &sinktemplate, "src");
  fail_unless (*sinkpad != NULL);
  gst_pad_set_active (*sinkpad, TRUE);

  gst_element_set_state (*srtsrc, GST_STATE_PLAYING);

  return TRUE;
}

static gboolean
srtsink_setup (GstElement ** srtsink, GstPad ** srcpad, const gchar * uri,
    gint poll_timeout, gboolean is_server)
{
  *srtsink =
      is_server ? gst_check_setup_element ("srtserversink") :
      gst_check_setup_element ("srtclientsink");
  fail_unless (*srtsink != NULL);

  g_object_set (G_OBJECT (*srtsink), "uri", uri, "poll-timeout", poll_timeout,
      "sync", FALSE, NULL);

  *srcpad = gst_check_setup_src_pad_by_name (*srtsink, &srctemplate, "sink");
  fail_unless (*srcpad != NULL);
  gst_pad_set_active (*srcpad, TRUE);

  gst_element_set_state (*srtsink, GST_STATE_PLAYING);

  return TRUE;
}

#define TS_PACKET_SIZE 188
#define NUM_TS_PACKET_PER_SRT_CHUNK 7

static GstBufferList *
create_srt_chunk (void)
{
  GstBufferList *list;
  GstBuffer *ts_buffer;
  gint i;

  list = gst_buffer_list_new ();

  for (i = 0; i < NUM_TS_PACKET_PER_SRT_CHUNK; i++) {
    ts_buffer = gst_buffer_new_allocate (NULL, TS_PACKET_SIZE, NULL);
    gst_buffer_memset (ts_buffer, 0, 0, TS_PACKET_SIZE);
    gst_buffer_list_add (list, ts_buffer);
  }

  return list;
}

GST_START_TEST (test_max_poll_timeout)
{
  GstElement *src = NULL;
  GstElement *sink = NULL;
  GstPad *sinkpad = NULL;
  GstPad *srcpad = NULL;
  GstBufferList *buf_list;
  GstSegment segment;

  /* Set maximaum poll timeout and check elements are cancallable */

  GST_INFO ("Check clientsink and seversrc pair");
  fail_unless (srtsrc_setup (&src, &sinkpad, "srt://:9999", 2147483647, TRUE));
  /* Wait src setup done */
  usleep (100);

  fail_unless (srtsink_setup (&sink, &srcpad, "srt://127.0.0.1:9999",
          2147483647, FALSE));

  buf_list = create_srt_chunk ();

  gst_pad_push_event (srcpad, gst_event_new_stream_start ("start-test!"));
  gst_segment_init (&segment, GST_FORMAT_TIME);
  gst_pad_push_event (srcpad, gst_event_new_segment (&segment));
  fail_unless_equals_int (gst_pad_push_list (srcpad, buf_list), GST_FLOW_OK);

  usleep (100);

  gst_check_teardown_pad_by_name (src, "src");
  gst_check_teardown_element (src);

  gst_check_teardown_pad_by_name (sink, "sink");
  gst_check_teardown_element (sink);
  sinkpad = NULL;
  srcpad = NULL;
  buf_list = NULL;

  GST_INFO ("Check serversink and clientsrc pair");
  fail_unless (srtsink_setup (&sink, &srcpad, "srt://:9999", 2147483647, TRUE));

  /* Wait sink setup done */
  usleep (100);

  fail_unless (srtsrc_setup (&src, &sinkpad, "srt://127.0.0.1:9999", 2147483647,
          FALSE));

  buf_list = create_srt_chunk ();

  gst_pad_push_event (srcpad, gst_event_new_stream_start ("start-test!"));
  gst_segment_init (&segment, GST_FORMAT_TIME);
  gst_pad_push_event (srcpad, gst_event_new_segment (&segment));
  fail_unless_equals_int (gst_pad_push_list (srcpad, buf_list), GST_FLOW_OK);

  usleep (100);

  gst_check_teardown_pad_by_name (src, "src");
  gst_check_teardown_element (src);

  gst_check_teardown_pad_by_name (sink, "sink");
  gst_check_teardown_element (sink);
}

GST_END_TEST;

static Suite *
srt_suite (void)
{
  Suite *s = suite_create ("srt");
  TCase *tc_chain = tcase_create ("general");

  tcase_add_test (tc_chain, test_properties);
  tcase_add_test (tc_chain, test_max_poll_timeout);

  suite_add_tcase (s, tc_chain);

  return s;
}

GST_CHECK_MAIN (srt);
