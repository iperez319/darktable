/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include "remote/remote_exposure.h"

#include "common/introspection.h"
#include "develop/imageop.h"

#include <math.h>
#include <stdarg.h>
#include <string.h>

static void _set_error(char **error, const char *format, ...)
{
  if(!error)
    return;
  va_list ap;
  va_start(ap, format);
  *error = g_strdup_vprintf(format, ap);
  va_end(ap);
}

static gboolean _field_matches(const dt_introspection_field_t *field, dt_introspection_type_t type,
                               size_t size, size_t params_size)
{
  return field && field->header.type == type && field->header.size == size &&
         field->header.offset <= params_size && size <= params_size - field->header.offset;
}

static void *_field_pointer(const dt_introspection_field_t *field, uint8_t *params)
{
  return params + field->header.offset;
}

static uint8_t *_duplicate_params(const void *params, size_t size)
{
  uint8_t *copy = g_try_malloc(size);
  if(copy)
    memcpy(copy, params, size);
  return copy;
}

void dt_remote_exposure_values(const dt_remote_exposure_t *exposure, const uint8_t *params,
                               float *exposure_ev, float *black)
{
  if(exposure_ev)
    memcpy(exposure_ev, params + exposure->exposure_field->header.offset, sizeof(float));
  if(black)
    memcpy(black, params + exposure->black_field->header.offset, sizeof(float));
}

static void _install(dt_remote_exposure_t *exposure, dt_develop_t *dev, const uint8_t *params)
{
  dt_pthread_mutex_lock(&dev->history_mutex);
  memcpy(exposure->module->params, params, exposure->params_size);
  exposure->module->enabled = TRUE;
  // This updates only the in-memory development history. Passing FALSE for
  // no_image is required so the persistent pipes receive TOP_CHANGED/SYNCH;
  // database/XMP persistence is a separate dt_dev_write_history_ext call that
  // the worker deliberately never makes.
  dt_dev_add_history_item_ext(dev, exposure->module, TRUE, FALSE);
  dt_dev_invalidate_all(dev);
  dt_pthread_mutex_unlock(&dev->history_mutex);
}

gboolean dt_remote_exposure_init(dt_remote_exposure_t *exposure, dt_develop_t *dev, char **error)
{
  if(error)
    *error = NULL;
  memset(exposure, 0, sizeof(*exposure));
  exposure->module = dt_iop_get_module_by_op_priority(dev->iop, "exposure", 0);
  if(!exposure->module)
  {
    _set_error(error, "exposure module priority 0 is unavailable");
    return FALSE;
  }
  dt_iop_module_so_t *so = exposure->module->so;
  if(!so || !so->have_introspection || !so->get_introspection || !so->get_f)
  {
    _set_error(error, "exposure module has no usable introspection");
    return FALSE;
  }
  dt_introspection_t *intro = so->get_introspection();
  exposure->module_version = intro->params_version;
  exposure->params_size = intro->size;
  exposure->mode_field = so->get_f("mode");
  exposure->black_field = so->get_f("black");
  exposure->exposure_field = so->get_f("exposure");
  if(exposure->params_size != (size_t)exposure->module->params_size ||
     !_field_matches(exposure->mode_field, DT_INTROSPECTION_TYPE_ENUM, sizeof(int),
                     exposure->params_size) ||
     !_field_matches(exposure->black_field, DT_INTROSPECTION_TYPE_FLOAT, sizeof(float),
                     exposure->params_size) ||
     !_field_matches(exposure->exposure_field, DT_INTROSPECTION_TYPE_FLOAT, sizeof(float),
                     exposure->params_size))
  {
    _set_error(error, "exposure introspection layout is incompatible (version %d, size %zu)",
               intro->params_version, intro->size);
    return FALSE;
  }
  if(exposure->black_field->Float.Min != -1.0f || exposure->black_field->Float.Max != 1.0f ||
     exposure->exposure_field->Float.Min != -18.0f || exposure->exposure_field->Float.Max != 18.0f)
  {
    _set_error(error, "exposure introspection ranges are incompatible");
    return FALSE;
  }
  gboolean found_manual = FALSE;
  for(dt_introspection_type_enum_tuple_t *value = exposure->mode_field->Enum.values;
      value && value->name; value++)
  {
    if(!strcmp(value->name, "EXPOSURE_MODE_MANUAL"))
    {
      exposure->manual_mode = value->value;
      found_manual = TRUE;
      break;
    }
  }
  if(!found_manual)
  {
    _set_error(error, "exposure introspection does not advertise manual mode");
    return FALSE;
  }

  exposure->baseline = _duplicate_params(exposure->module->params, exposure->params_size);
  exposure->accepted = _duplicate_params(exposure->module->params, exposure->params_size);
  if(!exposure->baseline || !exposure->accepted)
  {
    _set_error(error, "could not allocate exposure parameter state");
    dt_remote_exposure_cleanup(exposure);
    return FALSE;
  }
  memcpy(_field_pointer(exposure->mode_field, exposure->baseline), &exposure->manual_mode,
         sizeof(exposure->manual_mode));
  memcpy(_field_pointer(exposure->mode_field, exposure->accepted), &exposure->manual_mode,
         sizeof(exposure->manual_mode));
  _install(exposure, dev, exposure->baseline);
  return TRUE;
}

