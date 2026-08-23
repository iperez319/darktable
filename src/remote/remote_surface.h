/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#pragma once

#include "develop/pixelpipe_hb.h"

typedef struct dt_remote_surface_t
{
  uint8_t *pixels;
  size_t size;
  uint32_t width;
  uint32_t height;
  uint32_t bytes_per_row;
  char *pixel_digest;
  dt_hash_t backbuffer_hash;
} dt_remote_surface_t;

typedef struct dt_remote_render_timing_t
{
  double pixelpipe_ms;
  double snapshot_ms;
  double normalize_ms;
  double digest_ms;
  double analysis_ms;
} dt_remote_render_timing_t;

gboolean dt_remote_surface_from_pipe(dt_dev_pixelpipe_t *pipe, dt_remote_surface_t *surface,
                                     dt_remote_render_timing_t *timing, char **error);
gboolean dt_remote_surface_region_from_pipe(dt_dev_pixelpipe_t *pipe, uint32_t x, uint32_t y,
                                            uint32_t width, uint32_t height,
                                            dt_remote_surface_t *surface,
                                            dt_remote_render_timing_t *timing, char **error);
void dt_remote_surface_clear(dt_remote_surface_t *surface);
