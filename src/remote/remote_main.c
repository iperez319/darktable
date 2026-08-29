/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include "common/darktable.h"
#include "common/file_location.h"
#include "remote/remote_protocol.h"
#include "remote/remote_session.h"

#include <glib.h>
#include <glib/gi18n.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

static const gchar *_benchmark_run_id = NULL;

typedef struct dt_remote_input_t
{
  dt_remote_read_result_t result;
  dt_remote_frame_t frame;
  GError *error;
} dt_remote_input_t;

typedef struct dt_remote_reader_t
{
  GAsyncQueue *queue;
  dt_remote_session_t *session;
  gint stop;
} dt_remote_reader_t;

static gboolean _has_flag(char **argv, int argc, const char *flag)
{
  for(int i = 0; i < argc; i++)
    if(!g_strcmp0(argv[i], flag))
      return TRUE;
  return FALSE;
}

static gboolean _has_conf(char **argv, int argc, const char *prefix)
{
  for(int i = 0; i + 1 < argc; i++)
    if(!g_strcmp0(argv[i], "--conf") && g_str_has_prefix(argv[i + 1], prefix))
      return TRUE;
  return FALSE;
}

static JsonNode *_envelope(JsonObject *request, const char *type, JsonObject *body)
{
  JsonObject *result = json_object_new();
  json_object_set_string_member(result, "type", type);
  const char *request_id = json_object_has_member(request, "requestId")
                               ? json_object_get_string_member(request, "requestId")
                               : "";
  const char *session_id = json_object_has_member(request, "sessionId")
                               ? json_object_get_string_member(request, "sessionId")
                               : "";
  json_object_set_string_member(result, "requestId", request_id ? request_id : "");
  json_object_set_string_member(result, "sessionId", session_id ? session_id : "");
  json_object_set_object_member(result, "body", body ? body : json_object_new());
  JsonNode *node = json_node_new(JSON_NODE_OBJECT);
  json_node_take_object(node, result);
  return node;
}

static gboolean _send(FILE *output, uint16_t type, JsonNode *node, const void *attachment,
                      size_t attachment_size)
{
  GError *error = NULL;
  const gboolean ok =
      dt_remote_frame_write(output, type, node, attachment, attachment_size, &error);
  if(!ok)
    fprintf(stderr, "darktable-remote-worker: protocol write failed: %s\n",
            error ? error->message : "unknown error");
  g_clear_error(&error);
  json_node_unref(node);
  return ok;
}

static gboolean _send_error(FILE *output, JsonObject *request, const char *code,
                            const char *message)
{
  JsonObject *body = json_object_new();
  json_object_set_string_member(body, "code", code);
  json_object_set_string_member(body, "message", message ? message : "worker error");
  return _send(output, DT_REMOTE_WORKER_ERROR, _envelope(request, "worker.error", body), NULL, 0);
}

static JsonObject *_request_body(JsonObject *request)
{
  if(!json_object_has_member(request, "body"))
    return NULL;
  JsonNode *node = json_object_get_member(request, "body");
  return JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
}

static gboolean _get_uint64(JsonObject *object, const char *name, uint64_t *value)
{
  if(!object || !json_object_has_member(object, name))
    return FALSE;
  JsonNode *node = json_object_get_member(object, name);
  if(!JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_INT64)
    return FALSE;
  const gint64 number = json_node_get_int(node);
  if(number < 0)
    return FALSE;
  *value = (uint64_t)number;
  return TRUE;
}

static gboolean _cancel_generation(const dt_remote_frame_t *frame, uint64_t *generation)
{
  if(frame->type != DT_REMOTE_SURFACE_CANCEL || frame->attachment_size ||
     !frame->json || !JSON_NODE_HOLDS_OBJECT(frame->json))
    return FALSE;
  JsonObject *request = json_node_get_object(frame->json);
  const char *type = json_object_has_member(request, "type")
                         ? json_object_get_string_member(request, "type")
                         : NULL;
  JsonObject *body = _request_body(request);
  return type && !strcmp(type, "surface.cancel") && body &&
         _get_uint64(body, "generation", generation) && *generation > 0;
}

static gpointer _read_worker_input(gpointer user_data)
{
  dt_remote_reader_t *reader = (dt_remote_reader_t *)user_data;
  while(!g_atomic_int_get(&reader->stop))
  {
    struct pollfd descriptor = { STDIN_FILENO, POLLIN, 0 };
    const int ready = poll(&descriptor, 1, 100);
    if(ready == 0)
      continue;
    if(ready < 0)
    {
      if(errno == EINTR)
        continue;
      dt_remote_input_t *input = g_malloc0(sizeof(*input));
      input->result = DT_REMOTE_READ_ERROR;
      input->error = g_error_new(g_quark_from_static_string("dt-remote-input"), errno,
                                 "poll failed: %s", g_strerror(errno));
      g_async_queue_push(reader->queue, input);
      break;
    }
    dt_remote_input_t *input = g_malloc0(sizeof(*input));
    input->result = dt_remote_frame_read(stdin, &input->frame, &input->error);
    if(input->result == DT_REMOTE_READ_OK && input->frame.type == DT_REMOTE_SURFACE_CANCEL)
    {
      uint64_t generation = 0;
      if(_cancel_generation(&input->frame, &generation))
      {
        fprintf(stderr,
                "{\"component\":\"worker\",\"event\":\"viewportCancelReceived\","
                "\"generation\":%" G_GUINT64_FORMAT "}\n",
                generation);
        dt_remote_session_cancel_viewport(reader->session, generation);
      }
      else
        fprintf(stderr, "darktable-remote-worker: invalid surface.cancel ignored\n");
      dt_remote_frame_clear(&input->frame);
      g_clear_error(&input->error);
      g_free(input);
      continue;
    }
    const gboolean terminal = input->result != DT_REMOTE_READ_OK ||
                              input->frame.type == DT_REMOTE_WORKER_SHUTDOWN;
    g_async_queue_push(reader->queue, input);
    if(terminal)
      break;
  }
  return NULL;
}

static gboolean _get_uint32(JsonObject *object, const char *name, uint32_t *value)
{
  uint64_t number = 0;
  if(!_get_uint64(object, name, &number) || number > G_MAXUINT32)
    return FALSE;
  *value = (uint32_t)number;
  return TRUE;
}

static gboolean _get_double(JsonObject *object, const char *name, double *value)
{
  if(!object || !json_object_has_member(object, name))
    return FALSE;
  JsonNode *node = json_object_get_member(object, name);
  if(!JSON_NODE_HOLDS_VALUE(node) ||
     (json_node_get_value_type(node) != G_TYPE_DOUBLE &&
      json_node_get_value_type(node) != G_TYPE_INT64))
    return FALSE;
  *value = json_node_get_double(node);
  return isfinite(*value);
}

