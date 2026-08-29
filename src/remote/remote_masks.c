/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include "remote/remote_masks.h"

#include "develop/blend.h"
#include "develop/develop.h"
#include "develop/masks.h"

#include <json-glib/json-glib.h>
#include <math.h>
#include <stdarg.h>
#include <string.h>

#define DT_REMOTE_MAX_MASKS 32
#define DT_REMOTE_MAX_MASK_COMPONENTS 64

static void _set_error(char **error, const char *format, ...)
{
  if(!error) return;
  va_list ap;
  va_start(ap, format);
  *error = g_strdup_vprintf(format, ap);
  va_end(ap);
}

static gboolean _double(JsonObject *object, const char *name, double *value)
{
  if(!object || !json_object_has_member(object, name)) return FALSE;
  *value = json_object_get_double_member(object, name);
  return isfinite(*value);
}

static gboolean _point(JsonObject *object, float point[2])
{
  double x = 0.0, y = 0.0;
  if(!_double(object, "x", &x) || !_double(object, "y", &y)
     || x < 0.0 || x > 1.0 || y < 0.0 || y > 1.0)
    return FALSE;
  point[0] = (float)x;
  point[1] = (float)y;
  return TRUE;
}

static void _free_forms(GList *forms)
{
  g_list_free_full(forms, (GDestroyNotify)dt_masks_free_form);
}

static dt_masks_form_t *_gradient_form(JsonObject *geometry)
{
  JsonObject *start = json_object_get_object_member(geometry, "start");
  JsonObject *end = json_object_get_object_member(geometry, "end");
  float p0[2], p1[2];
  double feather = 0.0;
  if(!_point(start, p0) || !_point(end, p1) || !_double(geometry, "feather", &feather))
    return NULL;
  const double dx = p1[0] - p0[0], dy = p1[1] - p0[1];
  const double length = hypot(dx, dy);
  if(length <= 0.000001 || feather < 0.0 || feather > 1.0) return NULL;
  dt_masks_form_t *form = dt_masks_create(DT_MASKS_GRADIENT);
  if(!form) return NULL;
  dt_masks_point_gradient_t *point = calloc(1, sizeof(*point));
  if(!point)
  {
    dt_masks_free_form(form);
    return NULL;
  }
  point->anchor[0] = p0[0];
  point->anchor[1] = p0[1];
  point->rotation = (float)(atan2(dy, dx) * 180.0 / M_PI - 90.0);
  point->compression = (float)MAX(0.001, length * MAX(0.001, feather));
  point->steepness = 0.0f;
  point->curvature = 0.0f;
  point->state = DT_MASKS_GRADIENT_STATE_LINEAR;
  form->points = g_list_append(form->points, point);
  return form;
}

static dt_masks_form_t *_ellipse_form(JsonObject *geometry)
{
  JsonObject *center = json_object_get_object_member(geometry, "center");
  float p[2];
  double rx = 0.0, ry = 0.0, rotation = 0.0, feather = 0.0;
  if(!_point(center, p) || !_double(geometry, "radiusX", &rx)
     || !_double(geometry, "radiusY", &ry)
     || !_double(geometry, "rotationDegrees", &rotation)
     || !_double(geometry, "feather", &feather)
     || rx < 0.001 || rx > 1.0 || ry < 0.001 || ry > 1.0
     || rotation < -180.0 || rotation > 180.0 || feather < 0.0 || feather > 1.0)
    return NULL;
  dt_masks_form_t *form = dt_masks_create(DT_MASKS_ELLIPSE);
  if(!form) return NULL;
  dt_masks_point_ellipse_t *point = calloc(1, sizeof(*point));
  if(!point)
  {
    dt_masks_free_form(form);
    return NULL;
  }
  point->center[0] = p[0];
  point->center[1] = p[1];
  point->radius[0] = (float)rx;
  point->radius[1] = (float)ry;
  point->rotation = (float)rotation;
  point->border = (float)(MAX(rx, ry) * feather);
  point->flags = DT_MASKS_ELLIPSE_EQUIDISTANT;
  form->points = g_list_append(form->points, point);
  return form;
}

