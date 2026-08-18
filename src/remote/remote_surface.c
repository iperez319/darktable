/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include "remote/remote_surface.h"
#include "remote/remote_protocol.h"

#include <string.h>

gboolean dt_remote_surface_from_pipe(dt_dev_pixelpipe_t *pipe, dt_remote_surface_t *surface,
                                     char **error)
{
  if(error)
    *error = NULL;
  if(!surface)
  {
    if(error)
      *error = g_strdup("missing surface destination");
    return FALSE;
  }
  memset(surface, 0, sizeof(*surface));
  dt_dev_backbuffer_snapshot_t snapshot;
  if(!dt_dev_pixelpipe_backbuffer_snapshot(pipe, &snapshot, error))
    return FALSE;
  if(snapshot.width > 2048 || snapshot.height > 2048 ||
     snapshot.size > DT_REMOTE_MAX_ATTACHMENT_BYTES)
  {
    if(error)
      *error = g_strdup("completed surface exceeds protocol limits");
    g_free(snapshot.data);
    return FALSE;
  }

  surface->pixels = g_malloc(snapshot.size);
  surface->size = snapshot.size;
  surface->width = (uint32_t)snapshot.width;
  surface->height = (uint32_t)snapshot.height;
  surface->bytes_per_row = surface->width * 4;
  surface->backbuffer_hash = snapshot.hash;
  for(size_t i = 0; i < snapshot.size / 4; i++)
  {
    uint32_t argb = 0;
    memcpy(&argb, snapshot.data + i * 4, sizeof(argb));
    surface->pixels[i * 4 + 0] = (uint8_t)(argb & 0xffu);
    surface->pixels[i * 4 + 1] = (uint8_t)((argb >> 8) & 0xffu);
    surface->pixels[i * 4 + 2] = (uint8_t)((argb >> 16) & 0xffu);
    surface->pixels[i * 4 + 3] = 255;
  }
  g_free(snapshot.data);

  GChecksum *checksum = g_checksum_new(G_CHECKSUM_SHA256);
  g_checksum_update(checksum, surface->pixels, surface->size);
  surface->pixel_digest = g_strdup_printf("sha256:%s", g_checksum_get_string(checksum));
  g_checksum_free(checksum);
  return TRUE;
}

void dt_remote_surface_clear(dt_remote_surface_t *surface)
{
  if(!surface)
    return;
  g_free(surface->pixels);
  g_free(surface->pixel_digest);
  memset(surface, 0, sizeof(*surface));
}
