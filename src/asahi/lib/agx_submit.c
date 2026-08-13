/*
 * Copyright 2026 Khronos_AppleICDs contributors
 * SPDX-License-Identifier: MIT
 */

#include "agx_submit.h"

#include "agx_device.h"

#include <stdbool.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "util/u_dynarray.h"

static struct drm_asahi_helper_program
agx_submit_helper_to_drm(struct agx_submit_helper helper)
{
   return (struct drm_asahi_helper_program){
      .binary = helper.binary,
      .cfg = helper.config,
      .data = helper.data,
   };
}

static struct drm_asahi_zls_buffer
agx_submit_zls_buffer_to_drm(struct agx_submit_zls_buffer buffer)
{
   return (struct drm_asahi_zls_buffer){
      .base = buffer.base,
      .comp_base = buffer.compression_base,
      .stride = buffer.stride_B,
      .comp_stride = buffer.compression_stride_B,
   };
}

static struct drm_asahi_timestamps
agx_submit_timestamps_to_drm(struct agx_submit_timestamps timestamps)
{
   return (struct drm_asahi_timestamps){
      .start = {
         .handle = timestamps.start.object,
         .offset = timestamps.start.offset_B,
      },
      .end = {
         .handle = timestamps.end.object,
         .offset = timestamps.end.offset_B,
      },
   };
}

static struct drm_asahi_bg_eot
agx_submit_bg_eot_to_drm(struct agx_submit_bg_eot program)
{
   return (struct drm_asahi_bg_eot){
      .usc = program.usc,
      .rsrc_spec = program.resource_specifier,
   };
}

static struct drm_asahi_cmd_compute
agx_submit_compute_to_drm(const struct agx_submit_compute *compute)
{
   return (struct drm_asahi_cmd_compute){
      .flags = compute->flags,
      .sampler_count = compute->sampler_count,
      .cdm_ctrl_stream_base = compute->cdm_ctrl_stream_base,
      .cdm_ctrl_stream_end = compute->cdm_ctrl_stream_end,
      .sampler_heap = compute->sampler_heap,
      .helper = agx_submit_helper_to_drm(compute->helper),
      .ts = agx_submit_timestamps_to_drm(compute->timestamps),
   };
}

static struct drm_asahi_cmd_render
agx_submit_render_to_drm(const struct agx_submit_render *render)
{
   return (struct drm_asahi_cmd_render){
      .flags = render->flags,
      .isp_zls_pixels = render->isp_zls_pixels,
      .vdm_ctrl_stream_base = render->vdm_ctrl_stream_base,
      .vertex_helper = agx_submit_helper_to_drm(render->vertex_helper),
      .fragment_helper = agx_submit_helper_to_drm(render->fragment_helper),
      .isp_scissor_base = render->isp_scissor_base,
      .isp_dbias_base = render->isp_dbias_base,
      .isp_oclqry_base = render->isp_oclqry_base,
      .depth = agx_submit_zls_buffer_to_drm(render->depth),
      .stencil = agx_submit_zls_buffer_to_drm(render->stencil),
      .zls_ctrl = render->zls_ctrl,
      .ppp_multisamplectl = render->ppp_multisamplectl,
      .sampler_heap = render->sampler_heap,
      .ppp_ctrl = render->ppp_ctrl,
      .width_px = render->width_px,
      .height_px = render->height_px,
      .layers = render->layers,
      .sampler_count = render->sampler_count,
      .utile_width_px = render->utile_width_px,
      .utile_height_px = render->utile_height_px,
      .samples = render->samples,
      .sample_size_B = render->sample_size_B,
      .isp_merge_upper_x = render->isp_merge_upper_x,
      .isp_merge_upper_y = render->isp_merge_upper_y,
      .bg = agx_submit_bg_eot_to_drm(render->bg),
      .eot = agx_submit_bg_eot_to_drm(render->eot),
      .partial_bg = agx_submit_bg_eot_to_drm(render->partial_bg),
      .partial_eot = agx_submit_bg_eot_to_drm(render->partial_eot),
      .isp_bgobjdepth = render->isp_bgobjdepth,
      .isp_bgobjvals = render->isp_bgobjvals,
      .ts_vtx = agx_submit_timestamps_to_drm(render->ts_vtx),
      .ts_frag = agx_submit_timestamps_to_drm(render->ts_frag),
   };
}

