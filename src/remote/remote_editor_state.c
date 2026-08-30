/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include "remote/remote_editor_state.h"
#include "remote/remote_masks.h"

#include "common/colorspaces.h"
#include "common/curve_tools.h"
#include "common/image.h"
#include "common/introspection.h"
#include "develop/imageop.h"
#include "develop/pixelpipe_hb.h"

#include <math.h>
#include <stdarg.h>
#include <string.h>

#define DT_REMOTE_MIN_CROP_SIZE 0.01
#define DT_REMOTE_TEMPERATURE_MIN 1901.0
#define DT_REMOTE_TEMPERATURE_MAX 25000.0
#define DT_REMOTE_ENGINE_TINT_MIN 0.135
#define DT_REMOTE_ENGINE_TINT_MAX 2.326
#define DT_REMOTE_LENS_METHOD_LENSFUN 1
#define DT_REMOTE_LENS_MODIFY_ALL 7
#define DT_REMOTE_DENOISE_MODE_WAVELETS 1
#define DT_REMOTE_HIGHLIGHTS_CLIP 0
#define DT_REMOTE_HIGHLIGHTS_INPAINT 2
#define DT_REMOTE_HIGHLIGHTS_LAPLACIAN 3
#define DT_REMOTE_HIGHLIGHTS_SEGMENTS 4
#define DT_REMOTE_HIGHLIGHTS_OPPOSED 5

typedef struct dt_remote_rgbcurve_point_t
{
  float x;
  float y;
} dt_remote_rgbcurve_point_t;

static void _set_error(char **error, const char *format, ...)
{
  if(!error)
    return;
  va_list ap;
  va_start(ap, format);
  *error = g_strdup_vprintf(format, ap);
  va_end(ap);
}

static uint8_t *_duplicate_params(const void *params, size_t size)
{
  uint8_t *copy = g_try_malloc(size);
  if(copy)
    memcpy(copy, params, size);
  return copy;
}

static gboolean _module_init(dt_remote_module_state_t *state, dt_develop_t *dev,
                             const char *operation, int expected_version, char **error)
{
  memset(state, 0, sizeof(*state));
  state->module = dt_iop_get_module_by_op_priority(dev->iop, operation, 0);
  if(!state->module)
  {
    _set_error(error, "%s module priority 0 is unavailable", operation);
    return FALSE;
  }
  dt_iop_module_so_t *so = state->module->so;
  if(!so || !so->have_introspection || !so->get_introspection || !so->get_f)
  {
    _set_error(error, "%s module has no usable introspection", operation);
    return FALSE;
  }
  const dt_introspection_t *intro = so->get_introspection();
  if(!intro || intro->params_version != expected_version ||
     intro->size != (size_t)state->module->params_size)
  {
    _set_error(error, "%s introspection is incompatible (expected version %d)", operation,
               expected_version);
    return FALSE;
  }
  state->module_version = intro->params_version;
  state->params_size = intro->size;
  state->baseline = _duplicate_params(state->module->params, state->params_size);
  state->accepted = _duplicate_params(state->module->params, state->params_size);
  if(!state->baseline || !state->accepted)
  {
    _set_error(error, "could not allocate %s parameter state", operation);
    g_clear_pointer(&state->baseline, g_free);
    g_clear_pointer(&state->accepted, g_free);
    return FALSE;
  }
  state->baseline_enabled = state->module->enabled;
  state->accepted_enabled = state->module->enabled;
  return TRUE;
}

static void _module_cleanup(dt_remote_module_state_t *state)
{
  g_free(state->baseline);
  g_free(state->accepted);
  memset(state, 0, sizeof(*state));
}

static dt_introspection_field_t *_field(const dt_remote_module_state_t *state,
                                        const char *name)
{
  return state->module->so->get_f(name);
}

static gboolean _verify_field(const dt_remote_module_state_t *state, const char *name,
                              dt_introspection_type_t type, size_t size, char **error)
{
  const dt_introspection_field_t *field = _field(state, name);
  if(!field || field->header.type != type || field->header.size != size ||
     field->header.offset > state->params_size ||
     size > state->params_size - field->header.offset)
  {
    _set_error(error, "%s.%s introspection field is incompatible",
               state->module->op, name);
    return FALSE;
  }
  return TRUE;
}

static gboolean _verify_array(const dt_remote_module_state_t *state, const char *name,
                              size_t size, char **error)
{
  const dt_introspection_field_t *field = _field(state, name);
  if(!field || field->header.type != DT_INTROSPECTION_TYPE_ARRAY ||
     field->header.size != size || field->header.offset > state->params_size ||
     size > state->params_size - field->header.offset)
  {
    _set_error(error, "%s.%s introspection array is incompatible",
               state->module->op, name);
    return FALSE;
  }
  return TRUE;
}

static void *_field_pointer(const dt_remote_module_state_t *state, const char *name,
                            uint8_t *params)
{
  return params + _field(state, name)->header.offset;
}

static const void *_const_field_pointer(const dt_remote_module_state_t *state, const char *name,
                                        const uint8_t *params)
{
  return params + _field(state, name)->header.offset;
}

static float _read_float(const dt_remote_module_state_t *state, const char *name,
                         const uint8_t *params)
{
  float value = 0.0f;
  memcpy(&value, _const_field_pointer(state, name, params), sizeof(value));
  return value;
}

static int _read_int(const dt_remote_module_state_t *state, const char *name,
                     const uint8_t *params)
{
  int value = 0;
  memcpy(&value, _const_field_pointer(state, name, params), sizeof(value));
  return value;
}

static void _write_float(const dt_remote_module_state_t *state, const char *name,
                         uint8_t *params, double value)
{
  const float normalized = (float)value;
  memcpy(_field_pointer(state, name, params), &normalized, sizeof(normalized));
}

static void _write_int(const dt_remote_module_state_t *state, const char *name,
                       uint8_t *params, int value)
{
  memcpy(_field_pointer(state, name, params), &value, sizeof(value));
}

static void _write_string(const dt_remote_module_state_t *state, const char *name,
                          uint8_t *params, const char *value, size_t capacity)
{
  char *destination = _field_pointer(state, name, params);
  memset(destination, 0, capacity);
  g_strlcpy(destination, value ? value : "", capacity);
}

static void _install_module(dt_remote_module_state_t *state, dt_develop_t *dev,
                            const uint8_t *params, gboolean enabled)
{
  dt_pthread_mutex_lock(&dev->history_mutex);
  memcpy(state->module->params, params, state->params_size);
  state->module->enabled = enabled;
  dt_dev_add_history_item_ext(dev, state->module, enabled, FALSE);
  dt_dev_invalidate_all(dev);
  dt_pthread_mutex_unlock(&dev->history_mutex);
  memcpy(state->accepted, params, state->params_size);
  state->accepted_enabled = enabled;
}

