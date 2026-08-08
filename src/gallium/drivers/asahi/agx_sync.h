/*
 * Copyright 2026 Khronos_AppleICDs contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

struct agx_device;

enum agx_sync_create_flags {
   AGX_SYNC_CREATE_SIGNALED = 1u << 0,
};

enum agx_sync_wait_flags {
   AGX_SYNC_WAIT_ALL = 1u << 0,
};

int agx_sync_create(struct agx_device *dev, uint32_t flags, uint32_t *handle);
int agx_sync_destroy(struct agx_device *dev, uint32_t handle);
int agx_sync_wait(struct agx_device *dev, const uint32_t *handles,
                  uint32_t handle_count, uint64_t timeout, uint32_t flags,
                  uint32_t *first_signaled);
int agx_sync_export_fd(struct agx_device *dev, uint32_t handle, int *fd);
int agx_sync_import_fd(struct agx_device *dev, uint32_t handle, int fd);
int agx_sync_fd_to_handle(struct agx_device *dev, int fd, uint32_t *handle);