static dt_masks_form_t *_brush_stroke_form(JsonObject *stroke)
{
  JsonArray *points = json_object_get_array_member(stroke, "points");
  if(!points || json_array_get_length(points) == 0) return NULL;
  dt_masks_form_t *form = dt_masks_create(DT_MASKS_BRUSH);
  if(!form) return NULL;
  for(guint index = 0; index < json_array_get_length(points); index++)
  {
    JsonObject *source = json_array_get_object_element(points, index);
    float p[2];
    double radius = 0.0, hardness = 0.0, opacity = 0.0;
    if(!_point(source, p) || !_double(source, "radius", &radius)
       || !_double(source, "hardness", &hardness)
       || !_double(source, "opacity", &opacity)
       || radius < 0.001 || radius > 0.5 || hardness < 0.0 || hardness > 1.0
       || opacity < 0.0 || opacity > 1.0)
    {
      dt_masks_free_form(form);
      return NULL;
    }
    dt_masks_point_brush_t *point = calloc(1, sizeof(*point));
    if(!point)
    {
      dt_masks_free_form(form);
      return NULL;
    }
    point->corner[0] = point->ctrl1[0] = point->ctrl2[0] = p[0];
    point->corner[1] = point->ctrl1[1] = point->ctrl2[1] = p[1];
    point->border[0] = point->border[1] = (float)radius;
    point->hardness = (float)hardness;
    point->density = (float)opacity;
    point->state = DT_MASKS_POINT_STATE_USER;
    form->points = g_list_append(form->points, point);
  }
  return form;
}

static int _group_state(const char *operation, gboolean inverted, gboolean first,
                        gboolean enabled)
{
  int state = enabled ? DT_MASKS_STATE_USE : DT_MASKS_STATE_NONE;
  if(inverted) state |= DT_MASKS_STATE_INVERSE;
  if(first) return state;
  if(!g_strcmp0(operation, "subtract")) state |= DT_MASKS_STATE_DIFFERENCE;
  else if(!g_strcmp0(operation, "intersect")) state |= DT_MASKS_STATE_INTERSECTION;
  else state |= DT_MASKS_STATE_UNION;
  return state;
}

static gboolean _append_shape(dt_masks_form_t *group, dt_masks_form_t *shape,
                              const char *operation, gboolean inverted, gboolean enabled,
                              GList **forms)
{
  if(!shape) return FALSE;
  dt_masks_point_group_t *member = calloc(1, sizeof(*member));
  if(!member)
  {
    dt_masks_free_form(shape);
    return FALSE;
  }
  member->formid = shape->formid;
  member->parentid = group->formid;
  member->state = _group_state(operation, inverted, group->points == NULL, enabled);
  member->opacity = 1.0f;
  group->points = g_list_append(group->points, member);
  *forms = g_list_append(*forms, shape);
  return TRUE;
}

static gboolean _component_forms(JsonObject *component, dt_masks_form_t *group, GList **forms)
{
  if(!component) return FALSE;
  const gboolean enabled =
    json_object_get_boolean_member_with_default(component, "enabled", TRUE);
  const char *kind = json_object_get_string_member(component, "kind");
  const char *operation = json_object_get_string_member(component, "operation");
  if(g_strcmp0(operation, "add") && g_strcmp0(operation, "subtract")
     && g_strcmp0(operation, "intersect"))
    return FALSE;
  const gboolean inverted =
    json_object_get_boolean_member_with_default(component, "inverted", FALSE);
  if(!g_strcmp0(kind, "linearGradient"))
    return _append_shape(group,
                         _gradient_form(json_object_get_object_member(component, "gradient")),
                         operation, inverted, enabled, forms);
  if(!g_strcmp0(kind, "ellipse"))
    return _append_shape(group,
                         _ellipse_form(json_object_get_object_member(component, "ellipse")),
                         operation, inverted, enabled, forms);
  if(g_strcmp0(kind, "brush")) return FALSE;
  JsonObject *brush = json_object_get_object_member(component, "brush");
  JsonArray *strokes = brush ? json_object_get_array_member(brush, "strokes") : NULL;
  if(!strokes || json_array_get_length(strokes) == 0) return FALSE;
  dt_masks_form_t *brush_group = dt_masks_create(DT_MASKS_GROUP);
  if(!brush_group) return FALSE;
  for(guint index = 0; index < json_array_get_length(strokes); index++)
  {
    if(!_append_shape(brush_group,
                      _brush_stroke_form(json_array_get_object_element(strokes, index)),
                      "add", FALSE, TRUE, forms))
    {
      dt_masks_free_form(brush_group);
      return FALSE;
    }
  }
  return _append_shape(group, brush_group, operation, inverted, enabled, forms);
}