static gboolean _verify_layout(const dt_remote_editor_state_facade_t *facade, char **error)
{
#define VERIFY_FLOAT(module, name) \
  if(!_verify_field(&(module), (name), DT_INTROSPECTION_TYPE_FLOAT, sizeof(float), error)) \
    return FALSE
#define VERIFY_INT(module, name) \
  if(!_verify_field(&(module), (name), DT_INTROSPECTION_TYPE_INT, sizeof(int), error)) \
    return FALSE
#define VERIFY_ENUM(module, name) \
  if(!_verify_field(&(module), (name), DT_INTROSPECTION_TYPE_ENUM, sizeof(int), error)) \
    return FALSE

  VERIFY_FLOAT(facade->crop, "cx");
  VERIFY_FLOAT(facade->crop, "cy");
  VERIFY_FLOAT(facade->crop, "cw");
  VERIFY_FLOAT(facade->crop, "ch");
  VERIFY_INT(facade->crop, "ratio_n");
  VERIFY_INT(facade->crop, "ratio_d");
  VERIFY_ENUM(facade->orientation, "orientation");
  VERIFY_FLOAT(facade->straighten, "rotation");
  VERIFY_ENUM(facade->straighten, "cropmode");
  VERIFY_FLOAT(facade->temperature, "red");
  VERIFY_FLOAT(facade->temperature, "green");
  VERIFY_FLOAT(facade->temperature, "blue");
  VERIFY_FLOAT(facade->temperature, "various");
  VERIFY_INT(facade->temperature, "preset");
  VERIFY_FLOAT(facade->color_balance, "contrast");
  VERIFY_FLOAT(facade->color_balance, "vibrance");
  VERIFY_FLOAT(facade->color_balance, "saturation_global");
  VERIFY_FLOAT(facade->tone_equalizer, "noise");
  VERIFY_FLOAT(facade->tone_equalizer, "ultra_deep_blacks");
  VERIFY_FLOAT(facade->tone_equalizer, "deep_blacks");
  VERIFY_FLOAT(facade->tone_equalizer, "blacks");
  VERIFY_FLOAT(facade->tone_equalizer, "shadows");
  VERIFY_FLOAT(facade->tone_equalizer, "midtones");
  VERIFY_FLOAT(facade->tone_equalizer, "highlights");
  VERIFY_FLOAT(facade->tone_equalizer, "whites");
  VERIFY_FLOAT(facade->tone_equalizer, "speculars");
  if(!_verify_array(&facade->rgb_curve, "curve_nodes",
                    3 * DT_REMOTE_TONE_CURVE_MAX_POINTS * sizeof(dt_remote_rgbcurve_point_t),
                    error) ||
     !_verify_array(&facade->rgb_curve, "curve_num_nodes", 3 * sizeof(int), error) ||
     !_verify_array(&facade->rgb_curve, "curve_type", 3 * sizeof(int), error) ||
     !_verify_field(&facade->rgb_curve, "curve_autoscale", DT_INTROSPECTION_TYPE_ENUM,
                    sizeof(int), error))
    return FALSE;
  if(!_verify_field(&facade->lens, "method", DT_INTROSPECTION_TYPE_ENUM, sizeof(int), error) ||
     !_verify_field(&facade->lens, "modify_flags", DT_INTROSPECTION_TYPE_ENUM, sizeof(int), error) ||
     !_verify_array(&facade->lens, "camera", 128, error) ||
     !_verify_array(&facade->lens, "lens", 128, error))
    return FALSE;
  VERIFY_FLOAT(facade->defringe, "radius");
  VERIFY_FLOAT(facade->defringe, "thresh");
  VERIFY_ENUM(facade->defringe, "op_mode");
  VERIFY_FLOAT(facade->sharpen, "radius");
  VERIFY_FLOAT(facade->sharpen, "amount");
  VERIFY_FLOAT(facade->sharpen, "threshold");
  VERIFY_FLOAT(facade->denoise, "strength");
  VERIFY_ENUM(facade->denoise, "mode");
  VERIFY_ENUM(facade->highlight_reconstruction, "mode");

#undef VERIFY_ENUM
#undef VERIFY_INT
#undef VERIFY_FLOAT
  return TRUE;
}

static gboolean _prepare_temperature_matrices(dt_remote_editor_state_facade_t *facade,
                                               dt_develop_t *dev, char **error)
{
  static const double rgb_to_xyz[3][4] = {
    { 0.4124564, 0.3575761, 0.1804375, 0.0 },
    { 0.2126729, 0.7151522, 0.0721750, 0.0 },
    { 0.0193339, 0.1191920, 0.9503041, 0.0 }
  };
  static const double xyz_to_rgb[4][3] = {
    { 3.2404542, -1.5371385, -0.4985314 },
    { -0.9692660, 1.8760108, 0.0415560 },
    { 0.0556434, -0.2040259, 1.0572252 },
    { 0.0, 0.0, 0.0 }
  };
  if(!dt_image_is_raw(&dev->image_storage))
  {
    memcpy(facade->xyz_to_cam, xyz_to_rgb, sizeof(xyz_to_rgb));
    memcpy(facade->cam_to_xyz, rgb_to_xyz, sizeof(rgb_to_xyz));
    return TRUE;
  }
  if(!dt_colorspaces_conversion_matrices_xyz(dev->image_storage.adobe_XYZ_to_CAM,
                                              dev->image_storage.d65_color_matrix,
                                              facade->xyz_to_cam, facade->cam_to_xyz))
  {
    _set_error(error, "temperature mapping requires a usable camera color matrix");
    return FALSE;
  }
  return TRUE;
}

static double _product_tint_to_engine(double tint)
{
  return tint >= 0.0
             ? 1.0 + tint / 100.0 * (DT_REMOTE_ENGINE_TINT_MAX - 1.0)
             : 1.0 + tint / 100.0 * (1.0 - DT_REMOTE_ENGINE_TINT_MIN);
}

static double _engine_tint_to_product(double tint)
{
  return tint >= 1.0
             ? (tint - 1.0) * 100.0 / (DT_REMOTE_ENGINE_TINT_MAX - 1.0)
             : (tint - 1.0) * 100.0 / (1.0 - DT_REMOTE_ENGINE_TINT_MIN);
}

static void _temperature_xy(double temperature, double *x, double *y)
{
  if(temperature < 4000.0)
  {
    *x = ((-0.2661239e9 / temperature - 0.2343589e6) / temperature + 0.8776956e3) /
             temperature +
         0.179910;
    if(temperature <= 2222.0)
      *y = ((-1.1063814 * *x - 1.34811020) * *x + 2.18555832) * *x - 0.20219683;
    else
      *y = ((-0.9549476 * *x - 1.37418593) * *x + 2.09137015) * *x - 0.16748867;
  }
  else
  {
    if(temperature <= 7000.0)
      *x = ((-4.6070e9 / temperature + 2.9678e6) / temperature + 0.09911e3) /
               temperature +
           0.244063;
    else
      *x = ((-2.0064e9 / temperature + 1.9018e6) / temperature + 0.24748e3) /
               temperature +
           0.237040;
    *y = (-3.0 * *x + 2.87) * *x - 0.275;
  }
}

static void _temperature_to_xyz(double temperature, double engine_tint, double xyz[3])
{
  double x = 0.0, y = 0.0;
  _temperature_xy(temperature, &x, &y);
  xyz[0] = x / y;
  xyz[1] = 1.0 / engine_tint;
  xyz[2] = (1.0 - x - y) / y;
}

