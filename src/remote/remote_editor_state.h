/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#pragma once

#include "remote/remote_exposure.h"

#define DT_REMOTE_EDITOR_STATE_SCHEMA 3
#define DT_REMOTE_TONE_CURVE_MIN_POINTS 2
#define DT_REMOTE_TONE_CURVE_MAX_POINTS 20
#define DT_REMOTE_MASK_JSON_MAX (240 * 1024)

typedef struct dt_remote_curve_point_t
{
  double x;
  double y;
} dt_remote_curve_point_t;

typedef enum dt_remote_lens_correction_t
{
  DT_REMOTE_LENS_CORRECTION_OFF,
  DT_REMOTE_LENS_CORRECTION_AUTOMATIC,
  DT_REMOTE_LENS_CORRECTION_MANUAL
} dt_remote_lens_correction_t;

typedef enum dt_remote_lens_profile_status_t
{
  DT_REMOTE_LENS_PROFILE_NOT_REQUESTED,
  DT_REMOTE_LENS_PROFILE_APPLIED,
  DT_REMOTE_LENS_PROFILE_MISSING
} dt_remote_lens_profile_status_t;

typedef enum dt_remote_denoise_preset_t
{
  DT_REMOTE_DENOISE_OFF,
  DT_REMOTE_DENOISE_LOW,
  DT_REMOTE_DENOISE_MEDIUM,
  DT_REMOTE_DENOISE_HIGH,
  DT_REMOTE_DENOISE_CUSTOM
} dt_remote_denoise_preset_t;

typedef enum dt_remote_highlight_reconstruction_t
{
  DT_REMOTE_HIGHLIGHT_INPAINT_OPPOSED,
  DT_REMOTE_HIGHLIGHT_CLIP,
  DT_REMOTE_HIGHLIGHT_RECONSTRUCT_COLOR,
  DT_REMOTE_HIGHLIGHT_GUIDED_LAPLACIANS,
  DT_REMOTE_HIGHLIGHT_SEGMENTATION
} dt_remote_highlight_reconstruction_t;

typedef struct dt_remote_editor_state_t
{
  struct
  {
    double exposure_ev;
    double black_level;
  } exposure;
  struct
  {
    double x;
    double y;
    double width;
    double height;
    int rotation_quarter_turns;
    double straighten_degrees;
    gboolean flip_horizontal;
    gboolean flip_vertical;
  } geometry;
  struct
  {
    double temperature_kelvin;
    double tint;
  } white_balance;
  struct
  {
    double contrast;
    double highlights;
    double shadows;
    double whites;
    double blacks;
  } light;
  struct
  {
    double vibrance;
    double saturation;
  } color;
  struct
  {
    size_t point_count;
    dt_remote_curve_point_t points[DT_REMOTE_TONE_CURVE_MAX_POINTS];
  } tone_curve;
  struct
  {
    dt_remote_lens_correction_t lens_correction;
    char lens_profile[128];
    dt_remote_lens_profile_status_t lens_profile_status;
    gboolean chromatic_aberration_correction;
    double defringe;
  } optics;
  struct
  {
    double sharpening;
    double noise_reduction;
    dt_remote_denoise_preset_t denoise_preset;
    dt_remote_highlight_reconstruction_t highlight_reconstruction;
  } detail;
  char masks_json[DT_REMOTE_MASK_JSON_MAX];
} dt_remote_editor_state_t;

typedef struct dt_remote_module_state_t
{
  dt_iop_module_t *module;
  int module_version;
  size_t params_size;
  uint8_t *baseline;
  uint8_t *accepted;
  gboolean baseline_enabled;
  gboolean accepted_enabled;
} dt_remote_module_state_t;

typedef struct dt_remote_editor_state_facade_t
{
  dt_remote_module_state_t crop;
  dt_remote_module_state_t orientation;
  dt_remote_module_state_t straighten;
  dt_remote_module_state_t temperature;
  dt_remote_module_state_t color_balance;
  dt_remote_module_state_t tone_equalizer;
  dt_remote_module_state_t rgb_curve;
  dt_remote_module_state_t lens;
  dt_remote_module_state_t chromatic_aberration;
  dt_remote_module_state_t defringe;
  dt_remote_module_state_t sharpen;
  dt_remote_module_state_t denoise;
  dt_remote_module_state_t highlight_reconstruction;
  dt_remote_editor_state_t baseline;
  dt_remote_editor_state_t current;
  double xyz_to_cam[4][3];
  double cam_to_xyz[3][4];
  int base_orientation;
  GPtrArray *mask_exposure_modules;
  GPtrArray *mask_modules;
  gboolean initialized;
} dt_remote_editor_state_facade_t;

gboolean dt_remote_editor_state_facade_init(dt_remote_editor_state_facade_t *facade,
                                            dt_develop_t *dev,
                                            const dt_remote_exposure_t *exposure,
                                            char **error);
void dt_remote_editor_state_facade_cleanup(dt_remote_editor_state_facade_t *facade);
gboolean dt_remote_editor_state_apply(dt_remote_editor_state_facade_t *facade,
                                      dt_remote_exposure_t *exposure,
                                      dt_develop_t *dev,
                                      const dt_remote_editor_state_t *state,
                                      char **error);
void dt_remote_editor_state_sync_exposure(dt_remote_editor_state_facade_t *facade,
                                          const dt_remote_exposure_t *exposure);
void dt_remote_editor_state_checksum_update(const dt_remote_editor_state_facade_t *facade,
                                            GChecksum *checksum);
gboolean dt_remote_editor_state_run_self_tests(char **error);