static gboolean _get_normalized_rect(JsonObject *body, dt_remote_normalized_rect_t *rect)
{
  if(!json_object_has_member(body, "normalizedRect"))
    return FALSE;
  JsonNode *node = json_object_get_member(body, "normalizedRect");
  if(!JSON_NODE_HOLDS_OBJECT(node))
    return FALSE;
  JsonObject *object = json_node_get_object(node);
  return _get_double(object, "x", &rect->x) && _get_double(object, "y", &rect->y) &&
         _get_double(object, "width", &rect->width) &&
         _get_double(object, "height", &rect->height);
}

static JsonObject *_values_object(float exposure_ev, float black)
{
  JsonObject *values = json_object_new();
  json_object_set_double_member(values, "exposureEV", exposure_ev);
  json_object_set_double_member(values, "black", black);
  return values;
}

static JsonObject *_object_member(JsonObject *object, const char *name)
{
  if(!object || !json_object_has_member(object, name))
    return NULL;
  JsonNode *node = json_object_get_member(object, name);
  return JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
}

static JsonArray *_array_member(JsonObject *object, const char *name)
{
  if(!object || !json_object_has_member(object, name))
    return NULL;
  JsonNode *node = json_object_get_member(object, name);
  return JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
}

static gboolean _get_boolean(JsonObject *object, const char *name, gboolean *value)
{
  if(!object || !json_object_has_member(object, name))
    return FALSE;
  JsonNode *node = json_object_get_member(object, name);
  if(!JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_BOOLEAN)
    return FALSE;
  *value = json_node_get_boolean(node);
  return TRUE;
}

static gboolean _get_int(JsonObject *object, const char *name, int *value)
{
  if(!object || !json_object_has_member(object, name))
    return FALSE;
  JsonNode *node = json_object_get_member(object, name);
  if(!JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_INT64)
    return FALSE;
  const gint64 number = json_node_get_int(node);
  if(number < G_MININT || number > G_MAXINT)
    return FALSE;
  *value = (int)number;
  return TRUE;
}

static const char *_get_string(JsonObject *object, const char *name)
{
  if(!object || !json_object_has_member(object, name)) return NULL;
  JsonNode *node = json_object_get_member(object, name);
  if(!JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_STRING)
    return NULL;
  return json_node_get_string(node);
}

static gboolean _parse_phase21_enums(JsonObject *optics, JsonObject *detail,
                                     dt_remote_editor_state_t *state)
{
  const char *lens_correction = _get_string(optics, "lensCorrection");
  const char *lens_status = _get_string(optics, "lensProfileStatus");
  const char *denoise_preset = _get_string(detail, "denoisePreset");
  const char *highlight = _get_string(detail, "highlightReconstruction");
  const char *profile = _get_string(optics, "lensProfile");
  if(!lens_correction || !lens_status || !denoise_preset || !highlight || !profile ||
     strlen(profile) >= sizeof(state->optics.lens_profile))
    return FALSE;
  if(!strcmp(lens_correction, "off"))
    state->optics.lens_correction = DT_REMOTE_LENS_CORRECTION_OFF;
  else if(!strcmp(lens_correction, "automatic"))
    state->optics.lens_correction = DT_REMOTE_LENS_CORRECTION_AUTOMATIC;
  else if(!strcmp(lens_correction, "manual"))
    state->optics.lens_correction = DT_REMOTE_LENS_CORRECTION_MANUAL;
  else return FALSE;
  if(!strcmp(lens_status, "notRequested"))
    state->optics.lens_profile_status = DT_REMOTE_LENS_PROFILE_NOT_REQUESTED;
  else if(!strcmp(lens_status, "applied"))
    state->optics.lens_profile_status = DT_REMOTE_LENS_PROFILE_APPLIED;
  else if(!strcmp(lens_status, "missing"))
    state->optics.lens_profile_status = DT_REMOTE_LENS_PROFILE_MISSING;
  else return FALSE;
  if(!strcmp(denoise_preset, "off")) state->detail.denoise_preset = DT_REMOTE_DENOISE_OFF;
  else if(!strcmp(denoise_preset, "low")) state->detail.denoise_preset = DT_REMOTE_DENOISE_LOW;
  else if(!strcmp(denoise_preset, "medium")) state->detail.denoise_preset = DT_REMOTE_DENOISE_MEDIUM;
  else if(!strcmp(denoise_preset, "high")) state->detail.denoise_preset = DT_REMOTE_DENOISE_HIGH;
  else if(!strcmp(denoise_preset, "custom")) state->detail.denoise_preset = DT_REMOTE_DENOISE_CUSTOM;
  else return FALSE;
  if(!strcmp(highlight, "inpaintOpposed"))
    state->detail.highlight_reconstruction = DT_REMOTE_HIGHLIGHT_INPAINT_OPPOSED;
  else if(!strcmp(highlight, "clip"))
    state->detail.highlight_reconstruction = DT_REMOTE_HIGHLIGHT_CLIP;
  else if(!strcmp(highlight, "reconstructColor"))
    state->detail.highlight_reconstruction = DT_REMOTE_HIGHLIGHT_RECONSTRUCT_COLOR;
  else if(!strcmp(highlight, "guidedLaplacians"))
    state->detail.highlight_reconstruction = DT_REMOTE_HIGHLIGHT_GUIDED_LAPLACIANS;
  else if(!strcmp(highlight, "segmentation"))
    state->detail.highlight_reconstruction = DT_REMOTE_HIGHLIGHT_SEGMENTATION;
  else return FALSE;
  g_strlcpy(state->optics.lens_profile, profile, sizeof(state->optics.lens_profile));
  return TRUE;
}

