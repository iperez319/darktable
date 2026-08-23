/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include "remote/remote_session.h"

#include "common/colorspaces.h"
#include "common/darktable.h"
#include "common/exif.h"
#include "common/film.h"
#include "common/image.h"
#include "develop/pixelpipe.h"
#include "imageio/imageio_common.h"
#include "imageio/imageio_module.h"
#include "control/conf.h"
#include "remote/remote_protocol.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define DT_REMOTE_COLOR_CONTRACT "srgb-sdr-surface-v1"

static void _set_error(char **error, const char *format, ...)
{
  if(!error)
    return;
  va_list ap;
  va_start(ap, format);
  *error = g_strdup_vprintf(format, ap);
  va_end(ap);
}

static char *_file_sha256(const char *path, char **error)
{
  GError *glib_error = NULL;
  GMappedFile *mapped = g_mapped_file_new(path, FALSE, &glib_error);
  if(!mapped)
  {
    _set_error(error, "could not read image for digest: %s",
               glib_error ? glib_error->message : "unknown error");
    g_clear_error(&glib_error);
    return NULL;
  }
  GChecksum *checksum = g_checksum_new(G_CHECKSUM_SHA256);
  g_checksum_update(checksum, (const guchar *)g_mapped_file_get_contents(mapped),
                    g_mapped_file_get_length(mapped));
  char *digest = g_strdup_printf("sha256:%s", g_checksum_get_string(checksum));
  g_checksum_free(checksum);
  g_mapped_file_unref(mapped);
  return digest;
}

static void _update_state_digest(dt_remote_session_t *session)
{
  g_free(session->state_digest);
  GChecksum *checksum = g_checksum_new(G_CHECKSUM_SHA256);
  const char separator = '\0';
  g_checksum_update(checksum, (const guchar *)session->image_digest, strlen(session->image_digest));
  g_checksum_update(checksum, (const guchar *)&separator, 1);
  g_checksum_update(checksum, (const guchar *)DT_REMOTE_DARKTABLE_COMMIT,
                    strlen(DT_REMOTE_DARKTABLE_COMMIT));
  g_checksum_update(checksum, (const guchar *)&separator, 1);
  const uint32_t version_be = GUINT32_TO_BE((uint32_t)session->exposure.module_version);
  g_checksum_update(checksum, (const guchar *)&version_be, sizeof(version_be));
  g_checksum_update(checksum, session->exposure.accepted, session->exposure.params_size);
  g_checksum_update(checksum, (const guchar *)DT_REMOTE_COLOR_CONTRACT,
                    strlen(DT_REMOTE_COLOR_CONTRACT));
  session->state_digest = g_strdup_printf("sha256:%s", g_checksum_get_string(checksum));
  g_checksum_free(checksum);
}

static gboolean _allocate_pipes(dt_remote_session_t *session, char **error)
{
  session->dev.full.pipe = g_try_malloc0(sizeof(dt_dev_pixelpipe_t));
  session->dev.preview_pipe = g_try_malloc0(sizeof(dt_dev_pixelpipe_t));
  session->dev.preview2.pipe = g_try_malloc0(sizeof(dt_dev_pixelpipe_t));
  if(!session->dev.full.pipe || !session->dev.preview_pipe || !session->dev.preview2.pipe)
  {
    g_free(session->dev.full.pipe);
    g_free(session->dev.preview_pipe);
    g_free(session->dev.preview2.pipe);
    session->dev.full.pipe = session->dev.preview_pipe = session->dev.preview2.pipe = NULL;
    _set_error(error, "could not allocate persistent pixelpipes");
    return FALSE;
  }
  const gboolean full_ok = dt_dev_pixelpipe_init(session->dev.full.pipe);
  const gboolean preview_ok = dt_dev_pixelpipe_init_preview(session->dev.preview_pipe);
  const gboolean preview2_ok = dt_dev_pixelpipe_init_preview2(session->dev.preview2.pipe);
  if(!full_ok || !preview_ok || !preview2_ok)
  {
    _set_error(error, "could not initialize persistent pixelpipes");
    return FALSE;
  }
  return TRUE;
}

void dt_remote_session_init(dt_remote_session_t *session)
{
  memset(session, 0, sizeof(*session));
  session->image_id = NO_IMGID;
  g_mutex_init(&session->mutex);
  g_mutex_init(&session->viewport_cancel_mutex);
  session->initialized = TRUE;
}