static gboolean _mask_configuration_is_valid(JsonObject *mask)
{
  if(!mask) return FALSE;
  const char *name = json_object_get_string_member(mask, "name");
  double opacity = 0.0;
  if(!name || !name[0] || g_utf8_strlen(name, -1) > 80
     || !_double(mask, "opacity", &opacity) || opacity < 0.0 || opacity > 1.0)
    return FALSE;

  JsonObject *adjustments = json_object_get_object_member(mask, "adjustments");
  const char *adjustment_names[] = {
    "exposureEV", "contrast", "highlights", "shadows",
    "whites", "blacks", "vibrance", "saturation"
  };
  for(guint index = 0; index < G_N_ELEMENTS(adjustment_names); index++)
  {
    double value = 0.0;
    if(!_double(adjustments, adjustment_names[index], &value)) return FALSE;
    const double limit = index == 0 ? 5.0 : 100.0;
    if(value < -limit || value > limit) return FALSE;
  }

  JsonObject *range = json_object_get_object_member(mask, "range");
  if(!range || !json_object_get_boolean_member_with_default(range, "enabled", FALSE))
    return TRUE;
  const char *channel = json_object_get_string_member(range, "channel");
  if(g_strcmp0(channel, "luminance") && g_strcmp0(channel, "red")
     && g_strcmp0(channel, "green") && g_strcmp0(channel, "blue")
     && g_strcmp0(channel, "hue") && g_strcmp0(channel, "saturation"))
    return FALSE;
  JsonArray *handles = json_object_get_array_member(range, "handles");
  if(!handles || json_array_get_length(handles) != 4) return FALSE;
  double previous = -1.0;
  for(guint index = 0; index < 4; index++)
  {
    const double handle = json_array_get_double_element(handles, index);
    if(!isfinite(handle) || handle < 0.0 || handle > 1.0 || handle < previous)
      return FALSE;
    previous = handle;
  }
  return TRUE;
}

static gboolean _build_forms(JsonArray *masks, GList **forms, GPtrArray *groups)
{
  const guint count = json_array_get_length(masks);
  gboolean ok = TRUE;
  for(guint mask_index = 0; ok && mask_index < count; mask_index++)
  {
    JsonObject *mask = json_array_get_object_element(masks, mask_index);
    JsonArray *components = mask ? json_object_get_array_member(mask, "components") : NULL;
    if(!_mask_configuration_is_valid(mask) || !components
       || json_array_get_length(components) == 0
       || json_array_get_length(components) > DT_REMOTE_MAX_MASK_COMPONENTS)
      return FALSE;
    dt_masks_form_t *group = dt_masks_create(DT_MASKS_GROUP);
    if(!group) return FALSE;
    const char *name = json_object_get_string_member(mask, "name");
    g_strlcpy(group->name, name, sizeof(group->name));
    *forms = g_list_append(*forms, group);
    g_ptr_array_add(groups, group);
    for(guint component_index = 0;
        ok && component_index < json_array_get_length(components); component_index++)
      ok = _component_forms(json_array_get_object_element(components, component_index),
                            group, forms);
    if(!group->points) ok = FALSE;
  }
  return ok;
}

gboolean dt_remote_masks_validate(const char *masks_json, char **error)
{
  JsonParser *parser = json_parser_new();
  GError *json_error = NULL;
  if(!json_parser_load_from_data(parser, masks_json ? masks_json : "[]", -1, &json_error))
  {
    _set_error(error, "mask JSON is invalid: %s", json_error->message);
    g_clear_error(&json_error);
    g_object_unref(parser);
    return FALSE;
  }
  JsonNode *root = json_parser_get_root(parser);
  JsonArray *masks = root && JSON_NODE_HOLDS_ARRAY(root) ? json_node_get_array(root) : NULL;
  const guint count = masks ? json_array_get_length(masks) : DT_REMOTE_MAX_MASKS + 1;
  GList *forms = NULL;
  GPtrArray *groups = g_ptr_array_new();
  const gboolean ok = count <= DT_REMOTE_MAX_MASKS && _build_forms(masks, &forms, groups);
  if(!ok) _set_error(error, "mask state could not be normalized for darktable");
  _free_forms(forms);
  g_ptr_array_free(groups, TRUE);
  g_object_unref(parser);
  return ok;
}