static gboolean _editor_state_from_json(JsonObject *object, dt_remote_editor_state_t *state,
                                        char **error)
{
  memset(state, 0, sizeof(*state));
  JsonObject *exposure = _object_member(object, "exposure");
  JsonObject *geometry = _object_member(object, "geometry");
  JsonObject *crop = _object_member(geometry, "crop");
  JsonObject *white_balance = _object_member(object, "whiteBalance");
  JsonObject *light = _object_member(object, "light");
  JsonObject *color = _object_member(object, "color");
  JsonObject *tone_curve = _object_member(object, "toneCurve");
  JsonObject *optics = _object_member(object, "optics");
  JsonObject *detail = _object_member(object, "detail");
  JsonArray *points = _array_member(tone_curve, "points");
  JsonArray *masks = _array_member(object, "masks");
  if(!exposure || !geometry || !crop || !white_balance || !light || !color || !tone_curve ||
     !optics || !detail ||
     !points || !masks ||
     !_get_double(exposure, "exposureEV", &state->exposure.exposure_ev) ||
     !_get_double(exposure, "blackLevel", &state->exposure.black_level) ||
     !_get_double(crop, "x", &state->geometry.x) ||
     !_get_double(crop, "y", &state->geometry.y) ||
     !_get_double(crop, "width", &state->geometry.width) ||
     !_get_double(crop, "height", &state->geometry.height) ||
     !_get_int(geometry, "rotationQuarterTurns", &state->geometry.rotation_quarter_turns) ||
     !_get_double(geometry, "straightenDegrees", &state->geometry.straighten_degrees) ||
     !_get_boolean(geometry, "flipHorizontal", &state->geometry.flip_horizontal) ||
     !_get_boolean(geometry, "flipVertical", &state->geometry.flip_vertical) ||
     !_get_double(white_balance, "temperatureKelvin",
                  &state->white_balance.temperature_kelvin) ||
     !_get_double(white_balance, "tint", &state->white_balance.tint) ||
     !_get_double(light, "contrast", &state->light.contrast) ||
     !_get_double(light, "highlights", &state->light.highlights) ||
     !_get_double(light, "shadows", &state->light.shadows) ||
     !_get_double(light, "whites", &state->light.whites) ||
     !_get_double(light, "blacks", &state->light.blacks) ||
     !_get_double(color, "vibrance", &state->color.vibrance) ||
     !_get_double(color, "saturation", &state->color.saturation) ||
     !_parse_phase21_enums(optics, detail, state) ||
     !_get_boolean(optics, "chromaticAberrationCorrection",
                   &state->optics.chromatic_aberration_correction) ||
     !_get_double(optics, "defringe", &state->optics.defringe) ||
     !_get_double(detail, "sharpening", &state->detail.sharpening) ||
     !_get_double(detail, "noiseReduction", &state->detail.noise_reduction))
  {
    if(error)
      *error = g_strdup("canonical state has missing or incorrectly typed fields");
    return FALSE;
  }
  JsonNode *masks_node = json_node_new(JSON_NODE_ARRAY);
  json_node_set_array(masks_node, masks);
  char *masks_json = json_to_string(masks_node, FALSE);
  json_node_free(masks_node);
  if(!masks_json || g_strlcpy(state->masks_json, masks_json,
                              sizeof(state->masks_json)) >= sizeof(state->masks_json))
  {
    g_free(masks_json);
    if(error)
      *error = g_strdup("canonical mask state exceeds the facade-v3 limit");
    return FALSE;
  }
  g_free(masks_json);
  const guint point_count = json_array_get_length(points);
  if(point_count < DT_REMOTE_TONE_CURVE_MIN_POINTS ||
     point_count > DT_REMOTE_TONE_CURVE_MAX_POINTS)
  {
    if(error)
      *error = g_strdup("tone curve must contain 2...20 points");
    return FALSE;
  }
  state->tone_curve.point_count = point_count;
  for(guint index = 0; index < point_count; index++)
  {
    JsonNode *node = json_array_get_element(points, index);
    if(!node || !JSON_NODE_HOLDS_OBJECT(node))
    {
      if(error)
        *error = g_strdup("tone curve points must be objects");
      return FALSE;
    }
    JsonObject *point = json_node_get_object(node);
    if(!_get_double(point, "x", &state->tone_curve.points[index].x) ||
       !_get_double(point, "y", &state->tone_curve.points[index].y))
    {
      if(error)
        *error = g_strdup("tone curve point coordinates must be finite numbers");
      return FALSE;
    }
  }
  return TRUE;
}

static JsonObject *_editor_state_object(const dt_remote_editor_state_t *state)
{
  JsonObject *result = json_object_new();
  JsonObject *exposure = json_object_new();
  json_object_set_double_member(exposure, "exposureEV", state->exposure.exposure_ev);
  json_object_set_double_member(exposure, "blackLevel", state->exposure.black_level);
  json_object_set_object_member(result, "exposure", exposure);

  JsonObject *geometry = json_object_new();
  JsonObject *crop = json_object_new();
  json_object_set_double_member(crop, "x", state->geometry.x);
  json_object_set_double_member(crop, "y", state->geometry.y);
  json_object_set_double_member(crop, "width", state->geometry.width);
  json_object_set_double_member(crop, "height", state->geometry.height);
  json_object_set_object_member(geometry, "crop", crop);
  json_object_set_int_member(geometry, "rotationQuarterTurns",
                             state->geometry.rotation_quarter_turns);
  json_object_set_double_member(geometry, "straightenDegrees",
                                state->geometry.straighten_degrees);
  json_object_set_boolean_member(geometry, "flipHorizontal", state->geometry.flip_horizontal);
  json_object_set_boolean_member(geometry, "flipVertical", state->geometry.flip_vertical);
  json_object_set_object_member(result, "geometry", geometry);

  JsonObject *white_balance = json_object_new();
  json_object_set_double_member(white_balance, "temperatureKelvin",
                                state->white_balance.temperature_kelvin);
  json_object_set_double_member(white_balance, "tint", state->white_balance.tint);
  json_object_set_object_member(result, "whiteBalance", white_balance);

  JsonObject *light = json_object_new();
  json_object_set_double_member(light, "contrast", state->light.contrast);
  json_object_set_double_member(light, "highlights", state->light.highlights);
  json_object_set_double_member(light, "shadows", state->light.shadows);
  json_object_set_double_member(light, "whites", state->light.whites);
  json_object_set_double_member(light, "blacks", state->light.blacks);
  json_object_set_object_member(result, "light", light);

  JsonObject *color = json_object_new();
  json_object_set_double_member(color, "vibrance", state->color.vibrance);
  json_object_set_double_member(color, "saturation", state->color.saturation);
  json_object_set_object_member(result, "color", color);

  JsonObject *tone_curve = json_object_new();
  JsonArray *points = json_array_sized_new(state->tone_curve.point_count);
  for(size_t index = 0; index < state->tone_curve.point_count; index++)
  {
    JsonObject *point = json_object_new();
    json_object_set_double_member(point, "x", state->tone_curve.points[index].x);
    json_object_set_double_member(point, "y", state->tone_curve.points[index].y);
    json_array_add_object_element(points, point);
  }
  json_object_set_array_member(tone_curve, "points", points);
  json_object_set_object_member(result, "toneCurve", tone_curve);

  const char *lens_correction = state->optics.lens_correction == DT_REMOTE_LENS_CORRECTION_MANUAL
                                    ? "manual"
                                    : state->optics.lens_correction == DT_REMOTE_LENS_CORRECTION_AUTOMATIC
                                          ? "automatic" : "off";
  const char *lens_status = state->optics.lens_profile_status == DT_REMOTE_LENS_PROFILE_APPLIED
                                ? "applied"
                                : state->optics.lens_profile_status == DT_REMOTE_LENS_PROFILE_MISSING
                                      ? "missing" : "notRequested";
  JsonObject *optics = json_object_new();
  json_object_set_string_member(optics, "lensCorrection", lens_correction);
  json_object_set_string_member(optics, "lensProfile", state->optics.lens_profile);
  json_object_set_string_member(optics, "lensProfileStatus", lens_status);
  json_object_set_boolean_member(optics, "chromaticAberrationCorrection",
                                 state->optics.chromatic_aberration_correction);
  json_object_set_double_member(optics, "defringe", state->optics.defringe);
  json_object_set_object_member(result, "optics", optics);

  static const char *denoise_presets[] = { "off", "low", "medium", "high", "custom" };
  static const char *highlight_modes[] = {
    "inpaintOpposed", "clip", "reconstructColor", "guidedLaplacians", "segmentation"
  };
  JsonObject *detail = json_object_new();
  json_object_set_double_member(detail, "sharpening", state->detail.sharpening);
  json_object_set_double_member(detail, "noiseReduction", state->detail.noise_reduction);
  json_object_set_string_member(detail, "denoisePreset",
                                denoise_presets[state->detail.denoise_preset]);
  json_object_set_string_member(detail, "highlightReconstruction",
                                highlight_modes[state->detail.highlight_reconstruction]);
  json_object_set_object_member(result, "detail", detail);
  JsonParser *mask_parser = json_parser_new();
  if(json_parser_load_from_data(mask_parser, state->masks_json, -1, NULL) &&
     JSON_NODE_HOLDS_ARRAY(json_parser_get_root(mask_parser)))
    json_object_set_array_member(
        result, "masks", json_array_ref(json_node_get_array(json_parser_get_root(mask_parser))));
  else
    json_object_set_array_member(result, "masks", json_array_new());
  g_object_unref(mask_parser);
  return result;
}