void dt_remote_exposure_cleanup(dt_remote_exposure_t *exposure)
{
  if(!exposure)
    return;
  g_free(exposure->baseline);
  g_free(exposure->accepted);
  memset(exposure, 0, sizeof(*exposure));
}

gboolean dt_remote_exposure_apply(dt_remote_exposure_t *exposure, dt_develop_t *dev,
                                  double exposure_ev, double black,
                                  dt_remote_changed_field_t changed, float *accepted_exposure,
                                  float *accepted_black, char **error)
{
  if(error)
    *error = NULL;
  if(!isfinite(exposure_ev) || !isfinite(black))
  {
    _set_error(error, "exposure and black must be finite");
    return FALSE;
  }
  if(exposure_ev < exposure->exposure_field->Float.Min ||
     exposure_ev > exposure->exposure_field->Float.Max ||
     black < exposure->black_field->Float.Min || black > exposure->black_field->Float.Max)
  {
    _set_error(error, "candidate exposure is outside the advertised range");
    return FALSE;
  }

  float normalized_exposure = (float)exposure_ev;
  float normalized_black = (float)black;
  float white = exp2f(-normalized_exposure);
  if(normalized_black >= white)
  {
    if(changed == DT_REMOTE_CHANGED_EXPOSURE)
      normalized_black = white - 0.01f;
    else
    {
      white = normalized_black + 0.01f;
      normalized_exposure = -log2f(white);
    }
  }
  if(!isfinite(normalized_exposure) || !isfinite(normalized_black) ||
     normalized_exposure < exposure->exposure_field->Float.Min ||
     normalized_exposure > exposure->exposure_field->Float.Max ||
     normalized_black < exposure->black_field->Float.Min ||
     normalized_black > exposure->black_field->Float.Max)
  {
    _set_error(error, "coupled exposure values are outside the advertised range");
    return FALSE;
  }

  uint8_t *candidate = _duplicate_params(exposure->accepted, exposure->params_size);
  if(!candidate)
  {
    _set_error(error, "could not allocate exposure candidate");
    return FALSE;
  }
  memcpy(_field_pointer(exposure->mode_field, candidate), &exposure->manual_mode,
         sizeof(exposure->manual_mode));
  memcpy(_field_pointer(exposure->exposure_field, candidate), &normalized_exposure,
         sizeof(normalized_exposure));
  memcpy(_field_pointer(exposure->black_field, candidate), &normalized_black,
         sizeof(normalized_black));
  _install(exposure, dev, candidate);
  memcpy(exposure->accepted, candidate, exposure->params_size);
  g_free(candidate);
  dt_remote_exposure_values(exposure, exposure->accepted, accepted_exposure, accepted_black);
  return TRUE;
}

gboolean dt_remote_exposure_apply_exact(dt_remote_exposure_t *exposure, dt_develop_t *dev,
                                        double exposure_ev, double black, char **error)
{
  if(error)
    *error = NULL;
  if(!isfinite(exposure_ev) || !isfinite(black) ||
     exposure_ev < exposure->exposure_field->Float.Min ||
     exposure_ev > exposure->exposure_field->Float.Max ||
     black < exposure->black_field->Float.Min || black > exposure->black_field->Float.Max)
  {
    _set_error(error, "candidate exposure is outside the advertised range");
    return FALSE;
  }

  uint8_t *candidate = _duplicate_params(exposure->accepted, exposure->params_size);
  if(!candidate)
  {
    _set_error(error, "could not allocate exposure candidate");
    return FALSE;
  }
  const float normalized_exposure = (float)exposure_ev;
  const float normalized_black = (float)black;
  memcpy(_field_pointer(exposure->mode_field, candidate), &exposure->manual_mode,
         sizeof(exposure->manual_mode));
  memcpy(_field_pointer(exposure->exposure_field, candidate), &normalized_exposure,
         sizeof(normalized_exposure));
  memcpy(_field_pointer(exposure->black_field, candidate), &normalized_black,
         sizeof(normalized_black));
  _install(exposure, dev, candidate);
  memcpy(exposure->accepted, candidate, exposure->params_size);
  g_free(candidate);
  return TRUE;
}

gboolean dt_remote_exposure_reset(dt_remote_exposure_t *exposure, dt_develop_t *dev,
                                  float *accepted_exposure, float *accepted_black, char **error)
{
  if(error)
    *error = NULL;
  if(!exposure->baseline || !exposure->accepted)
  {
    _set_error(error, "exposure session is not initialized");
    return FALSE;
  }
  _install(exposure, dev, exposure->baseline);
  memcpy(exposure->accepted, exposure->baseline, exposure->params_size);
  dt_remote_exposure_values(exposure, exposure->accepted, accepted_exposure, accepted_black);
  return TRUE;
}