static dt_iop_module_t *_module_for_mask(dt_remote_editor_state_facade_t *facade,
                                         dt_develop_t *dev, guint index)
{
  if(!facade->mask_modules)
    facade->mask_modules = g_ptr_array_new();
  while(facade->mask_modules->len <= index)
  {
    dt_iop_module_t *module = dt_dev_module_duplicate(dev, facade->color_balance.module);
    if(!module) return NULL;
    g_ptr_array_add(facade->mask_modules, module);
    // A duplicate changes the pixelpipe topology. Synchronizing parameters is
    // insufficient until the pipe is rebuilt with the new instance.
    dt_dev_pixelpipe_rebuild(dev);
  }
  return g_ptr_array_index(facade->mask_modules, index);
}

static gboolean _write_float(dt_iop_module_t *module, uint8_t *params,
                             const char *name, double value)
{
  const dt_introspection_field_t *field = module->so->get_f(name);
  if(!field || field->header.type != DT_INTROSPECTION_TYPE_FLOAT
     || field->header.offset > (size_t)module->params_size
     || sizeof(float) > (size_t)module->params_size - field->header.offset)
    return FALSE;
  const float normalized = (float)value;
  memcpy(params + field->header.offset, &normalized, sizeof(normalized));
  return TRUE;
}

static int _parametric_channel(const char *channel)
{
  if(!g_strcmp0(channel, "red")) return DEVELOP_BLENDIF_RED_in;
  if(!g_strcmp0(channel, "green")) return DEVELOP_BLENDIF_GREEN_in;
  if(!g_strcmp0(channel, "blue")) return DEVELOP_BLENDIF_BLUE_in;
  if(!g_strcmp0(channel, "hue")) return DEVELOP_BLENDIF_H_in;
  if(!g_strcmp0(channel, "saturation")) return DEVELOP_BLENDIF_S_in;
  return DEVELOP_BLENDIF_GRAY_in;
}

static gboolean _configure_module(dt_iop_module_t *module, JsonObject *mask,
                                  dt_mask_id_t group_id, char **error)
{
  uint8_t *params = g_try_malloc(module->params_size);
  if(!params)
  {
    _set_error(error, "could not allocate local-adjustment parameters");
    return FALSE;
  }
  memcpy(params, module->default_params, module->params_size);
  JsonObject *adjustments = json_object_get_object_member(mask, "adjustments");
  double exposure = 0.0, contrast = 0.0, highlights = 0.0, shadows = 0.0;
  double whites = 0.0, blacks = 0.0, vibrance = 0.0, saturation = 0.0;
  const gboolean valid = adjustments
    && _double(adjustments, "exposureEV", &exposure)
    && _double(adjustments, "contrast", &contrast)
    && _double(adjustments, "highlights", &highlights)
    && _double(adjustments, "shadows", &shadows)
    && _double(adjustments, "whites", &whites)
    && _double(adjustments, "blacks", &blacks)
    && _double(adjustments, "vibrance", &vibrance)
    && _double(adjustments, "saturation", &saturation)
    && _write_float(module, params, "global_Y", exposure / 5.0)
    && _write_float(module, params, "contrast", contrast / 100.0)
    && _write_float(module, params, "highlights_Y", highlights / 100.0)
    && _write_float(module, params, "shadows_Y", shadows / 100.0)
    && _write_float(module, params, "brilliance_highlights", whites / 100.0)
    && _write_float(module, params, "brilliance_shadows", blacks / 100.0)
    && _write_float(module, params, "vibrance", vibrance / 100.0)
    && _write_float(module, params, "saturation_global", saturation / 100.0);
  if(!valid)
  {
    g_free(params);
    _set_error(error, "mask local-adjustment mapping is invalid");
    return FALSE;
  }

  dt_develop_blend_params_t blend = *module->default_blendop_params;
  blend.mask_mode = DEVELOP_MASK_MASK;
  blend.mask_id = group_id;
  blend.blend_mode = DEVELOP_BLEND_NORMAL2;
  blend.opacity = (float)(100.0 * json_object_get_double_member(mask, "opacity"));
  if(json_object_get_boolean_member_with_default(mask, "inverted", FALSE))
    blend.mask_combine = DEVELOP_COMBINE_INV_EXCL;
  JsonObject *range = json_object_get_object_member(mask, "range");
  if(range && json_object_get_boolean_member_with_default(range, "enabled", FALSE))
  {
    const int channel = _parametric_channel(json_object_get_string_member(range, "channel"));
    JsonArray *handles = json_object_get_array_member(range, "handles");
    if(!handles || json_array_get_length(handles) != 4)
    {
      g_free(params);
      _set_error(error, "mask parametric range must contain four handles");
      return FALSE;
    }
    blend.mask_mode |= DEVELOP_MASK_CONDITIONAL;
    blend.blendif |= (1u << DEVELOP_BLENDIF_active) | (1u << channel);
    for(guint index = 0; index < 4; index++)
      blend.blendif_parameters[4 * channel + index] =
        (float)json_array_get_double_element(handles, index);
    if(json_object_get_boolean_member_with_default(range, "inverted", FALSE))
      blend.blendif |= (1u << (channel + 16));
  }

  const gboolean enabled =
    json_object_get_boolean_member_with_default(mask, "enabled", TRUE);
  const char *name = json_object_get_string_member(mask, "name");
  dt_pthread_mutex_lock(&module->dev->history_mutex);
  memcpy(module->params, params, module->params_size);
  memcpy(module->blend_params, &blend, sizeof(blend));
  module->enabled = enabled;
  g_strlcpy(module->multi_name, name ? name : "Remote mask", sizeof(module->multi_name));
  module->multi_name_hand_edited = TRUE;
  dt_dev_add_history_item_ext(module->dev, module, module->enabled, FALSE);
  dt_dev_invalidate_all(module->dev);
  dt_pthread_mutex_unlock(&module->dev->history_mutex);
  g_free(params);
  return TRUE;
}

