/*-
 * Copyright (c) 2026 James Ding
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "terminal-share-protocol.h"

#include <string.h>



/* ------------------------------------------------------------------ */
/* Frame reader                                                        */
/* ------------------------------------------------------------------ */

struct _TerminalShareFrameReader
{
  GInputStream *stream;
  TerminalShareFrameFunc on_frame;
  TerminalShareFrameEndFunc on_end;
  gpointer user_data;
  guint8 header[4];
  guint8 *payload;
  gsize payload_len;
  gboolean stopped;
};

static void
frame_reader_read_header (TerminalShareFrameReader *reader);

static void
frame_reader_end (TerminalShareFrameReader *reader)
{
  if (reader->stopped)
    return;
  reader->stopped = TRUE;
  if (reader->on_end != NULL)
    reader->on_end (reader->user_data);
}

static void
frame_reader_payload_cb (GObject *source,
                         GAsyncResult *res,
                         gpointer user_data)
{
  TerminalShareFrameReader *reader = user_data;
  GError *error = NULL;
  gsize bytes_read = 0;

  if (reader->stopped)
    return;
  if (!g_input_stream_read_all_finish (G_INPUT_STREAM (source), res,
                                       &bytes_read, &error))
    {
      g_clear_error (&error);
      frame_reader_end (reader);
      return;
    }
  if (bytes_read != reader->payload_len)
    {
      frame_reader_end (reader);
      return;
    }

  reader->on_frame (reader->payload, reader->payload_len,
                    reader->user_data);
  g_free (reader->payload);
  reader->payload = NULL;
  reader->payload_len = 0;
  if (!reader->stopped)
    frame_reader_read_header (reader);
}

static void
frame_reader_header_cb (GObject *source,
                        GAsyncResult *res,
                        gpointer user_data)
{
  TerminalShareFrameReader *reader = user_data;
  GError *error = NULL;
  gsize bytes_read = 0;
  guint32 len;

  if (reader->stopped)
    return;
  if (!g_input_stream_read_all_finish (G_INPUT_STREAM (source), res,
                                       &bytes_read, &error)
      || bytes_read != 4)
    {
      g_clear_error (&error);
      frame_reader_end (reader);
      return;
    }

  len = ((guint32) reader->header[0] << 24)
      | ((guint32) reader->header[1] << 16)
      | ((guint32) reader->header[2] << 8)
      | ((guint32) reader->header[3]);
  if (len > TERMSHARE_MAX_FRAME)
    {
      frame_reader_end (reader);
      return;
    }

  reader->payload_len = len;
  reader->payload = g_malloc0 (len > 0 ? len : 1);
  g_input_stream_read_all_async (reader->stream, reader->payload, len,
                                 G_PRIORITY_DEFAULT, NULL,
                                 frame_reader_payload_cb, reader);
}

static void
frame_reader_read_header (TerminalShareFrameReader *reader)
{
  g_input_stream_read_all_async (reader->stream, reader->header, 4,
                                 G_PRIORITY_DEFAULT, NULL,
                                 frame_reader_header_cb, reader);
}

TerminalShareFrameReader *
terminal_share_frame_reader_new (GInputStream *stream,
                                 TerminalShareFrameFunc on_frame,
                                 TerminalShareFrameEndFunc on_end,
                                 gpointer user_data)
{
  TerminalShareFrameReader *reader = g_new0 (TerminalShareFrameReader, 1);
  reader->stream = g_object_ref (stream);
  reader->on_frame = on_frame;
  reader->on_end = on_end;
  reader->user_data = user_data;
  return reader;
}

void
terminal_share_frame_reader_start (TerminalShareFrameReader *reader)
{
  g_return_if_fail (reader != NULL);
  if (!reader->stopped)
    frame_reader_read_header (reader);
}

void
terminal_share_frame_reader_free (TerminalShareFrameReader *reader)
{
  if (reader == NULL)
    return;
  reader->stopped = TRUE;
  g_clear_object (&reader->stream);
  g_free (reader->payload);
  g_free (reader);
}