static bool
agx_submit_timestamp_is_declared(const struct agx_submit_info *info,
                                 struct agx_submit_timestamp timestamp)
{
   if (timestamp.object == 0)
      return true;

   for (uint32_t i = 0; i < info->object_count; ++i) {
      if (info->objects[i].type == AGX_SUBMIT_OBJECT_TIMESTAMPS &&
          info->objects[i].handle == timestamp.object) {
         return true;
      }
   }

   return false;
}

bool
agx_submit_info_validate(const struct agx_submit_info *info)
{
   uint32_t sync_count;

   if (!info || info->queue_id == 0 || !info->commands || info->command_count == 0 ||
       info->in_sync_count > UINT32_MAX - info->out_sync_count) {
      return false;
   }

   sync_count = info->in_sync_count + info->out_sync_count;
   if (sync_count && !info->syncs)
      return false;
   if (info->encoder_count && !info->encoders)
      return false;
   if (info->encoder_count && info->encoder_count != info->command_count)
      return false;
   if (info->object_count && !info->objects)
      return false;
   if (info->resource_count && !info->resources)
      return false;

   for (uint32_t i = 0; i < info->object_count; ++i) {
      if (info->objects[i].type != AGX_SUBMIT_OBJECT_TIMESTAMPS ||
          info->objects[i].handle == 0 || !info->objects[i].bo) {
         return false;
      }

      /* Timestamp commands identify their target by this handle. Keep that
       * identity one-to-one before any platform transport consumes it. */
      for (uint32_t j = 0; j < i; ++j) {
         if (info->objects[j].type == info->objects[i].type &&
             info->objects[j].handle == info->objects[i].handle) {
            return false;
         }
      }
   }

   for (uint32_t i = 0; i < info->resource_count; ++i) {
      if (!info->resources[i].bo || info->resources[i].gpu_va == 0 ||
          info->resources[i].size_B == 0) {
         return false;
      }
   }

   for (uint32_t i = 0; i < sync_count; ++i) {
      const struct agx_submit_sync *sync = &info->syncs[i];

      if (sync->handle == 0 ||
          (sync->type != AGX_SUBMIT_SYNC_BINARY &&
           sync->type != AGX_SUBMIT_SYNC_TIMELINE) ||
          (sync->type == AGX_SUBMIT_SYNC_BINARY &&
           sync->timeline_value != 0) ||
          (sync->type == AGX_SUBMIT_SYNC_TIMELINE &&
           sync->timeline_value == 0)) {
         return false;
      }
   }

   for (uint32_t i = 0; i < info->command_count; ++i) {
      const struct agx_submit_command *command = &info->commands[i];

      switch (command->type) {
      case AGX_SUBMIT_COMMAND_COMPUTE:
         if (info->encoder_count) {
            const struct agx_submit_encoder *encoder = &info->encoders[i];

            if (!encoder->bo ||
                encoder->type != AGX_SUBMIT_ENCODER_COMPUTE ||
                encoder->gpu_va != command->compute.cdm_ctrl_stream_base ||
                command->compute.cdm_ctrl_stream_end <=
                   command->compute.cdm_ctrl_stream_base ||
                encoder->size_B != command->compute.cdm_ctrl_stream_end -
                                      command->compute.cdm_ctrl_stream_base) {
               return false;
            }
         }
         if (!agx_submit_timestamp_is_declared(info,
                                               command->compute.timestamps.start) ||
             !agx_submit_timestamp_is_declared(info,
                                               command->compute.timestamps.end)) {
            return false;
         }
         break;

      case AGX_SUBMIT_COMMAND_RENDER:
         if (command->render.attachment_count &&
             !command->render.attachments) {
            return false;
         }
         if (info->encoder_count) {
            const struct agx_submit_encoder *encoder = &info->encoders[i];

            if (!encoder->bo ||
                encoder->type != AGX_SUBMIT_ENCODER_RENDER ||
                encoder->gpu_va != command->render.vdm_ctrl_stream_base) {
               return false;
            }
         }
         if (!agx_submit_timestamp_is_declared(info,
                                               command->render.ts_vtx.start) ||
             !agx_submit_timestamp_is_declared(info,
                                               command->render.ts_vtx.end) ||
             !agx_submit_timestamp_is_declared(info,
                                               command->render.ts_frag.start) ||
             !agx_submit_timestamp_is_declared(info,
                                               command->render.ts_frag.end)) {
            return false;
         }
         break;

      default:
         return false;
      }
   }

   return true;
}