gboolean dt_remote_masks_apply(dt_remote_editor_state_facade_t *facade,
                               dt_develop_t *dev,
                               const char *masks_json,
                               char **error)
{
  JsonParser *parser = json_parser_new();
  GError *json_error = NULL;
  if(!json_parser_load_from_data(parser, masks_json ? masks_json : "[]", -1, &json_error))
  {
    _set_error(error, "mask JSON is invalid: %s", json_error->message);
    g_clear_error(&json_error);
    g_object_unref(parser);
    return FALSE;
  }
  JsonNode *root = json_parser_get_root(parser);
  JsonArray *masks = root && JSON_NODE_HOLDS_ARRAY(root) ? json_node_get_array(root) : NULL;
  const guint count = masks ? json_array_get_length(masks) : DT_REMOTE_MAX_MASKS + 1;
  if(count > DT_REMOTE_MAX_MASKS)
  {
    _set_error(error, "mask array exceeds the facade-v3 limit");
    g_object_unref(parser);
    return FALSE;
  }

  GList *forms = NULL;
  GPtrArray *groups = g_ptr_array_new();
  gboolean ok = _build_forms(masks, &forms, groups);
  if(!ok)
  {
    _set_error(error, "mask geometry could not be normalized for darktable");
    _free_forms(forms);
    g_ptr_array_free(groups, TRUE);
    g_object_unref(parser);
    return FALSE;
  }

  dt_masks_replace_current_forms(dev, forms);
  for(guint index = 0; ok && index < count; index++)
  {
    dt_iop_module_t *module = _module_for_mask(facade, dev, index);
    dt_masks_form_t *group = g_ptr_array_index(groups, index);
    ok = module && _configure_module(module, json_array_get_object_element(masks, index),
                                     group->formid, error);
  }
  if(facade->mask_modules)
    for(guint index = count; index < facade->mask_modules->len; index++)
    {
      dt_iop_module_t *module = g_ptr_array_index(facade->mask_modules, index);
      module->enabled = FALSE;
      dt_dev_add_history_item_ext(dev, module, FALSE, FALSE);
    }
  if(!ok && error && !*error)
    _set_error(error, "could not create a darktable local-adjustment instance");
  _free_forms(forms);
  g_ptr_array_free(groups, TRUE);
  g_object_unref(parser);
  return ok;
}

void dt_remote_masks_cleanup(dt_remote_editor_state_facade_t *facade)
{
  if(facade->mask_modules)
    g_ptr_array_free(facade->mask_modules, TRUE);
  facade->mask_modules = NULL;
}
