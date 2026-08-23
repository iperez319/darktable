/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include "remote/remote_analysis.h"

#include "common/darktable.h"
#include "common/histogram.h"
#include "common/iop_profile.h"

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

static void _analysis_error(dt_remote_analysis_t *analysis, const char *message)
{
  analysis->valid = FALSE;
  g_strlcpy(analysis->error, message, sizeof(analysis->error));
}

void dt_remote_analysis_prepare(dt_remote_analysis_t *analysis, uint64_t revision,
                                uint64_t generation)
{
  memset(analysis, 0, sizeof(*analysis));
  analysis->revision = revision;
  analysis->generation = generation;
}

void dt_remote_analysis_crop(dt_remote_analysis_t *analysis, uint32_t x, uint32_t y,
                             uint32_t width, uint32_t height)
{
  analysis->crop_x = x;
  analysis->crop_y = y;
  analysis->crop_width = width;
  analysis->crop_height = height;
}

void dt_remote_analysis_callback(void *user_data, const float *input, int width, int height,
                                 dt_iop_colorspace_type_t colorspace,
                                 const dt_iop_order_iccprofile_info_t *profile_info)
{
  dt_remote_analysis_t *analysis = user_data;
  if(!analysis)
    return;

  const gint64 start = g_get_monotonic_time();
  const uint32_t invocation = analysis->callback_invocations + 1;
  const uint64_t revision = analysis->revision;
  const uint64_t generation = analysis->generation;
  const uint32_t crop_x = analysis->crop_x;
  const uint32_t crop_y = analysis->crop_y;
  const uint32_t crop_width = analysis->crop_width;
  const uint32_t crop_height = analysis->crop_height;
  memset(analysis, 0, sizeof(*analysis));
  analysis->revision = revision;
  analysis->generation = generation;
  analysis->callback_invocations = invocation;
  analysis->crop_x = crop_x;
  analysis->crop_y = crop_y;
  analysis->crop_width = crop_width;
  analysis->crop_height = crop_height;

  if(!input || width <= 0 || height <= 0)
  {
    _analysis_error(analysis, "pre-gamma analysis received an invalid buffer");
    goto done;
  }
  if(colorspace != IOP_CS_RGB)
  {
    _analysis_error(analysis, "pre-gamma analysis buffer is not RGB");
    goto done;
  }
  if(!profile_info)
  {
    _analysis_error(analysis, "pre-gamma analysis has no output profile identity");
    goto done;
  }
  if((size_t)width > G_MAXUINT32 / (size_t)height)
  {
    _analysis_error(analysis, "pre-gamma analysis pixel count exceeds uint32");
    goto done;
  }

  const gboolean cropped = crop_width && crop_height;
  if(cropped && ((uint64_t)crop_x + crop_width > (uint64_t)width ||
                 (uint64_t)crop_y + crop_height > (uint64_t)height))
  {
    _analysis_error(analysis, "pre-gamma histogram crop is outside the completed buffer");
    goto done;
  }
  const dt_histogram_roi_t roi = {
      .width = width,
      .height = height,
      .crop_x = cropped ? crop_x : 0,
      .crop_y = cropped ? crop_y : 0,
      .crop_right = cropped ? (uint32_t)width - crop_x - crop_width : 0,
      .crop_bottom = cropped ? (uint32_t)height - crop_y - crop_height : 0,
  };
  dt_dev_histogram_collection_params_t params = {
      .roi = &roi,
      .bins_count = DT_REMOTE_HISTOGRAM_BINS,
  };
  dt_dev_histogram_stats_t stats = {0};
  uint32_t maximum[4] = {0};
  uint32_t *interleaved = NULL;
  dt_histogram_helper(&params, &stats, IOP_CS_RGB, IOP_CS_RGB, input, &interleaved, maximum, FALSE,
                      profile_info);
  if(!interleaved || stats.bins_count != DT_REMOTE_HISTOGRAM_BINS || stats.ch != 3 ||
     stats.pixels != (cropped ? crop_width * crop_height
                              : (uint32_t)width * (uint32_t)height))
  {
    if(interleaved)
      dt_free_align(interleaved);
    _analysis_error(analysis, "pre-gamma histogram helper returned an invalid result");
    goto done;
  }

  for(size_t bin = 0; bin < DT_REMOTE_HISTOGRAM_BINS; bin++)
  {
    analysis->red[bin] = interleaved[bin * 4];
    analysis->green[bin] = interleaved[bin * 4 + 1];
    analysis->blue[bin] = interleaved[bin * 4 + 2];
  }
  dt_free_align(interleaved);
  analysis->width = cropped ? crop_width : (uint32_t)width;
  analysis->height = cropped ? crop_height : (uint32_t)height;
  analysis->sampled_pixels = stats.pixels;
  analysis->profile_type = profile_info->type;
  analysis->profile_intent = profile_info->intent;
  g_strlcpy(analysis->profile_filename, profile_info->filename, sizeof(analysis->profile_filename));
  analysis->valid = TRUE;

done:
  analysis->elapsed_ms = (double)(g_get_monotonic_time() - start) / 1000.0;
}