/* ------------------------------------------------------------------ */
/* Frame writer                                                        */
/* ------------------------------------------------------------------ */

struct _TerminalShareFrameWriter
{
  GOutputStream *stream;
  GQueue *queue; /* GBytes, in send order */
  GBytes *in_flight; /* owned while an async write is outstanding */
  gboolean writing;
};

static void
frame_writer_write_cb (GObject *source,
                       GAsyncResult *res,
                       gpointer user_data);
static void
frame_writer_write_next (TerminalShareFrameWriter *writer)
{
  gsize size;
  gconstpointer data;

  if (writer->writing || g_queue_is_empty (writer->queue))
    return;
  writer->in_flight = g_queue_pop_head (writer->queue);
  data = g_bytes_get_data (writer->in_flight, &size);
  writer->writing = TRUE;
  g_output_stream_write_all_async (writer->stream, data, size,
                                   G_PRIORITY_DEFAULT, NULL,
                                   frame_writer_write_cb, writer);
}

static void
frame_writer_write_cb (GObject *source,
                       GAsyncResult *res,
                       gpointer user_data)
{
  TerminalShareFrameWriter *writer = user_data;
  GError *error = NULL;
  gsize bytes_written = 0;

  writer->writing = FALSE;
  g_clear_pointer (&writer->in_flight, g_bytes_unref);
  if (!g_output_stream_write_all_finish (G_OUTPUT_STREAM (source), res,
                                         &bytes_written, &error))
    {
      g_clear_error (&error);
      /* The peer is gone or broken; drop the rest of the queue. The
       * owner learns about it through its own read side (EOF), which
       * is the single disconnect path. */
      g_queue_free_full (writer->queue, (GDestroyNotify) g_bytes_unref);
      writer->queue = g_queue_new ();
      return;
    }
  frame_writer_write_next (writer);
}

TerminalShareFrameWriter *
terminal_share_frame_writer_new (GOutputStream *stream)
{
  TerminalShareFrameWriter *writer = g_new0 (TerminalShareFrameWriter, 1);
  writer->stream = g_object_ref (stream);
  writer->queue = g_queue_new ();
  return writer;
}

void
terminal_share_frame_writer_queue (TerminalShareFrameWriter *writer,
                                   const gchar *data,
                                   gsize len)
{
  guint8 *combined = g_malloc (len + 4);
  GBytes *frame;

  g_return_if_fail (writer != NULL);
  combined[0] = (len >> 24) & 0xff;
  combined[1] = (len >> 16) & 0xff;
  combined[2] = (len >> 8) & 0xff;
  combined[3] = len & 0xff;
  if (len > 0)
    memcpy (combined + 4, data, len);
  frame = g_bytes_new_take (combined, len + 4);
  g_queue_push_tail (writer->queue, frame);
  frame_writer_write_next (writer);
}

void
terminal_share_frame_writer_queue_text (TerminalShareFrameWriter *writer,
                                        const gchar *text)
{
  terminal_share_frame_writer_queue (writer, text, strlen (text));
}

void
terminal_share_frame_writer_free (TerminalShareFrameWriter *writer)
{
  if (writer == NULL)
    return;
  g_clear_object (&writer->stream);
  g_clear_pointer (&writer->in_flight, g_bytes_unref);
  g_queue_free_full (writer->queue, (GDestroyNotify) g_bytes_unref);
  g_free (writer);
}

GBytes *
terminal_share_encode_frame (const gchar *data,
                             gsize len)
{
  guint8 *combined = g_malloc (len + 4);
  combined[0] = (len >> 24) & 0xff;
  combined[1] = (len >> 16) & 0xff;
  combined[2] = (len >> 8) & 0xff;
  combined[3] = len & 0xff;
  if (len > 0)
    memcpy (combined + 4, data, len);
  return g_bytes_new_take (combined, len + 4);
}