void dt_remote_session_cleanup(dt_remote_session_t *session)
{
  if(!session || !session->initialized)
    return;
  g_mutex_lock(&session->mutex);
  if(session->open)
  {
    session->dev.full.pipe->analysis_callback = NULL;
    session->dev.full.pipe->analysis_user_data = NULL;
    session->dev.preview_pipe->analysis_callback = NULL;
    session->dev.preview_pipe->analysis_user_data = NULL;
    session->dev.preview2.pipe->analysis_callback = NULL;
    session->dev.preview2.pipe->analysis_user_data = NULL;
    dt_remote_exposure_cleanup(&session->exposure);
    dt_dev_cleanup(&session->dev);
    if(dt_is_valid_imgid(session->image_id))
      dt_image_remove(session->image_id);
  }
  session->open = FALSE;
  g_free(session->image_digest);
  g_free(session->state_digest);
  session->image_digest = session->state_digest = NULL;
  g_mutex_unlock(&session->mutex);
  g_mutex_clear(&session->mutex);
  g_mutex_clear(&session->viewport_cancel_mutex);
  session->initialized = FALSE;
}

static gboolean _render_unlocked(dt_remote_session_t *session, uint64_t revision,
                                 uint64_t generation, uint32_t width, uint32_t height,
                                 dt_remote_surface_role_t role,
                                 dt_remote_normalized_rect_t coverage,
                                 double source_pixels_per_output_pixel,
                                 dt_remote_surface_t *surface,
                                 dt_remote_render_timing_t *timing, char **error);

