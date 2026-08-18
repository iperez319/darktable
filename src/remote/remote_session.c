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
#include "common/film.h"
#include "common/image.h"
#include "develop/pixelpipe.h"

#include <errno.h>
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
  session->initialized = TRUE;
}

void dt_remote_session_cleanup(dt_remote_session_t *session)
{
  if(!session || !session->initialized)
    return;
  g_mutex_lock(&session->mutex);
  if(session->open)
  {
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
  session->initialized = FALSE;
}

static gboolean _render_unlocked(dt_remote_session_t *session, uint64_t revision,
                                 uint64_t generation, uint32_t width, uint32_t height,
                                 dt_remote_surface_t *surface, double *render_ms, char **error);

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
  dt_dev_load_image(&session->dev, session->image_id);
  if(!dt_remote_exposure_init(&session->exposure, &session->dev, error))
    goto failed_dev;

  session->maximum_long_edge = maximum_long_edge;
  session->epoch = (((uint64_t)g_get_real_time()) << 1) ^ (uint64_t)g_random_int();
  if(!session->epoch)
    session->epoch = 1;
  session->revision = 0;
  session->desired_generation = 0;
  session->open = TRUE;
  _update_state_digest(session);

  dt_remote_surface_t warm_surface;
  double warm_ms = 0.0;
  if(!_render_unlocked(session, 0, 0, maximum_long_edge, maximum_long_edge, &warm_surface, &warm_ms,
                       error))
  {
    session->open = FALSE;
    dt_remote_exposure_cleanup(&session->exposure);
    goto failed_dev;
  }
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
  if(generation <= session->desired_generation)
  {
    _set_error(error, "generation must be greater than the current desired generation");
    return FALSE;
  }
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
                                 dt_remote_surface_t *surface, double *render_ms, char **error)
{
  if(error)
    *error = NULL;
  if(!session->open)
  {
    _set_error(error, "session is not open");
    return FALSE;
  }
  if(revision != session->revision || generation != session->desired_generation)
  {
    _set_error(error, "render revision or generation is not current");
    return FALSE;
  }
  if(width == 0 || height == 0 || width > session->maximum_long_edge ||
     height > session->maximum_long_edge)
  {
    _set_error(error, "render dimensions exceed the negotiated maximum");
    return FALSE;
  }
  session->dev.full.width = (int)width;
  session->dev.full.height = (int)height;
  session->dev.full.pipe->changed |= DT_DEV_PIPE_ZOOMED;
  dt_dev_invalidate_all(&session->dev);
  const gint64 start = g_get_monotonic_time();
  dt_dev_process_image_job(&session->dev, &session->dev.full, session->dev.full.pipe,
                           (dt_signal_t)-1, session->dev.full.pipe->devid);
  if(render_ms)
    *render_ms = (double)(g_get_monotonic_time() - start) / 1000.0;
  return dt_remote_surface_from_pipe(session->dev.full.pipe, surface, error);
}

gboolean dt_remote_session_render(dt_remote_session_t *session, uint64_t revision,
                                  uint64_t generation, uint32_t width, uint32_t height,
                                  dt_remote_surface_t *surface, double *render_ms, char **error)
{
  if(error)
    *error = NULL;
  g_mutex_lock(&session->mutex);
  const gboolean ok =
      _render_unlocked(session, revision, generation, width, height, surface, render_ms, error);
  g_mutex_unlock(&session->mutex);
  return ok;
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
