/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#pragma once

#include "develop/develop.h"

typedef enum dt_remote_changed_field_t
{
  DT_REMOTE_CHANGED_EXPOSURE,
  DT_REMOTE_CHANGED_BLACK
} dt_remote_changed_field_t;

typedef struct dt_remote_exposure_t
{
  dt_iop_module_t *module;
  dt_introspection_field_t *mode_field;
  dt_introspection_field_t *black_field;
  dt_introspection_field_t *exposure_field;
  int manual_mode;
  int module_version;
  size_t params_size;
  uint8_t *baseline;
  uint8_t *accepted;
} dt_remote_exposure_t;

gboolean dt_remote_exposure_init(dt_remote_exposure_t *exposure, dt_develop_t *dev, char **error);
void dt_remote_exposure_cleanup(dt_remote_exposure_t *exposure);
gboolean dt_remote_exposure_apply(dt_remote_exposure_t *exposure, dt_develop_t *dev,
                                  double exposure_ev, double black,
                                  dt_remote_changed_field_t changed, float *accepted_exposure,
                                  float *accepted_black, char **error);
gboolean dt_remote_exposure_reset(dt_remote_exposure_t *exposure, dt_develop_t *dev,
                                  float *accepted_exposure, float *accepted_black, char **error);
void dt_remote_exposure_values(const dt_remote_exposure_t *exposure, const uint8_t *params,
                               float *exposure_ev, float *black);