static JsonArray *_histogram_channel(const uint32_t *counts)
{
  JsonArray *channel = json_array_sized_new(DT_REMOTE_HISTOGRAM_BINS);
  for(size_t bin = 0; bin < DT_REMOTE_HISTOGRAM_BINS; bin++)
    json_array_add_int_element(channel, counts[bin]);
  return channel;
}

static JsonObject *_histogram_object(const dt_remote_analysis_t *analysis)
{
  JsonObject *histogram = json_object_new();
  json_object_set_string_member(histogram, "domain", DT_REMOTE_HISTOGRAM_DOMAIN);
  json_object_set_int_member(histogram, "bins", DT_REMOTE_HISTOGRAM_BINS);
  json_object_set_string_member(histogram, "channels", "rgb");
  json_object_set_array_member(histogram, "red", _histogram_channel(analysis->red));
  json_object_set_array_member(histogram, "green", _histogram_channel(analysis->green));
  json_object_set_array_member(histogram, "blue", _histogram_channel(analysis->blue));
  json_object_set_int_member(histogram, "sampledPixels", analysis->sampled_pixels);
  json_object_set_int_member(histogram, "sourceGeneration", (gint64)analysis->generation);
  json_object_set_string_member(histogram, "sourcePixelpipeResultDigest",
                                analysis->pixelpipe_result_digest);
  return histogram;
}

static JsonNode *_artifact_written(JsonObject *request, const char *kind, const char *path)
{
  FILE *file = g_fopen(path, "rb");
  if(!file) return NULL;
  GChecksum *checksum = g_checksum_new(G_CHECKSUM_SHA256);
  guint8 buffer[65536];
  uint64_t size = 0;
  size_t count = 0;
  while((count = fread(buffer, 1, sizeof(buffer), file)) > 0)
  {
    g_checksum_update(checksum, buffer, count);
    size += count;
  }
  const gboolean valid = !ferror(file);
  fclose(file);
  if(!valid)
  {
    g_checksum_free(checksum);
    return NULL;
  }
  JsonObject *body = json_object_new();
  json_object_set_string_member(body, "kind", kind);
  json_object_set_int_member(body, "bytes", (gint64)size);
  char *digest = g_strdup_printf("sha256:%s", g_checksum_get_string(checksum));
  json_object_set_string_member(body, "digest", digest);
  g_free(digest);
  g_checksum_free(checksum);
  return _envelope(request, "artifact.written", body);
}

static JsonNode *_capabilities(JsonObject *request)
{
  JsonObject *body = json_object_new();
  json_object_set_int_member(body, "protocolMajor", DT_REMOTE_PROTOCOL_MAJOR);
  json_object_set_string_member(body, "darktableCommit", DT_REMOTE_DARKTABLE_COMMIT);
  json_object_set_string_member(body, "darktableVersion", darktable_package_version);
  json_object_set_int_member(body, "exposureModuleVersion", 7);
  JsonObject *fields = json_object_new();
  JsonObject *exposure = json_object_new();
  json_object_set_double_member(exposure, "minimum", -18.0);
  json_object_set_double_member(exposure, "maximum", 18.0);
  json_object_set_object_member(fields, "exposure", exposure);
  JsonObject *black = json_object_new();
  json_object_set_double_member(black, "minimum", -1.0);
  json_object_set_double_member(black, "maximum", 1.0);
  json_object_set_object_member(fields, "black", black);
  json_object_set_object_member(body, "exposureFields", fields);
  JsonObject *editor_state = json_object_new();
  json_object_set_int_member(editor_state, "schemaVersion", DT_REMOTE_EDITOR_STATE_SCHEMA);
  json_object_set_string_member(editor_state, "masks",
                                "drawn-and-parametric-local-adjustments-facade-v3");
  JsonObject *modules = json_object_new();
#define ADD_MODULE(role, operation, version, mapping) do { \
    JsonObject *module = json_object_new(); \
    json_object_set_string_member(module, "operation", (operation)); \
    json_object_set_int_member(module, "version", (version)); \
    json_object_set_string_member(module, "mapping", (mapping)); \
    json_object_set_object_member(modules, (role), module); \
  } while(0)
  ADD_MODULE("exposure", "exposure", 7, "exposureEV,blackLevel -> exposure,black");
  ADD_MODULE("crop", "crop", 3, "right=x+width,bottom=y+height; free aspect");
  ADD_MODULE("orientation", "flip", 2,
             "clockwise quarter-turn followed by output-axis horizontal/vertical flips");
  ADD_MODULE("straighten", "ashift", 5, "degrees -> rotation; largest-area auto crop");
  ADD_MODULE("whiteBalance", "temperature", 4,
             "Kelvin analytic CIE xy + piecewise tint -> camera coefficients, green=1");
  ADD_MODULE("contrastVibranceSaturation", "colorbalancergb", 5,
             "signed percent / 100 -> contrast,vibrance,saturation_global");
  ADD_MODULE("tonalRanges", "toneequal", 2,
             "signed percent / 50 -> grouped -8EV...0EV bands");
  ADD_MODULE("toneCurve", "rgbcurve", 1,
             "linked RGB master, monotone Hermite, up to 20 points");
  ADD_MODULE("lensCorrection", "lens", 10,
             "off/automatic/manual Lensfun profile; all optical corrections");
  ADD_MODULE("chromaticAberration", "cacorrect", 2, "enabled -> raw CA correction");
  ADD_MODULE("defringe", "defringe", 1,
             "0 disables; amount -> threshold=128-1.275*amount");
  ADD_MODULE("sharpening", "sharpen", 1, "amount / 50 -> USM amount");
  ADD_MODULE("noiseReduction", "denoiseprofile", 12,
             "profiled wavelets; 0 disables; strength=0.25+1.75*amount/100");
  ADD_MODULE("highlightReconstruction", "highlights", 4,
             "named product option -> pinned reconstruction mode");
  ADD_MODULE("maskLocalAdjustments", "colorbalancergb", 5,
             "one duplicated instance per mask; gradient/ellipse/brush drawn forms plus range");