static gboolean _temperature_to_coefficients(const dt_remote_editor_state_facade_t *facade,
                                             double temperature, double product_tint,
                                             float coefficients[4])
{
  double xyz[3];
  _temperature_to_xyz(temperature, _product_tint_to_engine(product_tint), xyz);
  double camera[4] = { 0.0 };
  for(int channel = 0; channel < 4; channel++)
    for(int component = 0; component < 3; component++)
      camera[channel] += facade->xyz_to_cam[channel][component] * xyz[component];
  double multipliers[4];
  for(int channel = 0; channel < 4; channel++)
    multipliers[channel] = camera[channel] > 0.0 ? 1.0 / camera[channel] : 1.0;
  if(!isfinite(multipliers[1]) || multipliers[1] <= 0.0)
    return FALSE;
  double maximum_multiplier = 0.0;
  for(int channel = 0; channel < 4; channel++)
  {
    multipliers[channel] /= multipliers[1];
    if(!isfinite(multipliers[channel]) || multipliers[channel] <= 0.0)
      return FALSE;
    maximum_multiplier = MAX(maximum_multiplier, multipliers[channel]);
  }
  // Temperature v4 bounds each coefficient at 8. Preserve the requested
  // chromaticity at extreme Kelvin/tint combinations by applying one uniform
  // scale when green-normalized coefficients would exceed that limit.
  const double scale = maximum_multiplier > 8.0 ? 8.0 / maximum_multiplier : 1.0;
  for(int channel = 0; channel < 4; channel++)
    coefficients[channel] = (float)(multipliers[channel] * scale);
  return TRUE;
}

static gboolean _coefficients_to_temperature(const dt_remote_editor_state_facade_t *facade,
                                             const float coefficients[4], double *temperature,
                                             double *product_tint)
{
  double camera[4];
  for(int channel = 0; channel < 4; channel++)
  {
    if(!isfinite(coefficients[channel]) || coefficients[channel] <= 0.0f)
      return FALSE;
    camera[channel] = 1.0 / coefficients[channel];
  }
  double xyz[3] = { 0.0 };
  for(int component = 0; component < 3; component++)
    for(int channel = 0; channel < 4; channel++)
      xyz[component] += facade->cam_to_xyz[component][channel] * camera[channel];
  if(!isfinite(xyz[0]) || !isfinite(xyz[1]) || !isfinite(xyz[2]) ||
     xyz[0] <= 0.0 || xyz[1] <= 0.0 || xyz[2] <= 0.0)
    return FALSE;

  double minimum = DT_REMOTE_TEMPERATURE_MIN;
  double maximum = DT_REMOTE_TEMPERATURE_MAX;
  double model[3] = { 0.0 };
  for(int iteration = 0; iteration < 24; iteration++)
  {
    const double candidate = (minimum + maximum) * 0.5;
    _temperature_to_xyz(candidate, 1.0, model);
    if(model[2] / model[0] > xyz[2] / xyz[0])
      maximum = candidate;
    else
      minimum = candidate;
  }
  *temperature = floor((minimum + maximum) * 0.5 + 0.5);
  _temperature_to_xyz(*temperature, 1.0, model);
  const double engine_tint = (model[1] / model[0]) / (xyz[1] / xyz[0]);
  if(!isfinite(engine_tint))
    return FALSE;
  *product_tint = CLAMP(_engine_tint_to_product(engine_tint), -100.0, 100.0);
  return TRUE;
}

static int _product_orientation(int quarter_turns, gboolean horizontal, gboolean vertical)
{
  static const int rotations[4] = {
    ORIENTATION_NONE,
    ORIENTATION_ROTATE_CW_90_DEG,
    ORIENTATION_ROTATE_180_DEG,
    ORIENTATION_ROTATE_CCW_90_DEG
  };
  int orientation = rotations[quarter_turns];
  if(horizontal)
    orientation ^= orientation & ORIENTATION_SWAP_XY ? ORIENTATION_FLIP_Y : ORIENTATION_FLIP_X;
  if(vertical)
    orientation ^= orientation & ORIENTATION_SWAP_XY ? ORIENTATION_FLIP_X : ORIENTATION_FLIP_Y;
  return orientation;
}

static int _merge_orientations(int baseline, int product)
{
  int corrected = baseline;
  if(product & ORIENTATION_SWAP_XY)
  {
    if(baseline & ORIENTATION_FLIP_Y)
      corrected |= ORIENTATION_FLIP_X;
    else
      corrected &= ~ORIENTATION_FLIP_X;
    if(baseline & ORIENTATION_FLIP_X)
      corrected |= ORIENTATION_FLIP_Y;
    else
      corrected &= ~ORIENTATION_FLIP_Y;
  }
  return corrected ^ product;
}

static gboolean _is_identity_curve(const dt_remote_editor_state_t *state)
{
  return state->tone_curve.point_count == 2 &&
         state->tone_curve.points[0].x == 0.0 && state->tone_curve.points[0].y == 0.0 &&
         state->tone_curve.points[1].x == 1.0 && state->tone_curve.points[1].y == 1.0;
}

static dt_remote_denoise_preset_t _denoise_preset_for_amount(double amount)
{
  if(fabs(amount) < 0.0001) return DT_REMOTE_DENOISE_OFF;
  if(fabs(amount - 25.0) < 0.0001) return DT_REMOTE_DENOISE_LOW;
  if(fabs(amount - 50.0) < 0.0001) return DT_REMOTE_DENOISE_MEDIUM;
  if(fabs(amount - 75.0) < 0.0001) return DT_REMOTE_DENOISE_HIGH;
  return DT_REMOTE_DENOISE_CUSTOM;
}

static int _highlight_engine_mode(dt_remote_highlight_reconstruction_t mode)
{
  switch(mode)
  {
    case DT_REMOTE_HIGHLIGHT_CLIP: return DT_REMOTE_HIGHLIGHTS_CLIP;
    case DT_REMOTE_HIGHLIGHT_RECONSTRUCT_COLOR: return DT_REMOTE_HIGHLIGHTS_INPAINT;
    case DT_REMOTE_HIGHLIGHT_GUIDED_LAPLACIANS: return DT_REMOTE_HIGHLIGHTS_LAPLACIAN;
    case DT_REMOTE_HIGHLIGHT_SEGMENTATION: return DT_REMOTE_HIGHLIGHTS_SEGMENTS;
    case DT_REMOTE_HIGHLIGHT_INPAINT_OPPOSED:
    default: return DT_REMOTE_HIGHLIGHTS_OPPOSED;
  }
}

static dt_remote_highlight_reconstruction_t _highlight_product_mode(int mode)
{
  switch(mode)
  {
    case DT_REMOTE_HIGHLIGHTS_CLIP: return DT_REMOTE_HIGHLIGHT_CLIP;
    case DT_REMOTE_HIGHLIGHTS_INPAINT: return DT_REMOTE_HIGHLIGHT_RECONSTRUCT_COLOR;
    case DT_REMOTE_HIGHLIGHTS_LAPLACIAN: return DT_REMOTE_HIGHLIGHT_GUIDED_LAPLACIANS;
    case DT_REMOTE_HIGHLIGHTS_SEGMENTS: return DT_REMOTE_HIGHLIGHT_SEGMENTATION;
    case DT_REMOTE_HIGHLIGHTS_OPPOSED:
    default: return DT_REMOTE_HIGHLIGHT_INPAINT_OPPOSED;
  }
}

