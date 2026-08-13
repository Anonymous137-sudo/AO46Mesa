/*
 * Copyright 2010 Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

struct pipe_screen;
struct renderonly;
struct agx_device;

struct pipe_screen *agx_screen_create(int fd, struct renderonly *ro,
                                      const struct pipe_screen_config *config);

/* macOS owns device discovery outside DRM. This factory transfers a complete
 * native device into Gallium after the direct winsys has installed BO, sync,
 * and submit operations. It never accepts a Linux fd or renderonly object. */
struct pipe_screen *agx_screen_create_macos(
   struct agx_device *device, const struct pipe_screen_config *config);

#ifdef __cplusplus
}
#endif
