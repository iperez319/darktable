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
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

static const gchar *_benchmark_run_id = NULL;

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

static gboolean _get_uint32(JsonObject *object, const char *name, uint32_t *value)
{
  uint64_t number = 0;
  if(!_get_uint64(object, name, &number) || number > G_MAXUINT32)
    return FALSE;
  *value = (uint32_t)number;
  return TRUE;
}

static JsonObject *_values_object(float exposure_ev, float black)
{
  JsonObject *values = json_object_new();
  json_object_set_double_member(values, "exposureEV", exposure_ev);
  json_object_set_double_member(values, "black", black);
  return values;
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
  json_object_set_int_member(body, "sessionEpoch", (gint64)session->epoch);
  json_object_set_int_member(body, "revision", (gint64)session->revision);
  json_object_set_int_member(body, "desiredGeneration", (gint64)session->desired_generation);
  json_object_set_string_member(body, "stateDigest", session->state_digest);
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
  if(frame->type == DT_REMOTE_SURFACE_RENDER)
  {
    uint64_t revision = 0, public_revision = 0, session_epoch = 0, generation = 0;
    uint32_t width = 0, height = 0;
    if(!_get_uint64(body, "revision", &revision) || !_get_uint64(body, "generation", &generation) ||
       !_get_uint32(body, "width", &width) || !_get_uint32(body, "height", &height))
      return _send_error(output, request, "protocol_error", "invalid render request");
    const gboolean has_public_identity =
        _get_uint64(body, "publicRevision", &public_revision) &&
        _get_uint64(body, "sessionEpoch", &session_epoch);
    const char *trace_id = json_object_has_member(body, "traceId")
                               ? json_object_get_string_member(body, "traceId")
                               : NULL;
    dt_remote_surface_t surface;
    dt_remote_render_timing_t render_timing = {0};
    char *error = NULL;
    if(!dt_remote_session_render(session, revision, generation, width, height, &surface,
                                 &render_timing, &error))
    {
      const gboolean sent = _send_error(output, request, "render_failed", error);
      g_free(error);
      return sent;
    }
    JsonObject *response = json_object_new();
    json_object_set_int_member(response, "sessionEpoch", (gint64)session->epoch);
    json_object_set_int_member(response, "revision", (gint64)revision);
    json_object_set_int_member(response, "generation", (gint64)generation);
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
    json_object_set_double_member(timing, "surfaceCopy",
                                  render_timing.snapshot_ms + render_timing.normalize_ms +
                                      render_timing.digest_ms + render_timing.analysis_ms);
    json_object_set_double_member(timing, "histogram", session->analysis.elapsed_ms);
    json_object_set_object_member(response, "timingMs", timing);
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
  gboolean shutdown = FALSE;
  int result = 0;
  while(!shutdown)
  {
    dt_remote_frame_t frame;
    GError *error = NULL;
    const dt_remote_read_result_t read = dt_remote_frame_read(stdin, &frame, &error);
    if(read == DT_REMOTE_READ_EOF)
      break;
    if(read == DT_REMOTE_READ_ERROR)
    {
      fprintf(stderr, "darktable-remote-worker: protocol read failed: %s\n",
              error ? error->message : "unknown error");
      g_clear_error(&error);
      result = 2;
      break;
    }
    if(!_handle(protocol_output, &session, &frame, &shutdown))
    {
      dt_remote_frame_clear(&frame);
      result = 2;
      break;
    }
    dt_remote_frame_clear(&frame);
  }
  dt_remote_session_cleanup(&session);
  dt_cleanup();
  fflush(protocol_output);
  fclose(protocol_output);
  g_ptr_array_free(args, TRUE);
  return result;
}