static gboolean _validate_state(const dt_remote_editor_state_t *state, char **error)
{
  if(strnlen(state->masks_json, sizeof(state->masks_json)) >= sizeof(state->masks_json))
  {
    _set_error(error, "mask state exceeds the facade-v3 limit");
    return FALSE;
  }
  const double numbers[] = {
    state->exposure.exposure_ev, state->exposure.black_level,
    state->geometry.x, state->geometry.y, state->geometry.width, state->geometry.height,
    state->geometry.straighten_degrees, state->white_balance.temperature_kelvin,
    state->white_balance.tint, state->light.contrast, state->light.highlights,
    state->light.shadows, state->light.whites, state->light.blacks,
    state->color.vibrance, state->color.saturation, state->optics.defringe,
    state->detail.sharpening, state->detail.noise_reduction
  };
  for(size_t index = 0; index < G_N_ELEMENTS(numbers); index++)
    if(!isfinite(numbers[index]))
    {
      _set_error(error, "editor state values must be finite");
      return FALSE;
    }
  if(state->exposure.exposure_ev < -18.0 || state->exposure.exposure_ev > 18.0 ||
     state->exposure.black_level < -1.0 || state->exposure.black_level > 1.0)
  {
    _set_error(error, "exposure is outside the supported range");
    return FALSE;
  }
  if(state->geometry.rotation_quarter_turns < 0 ||
     state->geometry.rotation_quarter_turns > 3 ||
     state->geometry.straighten_degrees < -45.0 ||
     state->geometry.straighten_degrees > 45.0 || state->geometry.x < 0.0 ||
     state->geometry.y < 0.0 || state->geometry.width < DT_REMOTE_MIN_CROP_SIZE ||
     state->geometry.height < DT_REMOTE_MIN_CROP_SIZE ||
     state->geometry.x + state->geometry.width > 1.0000001 ||
     state->geometry.y + state->geometry.height > 1.0000001)
  {
    _set_error(error, "geometry is outside the supported range");
    return FALSE;
  }
  if(state->white_balance.temperature_kelvin < DT_REMOTE_TEMPERATURE_MIN ||
     state->white_balance.temperature_kelvin > DT_REMOTE_TEMPERATURE_MAX ||
     state->white_balance.tint < -100.0 || state->white_balance.tint > 100.0)
  {
    _set_error(error, "white balance is outside the supported range");
    return FALSE;
  }
  const double signed_percentages[] = {
    state->light.contrast, state->light.highlights, state->light.shadows,
    state->light.whites, state->light.blacks, state->color.vibrance,
    state->color.saturation
  };
  for(size_t index = 0; index < G_N_ELEMENTS(signed_percentages); index++)
    if(signed_percentages[index] < -100.0 || signed_percentages[index] > 100.0)
    {
      _set_error(error, "light and color values must be in -100...100");
      return FALSE;
    }
  if(state->optics.lens_correction < DT_REMOTE_LENS_CORRECTION_OFF ||
     state->optics.lens_correction > DT_REMOTE_LENS_CORRECTION_MANUAL ||
     state->optics.lens_profile_status < DT_REMOTE_LENS_PROFILE_NOT_REQUESTED ||
     state->optics.lens_profile_status > DT_REMOTE_LENS_PROFILE_MISSING ||
     strnlen(state->optics.lens_profile, sizeof(state->optics.lens_profile)) >=
         sizeof(state->optics.lens_profile) ||
     (state->optics.lens_correction == DT_REMOTE_LENS_CORRECTION_MANUAL &&
      !state->optics.lens_profile[0]) ||
     state->optics.defringe < 0.0 || state->optics.defringe > 100.0)
  {
    _set_error(error, "optics state is outside the supported range");
    return FALSE;
  }
  if(state->detail.sharpening < 0.0 || state->detail.sharpening > 100.0 ||
     state->detail.noise_reduction < 0.0 || state->detail.noise_reduction > 100.0 ||
     state->detail.denoise_preset < DT_REMOTE_DENOISE_OFF ||
     state->detail.denoise_preset > DT_REMOTE_DENOISE_CUSTOM ||
     state->detail.highlight_reconstruction < DT_REMOTE_HIGHLIGHT_INPAINT_OPPOSED ||
     state->detail.highlight_reconstruction > DT_REMOTE_HIGHLIGHT_SEGMENTATION)
  {
    _set_error(error, "detail state is outside the supported range");
    return FALSE;
  }
  if(state->tone_curve.point_count < DT_REMOTE_TONE_CURVE_MIN_POINTS ||
     state->tone_curve.point_count > DT_REMOTE_TONE_CURVE_MAX_POINTS ||
     state->tone_curve.points[0].x != 0.0 ||
     state->tone_curve.points[state->tone_curve.point_count - 1].x != 1.0)
  {
    _set_error(error, "tone curve must contain 2...20 points with x endpoints 0 and 1");
    return FALSE;
  }
  for(size_t index = 0; index < state->tone_curve.point_count; index++)
  {
    const dt_remote_curve_point_t point = state->tone_curve.points[index];
    if(!isfinite(point.x) || !isfinite(point.y) || point.x < 0.0 || point.x > 1.0 ||
       point.y < 0.0 || point.y > 1.0 ||
       (index && point.x <= state->tone_curve.points[index - 1].x))
    {
      _set_error(error, "tone curve points must be finite, bounded, and strictly x-sorted");
      return FALSE;
    }
  }
  return TRUE;
}

