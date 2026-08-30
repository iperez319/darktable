/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#pragma once

#include "remote/remote_editor_state.h"

gboolean dt_remote_masks_validate(const char *masks_json, char **error);
gboolean dt_remote_masks_apply(dt_remote_editor_state_facade_t *facade,
                               dt_develop_t *dev,
                               dt_iop_module_t *exposure_module,
                               double crop_x,
                               double crop_y,
                               double crop_width,
                               double crop_height,
                               const char *masks_json,
                               char **error);
void dt_remote_masks_cleanup(dt_remote_editor_state_facade_t *facade);