static uint64_t _channel_total(const uint32_t *channel)
{
  uint64_t total = 0;
  for(size_t bin = 0; bin < DT_REMOTE_HISTOGRAM_BINS; bin++)
    total += channel[bin];
  return total;
}

static void _checksum_uint32(GChecksum *checksum, uint32_t value)
{
  const uint32_t encoded = GUINT32_TO_BE(value);
  g_checksum_update(checksum, (const guchar *)&encoded, sizeof(encoded));
}

static void _checksum_uint64(GChecksum *checksum, uint64_t value)
{
  const uint64_t encoded = GUINT64_TO_BE(value);
  g_checksum_update(checksum, (const guchar *)&encoded, sizeof(encoded));
}

gboolean dt_remote_analysis_finalize(dt_remote_analysis_t *analysis, const char *state_digest,
                                     const char *pixel_digest, uint32_t surface_width,
                                     uint32_t surface_height, char **error)
{
  if(error)
    *error = NULL;
  if(!analysis || !analysis->valid)
  {
    _set_error(error, "pixelpipe produced no valid pre-gamma histogram%s%s",
               analysis && analysis->error[0] ? ": " : "",
               analysis && analysis->error[0] ? analysis->error : "");
    return FALSE;
  }
  if(!state_digest || !pixel_digest)
  {
    _set_error(error, "cannot bind histogram without state and pixel digests");
    return FALSE;
  }
  if(analysis->width != surface_width || analysis->height != surface_height)
  {
    _set_error(error, "histogram and surface dimensions do not match");
    return FALSE;
  }
  // colorout's profile-info cache is keyed by type/filename rather than
  // intent, so its interned object's intent can reflect an earlier lookup.
  // The worker separately installs and state-digests relative-colorimetric
  // output; validate the stable profile identity carried by this tap here.
  if(analysis->profile_type != DT_COLORSPACE_SRGB || analysis->profile_filename[0])
  {
    _set_error(error,
               "histogram output profile is not built-in sRGB"
               " (type=%d intent=%d filename=%s)",
               analysis->profile_type, analysis->profile_intent,
               analysis->profile_filename[0] ? analysis->profile_filename : "<built-in>");
    return FALSE;
  }
  if(_channel_total(analysis->red) != analysis->sampled_pixels ||
     _channel_total(analysis->green) != analysis->sampled_pixels ||
     _channel_total(analysis->blue) != analysis->sampled_pixels)
  {
    _set_error(error, "histogram channel totals do not match sampled pixels");
    return FALSE;
  }

  GChecksum *checksum = g_checksum_new(G_CHECKSUM_SHA256);
  const char separator = '\0';
  g_checksum_update(checksum, (const guchar *)DT_REMOTE_HISTOGRAM_DOMAIN,
                    strlen(DT_REMOTE_HISTOGRAM_DOMAIN));
  g_checksum_update(checksum, (const guchar *)&separator, 1);
  g_checksum_update(checksum, (const guchar *)state_digest, strlen(state_digest));
  g_checksum_update(checksum, (const guchar *)&separator, 1);
  g_checksum_update(checksum, (const guchar *)pixel_digest, strlen(pixel_digest));
  _checksum_uint64(checksum, analysis->revision);
  _checksum_uint64(checksum, analysis->generation);
  _checksum_uint32(checksum, analysis->width);
  _checksum_uint32(checksum, analysis->height);
  _checksum_uint32(checksum, analysis->sampled_pixels);
  for(size_t bin = 0; bin < DT_REMOTE_HISTOGRAM_BINS; bin++)
  {
    _checksum_uint32(checksum, analysis->red[bin]);
    _checksum_uint32(checksum, analysis->green[bin]);
    _checksum_uint32(checksum, analysis->blue[bin]);
  }
  g_snprintf(analysis->pixelpipe_result_digest, sizeof(analysis->pixelpipe_result_digest),
             "sha256:%s", g_checksum_get_string(checksum));
  g_checksum_free(checksum);
  return TRUE;
}
