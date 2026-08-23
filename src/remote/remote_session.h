/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#pragma once

#include "remote/remote_analysis.h"
#include "remote/remote_exposure.h"
#include "remote/remote_surface.h"

typedef enum dt_remote_surface_role_t
{
  DT_REMOTE_SURFACE_OVERVIEW,
  DT_REMOTE_SURFACE_VIEWPORT,
  DT_REMOTE_SURFACE_BASELINE
} dt_remote_surface_role_t;

typedef struct dt_remote_normalized_rect_t
{
  double x, y, width, height;
} dt_remote_normalized_rect_t;

typedef struct dt_remote_session_t
{
  dt_imgid_t image_id;
  dt_develop_t dev;
  dt_dev_viewport_t overview;
  dt_remote_exposure_t exposure;
  dt_remote_analysis_t analysis;
  GMutex mutex;
  GMutex viewport_cancel_mutex;
  gboolean initialized;
  gboolean open;
  uint64_t epoch;
  uint64_t revision;
  uint64_t desired_generation;
  uint64_t active_viewport_generation;
  // A cancel can arrive after surface.render is read but before that render
  // registers its pixelpipe. Keep it latched until the matching generation
  // starts instead of dropping it in that startup window.
  uint64_t pending_viewport_cancel_generation;
  uint32_t maximum_long_edge;
  uint32_t source_pixel_width;
  uint32_t source_pixel_height;
  char *image_digest;
  char *state_digest;
} dt_remote_session_t;

void dt_remote_session_init(dt_remote_session_t *session);
void dt_remote_session_cleanup(dt_remote_session_t *session);
gboolean dt_remote_session_open(dt_remote_session_t *session, const char *image_path,
                                uint32_t maximum_long_edge, const char *color_contract,
                                char **error);
gboolean dt_remote_session_set_exposure(dt_remote_session_t *session, double exposure_ev,
                                        double black, dt_remote_changed_field_t changed,
                                        uint64_t generation, float *accepted_exposure,
                                        float *accepted_black, char **error);
gboolean dt_remote_session_reset_exposure(dt_remote_session_t *session, uint64_t generation,
                                          float *accepted_exposure, float *accepted_black,
                                          char **error);
gboolean dt_remote_session_render(dt_remote_session_t *session, uint64_t revision,
                                  uint64_t generation, uint32_t width, uint32_t height,
                                  dt_remote_surface_role_t role,
                                  dt_remote_normalized_rect_t coverage,
                                  double source_pixels_per_output_pixel,
                                  dt_remote_surface_t *surface,
                                  dt_remote_render_timing_t *timing, char **error);
void dt_remote_session_cancel_viewport(dt_remote_session_t *session, uint64_t generation);
void dt_remote_session_current_values(const dt_remote_session_t *session, float *exposure_ev,
                                      float *black);
void dt_remote_session_baseline_values(const dt_remote_session_t *session, float *exposure_ev,
                                       float *black);
gboolean dt_remote_session_checkpoint_xmp(dt_remote_session_t *session, const char *path,
                                          char **error);
gboolean dt_remote_session_export_jpeg(dt_remote_session_t *session, const char *path,
                                       char **error);
