/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#pragma once

#include "common/iop_profile.h"
#include "develop/pixelpipe_hb.h"

#define DT_REMOTE_HISTOGRAM_BINS 1024
#define DT_REMOTE_HISTOGRAM_DOMAIN "display-referred-float-pre-pack-v1"

typedef struct dt_remote_analysis_t
{
  gboolean valid;
  uint64_t revision;
  uint64_t generation;
  uint32_t width;
  uint32_t height;
  uint32_t sampled_pixels;
  uint32_t callback_invocations;
  uint32_t crop_x;
  uint32_t crop_y;
  uint32_t crop_width;
  uint32_t crop_height;
  uint32_t red[DT_REMOTE_HISTOGRAM_BINS];
  uint32_t green[DT_REMOTE_HISTOGRAM_BINS];
  uint32_t blue[DT_REMOTE_HISTOGRAM_BINS];
  dt_colorspaces_color_profile_type_t profile_type;
  dt_iop_color_intent_t profile_intent;
  char profile_filename[DT_IOP_COLOR_ICC_LEN];
  char pixelpipe_result_digest[72];
  char error[160];
  double elapsed_ms;
} dt_remote_analysis_t;

void dt_remote_analysis_prepare(dt_remote_analysis_t *analysis, uint64_t revision,
                                uint64_t generation);
void dt_remote_analysis_crop(dt_remote_analysis_t *analysis, uint32_t x, uint32_t y,
                             uint32_t width, uint32_t height);
void dt_remote_analysis_callback(void *user_data, const float *input, int width, int height,
                                 dt_iop_colorspace_type_t colorspace,
                                 const struct dt_iop_order_iccprofile_info_t *profile_info);
gboolean dt_remote_analysis_finalize(dt_remote_analysis_t *analysis, const char *state_digest,
                                     const char *pixel_digest, uint32_t surface_width,
                                     uint32_t surface_height, char **error);
