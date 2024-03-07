/*
 * SPDX-License-Identifier: MIT
 */

#ifndef AMDGPU_DRM_PUBLIC_H
#define AMDGPU_DRM_PUBLIC_H

#include <stdbool.h>

struct pipe_screen;
struct pipe_screen_config;
struct renderonly;

struct pipe_screen *
amdgpu_drm_screen_create_renderonly(int fd, struct renderonly *ro,
                                   const struct pipe_screen_config *config);

#endif