static void _state_from_baseline_modules(dt_remote_editor_state_facade_t *facade,
                                         const dt_remote_exposure_t *exposure,
                                         dt_remote_editor_state_t *state)
{
  memset(state, 0, sizeof(*state));
  g_strlcpy(state->masks_json, "[]", sizeof(state->masks_json));
  float exposure_ev = 0.0f, black = 0.0f;
  dt_remote_exposure_values(exposure, exposure->baseline, &exposure_ev, &black);
  state->exposure.exposure_ev = exposure_ev;
  state->exposure.black_level = black;

  const uint8_t *crop = facade->crop.baseline;
  if(facade->crop.baseline_enabled)
  {
    state->geometry.x = _read_float(&facade->crop, "cx", crop);
    state->geometry.y = _read_float(&facade->crop, "cy", crop);
    state->geometry.width = _read_float(&facade->crop, "cw", crop) - state->geometry.x;
    state->geometry.height = _read_float(&facade->crop, "ch", crop) - state->geometry.y;
  }
  else
  {
    state->geometry.width = 1.0;
    state->geometry.height = 1.0;
  }
  state->geometry.straighten_degrees = facade->straighten.baseline_enabled
                                            ? _read_float(&facade->straighten, "rotation",
                                                          facade->straighten.baseline)
                                            : 0.0;

  float coefficients[4] = {
    _read_float(&facade->temperature, "red", facade->temperature.baseline),
    _read_float(&facade->temperature, "green", facade->temperature.baseline),
    _read_float(&facade->temperature, "blue", facade->temperature.baseline),
    _read_float(&facade->temperature, "various", facade->temperature.baseline)
  };
  if(!_coefficients_to_temperature(facade, coefficients,
                                   &state->white_balance.temperature_kelvin,
                                   &state->white_balance.tint))
  {
    state->white_balance.temperature_kelvin = 6500.0;
    state->white_balance.tint = 0.0;
  }

  if(facade->color_balance.baseline_enabled)
  {
    state->light.contrast =
        100.0 * _read_float(&facade->color_balance, "contrast", facade->color_balance.baseline);
    state->color.vibrance =
        100.0 * _read_float(&facade->color_balance, "vibrance", facade->color_balance.baseline);
    state->color.saturation = 100.0 * _read_float(
        &facade->color_balance, "saturation_global", facade->color_balance.baseline);
  }
  if(facade->tone_equalizer.baseline_enabled)
  {
    state->light.blacks = 50.0 * _read_float(
        &facade->tone_equalizer, "ultra_deep_blacks", facade->tone_equalizer.baseline);
    state->light.shadows = 50.0 * _read_float(
        &facade->tone_equalizer, "deep_blacks", facade->tone_equalizer.baseline);
    state->light.highlights = 50.0 * _read_float(
        &facade->tone_equalizer, "highlights", facade->tone_equalizer.baseline);
    state->light.whites =
        50.0 * _read_float(&facade->tone_equalizer, "whites", facade->tone_equalizer.baseline);
  }

  state->tone_curve.point_count = 2;
  state->tone_curve.points[0] = (dt_remote_curve_point_t){ 0.0, 0.0 };
  state->tone_curve.points[1] = (dt_remote_curve_point_t){ 1.0, 1.0 };
  if(facade->rgb_curve.baseline_enabled)
  {
    const int *counts = _const_field_pointer(&facade->rgb_curve, "curve_num_nodes",
                                             facade->rgb_curve.baseline);
    const dt_remote_rgbcurve_point_t(*points)[DT_REMOTE_TONE_CURVE_MAX_POINTS] =
        _const_field_pointer(&facade->rgb_curve, "curve_nodes", facade->rgb_curve.baseline);
    if(counts[0] >= DT_REMOTE_TONE_CURVE_MIN_POINTS &&
       counts[0] <= DT_REMOTE_TONE_CURVE_MAX_POINTS)
    {
      state->tone_curve.point_count = counts[0];
      for(size_t index = 0; index < state->tone_curve.point_count; index++)
      {
        state->tone_curve.points[index].x = points[0][index].x;
        state->tone_curve.points[index].y = points[0][index].y;
      }
    }
  }

  state->optics.lens_correction = facade->lens.baseline_enabled
                                      ? DT_REMOTE_LENS_CORRECTION_AUTOMATIC
                                      : DT_REMOTE_LENS_CORRECTION_OFF;
  g_strlcpy(state->optics.lens_profile,
            _const_field_pointer(&facade->lens, "lens", facade->lens.baseline),
            sizeof(state->optics.lens_profile));
  state->optics.lens_profile_status = !facade->lens.baseline_enabled
                                          ? DT_REMOTE_LENS_PROFILE_NOT_REQUESTED
                                          : state->optics.lens_profile[0]
                                                ? DT_REMOTE_LENS_PROFILE_APPLIED
                                                : DT_REMOTE_LENS_PROFILE_MISSING;
  state->optics.chromatic_aberration_correction =
      facade->chromatic_aberration.baseline_enabled;
  if(facade->defringe.baseline_enabled)
    state->optics.defringe = CLAMP((128.0 - _read_float(&facade->defringe, "thresh",
                                                        facade->defringe.baseline)) /
                                       1.275,
                                   0.0, 100.0);
  if(facade->sharpen.baseline_enabled)
    state->detail.sharpening = CLAMP(50.0 * _read_float(&facade->sharpen, "amount",
                                                        facade->sharpen.baseline),
                                     0.0, 100.0);
  if(facade->denoise.baseline_enabled)
    state->detail.noise_reduction =
        CLAMP((_read_float(&facade->denoise, "strength", facade->denoise.baseline) - 0.25) /
                  1.75 * 100.0,
              0.0, 100.0);
  state->detail.denoise_preset = _denoise_preset_for_amount(state->detail.noise_reduction);
  state->detail.highlight_reconstruction = _highlight_product_mode(
      _read_int(&facade->highlight_reconstruction, "mode",
                facade->highlight_reconstruction.baseline));
}

gboolean dt_remote_editor_state_facade_init(dt_remote_editor_state_facade_t *facade,
                                            dt_develop_t *dev,
                                            const dt_remote_exposure_t *exposure,
                                            char **error)
{
  if(error)
    *error = NULL;
  memset(facade, 0, sizeof(*facade));
  if(!_module_init(&facade->crop, dev, "crop", 3, error) ||
     !_module_init(&facade->orientation, dev, "flip", 2, error) ||
     !_module_init(&facade->straighten, dev, "ashift", 5, error) ||
     !_module_init(&facade->temperature, dev, "temperature", 4, error) ||
     !_module_init(&facade->color_balance, dev, "colorbalancergb", 5, error) ||
     !_module_init(&facade->tone_equalizer, dev, "toneequal", 2, error) ||
     !_module_init(&facade->rgb_curve, dev, "rgbcurve", 1, error) ||
     !_module_init(&facade->lens, dev, "lens", 10, error) ||
     !_module_init(&facade->chromatic_aberration, dev, "cacorrect", 2, error) ||
     !_module_init(&facade->defringe, dev, "defringe", 1, error) ||
     !_module_init(&facade->sharpen, dev, "sharpen", 1, error) ||
     !_module_init(&facade->denoise, dev, "denoiseprofile", 12, error) ||
     !_module_init(&facade->highlight_reconstruction, dev, "highlights", 4, error) ||
     !_verify_layout(facade, error) || !_prepare_temperature_matrices(facade, dev, error))
  {
    dt_remote_editor_state_facade_cleanup(facade);
    return FALSE;
  }
  facade->base_orientation = _read_int(&facade->orientation, "orientation",
                                       facade->orientation.baseline);
  if(facade->base_orientation == ORIENTATION_NULL)
    facade->base_orientation = dt_image_orientation(&dev->image_storage);
  _state_from_baseline_modules(facade, exposure, &facade->baseline);
  facade->current = facade->baseline;
  facade->initialized = TRUE;
  return TRUE;
}

void dt_remote_editor_state_facade_cleanup(dt_remote_editor_state_facade_t *facade)
{
  if(!facade)
    return;
  dt_remote_masks_cleanup(facade);
  _module_cleanup(&facade->crop);
  _module_cleanup(&facade->orientation);
  _module_cleanup(&facade->straighten);
  _module_cleanup(&facade->temperature);
  _module_cleanup(&facade->color_balance);
  _module_cleanup(&facade->tone_equalizer);
  _module_cleanup(&facade->rgb_curve);
  _module_cleanup(&facade->lens);
  _module_cleanup(&facade->chromatic_aberration);
  _module_cleanup(&facade->defringe);
  _module_cleanup(&facade->sharpen);
  _module_cleanup(&facade->denoise);
  _module_cleanup(&facade->highlight_reconstruction);
  memset(facade, 0, sizeof(*facade));
}