#undef ADD_MODULE
  json_object_set_object_member(editor_state, "modules", modules);
  JsonObject *ranges = json_object_new();
#define ADD_RANGE(name, minimum, maximum) do { \
    JsonObject *range = json_object_new(); \
    json_object_set_double_member(range, "minimum", (minimum)); \
    json_object_set_double_member(range, "maximum", (maximum)); \
    json_object_set_object_member(ranges, (name), range); \
  } while(0)
  ADD_RANGE("exposure.exposureEV", -18.0, 18.0);
  ADD_RANGE("exposure.blackLevel", -1.0, 1.0);
  ADD_RANGE("geometry.cropCoordinates", 0.0, 1.0);
  ADD_RANGE("geometry.rotationQuarterTurns", 0.0, 3.0);
  ADD_RANGE("geometry.straightenDegrees", -45.0, 45.0);
  ADD_RANGE("whiteBalance.temperatureKelvin", 1901.0, 25000.0);
  ADD_RANGE("whiteBalance.tint", -100.0, 100.0);
  ADD_RANGE("lightAndColor", -100.0, 100.0);
  ADD_RANGE("toneCurve.coordinates", 0.0, 1.0);
  ADD_RANGE("optics.defringe", 0.0, 100.0);
  ADD_RANGE("detail.sharpening", 0.0, 100.0);
  ADD_RANGE("detail.noiseReduction", 0.0, 100.0);
#undef ADD_RANGE
  json_object_set_double_member(ranges, "geometry.minimumCropSize", 0.01);
  json_object_set_int_member(ranges, "toneCurve.minimumPoints", 2);
  json_object_set_int_member(ranges, "toneCurve.maximumPoints", 20);
  json_object_set_object_member(editor_state, "ranges", ranges);
  json_object_set_object_member(body, "editorState", editor_state);
  JsonObject *facade_mappings = json_object_new();
  json_object_set_int_member(facade_mappings, "version", DT_REMOTE_EDITOR_STATE_SCHEMA);
  json_object_set_string_member(facade_mappings, "name", "photographic-facade-v3");
  json_object_set_object_member(body, "facadeMappings", facade_mappings);
  JsonArray *surfaces = json_array_new();
  json_array_add_string_element(surfaces, "srgb-sdr-surface-v1");
  json_object_set_array_member(body, "surfaceContracts", surfaces);
  JsonArray *histogram_domains = json_array_new();
  json_array_add_string_element(histogram_domains, DT_REMOTE_HISTOGRAM_DOMAIN);
  json_object_set_array_member(body, "histogramDomains", histogram_domains);
  return _envelope(request, "worker.capabilities", body);
}

static JsonNode *_session_description(JsonObject *request, const dt_remote_session_t *session)
{
  float baseline_exposure = 0.0f, baseline_black = 0.0f;
  float current_exposure = 0.0f, current_black = 0.0f;
  dt_remote_session_baseline_values(session, &baseline_exposure, &baseline_black);
  dt_remote_session_current_values(session, &current_exposure, &current_black);
  JsonObject *body = json_object_new();
  json_object_set_object_member(body, "baselineValues",
                                _values_object(baseline_exposure, baseline_black));
  json_object_set_object_member(body, "currentValues",
                                _values_object(current_exposure, current_black));
  json_object_set_object_member(body, "baselineState",
                                _editor_state_object(&session->editor_state.baseline));
  json_object_set_object_member(body, "currentState",
                                _editor_state_object(&session->editor_state.current));
  json_object_set_int_member(body, "sessionEpoch", (gint64)session->epoch);
  json_object_set_int_member(body, "revision", (gint64)session->revision);
  json_object_set_int_member(body, "desiredGeneration", (gint64)session->desired_generation);
  json_object_set_string_member(body, "stateDigest", session->state_digest);
  json_object_set_int_member(body, "sourcePixelWidth", session->source_pixel_width);
  json_object_set_int_member(body, "sourcePixelHeight", session->source_pixel_height);
  return _envelope(request, "session.opened", body);
}

