/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#pragma once

#include <glib.h>
#include <json-glib/json-glib.h>
#include <stdint.h>
#include <stdio.h>

#define DT_REMOTE_PROTOCOL_MAJOR 2
#define DT_REMOTE_MAX_JSON_BYTES (256u * 1024u)
#define DT_REMOTE_MAX_ATTACHMENT_BYTES (32u * 1024u * 1024u)

typedef enum dt_remote_message_type_t
{
  DT_REMOTE_WORKER_HELLO = 1,
  DT_REMOTE_SESSION_OPEN = 2,
  DT_REMOTE_SESSION_SET_EXPOSURE = 3,
  DT_REMOTE_SESSION_RESET_EXPOSURE = 4,
  DT_REMOTE_SURFACE_RENDER = 5,
  DT_REMOTE_SESSION_DESCRIBE = 6,
  DT_REMOTE_WORKER_SHUTDOWN = 7,
  DT_REMOTE_SESSION_CHECKPOINT_XMP = 8,
  DT_REMOTE_SESSION_EXPORT_JPEG = 9,
  DT_REMOTE_SURFACE_CANCEL = 10,
  DT_REMOTE_WORKER_CAPABILITIES = 0x8001,
  DT_REMOTE_SESSION_OPENED = 0x8002,
  DT_REMOTE_SESSION_EXPOSURE_ACCEPTED = 0x8003,
  DT_REMOTE_SURFACE_RENDERED = 0x8004,
  DT_REMOTE_ARTIFACT_WRITTEN = 0x8005,
  DT_REMOTE_WORKER_ERROR = 0xffff
} dt_remote_message_type_t;

typedef struct dt_remote_frame_t
{
  uint16_t type;
  JsonNode *json;
  uint8_t *attachment;
  size_t attachment_size;
} dt_remote_frame_t;

typedef enum dt_remote_read_result_t
{
  DT_REMOTE_READ_ERROR = -1,
  DT_REMOTE_READ_EOF = 0,
  DT_REMOTE_READ_OK = 1
} dt_remote_read_result_t;

dt_remote_read_result_t dt_remote_frame_read(FILE *input, dt_remote_frame_t *frame, GError **error);
gboolean dt_remote_frame_write(FILE *output, uint16_t type, JsonNode *json, const void *attachment,
                               size_t attachment_size, GError **error);
void dt_remote_frame_clear(dt_remote_frame_t *frame);
const char *dt_remote_message_name(uint16_t type);