gboolean dt_remote_session_open(dt_remote_session_t *session, const char *image_path,
                                uint32_t maximum_long_edge, const char *color_contract,
                                char **error)
{
  if(error)
    *error = NULL;
  g_mutex_lock(&session->mutex);
  gboolean ok = FALSE;
  if(session->open)
  {
    _set_error(error, "a worker session is already open");
    goto done;
  }
  if(!image_path || !g_file_test(image_path, G_FILE_TEST_IS_REGULAR))
  {
    _set_error(error, "image path is not a regular file");
    goto done;
  }
  if(maximum_long_edge == 0 || maximum_long_edge > 2048)
  {
    _set_error(error, "maximum long edge must be in 1...2048");
    goto done;
  }
  if(g_strcmp0(color_contract, DT_REMOTE_COLOR_CONTRACT))
  {
    _set_error(error, "unsupported color contract");
    goto done;
  }

  session->image_digest = _file_sha256(image_path, error);
  if(!session->image_digest)
    goto done;
  gchar *directory = g_path_get_dirname(image_path);
  dt_film_t film;
  const dt_filmid_t film_id = dt_film_new(&film, directory);
  g_free(directory);
  session->image_id = dt_image_import(film_id, image_path, TRUE, FALSE);
  if(!dt_is_valid_imgid(session->image_id))
  {
    _set_error(error, "could not import fixed image");
    g_clear_pointer(&session->image_digest, g_free);
    goto done;
  }

  darktable.color_profiles->display_type = DT_COLORSPACE_SRGB;
  darktable.color_profiles->display_filename[0] = '\0';
  darktable.color_profiles->display_intent = DT_INTENT_RELATIVE_COLORIMETRIC;
  darktable.color_profiles->display2_type = DT_COLORSPACE_SRGB;
  darktable.color_profiles->display2_filename[0] = '\0';
  darktable.color_profiles->display2_intent = DT_INTENT_RELATIVE_COLORIMETRIC;
  darktable.color_profiles->mode = DT_PROFILE_NORMAL;

  dt_dev_init(&session->dev, FALSE);
  if(!_allocate_pipes(session, error))
    goto failed_dev;
  session->dev.gui_attached = FALSE;
  session->dev.full.zoom = DT_ZOOM_FIT;
  session->dev.full.width = maximum_long_edge;
  session->dev.full.height = maximum_long_edge;
  session->dev.full.ppd = 1.0;
  session->dev.full.color_assessment = FALSE;
  session->dev.full.dev = &session->dev;
  session->dev.preview2.dev = &session->dev;
  session->overview.zoom = DT_ZOOM_FIT;
  session->overview.width = maximum_long_edge;
  session->overview.height = maximum_long_edge;
  session->overview.ppd = 1.0;
  session->overview.color_assessment = FALSE;
  session->overview.dev = &session->dev;
  session->overview.pipe = session->dev.preview_pipe;
  dt_dev_load_image(&session->dev, session->image_id);
  if(!dt_remote_exposure_init(&session->exposure, &session->dev, error))
    goto failed_dev;
  session->dev.full.pipe->analysis_callback = dt_remote_analysis_callback;
  session->dev.full.pipe->analysis_user_data = &session->analysis;
  session->dev.preview_pipe->analysis_callback = dt_remote_analysis_callback;
  session->dev.preview_pipe->analysis_user_data = &session->analysis;
  session->dev.preview2.pipe->analysis_callback = dt_remote_analysis_callback;
  session->dev.preview2.pipe->analysis_user_data = &session->analysis;

  session->maximum_long_edge = maximum_long_edge;
  session->epoch = (((uint64_t)g_get_real_time()) << 1) ^ (uint64_t)g_random_int();
  if(!session->epoch)
    session->epoch = 1;
  session->revision = 0;
  session->desired_generation = 0;
  session->open = TRUE;
  _update_state_digest(session);

  dt_remote_surface_t warm_surface;
  dt_remote_render_timing_t warm_timing = {0};
  if(!_render_unlocked(session, 0, 0, maximum_long_edge, maximum_long_edge,
                       DT_REMOTE_SURFACE_OVERVIEW,
                       (dt_remote_normalized_rect_t){0.0, 0.0, 1.0, 1.0}, 1.0,
                       &warm_surface, &warm_timing, error))
  {
    session->open = FALSE;
    dt_remote_exposure_cleanup(&session->exposure);
    goto failed_dev;
  }
  const float input_scale = session->dev.full.pipe->iscale > 0.0f
                                ? session->dev.full.pipe->iscale
                                : 1.0f;
  session->source_pixel_width = (uint32_t)llround(
      (double)session->dev.full.pipe->processed_width / input_scale);
  session->source_pixel_height = (uint32_t)llround(
      (double)session->dev.full.pipe->processed_height / input_scale);
  if(!session->source_pixel_width || !session->source_pixel_height)
  {
    _set_error(error, "could not determine developed source dimensions");
    dt_remote_surface_clear(&warm_surface);
    session->open = FALSE;
    dt_remote_exposure_cleanup(&session->exposure);
    goto failed_dev;
  }
  // Headless full-pipe initialization clears the preview loading flags before
  // those pipes have created their module nodes. Prime both persistent remote
  // roles explicitly so their first request performs the normal node setup.
  session->dev.preview_pipe->loading = TRUE;
  session->dev.preview_pipe->input_changed = TRUE;
  session->dev.preview_pipe->changed |= DT_DEV_PIPE_SYNCH;
  session->dev.preview2.pipe->loading = TRUE;
  session->dev.preview2.pipe->input_changed = TRUE;
  session->dev.preview2.pipe->changed |= DT_DEV_PIPE_SYNCH;
  dt_remote_surface_clear(&warm_surface);
  ok = TRUE;
  goto done;

failed_dev:
  dt_dev_cleanup(&session->dev);
  if(dt_is_valid_imgid(session->image_id))
    dt_image_remove(session->image_id);
  session->image_id = NO_IMGID;
  g_clear_pointer(&session->image_digest, g_free);
  g_clear_pointer(&session->state_digest, g_free);
done:
  g_mutex_unlock(&session->mutex);
  return ok;
}

static gboolean _validate_generation(const dt_remote_session_t *session, uint64_t generation,
                                     char **error)
{
  // The trusted gateway serializes public intents and owns ordering. The
  // worker generation is correlation metadata, not a second durable clock;
  // allowing replay is required for historical comparison and reconstruction.
  (void)session;
  (void)generation;
  (void)error;
  return TRUE;
}

gboolean dt_remote_session_set_exposure(dt_remote_session_t *session, double exposure_ev,
                                        double black, dt_remote_changed_field_t changed,
                                        uint64_t generation, float *accepted_exposure,
                                        float *accepted_black, char **error)
{
  if(error)
    *error = NULL;
  g_mutex_lock(&session->mutex);
  gboolean ok = FALSE;
  if(!session->open)
    _set_error(error, "session is not open");
  else if(_validate_generation(session, generation, error) &&
          dt_remote_exposure_apply(&session->exposure, &session->dev, exposure_ev, black, changed,
                                   accepted_exposure, accepted_black, error))
  {
    session->revision++;
    session->desired_generation = generation;
    _update_state_digest(session);
    ok = TRUE;
  }
  g_mutex_unlock(&session->mutex);
  return ok;
}