gboolean dt_remote_editor_state_apply(dt_remote_editor_state_facade_t *facade,
                                      dt_remote_exposure_t *exposure,
                                      dt_develop_t *dev,
                                      const dt_remote_editor_state_t *state,
                                      char **error)
{
  if(error)
    *error = NULL;
  if(!facade->initialized || !_validate_state(state, error))
    return FALSE;
  // Validate every mask before installing exposure or module parameters so a
  // malformed private-worker request cannot partially mutate the edit stack.
  if(!dt_remote_masks_validate(state->masks_json, error))
    return FALSE;

  uint8_t *crop = _duplicate_params(facade->crop.baseline, facade->crop.params_size);
  uint8_t *orientation =
      _duplicate_params(facade->orientation.baseline, facade->orientation.params_size);
  uint8_t *straighten =
      _duplicate_params(facade->straighten.baseline, facade->straighten.params_size);
  uint8_t *temperature =
      _duplicate_params(facade->temperature.baseline, facade->temperature.params_size);
  uint8_t *color =
      _duplicate_params(facade->color_balance.baseline, facade->color_balance.params_size);
  uint8_t *tone =
      _duplicate_params(facade->tone_equalizer.baseline, facade->tone_equalizer.params_size);
  uint8_t *curve = _duplicate_params(facade->rgb_curve.baseline, facade->rgb_curve.params_size);
  uint8_t *lens = _duplicate_params(facade->lens.baseline, facade->lens.params_size);
  uint8_t *chromatic_aberration = _duplicate_params(
      facade->chromatic_aberration.baseline, facade->chromatic_aberration.params_size);
  uint8_t *defringe = _duplicate_params(facade->defringe.baseline, facade->defringe.params_size);
  uint8_t *sharpen = _duplicate_params(facade->sharpen.baseline, facade->sharpen.params_size);
  uint8_t *denoise = _duplicate_params(facade->denoise.baseline, facade->denoise.params_size);
  uint8_t *highlight_params = _duplicate_params(facade->highlight_reconstruction.baseline,
                                                facade->highlight_reconstruction.params_size);
  if(!crop || !orientation || !straighten || !temperature || !color || !tone || !curve ||
     !lens || !chromatic_aberration || !defringe || !sharpen || !denoise ||
     !highlight_params)
  {
    _set_error(error, "could not allocate canonical editor-state candidates");
    goto failed;
  }

  _write_float(&facade->crop, "cx", crop, state->geometry.x);
  _write_float(&facade->crop, "cy", crop, state->geometry.y);
  _write_float(&facade->crop, "cw", crop, state->geometry.x + state->geometry.width);
  _write_float(&facade->crop, "ch", crop, state->geometry.y + state->geometry.height);
  _write_int(&facade->crop, "ratio_n", crop, 0);
  _write_int(&facade->crop, "ratio_d", crop, 0);

  const int product_orientation =
      _product_orientation(state->geometry.rotation_quarter_turns,
                           state->geometry.flip_horizontal, state->geometry.flip_vertical);
  _write_int(&facade->orientation, "orientation", orientation,
             _merge_orientations(facade->base_orientation, product_orientation));
  _write_float(&facade->straighten, "rotation", straighten,
               state->geometry.straighten_degrees);
  _write_int(&facade->straighten, "cropmode", straighten, 1);

  float coefficients[4];
  if(!_temperature_to_coefficients(facade, state->white_balance.temperature_kelvin,
                                   state->white_balance.tint, coefficients))
  {
    _set_error(error, "white balance cannot be represented by temperature v4");
    goto failed;
  }
  memcpy(_field_pointer(&facade->temperature, "red", temperature), &coefficients[0],
         sizeof(float));
  memcpy(_field_pointer(&facade->temperature, "green", temperature), &coefficients[1],
         sizeof(float));
  memcpy(_field_pointer(&facade->temperature, "blue", temperature), &coefficients[2],
         sizeof(float));
  memcpy(_field_pointer(&facade->temperature, "various", temperature), &coefficients[3],
         sizeof(float));
  _write_int(&facade->temperature, "preset", temperature, 2);

  _write_float(&facade->color_balance, "contrast", color, state->light.contrast / 100.0);
  _write_float(&facade->color_balance, "vibrance", color, state->color.vibrance / 100.0);
  _write_float(&facade->color_balance, "saturation_global", color,
               state->color.saturation / 100.0);

  const double blacks = state->light.blacks / 50.0;
  const double shadows = state->light.shadows / 50.0;
  const double highlights = state->light.highlights / 50.0;
  const double whites = state->light.whites / 50.0;
  _write_float(&facade->tone_equalizer, "noise", tone, blacks);
  _write_float(&facade->tone_equalizer, "ultra_deep_blacks", tone, blacks);
  _write_float(&facade->tone_equalizer, "deep_blacks", tone, shadows);
  _write_float(&facade->tone_equalizer, "blacks", tone, shadows);
  _write_float(&facade->tone_equalizer, "shadows", tone, shadows);
  _write_float(&facade->tone_equalizer, "midtones", tone, 0.0);
  _write_float(&facade->tone_equalizer, "highlights", tone, highlights);
  _write_float(&facade->tone_equalizer, "whites", tone, whites);
  _write_float(&facade->tone_equalizer, "speculars", tone, whites);

  dt_remote_rgbcurve_point_t(*curve_points)[DT_REMOTE_TONE_CURVE_MAX_POINTS] =
      _field_pointer(&facade->rgb_curve, "curve_nodes", curve);
  int *curve_counts = _field_pointer(&facade->rgb_curve, "curve_num_nodes", curve);
  int *curve_types = _field_pointer(&facade->rgb_curve, "curve_type", curve);
  memset(curve_points, 0,
         3 * DT_REMOTE_TONE_CURVE_MAX_POINTS * sizeof(dt_remote_rgbcurve_point_t));
  for(size_t index = 0; index < state->tone_curve.point_count; index++)
  {
    curve_points[0][index].x = (float)state->tone_curve.points[index].x;
    curve_points[0][index].y = (float)state->tone_curve.points[index].y;
  }
  curve_counts[0] = (int)state->tone_curve.point_count;
  curve_types[0] = MONOTONE_HERMITE;
  _write_int(&facade->rgb_curve, "curve_autoscale", curve, 0);

  _write_int(&facade->lens, "method", lens, DT_REMOTE_LENS_METHOD_LENSFUN);
  _write_int(&facade->lens, "modify_flags", lens, DT_REMOTE_LENS_MODIFY_ALL);
  if(state->optics.lens_correction == DT_REMOTE_LENS_CORRECTION_MANUAL)
    _write_string(&facade->lens, "lens", lens, state->optics.lens_profile, 128);
  _write_float(&facade->defringe, "thresh", defringe,
               128.0 - state->optics.defringe * 1.275);
  _write_int(&facade->defringe, "op_mode", defringe, 0);
  _write_float(&facade->sharpen, "amount", sharpen, state->detail.sharpening / 50.0);
  _write_float(&facade->denoise, "strength", denoise,
               0.25 + state->detail.noise_reduction / 100.0 * 1.75);
  _write_int(&facade->denoise, "mode", denoise, DT_REMOTE_DENOISE_MODE_WAVELETS);
  _write_int(&facade->highlight_reconstruction, "mode", highlight_params,
             _highlight_engine_mode(state->detail.highlight_reconstruction));

  if(!dt_remote_exposure_apply_exact(exposure, dev, state->exposure.exposure_ev,
                                     state->exposure.black_level, error))
    goto failed;
  const gboolean crop_enabled = state->geometry.x != 0.0 || state->geometry.y != 0.0 ||
                                state->geometry.width != 1.0 ||
                                state->geometry.height != 1.0;
  const gboolean straighten_enabled = state->geometry.straighten_degrees != 0.0;
  const gboolean color_enabled = state->light.contrast != 0.0 ||
                                 state->color.vibrance != 0.0 ||
                                 state->color.saturation != 0.0;
  const gboolean tone_enabled = state->light.highlights != 0.0 ||
                                state->light.shadows != 0.0 || state->light.whites != 0.0 ||
                                state->light.blacks != 0.0;
  const char *lens_profile = _const_field_pointer(&facade->lens, "lens", lens);
  const gboolean lens_requested =
      state->optics.lens_correction != DT_REMOTE_LENS_CORRECTION_OFF;
  const gboolean lens_available = lens_profile[0] != '\0';
  _install_module(&facade->orientation, dev, orientation, TRUE);
  _install_module(&facade->straighten, dev, straighten, straighten_enabled);
  _install_module(&facade->crop, dev, crop, crop_enabled);
  _install_module(&facade->temperature, dev, temperature, TRUE);
  _install_module(&facade->color_balance, dev, color, color_enabled);
  _install_module(&facade->tone_equalizer, dev, tone, tone_enabled);
  _install_module(&facade->rgb_curve, dev, curve, !_is_identity_curve(state));
  _install_module(&facade->lens, dev, lens, lens_requested && lens_available);
  _install_module(&facade->chromatic_aberration, dev, chromatic_aberration,
                  state->optics.chromatic_aberration_correction);
  _install_module(&facade->defringe, dev, defringe, state->optics.defringe > 0.0);
  _install_module(&facade->sharpen, dev, sharpen, state->detail.sharpening > 0.0);
  _install_module(&facade->denoise, dev, denoise, state->detail.noise_reduction > 0.0);
  _install_module(&facade->highlight_reconstruction, dev, highlight_params,
                  facade->highlight_reconstruction.baseline_enabled);

  // darktable's interactive mask tools store points only after reversing the
  // active pixelpipe geometry. Synchronize the full-resolution pipe used by
  // remote rendering so the mask adapter performs that same back-transform.
  dt_dev_pixelpipe_change(dev->full.pipe, dev);
  if(!dt_remote_masks_apply(facade, dev, exposure->module,
                            state->geometry.x, state->geometry.y,
                            state->geometry.width, state->geometry.height,
                            state->masks_json, error))
    goto failed;

  facade->current = *state;
  facade->current.exposure.exposure_ev = (float)state->exposure.exposure_ev;
  facade->current.exposure.black_level = (float)state->exposure.black_level;
  facade->current.geometry.x = _read_float(&facade->crop, "cx", facade->crop.accepted);
  facade->current.geometry.y = _read_float(&facade->crop, "cy", facade->crop.accepted);
  facade->current.geometry.width =
      _read_float(&facade->crop, "cw", facade->crop.accepted) - facade->current.geometry.x;
  facade->current.geometry.height =
      _read_float(&facade->crop, "ch", facade->crop.accepted) - facade->current.geometry.y;
  facade->current.geometry.straighten_degrees =
      _read_float(&facade->straighten, "rotation", facade->straighten.accepted);
  facade->current.light.contrast =
      100.0 * _read_float(&facade->color_balance, "contrast", facade->color_balance.accepted);
  facade->current.color.vibrance =
      100.0 * _read_float(&facade->color_balance, "vibrance", facade->color_balance.accepted);
  facade->current.color.saturation = 100.0 * _read_float(
      &facade->color_balance, "saturation_global", facade->color_balance.accepted);
  facade->current.light.blacks = 50.0 * _read_float(
      &facade->tone_equalizer, "ultra_deep_blacks", facade->tone_equalizer.accepted);
  facade->current.light.shadows = 50.0 * _read_float(
      &facade->tone_equalizer, "deep_blacks", facade->tone_equalizer.accepted);
  facade->current.light.highlights = 50.0 * _read_float(
      &facade->tone_equalizer, "highlights", facade->tone_equalizer.accepted);
  facade->current.light.whites =
      50.0 * _read_float(&facade->tone_equalizer, "whites", facade->tone_equalizer.accepted);
  // Kelvin/tint are the canonical controls; temperature v4 stores only
  // camera-specific multipliers and has no lossless inverse. Preserve the
  // float-normalized canonical values used to derive those coefficients.
  facade->current.white_balance.temperature_kelvin =
      (float)state->white_balance.temperature_kelvin;
  facade->current.white_balance.tint = (float)state->white_balance.tint;
  for(size_t index = 0; index < facade->current.tone_curve.point_count; index++)
  {
    facade->current.tone_curve.points[index].x = curve_points[0][index].x;
    facade->current.tone_curve.points[index].y = curve_points[0][index].y;
  }
  facade->current.optics.lens_correction = state->optics.lens_correction;
  g_strlcpy(facade->current.optics.lens_profile, lens_profile,
            sizeof(facade->current.optics.lens_profile));
  facade->current.optics.lens_profile_status =
      !lens_requested ? DT_REMOTE_LENS_PROFILE_NOT_REQUESTED
                      : lens_available ? DT_REMOTE_LENS_PROFILE_APPLIED
                                       : DT_REMOTE_LENS_PROFILE_MISSING;
  facade->current.optics.defringe =
      (float)CLAMP(state->optics.defringe, 0.0, 100.0);
  facade->current.detail.sharpening =
      50.0 * _read_float(&facade->sharpen, "amount", facade->sharpen.accepted);
  facade->current.detail.noise_reduction = (float)state->detail.noise_reduction;
  facade->current.detail.denoise_preset =
      _denoise_preset_for_amount(facade->current.detail.noise_reduction);
  facade->current.detail.highlight_reconstruction = _highlight_product_mode(
      _read_int(&facade->highlight_reconstruction, "mode",
                facade->highlight_reconstruction.accepted));

  g_free(crop);
  g_free(orientation);
  g_free(straighten);
  g_free(temperature);
  g_free(color);
  g_free(tone);
  g_free(curve);
  g_free(lens);
  g_free(chromatic_aberration);
  g_free(defringe);
  g_free(sharpen);
  g_free(denoise);
  g_free(highlight_params);
  return TRUE;

failed:
  g_free(crop);
  g_free(orientation);
  g_free(straighten);
  g_free(temperature);
  g_free(color);
  g_free(tone);
  g_free(curve);
  g_free(lens);
  g_free(chromatic_aberration);
  g_free(defringe);
  g_free(sharpen);
  g_free(denoise);
  g_free(highlight_params);
  return FALSE;
}

