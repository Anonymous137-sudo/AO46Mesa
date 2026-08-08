/*
 * Copyright 2026 Khronos_AppleICDs contributors
 * SPDX-License-Identifier: MIT
 */

#include "agx_sync.h"

#include <errno.h>

#include "agx_device.h"

#if defined(MESA_SYSTEM_HAS_KMS_DRM) && MESA_SYSTEM_HAS_KMS_DRM
#include <xf86drm.h>

int
agx_sync_create(struct agx_device *dev, uint32_t flags, uint32_t *handle)
{
   uint32_t drm_flags =
      flags & AGX_SYNC_CREATE_SIGNALED ? DRM_SYNCOBJ_CREATE_SIGNALED : 0;

   return drmSyncobjCreate(dev->fd, drm_flags, handle);
}

int
agx_sync_destroy(struct agx_device *dev, uint32_t handle)
{
   return drmSyncobjDestroy(dev->fd, handle);
}

int
agx_sync_wait(struct agx_device *dev, const uint32_t *handles,
              uint32_t handle_count, uint64_t timeout, uint32_t flags,
              uint32_t *first_signaled)
{
   uint32_t drm_flags =
      flags & AGX_SYNC_WAIT_ALL ? DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL : 0;

   return drmSyncobjWait(dev->fd, handles, handle_count, timeout, drm_flags,
                         first_signaled);
}

int
agx_sync_export_fd(struct agx_device *dev, uint32_t handle, int *fd)
{
   return drmSyncobjExportSyncFile(dev->fd, handle, fd);
}

int
agx_sync_import_fd(struct agx_device *dev, uint32_t handle, int fd)
{
   return drmSyncobjImportSyncFile(dev->fd, handle, fd);
}

int
agx_sync_fd_to_handle(struct agx_device *dev, int fd, uint32_t *handle)
{
   return drmSyncobjFDToHandle(dev->fd, fd, handle);
}
#else
/* macOS AGX completion records are observed but not decoded yet. Returning
 * ENOTSUP keeps the native driver from treating a CPU event as GPU completion. */
int
agx_sync_create(struct agx_device *dev, uint32_t flags, uint32_t *handle)
{
   (void)dev;
   (void)flags;
   (void)handle;
   return -ENOTSUP;
}

int
agx_sync_destroy(struct agx_device *dev, uint32_t handle)
{
   (void)dev;
   (void)handle;
   return -ENOTSUP;
}

int
agx_sync_wait(struct agx_device *dev, const uint32_t *handles,
              uint32_t handle_count, uint64_t timeout, uint32_t flags,
              uint32_t *first_signaled)
{
   (void)dev;
   (void)handles;
   (void)handle_count;
   (void)timeout;
   (void)flags;
   (void)first_signaled;
   return -ENOTSUP;
}

int
agx_sync_export_fd(struct agx_device *dev, uint32_t handle, int *fd)
{
   (void)dev;
   (void)handle;
   (void)fd;
   return -ENOTSUP;
}

int
agx_sync_import_fd(struct agx_device *dev, uint32_t handle, int fd)
{
   (void)dev;
   (void)handle;
   (void)fd;
   return -ENOTSUP;
}

int
agx_sync_fd_to_handle(struct agx_device *dev, int fd, uint32_t *handle)
{
   (void)dev;
   (void)fd;
   (void)handle;
   return -ENOTSUP;
}
#endif