gboolean dt_remote_session_reset_exposure(dt_remote_session_t *session, uint64_t generation,
                                          float *accepted_exposure, float *accepted_black,
                                          char **error)
{
  if(error)
    *error = NULL;
  g_mutex_lock(&session->mutex);
  gboolean ok = FALSE;
  if(!session->open)
    _set_error(error, "session is not open");
  else if(_validate_generation(session, generation, error) &&
          dt_remote_exposure_reset(&session->exposure, &session->dev, accepted_exposure,
                                   accepted_black, error))
  {
    session->revision++;
    session->desired_generation = generation;
    _update_state_digest(session);
    ok = TRUE;
  }
  g_mutex_unlock(&session->mutex);
  return ok;
}

static gboolean _render_unlocked(dt_remote_session_t *session, uint64_t revision,
                                 uint64_t generation, uint32_t width, uint32_t height,
                                 dt_remote_surface_role_t role,
                                 dt_remote_normalized_rect_t coverage,
                                 double source_pixels_per_output_pixel,
                                 dt_remote_surface_t *surface,
                                 dt_remote_render_timing_t *timing, char **error)
{
  if(error)
    *error = NULL;
  if(timing)
    memset(timing, 0, sizeof(*timing));
  if(!session->open)
  {
    _set_error(error, "session is not open");
    return FALSE;
  }
  if(revision != session->revision)
  {
    _set_error(error, "render revision is not current");
    return FALSE;
  }
  if(width == 0 || height == 0 || width > 4096 || height > 4096 ||
     (uint64_t)width * (uint64_t)height * 4u > DT_REMOTE_MAX_ATTACHMENT_BYTES)
  {
    _set_error(error, "render dimensions exceed the bounded surface maximum");
    return FALSE;
  }
  if(!isfinite(source_pixels_per_output_pixel) || source_pixels_per_output_pixel < 1.0 ||
     !isfinite(coverage.x) || !isfinite(coverage.y) || !isfinite(coverage.width) ||
     !isfinite(coverage.height) || coverage.x < 0.0 || coverage.y < 0.0 ||
     coverage.width <= 0.0 || coverage.height <= 0.0 ||
     coverage.x + coverage.width > 1.000001 || coverage.y + coverage.height > 1.000001)
  {
    _set_error(error, "render coverage or scale is invalid");
    return FALSE;
  }

  const gboolean bootstrapping = role != DT_REMOTE_SURFACE_VIEWPORT
                                 && !session->source_pixel_width;
  dt_dev_viewport_t *port = role == DT_REMOTE_SURFACE_VIEWPORT
                                ? &session->dev.preview2
                                : (bootstrapping ? &session->dev.full : &session->overview);
  dt_dev_pixelpipe_t *pipe = port->pipe;
  uint32_t crop_x = 0, crop_y = 0, crop_width = 0, crop_height = 0;
  port->width = (int)width;
  port->height = (int)height;
  port->ppd = 1.0;
  port->dev = &session->dev;
  if(role == DT_REMOTE_SURFACE_VIEWPORT)
  {
    // The current RAW + Exposure stack does not produce byte-identical output
    // for an independently processed tile. Follow the conservative correctness
    // policy: process the whole developed image at the requested native-or-
    // reduced scale, then crop the completed display buffer. Pixelpipe caches
    // remain persistent, and no seam-prone tile is marked authoritative.
    const double scale = 1.0 / source_pixels_per_output_pixel;
    const uint32_t full_width = MAX(1u, (uint32_t)floor(session->source_pixel_width * scale));
    const uint32_t full_height = MAX(1u, (uint32_t)floor(session->source_pixel_height * scale));
    crop_width = MIN(width, full_width);
    crop_height = MIN(height, full_height);
    const double center_x = full_width * (coverage.x + coverage.width * 0.5);
    const double center_y = full_height * (coverage.y + coverage.height * 0.5);
    const int64_t desired_x = (int64_t)(center_x - crop_width / 2u);
    const int64_t desired_y = (int64_t)(center_y - crop_height / 2u);
    crop_x = (uint32_t)CLAMP(desired_x, 0, (int64_t)full_width - crop_width);
    crop_y = (uint32_t)CLAMP(desired_y, 0, (int64_t)full_height - crop_height);
    // dt_dev_process_image_job() adds a one-pixel movement guard on every
    // viewport edge. Counter it so the completed backbuffer is exactly the
    // full scaled image before the authoritative post-process crop.
    port->width = full_width > 2 ? (int)full_width - 2 : (int)full_width;
    port->height = full_height > 2 ? (int)full_height - 2 : (int)full_height;
    port->zoom = DT_ZOOM_FREE;
    port->zoom_scale = (float)scale;
    port->closeup = 0;
    dt_dev_zoom_move(port, DT_ZOOM_POSITION, 0.0f, 0, 0.0f, 0.0f, TRUE);
  }
  else
  {
    port->width = (int)width;
    port->height = (int)height;
    port->zoom = DT_ZOOM_FIT;
    port->zoom_x = port->zoom_y = 0.0f;
    port->closeup = 0;
  }
  pipe->changed |= DT_DEV_PIPE_ZOOMED;
  dt_remote_analysis_prepare(&session->analysis, revision, generation);
  if(role == DT_REMOTE_SURFACE_VIEWPORT)
  {
    dt_remote_analysis_crop(&session->analysis, crop_x, crop_y, crop_width, crop_height);
    g_mutex_lock(&session->viewport_cancel_mutex);
    session->active_viewport_generation = generation;
    const gboolean canceled_before_start =
        session->pending_viewport_cancel_generation == generation;
    if(canceled_before_start)
    {
      session->active_viewport_generation = 0;
      session->pending_viewport_cancel_generation = 0;
    }
    else if(session->pending_viewport_cancel_generation < generation)
      session->pending_viewport_cancel_generation = 0;
    g_mutex_unlock(&session->viewport_cancel_mutex);
    if(canceled_before_start)
    {
      _set_error(error, "render superseded");
      return FALSE;
    }
  }
  const gint64 start = g_get_monotonic_time();
  dt_dev_process_image_job(&session->dev, port, pipe, (dt_signal_t)-1, pipe->devid);
  if(timing)
    timing->pixelpipe_ms = (double)(g_get_monotonic_time() - start) / 1000.0;
  if(role == DT_REMOTE_SURFACE_VIEWPORT)
  {
    g_mutex_lock(&session->viewport_cancel_mutex);
    const gboolean canceled = session->pending_viewport_cancel_generation == generation;
    if(canceled)
    {
      session->active_viewport_generation = 0;
      session->pending_viewport_cancel_generation = 0;
    }
    g_mutex_unlock(&session->viewport_cancel_mutex);
    if(canceled)
    {
      _set_error(error, "render superseded");
      return FALSE;
    }
  }
  const gboolean surface_ok = role == DT_REMOTE_SURFACE_VIEWPORT
                                  ? dt_remote_surface_region_from_pipe(
                                        pipe, crop_x, crop_y, crop_width, crop_height,
                                        surface, timing, error)
                                  : dt_remote_surface_from_pipe(pipe, surface, timing, error);
  gboolean canceled = FALSE;
  if(role == DT_REMOTE_SURFACE_VIEWPORT)
  {
    g_mutex_lock(&session->viewport_cancel_mutex);
    canceled = session->pending_viewport_cancel_generation == generation;
    if(session->active_viewport_generation == generation)
      session->active_viewport_generation = 0;
    if(canceled)
      session->pending_viewport_cancel_generation = 0;
    g_mutex_unlock(&session->viewport_cancel_mutex);
  }
  if(canceled)
  {
    if(surface_ok)
      dt_remote_surface_clear(surface);
    _set_error(error, "render superseded");
    return FALSE;
  }
  if(!surface_ok)
    return FALSE;
  const gint64 analysis_start = g_get_monotonic_time();
  if(!dt_remote_analysis_finalize(&session->analysis, session->state_digest,
                                  surface->pixel_digest, surface->width, surface->height, error))
  {
    dt_remote_surface_clear(surface);
    return FALSE;
  }
  if(timing)
    timing->analysis_ms = (double)(g_get_monotonic_time() - analysis_start) / 1000.0;
  return TRUE;
}