void dt_remote_editor_state_sync_exposure(dt_remote_editor_state_facade_t *facade,
                                          const dt_remote_exposure_t *exposure)
{
  float exposure_ev = 0.0f, black = 0.0f;
  dt_remote_exposure_values(exposure, exposure->accepted, &exposure_ev, &black);
  facade->current.exposure.exposure_ev = exposure_ev;
  facade->current.exposure.black_level = black;
}

static void _checksum_uint32(GChecksum *checksum, uint32_t value)
{
  const uint32_t big_endian = GUINT32_TO_BE(value);
  g_checksum_update(checksum, (const guchar *)&big_endian, sizeof(big_endian));
}

static void _checksum_double(GChecksum *checksum, double value)
{
  const float normalized = (float)value;
  uint32_t bits = 0;
  memcpy(&bits, &normalized, sizeof(bits));
  _checksum_uint32(checksum, bits);
}

static void _checksum_module(GChecksum *checksum, const dt_remote_module_state_t *state)
{
  const char separator = '\0';
  g_checksum_update(checksum, (const guchar *)state->module->op, strlen(state->module->op));
  g_checksum_update(checksum, (const guchar *)&separator, 1);
  _checksum_uint32(checksum, (uint32_t)state->module_version);
  _checksum_uint32(checksum, state->accepted_enabled ? 1u : 0u);
  g_checksum_update(checksum, state->accepted, state->params_size);
}

