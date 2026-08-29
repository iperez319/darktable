/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include "remote/remote_protocol.h"

#include <errno.h>
#include <string.h>

#define DT_REMOTE_ERROR dt_remote_error_quark()

static GQuark dt_remote_error_quark(void)
{
  return g_quark_from_static_string("darktable-remote-protocol");
}

static gboolean _read_exact(FILE *input, void *data, size_t size, gboolean allow_clean_eof,
                            gboolean *clean_eof, GError **error)
{
  uint8_t *out = data;
  size_t done = 0;
  if(clean_eof)
    *clean_eof = FALSE;
  while(done < size)
  {
    const size_t n = fread(out + done, 1, size - done, input);
    if(n)
    {
      done += n;
      continue;
    }
    if(feof(input))
    {
      if(allow_clean_eof && done == 0 && clean_eof)
        *clean_eof = TRUE;
      else
        g_set_error(error, DT_REMOTE_ERROR, 1, "unexpected EOF after %zu of %zu bytes", done, size);
      return FALSE;
    }
    g_set_error(error, DT_REMOTE_ERROR, 1, "read failed: %s", g_strerror(errno));
    return FALSE;
  }
  return TRUE;
}

static gboolean _write_exact(FILE *output, const void *data, size_t size, GError **error)
{
  const uint8_t *in = data;
  size_t done = 0;
  while(done < size)
  {
    const size_t n = fwrite(in + done, 1, size - done, output);
    if(n)
    {
      done += n;
      continue;
    }
    g_set_error(error, DT_REMOTE_ERROR, 2, "write failed: %s", g_strerror(errno));
    return FALSE;
  }
  return TRUE;
}

void dt_remote_frame_clear(dt_remote_frame_t *frame)
{
  if(!frame)
    return;
  if(frame->json)
    json_node_unref(frame->json);
  g_free(frame->attachment);
  memset(frame, 0, sizeof(*frame));
}