gboolean dt_remote_session_render(dt_remote_session_t *session, uint64_t revision,
                                  uint64_t generation, uint32_t width, uint32_t height,
                                  dt_remote_surface_role_t role,
                                  dt_remote_normalized_rect_t coverage,
                                  double source_pixels_per_output_pixel,
                                  dt_remote_surface_t *surface,
                                  dt_remote_render_timing_t *timing, char **error)
{
  if(error)
    *error = NULL;
  g_mutex_lock(&session->mutex);
  const gboolean ok =
      _render_unlocked(session, revision, generation, width, height, role, coverage,
                       source_pixels_per_output_pixel, surface, timing, error);
  g_mutex_unlock(&session->mutex);
  return ok;
}

void dt_remote_session_cancel_viewport(dt_remote_session_t *session, uint64_t generation)
{
  if(!session || !generation)
    return;
  g_mutex_lock(&session->viewport_cancel_mutex);
  const uint64_t active_generation = session->active_viewport_generation;
  if(generation > session->pending_viewport_cancel_generation)
    session->pending_viewport_cancel_generation = generation;
  const gboolean signaled = session->active_viewport_generation == generation
                            && session->dev.preview2.pipe;
  if(signaled)
    dt_dev_pixelpipe_set_shutdown(session->dev.preview2.pipe, DT_DEV_PIXELPIPE_STOP_CANCEL);
  fprintf(stderr,
          "{\"component\":\"worker\",\"event\":\"viewportCancelApplied\","
          "\"generation\":%" G_GUINT64_FORMAT ",\"activeGeneration\":%"
          G_GUINT64_FORMAT ",\"pendingGeneration\":%" G_GUINT64_FORMAT
          ",\"pixelpipeSignaled\":%s}\n",
          generation, active_generation, session->pending_viewport_cancel_generation,
          signaled ? "true" : "false");
  g_mutex_unlock(&session->viewport_cancel_mutex);
}