static gboolean _handle(FILE *output, dt_remote_session_t *session, const dt_remote_frame_t *frame,
                        gboolean *shutdown)
{
  JsonObject *request = json_node_get_object(frame->json);
  const char *expected = dt_remote_message_name(frame->type);
  const char *actual = json_object_has_member(request, "type")
                           ? json_object_get_string_member(request, "type")
                           : NULL;
  if(!expected || !actual || strcmp(expected, actual) || frame->attachment_size)
    return _send_error(output, request, "protocol_error",
                       "numeric message type, JSON type, or attachment is invalid");
  JsonObject *body = _request_body(request);
  if(!body)
    return _send_error(output, request, "protocol_error", "request body must be an object");

  if(frame->type == DT_REMOTE_WORKER_HELLO)
  {
    uint64_t major = 0;
    if(!_get_uint64(body, "protocolMajor", &major) || major != DT_REMOTE_PROTOCOL_MAJOR)
      return _send_error(output, request, "protocol_mismatch", "unsupported protocol major");
    return _send(output, DT_REMOTE_WORKER_CAPABILITIES, _capabilities(request), NULL, 0);
  }
  if(frame->type == DT_REMOTE_WORKER_SHUTDOWN)
  {
    *shutdown = TRUE;
    return TRUE;
  }
  if(frame->type == DT_REMOTE_SESSION_OPEN)
  {
    const char *path = json_object_has_member(body, "imagePath")
                           ? json_object_get_string_member(body, "imagePath")
                           : NULL;
    const char *contract = json_object_has_member(body, "colorContract")
                               ? json_object_get_string_member(body, "colorContract")
                               : NULL;
    uint32_t max_edge = 0, histogram_bins = 0;
    char *error = NULL;
    if(!_get_uint32(body, "maxLongEdge", &max_edge) ||
       !_get_uint32(body, "histogramBins", &histogram_bins) || histogram_bins != 1024)
      return _send_error(output, request, "protocol_error", "invalid session limits");
    if(!dt_remote_session_open(session, path, max_edge, contract, &error))
    {
      const gboolean sent = _send_error(output, request, "session_open_failed", error);
      g_free(error);
      return sent;
    }
    return _send(output, DT_REMOTE_SESSION_OPENED, _session_description(request, session), NULL, 0);
  }
  if(frame->type == DT_REMOTE_SESSION_DESCRIBE)
  {
    if(!session->open)
      return _send_error(output, request, "session_not_ready", "session is not open");
    return _send(output, DT_REMOTE_SESSION_OPENED, _session_description(request, session), NULL, 0);
  }
  if(frame->type == DT_REMOTE_SESSION_SET_EXPOSURE)
  {
    uint64_t generation = 0;
    if(!_get_uint64(body, "generation", &generation) ||
       !json_object_has_member(body, "exposureEV") || !json_object_has_member(body, "black") ||
       !json_object_has_member(body, "changedField"))
      return _send_error(output, request, "protocol_error", "invalid exposure request");
    const double exposure_ev = json_object_get_double_member(body, "exposureEV");
    const double black = json_object_get_double_member(body, "black");
    const char *changed_name = json_object_get_string_member(body, "changedField");
    dt_remote_changed_field_t changed;
    if(!strcmp(changed_name, "exposureEV"))
      changed = DT_REMOTE_CHANGED_EXPOSURE;
    else if(!strcmp(changed_name, "black"))
      changed = DT_REMOTE_CHANGED_BLACK;
    else
      return _send_error(output, request, "protocol_error", "invalid changedField");
    float accepted_exposure = 0.0f, accepted_black = 0.0f;
    char *error = NULL;
    if(!dt_remote_session_set_exposure(session, exposure_ev, black, changed, generation,
                                       &accepted_exposure, &accepted_black, &error))
    {
      const gboolean sent = _send_error(output, request, "invalid_exposure", error);
      g_free(error);
      return sent;
    }
    JsonObject *response = json_object_new();
    json_object_set_int_member(response, "revision", (gint64)session->revision);
    json_object_set_int_member(response, "generation", (gint64)generation);
    json_object_set_string_member(response, "stateDigest", session->state_digest);
    json_object_set_object_member(response, "values",
                                  _values_object(accepted_exposure, accepted_black));
    return _send(output, DT_REMOTE_SESSION_EXPOSURE_ACCEPTED,
                 _envelope(request, "session.exposureAccepted", response), NULL, 0);
  }
  if(frame->type == DT_REMOTE_SESSION_RESET_EXPOSURE)
  {
    uint64_t generation = 0;
    if(!_get_uint64(body, "generation", &generation))
      return _send_error(output, request, "protocol_error", "invalid reset request");
    float accepted_exposure = 0.0f, accepted_black = 0.0f;
    char *error = NULL;
    if(!dt_remote_session_reset_exposure(session, generation, &accepted_exposure, &accepted_black,
                                         &error))
    {
      const gboolean sent = _send_error(output, request, "invalid_exposure", error);
      g_free(error);
      return sent;
    }
    JsonObject *response = json_object_new();
    json_object_set_int_member(response, "revision", (gint64)session->revision);
    json_object_set_int_member(response, "generation", (gint64)generation);
    json_object_set_string_member(response, "stateDigest", session->state_digest);
    json_object_set_object_member(response, "values",
                                  _values_object(accepted_exposure, accepted_black));
    return _send(output, DT_REMOTE_SESSION_EXPOSURE_ACCEPTED,
                 _envelope(request, "session.exposureAccepted", response), NULL, 0);
  }
  if(frame->type == DT_REMOTE_SESSION_SET_STATE)
  {
    uint64_t generation = 0;
    JsonObject *state_object = _object_member(body, "state");
    dt_remote_editor_state_t state;
    char *error = NULL;
    if(!_get_uint64(body, "generation", &generation) || !state_object ||
       !_editor_state_from_json(state_object, &state, &error))
    {
      const gboolean sent = _send_error(output, request, "invalid_state",
                                        error ? error : "invalid canonical state request");
      g_free(error);
      return sent;
    }
    if(!dt_remote_session_set_state(session, &state, generation, &error))
    {
      const gboolean sent = _send_error(output, request, "invalid_state", error);
      g_free(error);
      return sent;
    }
    JsonObject *response = json_object_new();
    json_object_set_int_member(response, "revision", (gint64)session->revision);
    json_object_set_int_member(response, "generation", (gint64)generation);
    json_object_set_string_member(response, "stateDigest", session->state_digest);
    json_object_set_object_member(response, "state",
                                  _editor_state_object(&session->editor_state.current));
    json_object_set_object_member(
        response, "values",
        _values_object((float)session->editor_state.current.exposure.exposure_ev,
                       (float)session->editor_state.current.exposure.black_level));
    json_object_set_int_member(response, "sourcePixelWidth", session->source_pixel_width);
    json_object_set_int_member(response, "sourcePixelHeight", session->source_pixel_height);
    return _send(output, DT_REMOTE_SESSION_STATE_ACCEPTED,
                 _envelope(request, "session.stateAccepted", response), NULL, 0);
  }
  if(frame->type == DT_REMOTE_SESSION_CHECKPOINT_XMP ||
     frame->type == DT_REMOTE_SESSION_EXPORT_JPEG)
  {
    const char *path = json_object_has_member(body, "outputPath")
                           ? json_object_get_string_member(body, "outputPath")
                           : NULL;
    uint64_t revision = 0;
    if(!path || !*path || !_get_uint64(body, "revision", &revision) || revision != session->revision)
      return _send_error(output, request, "protocol_error", "invalid artifact request");
    char *error = NULL;
    const gboolean xmp = frame->type == DT_REMOTE_SESSION_CHECKPOINT_XMP;
    const gboolean ok = xmp ? dt_remote_session_checkpoint_xmp(session, path, &error)
                            : dt_remote_session_export_jpeg(session, path, &error);
    if(!ok)
    {
      const gboolean sent = _send_error(output, request, "artifact_failed", error);
      g_free(error);
      return sent;
    }
    JsonNode *response = _artifact_written(request, xmp ? "xmp" : "jpeg", path);
    if(!response)
      return _send_error(output, request, "artifact_failed", "could not verify artifact");
    return _send(output, DT_REMOTE_ARTIFACT_WRITTEN, response, NULL, 0);
  }
  if(frame->type == DT_REMOTE_SURFACE_RENDER)
  {
    uint64_t revision = 0, public_revision = 0, session_epoch = 0, generation = 0;
    uint32_t width = 0, height = 0, overscan_pixels = 0;
    double source_pixels_per_output_pixel = 0.0;
    dt_remote_normalized_rect_t coverage = {0};
    if(!_get_uint64(body, "revision", &revision) || !_get_uint64(body, "generation", &generation) ||
       !_get_uint32(body, "width", &width) || !_get_uint32(body, "height", &height) ||
       !_get_uint32(body, "overscanPixels", &overscan_pixels) ||
       !_get_double(body, "sourcePixelsPerOutputPixel", &source_pixels_per_output_pixel) ||
       !_get_normalized_rect(body, &coverage) || !json_object_has_member(body, "role"))
      return _send_error(output, request, "protocol_error", "invalid render request");
    if(overscan_pixels > 256)
      return _send_error(output, request, "protocol_error", "invalid viewport overscan");
    const char *role_name = json_object_get_string_member(body, "role");
    dt_remote_surface_role_t role;
    if(!strcmp(role_name, "overview"))
      role = DT_REMOTE_SURFACE_OVERVIEW;
    else if(!strcmp(role_name, "viewport"))
      role = DT_REMOTE_SURFACE_VIEWPORT;
    else if(!strcmp(role_name, "baseline"))
      role = DT_REMOTE_SURFACE_BASELINE;
    else if(!strcmp(role_name, "geometry-preview"))
      role = DT_REMOTE_SURFACE_GEOMETRY_PREVIEW;
    else
      return _send_error(output, request, "protocol_error", "invalid surface role");
    const gboolean has_public_identity =
        _get_uint64(body, "publicRevision", &public_revision) &&
        _get_uint64(body, "sessionEpoch", &session_epoch);
    const char *trace_id = json_object_has_member(body, "traceId")
                               ? json_object_get_string_member(body, "traceId")
                               : NULL;
    dt_remote_surface_t surface;
    dt_remote_render_timing_t render_timing = {0};
    char *error = NULL;
    const gint64 render_start = g_get_monotonic_time();
    fprintf(stderr,
            "{\"component\":\"worker\",\"event\":\"renderStart\",\"revision\":%"
            G_GUINT64_FORMAT ",\"generation\":%" G_GUINT64_FORMAT
            ",\"role\":\"%s\",\"width\":%u,\"height\":%u,"
            "\"sourcePixelsPerOutputPixel\":%.9f}\n",
            revision, generation, role_name, width, height, source_pixels_per_output_pixel);
    if(!dt_remote_session_render(session, revision, generation, width, height, role, coverage,
                                 source_pixels_per_output_pixel, &surface, &render_timing, &error))
    {
      const char *code = error && !strcmp(error, "render superseded")
                             ? "render_superseded"
                             : "render_failed";
      fprintf(stderr,
              "{\"component\":\"worker\",\"event\":\"renderFailed\",\"revision\":%"
              G_GUINT64_FORMAT ",\"generation\":%" G_GUINT64_FORMAT
              ",\"role\":\"%s\",\"code\":\"%s\",\"durationMs\":%.6f}\n",
              revision, generation, role_name, code,
              (double)(g_get_monotonic_time() - render_start) / 1000.0);
      const gboolean sent = _send_error(output, request, code, error);
      g_free(error);
      return sent;
    }
    JsonObject *response = json_object_new();
    json_object_set_int_member(response, "sessionEpoch", (gint64)session->epoch);
    json_object_set_int_member(response, "revision", (gint64)revision);
    json_object_set_int_member(response, "generation", (gint64)generation);
    json_object_set_string_member(response, "role", role_name);
    JsonObject *coverage_object = json_object_new();
    json_object_set_double_member(coverage_object, "x", coverage.x);
    json_object_set_double_member(coverage_object, "y", coverage.y);
    json_object_set_double_member(coverage_object, "width", coverage.width);
    json_object_set_double_member(coverage_object, "height", coverage.height);
    json_object_set_object_member(response, "coverage", coverage_object);
    json_object_set_int_member(response, "sourcePixelWidth", session->source_pixel_width);
    json_object_set_int_member(response, "sourcePixelHeight", session->source_pixel_height);
    json_object_set_string_member(response, "stateDigest", session->state_digest);
    json_object_set_string_member(response, "moduleStackDigest", session->state_digest);
    json_object_set_string_member(response, "pixelpipeResultDigest",
                                  session->analysis.pixelpipe_result_digest);
    json_object_set_int_member(response, "width", surface.width);
    json_object_set_int_member(response, "height", surface.height);
    json_object_set_int_member(response, "bytesPerRow", surface.bytes_per_row);
    json_object_set_int_member(response, "payloadBytes", (gint64)surface.size);
    json_object_set_string_member(response, "pixelFormat", "bgra8Unorm");
    json_object_set_string_member(response, "pixelDigest", surface.pixel_digest);
    json_object_set_object_member(response, "histogram",
                                  _histogram_object(&session->analysis));
    JsonObject *timing = json_object_new();
    json_object_set_double_member(timing, "apply", 0.0);
    json_object_set_double_member(timing, "pixelpipe", render_timing.pixelpipe_ms);
    json_object_set_double_member(timing, "snapshot", render_timing.snapshot_ms);
    json_object_set_double_member(timing, "normalize", render_timing.normalize_ms);
    json_object_set_double_member(timing, "digest", render_timing.digest_ms);
    json_object_set_double_member(timing, "analysis", render_timing.analysis_ms);
    json_object_set_double_member(timing, "roiFallbackFullFrame",
                                  role == DT_REMOTE_SURFACE_VIEWPORT ? 1.0 : 0.0);
    json_object_set_double_member(timing, "surfaceCopy",
                                  render_timing.snapshot_ms + render_timing.normalize_ms +
                                      render_timing.digest_ms + render_timing.analysis_ms);
    json_object_set_double_member(timing, "histogram", session->analysis.elapsed_ms);
    json_object_set_object_member(response, "timingMs", timing);
    fprintf(stderr,
            "{\"component\":\"worker\",\"event\":\"renderComplete\",\"revision\":%"
            G_GUINT64_FORMAT ",\"generation\":%" G_GUINT64_FORMAT
            ",\"role\":\"%s\",\"durationMs\":%.6f,\"pixelpipeMs\":%.6f,"
            "\"snapshotMs\":%.6f,\"normalizeMs\":%.6f,\"digestMs\":%.6f,"
            "\"analysisMs\":%.6f}\n",
            revision, generation, role_name,
            (double)(g_get_monotonic_time() - render_start) / 1000.0,
            render_timing.pixelpipe_ms, render_timing.snapshot_ms, render_timing.normalize_ms,
            render_timing.digest_ms, render_timing.analysis_ms);
    JsonNode *node = _envelope(request, "surface.rendered", response);
    const gint64 write_start = g_get_monotonic_time();
    const gboolean sent =
        _send(output, DT_REMOTE_SURFACE_RENDERED, node, surface.pixels, surface.size);
    const double write_ms = (double)(g_get_monotonic_time() - write_start) / 1000.0;
    if(_benchmark_run_id)
    {
      char expected_trace[96] = {0};
      g_snprintf(expected_trace, sizeof(expected_trace), "%s/%" G_GUINT64_FORMAT,
                 _benchmark_run_id, generation);
      const char *correlated_trace = trace_id && !strcmp(trace_id, expected_trace)
                                         ? trace_id
                                         : expected_trace;
      fprintf(stderr,
              "{\"runId\":\"%s\",\"traceId\":\"%s"
              "\",\"component\":\"worker\",\"event\":\"privateWrite\",\"stateDigest\":\"%s\",\"sessionEpoch\":%"
              G_GUINT64_FORMAT ",\"revision\":%" G_GUINT64_FORMAT
              ",\"engineRevision\":%" G_GUINT64_FORMAT ",\"generation\":%" G_GUINT64_FORMAT
              ",\"bytes\":%zu,\"durationMs\":%.6f}\n",
              _benchmark_run_id, correlated_trace, session->state_digest,
              has_public_identity ? session_epoch : 0,
              has_public_identity ? public_revision : revision, revision, generation, surface.size,
              write_ms);
    }
    else
      fprintf(stderr,
              "{\"component\":\"worker\",\"event\":\"privateWrite\",\"revision\":%" G_GUINT64_FORMAT
              ",\"generation\":%" G_GUINT64_FORMAT ",\"bytes\":%zu,\"durationMs\":%.6f}\n",
              revision, generation, surface.size, write_ms);
    dt_remote_surface_clear(&surface);
    return sent;
  }
  return _send_error(output, request, "protocol_error", "unsupported worker command");
}

