/*
 * Based on panfrost/pan_fence.c:
 *
 * Copyright 2022 Amazon.com, Inc. or its affiliates.
 * Copyright 2008 VMware, Inc.
 * Copyright 2014 Broadcom
 * Copyright 2018 Alyssa Rosenzweig
 * Copyright 2019 Collabora, Ltd.
 * Copyright 2012 Rob Clark
 * SPDX-License-Identifier: MIT
 */

#include <unistd.h>

#include "agx_fence.h"
#include "agx_state.h"
#include "agx_sync.h"

#include "asahi/lib/agx_macos_mesa_device.h"
#include "util/libsync.h"
#include "util/os_time.h"
#include "util/u_inlines.h"

void
agx_fence_reference(struct pipe_screen *pscreen, struct pipe_fence_handle **ptr,
                    struct pipe_fence_handle *fence)
{
   struct agx_device *dev = agx_device(pscreen);
   struct pipe_fence_handle *old = *ptr;

   if (pipe_reference(old ? &old->reference : NULL,
                      fence ? &fence->reference : NULL)) {
      int ret = agx_sync_destroy(dev, old->syncobj);

      /* A completion-backed macOS sync cannot be discarded while it still
       * owns Mesa BO references. Existing callers must finish it first. */
      assert(ret == 0);
      free(old);
   }

   *ptr = fence;
}

bool
agx_fence_finish(struct pipe_screen *pscreen, struct pipe_context *ctx,
                 struct pipe_fence_handle *fence, uint64_t timeout)
{
   struct agx_device *dev = agx_device(pscreen);
   int ret;

   if (fence->signaled)
      return true;

   uint64_t abs_timeout = os_time_get_absolute_timeout(timeout);
   if (abs_timeout == OS_TIMEOUT_INFINITE)
      abs_timeout = INT64_MAX;

   ret = agx_sync_wait(dev, &fence->syncobj, 1, abs_timeout,
                       AGX_SYNC_WAIT_ALL, NULL);

   /* A live macOS completion queue can disappear with its AGX session. That
    * is a failed wait, not an invariant violation in Gallium's fence API. */
   if (fence->macos_native && ret < 0 && ret != -ETIME)
      return false;

   assert(ret >= 0 || ret == -ETIME);
   fence->signaled = (ret >= 0);
   return fence->signaled;
}

int
agx_fence_get_fd(struct pipe_screen *screen, struct pipe_fence_handle *f)
{
   struct agx_device *dev = agx_device(screen);
   int fd = -1;

   if (f->macos_native)
      return -1;

   int ret = agx_sync_export_fd(dev, f->syncobj, &fd);
   assert(ret >= 0);
   assert(fd >= 0);

   return fd;
}

struct pipe_fence_handle *
agx_fence_from_fd(struct agx_context *ctx, int fd, enum pipe_fd_type type)
{
   struct agx_device *dev = agx_device(ctx->base.screen);
   int ret;

   if (agx_macos_mesa_sync_is_supported(dev))
      return NULL;

   struct pipe_fence_handle *f = calloc(1, sizeof(*f));
   if (!f)
      return NULL;

   if (type == PIPE_FD_TYPE_NATIVE_SYNC) {
      ret = agx_sync_create(dev, 0, &f->syncobj);
      if (ret) {
         agx_msg("create syncobj failed\n");
         goto err_free_fence;
      }

      ret = agx_sync_import_fd(dev, f->syncobj, fd);
      if (ret) {
         agx_msg("import syncfile failed\n");
         goto err_destroy_syncobj;
      }
   } else {
      assert(type == PIPE_FD_TYPE_SYNCOBJ);
      ret = agx_sync_fd_to_handle(dev, fd, &f->syncobj);
      if (ret) {
         agx_msg("import syncobj FD failed\n");
         goto err_free_fence;
      }
   }

   pipe_reference_init(&f->reference, 1);

   return f;

err_destroy_syncobj:
   agx_sync_destroy(dev, f->syncobj);
err_free_fence:
   free(f);
   return NULL;
}

struct pipe_fence_handle *
agx_fence_create(struct agx_context *ctx)
{
   struct agx_device *dev = agx_device(ctx->base.screen);
   int fd = -1, ret;

   if (agx_macos_mesa_sync_is_supported(dev)) {
      struct pipe_fence_handle *f = calloc(1, sizeof(*f));

      if (!f ||
          agx_macos_mesa_sync_reference(dev, ctx->syncobj) != 0) {
         free(f);
         return NULL;
      }

      *f = (struct pipe_fence_handle){
         .syncobj = ctx->syncobj,
         .macos_native = true,
      };
      pipe_reference_init(&f->reference, 1);
      return f;
   }

   /* Snapshot the last rendering out fence. We'd rather have another
    * syncobj instead of a sync file, but this is all we get.
    * (HandleToFD/FDToHandle just gives you another syncobj ID for the
    * same syncobj).
    */
   ret = agx_sync_export_fd(dev, ctx->syncobj, &fd);
   assert(ret >= 0 && fd != -1 && "export failed");
   if (ret || fd == -1) {
      agx_msg("export failed\n");
      return NULL;
   }

   struct pipe_fence_handle *f =
      agx_fence_from_fd(ctx, fd, PIPE_FD_TYPE_NATIVE_SYNC);

   close(fd);

   return f;
}

void
agx_create_fence_fd(struct pipe_context *pctx,
                    struct pipe_fence_handle **pfence, int fd,
                    enum pipe_fd_type type)
{
   *pfence = agx_fence_from_fd(agx_context(pctx), fd, type);
}

void
agx_fence_server_sync(struct pipe_context *pctx, struct pipe_fence_handle *f,
                      uint64_t value)
{
   struct agx_device *dev = agx_device(pctx->screen);
   struct agx_context *ctx = agx_context(pctx);
   int fd = -1, ret;
   assert(!value);

   if (agx_macos_mesa_sync_is_supported(dev)) {
      agx_msg("macOS native cross-context fence import is unavailable\n");
      return;
   }

   ret = agx_sync_export_fd(dev, f->syncobj, &fd);
   assert(!ret);

   sync_accumulate("asahi", &ctx->in_sync_fd, fd);
   close(fd);
}