void dt_remote_editor_state_checksum_update(const dt_remote_editor_state_facade_t *facade,
                                            GChecksum *checksum)
{
  _checksum_uint32(checksum, DT_REMOTE_EDITOR_STATE_SCHEMA);
  _checksum_double(checksum, facade->current.exposure.exposure_ev);
  _checksum_double(checksum, facade->current.exposure.black_level);
  _checksum_double(checksum, facade->current.geometry.x);
  _checksum_double(checksum, facade->current.geometry.y);
  _checksum_double(checksum, facade->current.geometry.width);
  _checksum_double(checksum, facade->current.geometry.height);
  _checksum_uint32(checksum, (uint32_t)facade->current.geometry.rotation_quarter_turns);
  _checksum_double(checksum, facade->current.geometry.straighten_degrees);
  _checksum_uint32(checksum, facade->current.geometry.flip_horizontal ? 1u : 0u);
  _checksum_uint32(checksum, facade->current.geometry.flip_vertical ? 1u : 0u);
  _checksum_double(checksum, facade->current.white_balance.temperature_kelvin);
  _checksum_double(checksum, facade->current.white_balance.tint);
  _checksum_double(checksum, facade->current.light.contrast);
  _checksum_double(checksum, facade->current.light.highlights);
  _checksum_double(checksum, facade->current.light.shadows);
  _checksum_double(checksum, facade->current.light.whites);
  _checksum_double(checksum, facade->current.light.blacks);
  _checksum_double(checksum, facade->current.color.vibrance);
  _checksum_double(checksum, facade->current.color.saturation);
  _checksum_uint32(checksum, (uint32_t)facade->current.tone_curve.point_count);
  for(size_t index = 0; index < facade->current.tone_curve.point_count; index++)
  {
    _checksum_double(checksum, facade->current.tone_curve.points[index].x);
    _checksum_double(checksum, facade->current.tone_curve.points[index].y);
  }
  _checksum_uint32(checksum, (uint32_t)facade->current.optics.lens_correction);
  g_checksum_update(checksum, (const guchar *)facade->current.optics.lens_profile,
                    strlen(facade->current.optics.lens_profile) + 1);
  _checksum_uint32(checksum, (uint32_t)facade->current.optics.lens_profile_status);
  _checksum_uint32(checksum,
                   facade->current.optics.chromatic_aberration_correction ? 1u : 0u);
  _checksum_double(checksum, facade->current.optics.defringe);
  _checksum_double(checksum, facade->current.detail.sharpening);
  _checksum_double(checksum, facade->current.detail.noise_reduction);
  _checksum_uint32(checksum, (uint32_t)facade->current.detail.denoise_preset);
  _checksum_uint32(checksum,
                   (uint32_t)facade->current.detail.highlight_reconstruction);
  g_checksum_update(checksum, (const guchar *)facade->current.masks_json,
                    strlen(facade->current.masks_json) + 1);
  _checksum_module(checksum, &facade->crop);
  _checksum_module(checksum, &facade->orientation);
  _checksum_module(checksum, &facade->straighten);
  _checksum_module(checksum, &facade->temperature);
  _checksum_module(checksum, &facade->color_balance);
  _checksum_module(checksum, &facade->tone_equalizer);
  _checksum_module(checksum, &facade->rgb_curve);
  _checksum_module(checksum, &facade->lens);
  _checksum_module(checksum, &facade->chromatic_aberration);
  _checksum_module(checksum, &facade->defringe);
  _checksum_module(checksum, &facade->sharpen);
  _checksum_module(checksum, &facade->denoise);
  _checksum_module(checksum, &facade->highlight_reconstruction);
}

gboolean dt_remote_editor_state_run_self_tests(char **error)
{
  if(error)
    *error = NULL;
  if(!dt_remote_masks_run_self_tests(error)) return FALSE;
  dt_remote_editor_state_t state = {
    .geometry = {
      .width = 1.0,
      .height = 1.0,
    },
    .white_balance = {
      .temperature_kelvin = 6500.0,
    },
    .tone_curve = {
      .point_count = 2,
      .points = { { 0.0, 0.0 }, { 1.0, 1.0 } },
    },
    .detail = {
      .highlight_reconstruction = DT_REMOTE_HIGHLIGHT_INPAINT_OPPOSED,
    },
  };
  if(!_validate_state(&state, error))
    return FALSE;
  state.geometry.width = DT_REMOTE_MIN_CROP_SIZE - 0.001;
  char *expected_error = NULL;
  if(_validate_state(&state, &expected_error))
  {
    _set_error(error, "minimum crop validation self-test failed");
    return FALSE;
  }
  g_free(expected_error);

  state.optics.lens_correction = DT_REMOTE_LENS_CORRECTION_MANUAL;
  expected_error = NULL;
  if(_validate_state(&state, &expected_error))
  {
    _set_error(error, "manual lens profile validation self-test failed");
    return FALSE;
  }
  g_free(expected_error);
  state.optics.lens_correction = DT_REMOTE_LENS_CORRECTION_OFF;
  state.detail.noise_reduction = 50.0;
  state.detail.denoise_preset = DT_REMOTE_DENOISE_MEDIUM;
  if(_denoise_preset_for_amount(state.detail.noise_reduction) != DT_REMOTE_DENOISE_MEDIUM ||
     _highlight_engine_mode(DT_REMOTE_HIGHLIGHT_GUIDED_LAPLACIANS) !=
         DT_REMOTE_HIGHLIGHTS_LAPLACIAN)
  {
    _set_error(error, "Phase 21 preset mapping self-test failed");
    return FALSE;
  }
  state.geometry.width = 1.0;
  state.tone_curve.point_count = 4;
  state.tone_curve.points[1] = (dt_remote_curve_point_t){ 0.8, 0.5 };
  state.tone_curve.points[2] = (dt_remote_curve_point_t){ 0.7, 0.7 };
  state.tone_curve.points[3] = (dt_remote_curve_point_t){ 1.0, 1.0 };
  expected_error = NULL;
  if(_validate_state(&state, &expected_error))
  {
    _set_error(error, "tone-curve ordering self-test failed");
    return FALSE;
  }
  g_free(expected_error);

  if(fabs(_product_tint_to_engine(-100.0) - DT_REMOTE_ENGINE_TINT_MIN) > 1e-12 ||
     fabs(_product_tint_to_engine(0.0) - 1.0) > 1e-12 ||
     fabs(_product_tint_to_engine(100.0) - DT_REMOTE_ENGINE_TINT_MAX) > 1e-12 ||
     fabs(_engine_tint_to_product(DT_REMOTE_ENGINE_TINT_MIN) + 100.0) > 1e-9 ||
     fabs(_engine_tint_to_product(DT_REMOTE_ENGINE_TINT_MAX) - 100.0) > 1e-9)
  {
    _set_error(error, "white-balance tint mapping self-test failed");
    return FALSE;
  }
  if(_product_orientation(1, FALSE, FALSE) != ORIENTATION_ROTATE_CW_90_DEG ||
     _product_orientation(1, TRUE, FALSE) != ORIENTATION_TRANSPOSE ||
     _product_orientation(1, FALSE, TRUE) != ORIENTATION_TRANSVERSE ||
     _product_orientation(1, TRUE, TRUE) != ORIENTATION_ROTATE_CCW_90_DEG)
  {
    _set_error(error, "orientation composition self-test failed");
    return FALSE;
  }
  double x = 0.0, y = 0.0;
  _temperature_xy(6500.0, &x, &y);
  if(fabs(x - 0.3128) > 0.002 || fabs(y - 0.3292) > 0.002)
  {
    _set_error(error, "Kelvin-to-CIE-xy self-test failed");
    return FALSE;
  }
  return TRUE;
}