dt_remote_read_result_t dt_remote_frame_read(FILE *input, dt_remote_frame_t *frame, GError **error)
{
  g_return_val_if_fail(frame != NULL, DT_REMOTE_READ_ERROR);
  memset(frame, 0, sizeof(*frame));
  uint8_t header[20];
  gboolean clean_eof = FALSE;
  if(!_read_exact(input, header, sizeof(header), TRUE, &clean_eof, error))
    return clean_eof ? DT_REMOTE_READ_EOF : DT_REMOTE_READ_ERROR;
  if(memcmp(header, "DTRW", 4))
  {
    g_set_error(error, DT_REMOTE_ERROR, 3, "invalid worker frame magic");
    return DT_REMOTE_READ_ERROR;
  }

  uint16_t major_be = 0, type_be = 0;
  uint32_t json_be = 0;
  uint64_t attachment_be = 0;
  memcpy(&major_be, header + 4, sizeof(major_be));
  memcpy(&type_be, header + 6, sizeof(type_be));
  memcpy(&json_be, header + 8, sizeof(json_be));
  memcpy(&attachment_be, header + 12, sizeof(attachment_be));
  const uint16_t major = GUINT16_FROM_BE(major_be);
  const uint32_t json_size = GUINT32_FROM_BE(json_be);
  const uint64_t attachment_size = GUINT64_FROM_BE(attachment_be);
  frame->type = GUINT16_FROM_BE(type_be);
  if(major != DT_REMOTE_PROTOCOL_MAJOR)
  {
    g_set_error(error, DT_REMOTE_ERROR, 4, "unsupported worker protocol major %u", major);
    return DT_REMOTE_READ_ERROR;
  }
  if(json_size == 0 || json_size > DT_REMOTE_MAX_JSON_BYTES ||
     attachment_size > DT_REMOTE_MAX_ATTACHMENT_BYTES || attachment_size > G_MAXSIZE)
  {
    g_set_error(error, DT_REMOTE_ERROR, 5,
                "worker frame lengths exceed limits (%u JSON, %" G_GUINT64_FORMAT " attachment)",
                json_size, attachment_size);
    return DT_REMOTE_READ_ERROR;
  }

  gchar *json = g_malloc((size_t)json_size + 1);
  if(!_read_exact(input, json, json_size, FALSE, NULL, error))
  {
    g_free(json);
    return DT_REMOTE_READ_ERROR;
  }
  json[json_size] = '\0';
  JsonParser *parser = json_parser_new_immutable();
  gboolean parsed = json_parser_load_from_data(parser, json, json_size, error);
  g_free(json);
  if(!parsed || !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
  {
    if(parsed)
      g_set_error(error, DT_REMOTE_ERROR, 6, "worker JSON header is not an object");
    g_object_unref(parser);
    return DT_REMOTE_READ_ERROR;
  }
  frame->json = json_node_copy(json_parser_get_root(parser));
  g_object_unref(parser);

  if(attachment_size)
  {
    frame->attachment = g_try_malloc((gsize)attachment_size);
    if(!frame->attachment)
    {
      g_set_error(error, DT_REMOTE_ERROR, 7, "could not allocate worker attachment");
      dt_remote_frame_clear(frame);
      return DT_REMOTE_READ_ERROR;
    }
    frame->attachment_size = (size_t)attachment_size;
    if(!_read_exact(input, frame->attachment, frame->attachment_size, FALSE, NULL, error))
    {
      dt_remote_frame_clear(frame);
      return DT_REMOTE_READ_ERROR;
    }
  }
  return DT_REMOTE_READ_OK;
}

gboolean dt_remote_frame_write(FILE *output, uint16_t type, JsonNode *json, const void *attachment,
                               size_t attachment_size, GError **error)
{
  if(!json || !JSON_NODE_HOLDS_OBJECT(json) || attachment_size > DT_REMOTE_MAX_ATTACHMENT_BYTES ||
     (attachment_size && !attachment))
  {
    g_set_error(error, DT_REMOTE_ERROR, 8, "invalid outbound worker frame");
    return FALSE;
  }
  JsonGenerator *generator = json_generator_new();
  json_generator_set_root(generator, json);
  json_generator_set_pretty(generator, FALSE);
  gsize json_size = 0;
  gchar *data = json_generator_to_data(generator, &json_size);
  g_object_unref(generator);
  if(!data || json_size == 0 || json_size > DT_REMOTE_MAX_JSON_BYTES || json_size > G_MAXUINT32)
  {
    g_free(data);
    g_set_error(error, DT_REMOTE_ERROR, 8, "outbound worker JSON exceeds limits");
    return FALSE;
  }

  uint8_t header[20] = {'D', 'T', 'R', 'W'};
  const uint16_t major_be = GUINT16_TO_BE(DT_REMOTE_PROTOCOL_MAJOR);
  const uint16_t type_be = GUINT16_TO_BE(type);
  const uint32_t json_be = GUINT32_TO_BE((uint32_t)json_size);
  const uint64_t attachment_be = GUINT64_TO_BE((uint64_t)attachment_size);
  memcpy(header + 4, &major_be, sizeof(major_be));
  memcpy(header + 6, &type_be, sizeof(type_be));
  memcpy(header + 8, &json_be, sizeof(json_be));
  memcpy(header + 12, &attachment_be, sizeof(attachment_be));

  const gboolean ok =
      _write_exact(output, header, sizeof(header), error) &&
      _write_exact(output, data, json_size, error) &&
      (!attachment_size || _write_exact(output, attachment, attachment_size, error)) &&
      fflush(output) == 0;
  if(!ok && error && !*error)
    g_set_error(error, DT_REMOTE_ERROR, 2, "flush failed: %s", g_strerror(errno));
  g_free(data);
  return ok;
}

const char *dt_remote_message_name(uint16_t type)
{
  switch(type)
  {
  case DT_REMOTE_WORKER_HELLO:
    return "worker.hello";
  case DT_REMOTE_SESSION_OPEN:
    return "session.open";
  case DT_REMOTE_SESSION_SET_EXPOSURE:
    return "session.setExposure";
  case DT_REMOTE_SESSION_RESET_EXPOSURE:
    return "session.resetExposure";
  case DT_REMOTE_SURFACE_RENDER:
    return "surface.render";
  case DT_REMOTE_SESSION_DESCRIBE:
    return "session.describe";
  case DT_REMOTE_WORKER_SHUTDOWN:
    return "worker.shutdown";
  case DT_REMOTE_SESSION_CHECKPOINT_XMP:
    return "session.checkpointXmp";
  case DT_REMOTE_SESSION_EXPORT_JPEG:
    return "session.exportJpeg";
  case DT_REMOTE_SURFACE_CANCEL:
    return "surface.cancel";
  case DT_REMOTE_SESSION_SET_STATE:
    return "session.setState";
  case DT_REMOTE_WORKER_CAPABILITIES:
    return "worker.capabilities";
  case DT_REMOTE_SESSION_OPENED:
    return "session.opened";
  case DT_REMOTE_SESSION_EXPOSURE_ACCEPTED:
    return "session.exposureAccepted";
  case DT_REMOTE_SURFACE_RENDERED:
    return "surface.rendered";
  case DT_REMOTE_ARTIFACT_WRITTEN:
    return "artifact.written";
  case DT_REMOTE_SESSION_STATE_ACCEPTED:
    return "session.stateAccepted";
  case DT_REMOTE_WORKER_ERROR:
    return "worker.error";
  default:
    return NULL;
  }
}
