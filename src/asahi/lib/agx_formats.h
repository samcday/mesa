/*
 * Copyright 2021 Alyssa Rosenzweig
 * SPDX-License-Identifier: MIT
 *
 */

#pragma once

#include "util/format/u_format.h"

struct agx_pixel_format_entry {
   uint8_t channels;
   uint8_t type;
   bool texturable : 1;
   enum pipe_format renderable;
};

extern const struct agx_pixel_format_entry agx_pixel_format[PIPE_FORMAT_COUNT];

static inline bool
agx_is_valid_pixel_format(enum pipe_format format)
{
   return agx_pixel_format[format].texturable;
}

struct agx_border_packed;

void agx_pack_border(struct agx_border_packed *out, const uint32_t in[4],
                     enum pipe_format format);
