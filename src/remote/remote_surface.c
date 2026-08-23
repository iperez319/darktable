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

static gboolean _surface_from_pipe(dt_dev_pixelpipe_t *pipe, uint32_t crop_x, uint32_t crop_y,
                                   uint32_t crop_width, uint32_t crop_height,
                                   dt_remote_surface_t *surface,
                                   dt_remote_render_timing_t *timing, char **error)
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
  const gint64 snapshot_start = g_get_monotonic_time();
  if(!dt_dev_pixelpipe_backbuffer_snapshot(pipe, &snapshot, error))
    return FALSE;
  if(timing)
    timing->snapshot_ms = (double)(g_get_monotonic_time() - snapshot_start) / 1000.0;
  const uint32_t width = crop_width ? crop_width : (uint32_t)snapshot.width;
  const uint32_t height = crop_height ? crop_height : (uint32_t)snapshot.height;
  if((uint64_t)crop_x + width > (uint64_t)snapshot.width ||
     (uint64_t)crop_y + height > (uint64_t)snapshot.height)
  {
    if(error)
      *error = g_strdup("surface crop is outside the completed backbuffer");
    g_free(snapshot.data);
    return FALSE;
  }
  const size_t output_size = (size_t)width * (size_t)height * 4u;
  if(width > 4096 || height > 4096 || output_size > DT_REMOTE_MAX_ATTACHMENT_BYTES)
  {
    if(error)
      *error = g_strdup("completed surface exceeds protocol limits");
    g_free(snapshot.data);
    return FALSE;
  }

  surface->pixels = g_malloc(output_size);
  surface->size = output_size;
  surface->width = width;
  surface->height = height;
  surface->bytes_per_row = surface->width * 4;
  surface->backbuffer_hash = snapshot.hash;
  const gint64 normalize_start = g_get_monotonic_time();
  for(uint32_t row = 0; row < height; row++)
  {
    for(uint32_t column = 0; column < width; column++)
    {
      const size_t source_index =
          ((size_t)(crop_y + row) * (size_t)snapshot.width + crop_x + column) * 4u;
      const size_t destination_index = ((size_t)row * width + column) * 4u;
      uint32_t argb = 0;
      memcpy(&argb, snapshot.data + source_index, sizeof(argb));
      surface->pixels[destination_index + 0] = (uint8_t)(argb & 0xffu);
      surface->pixels[destination_index + 1] = (uint8_t)((argb >> 8) & 0xffu);
      surface->pixels[destination_index + 2] = (uint8_t)((argb >> 16) & 0xffu);
      surface->pixels[destination_index + 3] = 255;
    }
  }
  g_free(snapshot.data);
  if(timing)
    timing->normalize_ms = (double)(g_get_monotonic_time() - normalize_start) / 1000.0;

  const gint64 digest_start = g_get_monotonic_time();
  GChecksum *checksum = g_checksum_new(G_CHECKSUM_SHA256);
  g_checksum_update(checksum, surface->pixels, surface->size);
  surface->pixel_digest = g_strdup_printf("sha256:%s", g_checksum_get_string(checksum));
  g_checksum_free(checksum);
  if(timing)
    timing->digest_ms = (double)(g_get_monotonic_time() - digest_start) / 1000.0;
  return TRUE;
}

gboolean dt_remote_surface_from_pipe(dt_dev_pixelpipe_t *pipe, dt_remote_surface_t *surface,
                                     dt_remote_render_timing_t *timing, char **error)
{
  return _surface_from_pipe(pipe, 0, 0, 0, 0, surface, timing, error);
}

gboolean dt_remote_surface_region_from_pipe(dt_dev_pixelpipe_t *pipe, uint32_t x, uint32_t y,
                                            uint32_t width, uint32_t height,
                                            dt_remote_surface_t *surface,
                                            dt_remote_render_timing_t *timing, char **error)
{
  if(!width || !height)
  {
    if(error)
      *error = g_strdup("surface crop dimensions must be positive");
    return FALSE;
  }
  return _surface_from_pipe(pipe, x, y, width, height, surface, timing, error);
}

void dt_remote_surface_clear(dt_remote_surface_t *surface)
{
  if(!surface)
    return;
  g_free(surface->pixels);
  g_free(surface->pixel_digest);
  memset(surface, 0, sizeof(*surface));
}