void dt_remote_session_current_values(const dt_remote_session_t *session, float *exposure_ev,
                                      float *black)
{
  dt_remote_exposure_values(&session->exposure, session->exposure.accepted, exposure_ev, black);
}

void dt_remote_session_baseline_values(const dt_remote_session_t *session, float *exposure_ev,
                                       float *black)
{
  dt_remote_exposure_values(&session->exposure, session->exposure.baseline, exposure_ev, black);
}

gboolean dt_remote_session_checkpoint_xmp(dt_remote_session_t *session, const char *path,
                                          char **error)
{
  if(error) *error = NULL;
  g_mutex_lock(&session->mutex);
  gboolean ok = FALSE;
  if(!session->open || !path || !*path)
    _set_error(error, "session or XMP destination is invalid");
  else
  {
    dt_dev_write_history_ext(&session->dev, session->image_id);
    if(dt_exif_xmp_write(session->image_id, path, TRUE))
      _set_error(error, "darktable could not materialize XMP");
    else
      ok = TRUE;
  }
  g_mutex_unlock(&session->mutex);
  return ok;
}

gboolean dt_remote_session_export_jpeg(dt_remote_session_t *session, const char *path,
                                       char **error)
{
  if(error) *error = NULL;
  g_mutex_lock(&session->mutex);
  gboolean ok = FALSE;
  if(!session->open || !path || !*path)
  {
    _set_error(error, "session or export destination is invalid");
    goto done;
  }
  dt_dev_write_history_ext(&session->dev, session->image_id);
  dt_imageio_module_format_t *format = dt_imageio_get_format_by_name("jpeg");
  if(!format)
  {
    _set_error(error, "JPEG format module is unavailable");
    goto done;
  }
  dt_conf_set_int("plugins/imageio/format/jpeg/quality", 95);
  dt_conf_set_int("plugins/imageio/format/jpeg/subsample", 1);
  dt_imageio_module_data_t *params = format->get_params(format);
  if(!params)
  {
    _set_error(error, "JPEG format parameters are unavailable");
    goto done;
  }
  params->max_width = 0;
  params->max_height = 0;
  params->style[0] = '\0';
  params->style_append = TRUE;
  const gboolean failed = dt_imageio_export(
      session->image_id, path, format, params, TRUE, FALSE, FALSE, 1.0, TRUE, TRUE,
      DT_COLORSPACE_SRGB, NULL, DT_INTENT_RELATIVE_COLORIMETRIC, NULL, NULL, 1, 1, NULL);
  format->free_params(format, params);
  if(failed)
    _set_error(error, "darktable full-resolution JPEG export failed");
  else
    ok = TRUE;
done:
  g_mutex_unlock(&session->mutex);
  return ok;
}