bool
agx_submit_info_encode_drm_commands(const struct agx_submit_info *info,
                                    struct util_dynarray *cmdbuf)
{
   if (!cmdbuf || !agx_submit_info_validate(info))
      return false;

   for (uint32_t i = 0; i < info->command_count; ++i) {
      const struct agx_submit_command *command = &info->commands[i];
      struct drm_asahi_cmd_header header = {
         .vdm_barrier = command->vdm_barrier,
         .cdm_barrier = command->cdm_barrier,
      };

      switch (command->type) {
      case AGX_SUBMIT_COMMAND_COMPUTE: {
         struct drm_asahi_cmd_compute compute =
            agx_submit_compute_to_drm(&command->compute);

         header.cmd_type = DRM_ASAHI_CMD_COMPUTE;
         header.size = sizeof(compute);
         util_dynarray_append(cmdbuf, header);
         util_dynarray_append(cmdbuf, compute);
         break;
      }

      case AGX_SUBMIT_COMMAND_RENDER: {
         const struct agx_submit_render *render = &command->render;
         struct drm_asahi_cmd_render drm_render =
            agx_submit_render_to_drm(render);

         if (render->attachment_count) {
            struct drm_asahi_cmd_header attachment_header = {
               .cmd_type = DRM_ASAHI_SET_FRAGMENT_ATTACHMENTS,
               .size = sizeof(struct drm_asahi_attachment) *
                       render->attachment_count,
               .vdm_barrier = DRM_ASAHI_BARRIER_NONE,
               .cdm_barrier = DRM_ASAHI_BARRIER_NONE,
            };

            util_dynarray_append(cmdbuf, attachment_header);
            for (uint32_t j = 0; j < render->attachment_count; ++j) {
               struct drm_asahi_attachment attachment = {
                  .pointer = render->attachments[j].gpu_va,
                  .size = render->attachments[j].size_B,
               };
               util_dynarray_append(cmdbuf, attachment);
            }
         }

         header.cmd_type = DRM_ASAHI_CMD_RENDER;
         header.size = sizeof(drm_render);
         util_dynarray_append(cmdbuf, header);
         util_dynarray_append(cmdbuf, drm_render);
         break;
      }

      default:
         return false;
      }
   }

   return true;
}

int
agx_submit_info_submit_linux(struct agx_device *dev,
                             const struct agx_submit_info *info,
                             struct agx_submit_virt *virt)
{
   struct util_dynarray cmdbuf = UTIL_DYNARRAY_INIT;
   struct drm_asahi_sync *syncs = NULL;
   struct drm_asahi_submit submit;
   uint32_t sync_count;
   int ret = -EINVAL;

   if (!dev || !dev->ops.submit || !agx_submit_info_validate(info)) {
      return -EINVAL;
   }

   if (!agx_submit_info_encode_drm_commands(info, &cmdbuf))
      goto fail;

   sync_count = info->in_sync_count + info->out_sync_count;
   if (sync_count) {
      syncs = calloc(sync_count, sizeof(*syncs));
      if (!syncs)
         goto fail;
   }

   for (uint32_t i = 0; i < sync_count; ++i) {
      switch (info->syncs[i].type) {
      case AGX_SUBMIT_SYNC_BINARY:
         syncs[i].sync_type = DRM_ASAHI_SYNC_SYNCOBJ;
         break;
      case AGX_SUBMIT_SYNC_TIMELINE:
         syncs[i].sync_type = DRM_ASAHI_SYNC_TIMELINE_SYNCOBJ;
         break;
      default:
         goto fail;
      }

      syncs[i].handle = info->syncs[i].handle;
      syncs[i].timeline_value = info->syncs[i].timeline_value;
   }

   if (cmdbuf.size > UINT32_MAX) {
      ret = -E2BIG;
      goto fail;
   }

   submit = (struct drm_asahi_submit){
      .flags = 0,
      .queue_id = info->queue_id,
      .in_sync_count = info->in_sync_count,
      .out_sync_count = info->out_sync_count,
      .syncs = (uintptr_t)syncs,
      .cmdbuf = (uintptr_t)cmdbuf.data,
      .cmdbuf_size = cmdbuf.size,
   };
   ret = dev->ops.submit(dev, &submit, virt);
   free(syncs);
   util_dynarray_fini(&cmdbuf);
   return ret;

fail:
   free(syncs);
   util_dynarray_fini(&cmdbuf);
   return ret ? ret : -EINVAL;
}