int main(int argc, char **argv)
{
  if(_has_flag(argv, argc, "--editor-state-self-test"))
  {
    char *error = NULL;
    const gboolean ok = dt_remote_editor_state_run_self_tests(&error);
    if(ok)
      fprintf(stdout, "PASS: canonical editor-state mapping self-tests\n");
    else
      fprintf(stderr, "FAIL: canonical editor-state mapping self-tests: %s\n",
              error ? error : "unknown error");
    g_free(error);
    return ok ? 0 : 1;
  }
  _benchmark_run_id = g_getenv("DARKTABLE_REMOTE_BENCHMARK_RUN_ID");
  if(_benchmark_run_id && !g_uuid_string_is_valid(_benchmark_run_id)) _benchmark_run_id = NULL;
  dt_loc_init(NULL, NULL, NULL, NULL, NULL, NULL);
  char localedir[PATH_MAX] = {0};
  dt_loc_get_localedir(localedir, sizeof(localedir));
  bindtextdomain(GETTEXT_PACKAGE, localedir);
  bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
  textdomain(GETTEXT_PACKAGE);

  int core_start = argc;
  for(int i = 1; i < argc; i++)
    if(!g_strcmp0(argv[i], "--core"))
    {
      core_start = i + 1;
      break;
    }
  const int core_count = argc - core_start;
  char **core = core_count > 0 ? &argv[core_start] : NULL;
  GPtrArray *args = g_ptr_array_new();
  g_ptr_array_add(args, (gpointer) "darktable-remote-worker");
  if(!_has_flag(core, core_count, "--library"))
  {
    g_ptr_array_add(args, (gpointer) "--library");
    g_ptr_array_add(args, (gpointer) ":memory:");
  }
  if(!_has_conf(core, core_count, "write_sidecar_files="))
  {
    g_ptr_array_add(args, (gpointer) "--conf");
    g_ptr_array_add(args, (gpointer) "write_sidecar_files=never");
  }
  for(int i = 0; i < core_count; i++)
    g_ptr_array_add(args, core[i]);

  fflush(stdout);
  const int protocol_fd = dup(STDOUT_FILENO);
  FILE *protocol_output = fdopen(protocol_fd, "wb");
  dup2(STDERR_FILENO, STDOUT_FILENO);
  if(!protocol_output || dt_init((int)args->len, (char **)args->pdata, FALSE, TRUE, NULL))
  {
    fprintf(stderr, "darktable-remote-worker: initialization failed\n");
    if(protocol_output)
      fclose(protocol_output);
    g_ptr_array_free(args, TRUE);
    return 1;
  }

  dt_remote_session_t session;
  dt_remote_session_init(&session);
  GAsyncQueue *input_queue = g_async_queue_new();
  dt_remote_reader_t reader = { input_queue, &session, FALSE };
  GThread *reader_thread = g_thread_new("remote-input", _read_worker_input, &reader);
  gboolean shutdown = FALSE;
  int result = 0;
  while(!shutdown)
  {
    dt_remote_input_t *input = g_async_queue_pop(input_queue);
    if(input->result == DT_REMOTE_READ_EOF)
    {
      g_free(input);
      break;
    }
    if(input->result == DT_REMOTE_READ_ERROR)
    {
      fprintf(stderr, "darktable-remote-worker: protocol read failed: %s\n",
              input->error ? input->error->message : "unknown error");
      g_clear_error(&input->error);
      g_free(input);
      result = 2;
      break;
    }
    if(!_handle(protocol_output, &session, &input->frame, &shutdown))
    {
      dt_remote_frame_clear(&input->frame);
      g_clear_error(&input->error);
      g_free(input);
      result = 2;
      break;
    }
    dt_remote_frame_clear(&input->frame);
    g_clear_error(&input->error);
    g_free(input);
  }
  g_atomic_int_set(&reader.stop, TRUE);
  g_thread_join(reader_thread);
  g_async_queue_unref(input_queue);
  dt_remote_session_cleanup(&session);
  dt_cleanup();
  fflush(protocol_output);
  fclose(protocol_output);
  g_ptr_array_free(args, TRUE);
  return result;
}
