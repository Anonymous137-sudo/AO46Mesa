/*
 * Copyright 2026 Khronos_AppleICDs contributors
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include "agx_device.h"
#include "agx_submit.h"

struct agx_submit_capture {
   bool seen;
   bool valid;
};

static struct agx_submit_capture capture;

static int
capture_linux_submit(struct agx_device *dev, struct drm_asahi_submit *submit,
                     struct agx_submit_virt *virt)
{
   const struct drm_asahi_sync *syncs =
      (const struct drm_asahi_sync *)(uintptr_t)submit->syncs;
   const uint8_t *cursor = (const uint8_t *)(uintptr_t)submit->cmdbuf;
   const uint8_t *end = cursor + submit->cmdbuf_size;
   struct drm_asahi_cmd_header header;
   struct drm_asahi_cmd_compute compute;
   struct drm_asahi_attachment attachment;
   struct drm_asahi_cmd_render render;

   (void)dev;
   (void)virt;
   capture.seen = true;

   if (submit->flags != 0 || submit->queue_id != 7 ||
       submit->in_sync_count != 1 || submit->out_sync_count != 2 ||
       !syncs || !cursor ||
       syncs[0].sync_type != DRM_ASAHI_SYNC_SYNCOBJ ||
       syncs[0].handle != 0x11 || syncs[1].sync_type != DRM_ASAHI_SYNC_SYNCOBJ ||
       syncs[1].handle != 0x22 ||
       syncs[2].sync_type != DRM_ASAHI_SYNC_TIMELINE_SYNCOBJ ||
       syncs[2].handle != 0x33 || syncs[2].timeline_value != 0x44) {
      return -1;
   }

   memcpy(&header, cursor, sizeof(header));
   cursor += sizeof(header);
   if (header.cmd_type != DRM_ASAHI_CMD_COMPUTE ||
       header.size != sizeof(compute) || header.vdm_barrier != 0 ||
       header.cdm_barrier != 0 || cursor + sizeof(compute) > end) {
      return -1;
   }
   memcpy(&compute, cursor, sizeof(compute));
   cursor += sizeof(compute);
   if (compute.cdm_ctrl_stream_base != UINT64_C(0x100000) ||
       compute.cdm_ctrl_stream_end != UINT64_C(0x100200) ||
       compute.sampler_heap != UINT64_C(0x100400) ||
       compute.helper.binary != 0x55 || compute.helper.cfg != 0x66 ||
       compute.helper.data != UINT64_C(0x100800) ||
       compute.ts.start.handle != 0x77 || compute.ts.end.offset != 0x88) {
      return -1;
   }

   memcpy(&header, cursor, sizeof(header));
   cursor += sizeof(header);
   if (header.cmd_type != DRM_ASAHI_SET_FRAGMENT_ATTACHMENTS ||
       header.size != sizeof(attachment) ||
       header.vdm_barrier != DRM_ASAHI_BARRIER_NONE ||
       header.cdm_barrier != DRM_ASAHI_BARRIER_NONE ||
       cursor + sizeof(attachment) > end) {
      return -1;
   }
   memcpy(&attachment, cursor, sizeof(attachment));
   cursor += sizeof(attachment);
   if (attachment.pointer != UINT64_C(0x200000) || attachment.size != 0x4000) {
      return -1;
   }

   memcpy(&header, cursor, sizeof(header));
   cursor += sizeof(header);
   if (header.cmd_type != DRM_ASAHI_CMD_RENDER ||
       header.size != sizeof(render) ||
       header.vdm_barrier != DRM_ASAHI_BARRIER_NONE ||
       header.cdm_barrier != 1 || cursor + sizeof(render) != end) {
      return -1;
   }
   memcpy(&render, cursor, sizeof(render));
   if (render.vdm_ctrl_stream_base != UINT64_C(0x300000) ||
       render.sampler_heap != UINT64_C(0x300400) || render.width_px != 64 ||
       render.height_px != 32 || render.layers != 1 || render.samples != 1 ||
       render.vertex_helper.binary != 0x99 ||
       render.fragment_helper.data != UINT64_C(0x300800)) {
      return -1;
   }

   capture.valid = true;
   return 0;
}

int
main(void)
{
   struct agx_va compute_va = {
      .addr = UINT64_C(0x100000),
      .size_B = 0x1000,
   };
   struct agx_va render_va = {
      .addr = UINT64_C(0x300000),
      .size_B = 0x1000,
   };
   struct agx_va attachment_va = {
      .addr = UINT64_C(0x200000),
      .size_B = 0x4000,
   };
   struct agx_bo compute_bo = {.va = &compute_va};
   struct agx_bo render_bo = {.va = &render_va};
   struct agx_bo timestamp_bo = {.va = &compute_va};
   struct agx_bo attachment_bo = {.va = &attachment_va};
   const struct agx_submit_sync syncs[] = {
      {.type = AGX_SUBMIT_SYNC_BINARY, .handle = 0x11},
      {.type = AGX_SUBMIT_SYNC_BINARY, .handle = 0x22},
      {.type = AGX_SUBMIT_SYNC_TIMELINE, .handle = 0x33, .timeline_value = 0x44},
   };
   const struct agx_submit_attachment attachments[] = {
      {.gpu_va = UINT64_C(0x200000), .size_B = 0x4000},
   };
   const struct agx_submit_command commands[] = {
      {
         .type = AGX_SUBMIT_COMMAND_COMPUTE,
         .vdm_barrier = 0,
         .cdm_barrier = 0,
         .compute = {
            .sampler_count = 3,
            .cdm_ctrl_stream_base = UINT64_C(0x100000),
            .cdm_ctrl_stream_end = UINT64_C(0x100200),
            .sampler_heap = UINT64_C(0x100400),
            .helper = {.binary = 0x55, .config = 0x66,
                       .data = UINT64_C(0x100800)},
            .timestamps = {
               .start = {.object = 0x77},
               .end = {.offset_B = 0x88},
            },
         },
      },
      {
         .type = AGX_SUBMIT_COMMAND_RENDER,
         .vdm_barrier = AGX_SUBMIT_BARRIER_NONE,
         .cdm_barrier = 1,
         .render = {
            .vdm_ctrl_stream_base = UINT64_C(0x300000),
            .sampler_heap = UINT64_C(0x300400),
            .width_px = 64,
            .height_px = 32,
            .layers = 1,
            .samples = 1,
            .vertex_helper = {.binary = 0x99},
            .fragment_helper = {.data = UINT64_C(0x300800)},
            .attachments = attachments,
            .attachment_count = 1,
         },
      },
   };
   struct agx_submit_encoder encoders[] = {
      {
         .type = AGX_SUBMIT_ENCODER_COMPUTE,
         .bo = &compute_bo,
         .gpu_va = UINT64_C(0x100000),
         .size_B = 0x200,
      },
      {
         .type = AGX_SUBMIT_ENCODER_RENDER,
         .bo = &render_bo,
         .gpu_va = UINT64_C(0x300000),
      },
   };
   struct agx_submit_object objects[] = {
      {
         .type = AGX_SUBMIT_OBJECT_TIMESTAMPS,
         .handle = 0x77,
         .bo = &timestamp_bo,
      },
   };
   struct agx_submit_resource resources[] = {
      {.bo = &compute_bo, .gpu_va = compute_va.addr, .size_B = compute_va.size_B},
      {.bo = &render_bo, .gpu_va = render_va.addr, .size_B = render_va.size_B},
      {.bo = &attachment_bo, .gpu_va = attachment_va.addr,
       .size_B = attachment_va.size_B},
      {.bo = &timestamp_bo, .gpu_va = compute_va.addr, .size_B = compute_va.size_B},
   };
   const struct agx_submit_info info = {
      .queue_id = 7,
      .syncs = syncs,
      .in_sync_count = 1,
      .out_sync_count = 2,
      .commands = commands,
      .command_count = 2,
      .encoders = encoders,
      .encoder_count = 2,
      .objects = objects,
      .object_count = 1,
      .resources = resources,
      .resource_count = 4,
   };
   struct agx_device device = {
      .ops.submit = capture_linux_submit,
   };

   if (agx_submit_info_submit_linux(&device, &info, NULL) != 0 ||
       !capture.seen || !capture.valid) {
      fputs("AGX_SUBMIT_INFO_SMOKE Linux transport serialization failed\n",
            stderr);
      return 1;
   }

   {
      struct agx_submit_info malformed = info;
      struct agx_submit_command malformed_commands[2];
      struct agx_submit_encoder malformed_encoders[2];
      struct agx_submit_object malformed_objects[1];
      struct agx_submit_object malformed_duplicate_objects[2];
      struct agx_submit_resource malformed_resources[4];
      struct agx_submit_sync malformed_syncs[3];

      malformed_commands[0] = commands[0];
      malformed_commands[1] = commands[1];
      malformed_encoders[0] = encoders[0];
      malformed_encoders[1] = encoders[1];
      malformed_objects[0] = objects[0];
      malformed_duplicate_objects[0] = objects[0];
      malformed_duplicate_objects[1] = objects[0];
      memcpy(malformed_resources, resources, sizeof(resources));
      memcpy(malformed_syncs, syncs, sizeof(syncs));

      malformed = info;
      malformed.queue_id = 0;
      if (agx_submit_info_validate(&malformed)) {
         fputs("AGX_SUBMIT_INFO_SMOKE accepted a zero queue ID\n", stderr);
         return 1;
      }

      malformed = info;
      malformed.syncs = NULL;
      if (agx_submit_info_validate(&malformed)) {
         fputs("AGX_SUBMIT_INFO_SMOKE accepted missing sync storage\n", stderr);
         return 1;
      }

      malformed = info;
      malformed_syncs[0].timeline_value = 1;
      malformed.syncs = malformed_syncs;
      if (agx_submit_info_validate(&malformed)) {
         fputs("AGX_SUBMIT_INFO_SMOKE accepted a binary timeline value\n",
               stderr);
         return 1;
      }

      memcpy(malformed_syncs, syncs, sizeof(syncs));
      malformed = info;
      malformed_syncs[2].timeline_value = 0;
      malformed.syncs = malformed_syncs;
      if (agx_submit_info_validate(&malformed)) {
         fputs("AGX_SUBMIT_INFO_SMOKE accepted a zero timeline value\n",
               stderr);
         return 1;
      }

      memcpy(malformed_syncs, syncs, sizeof(syncs));
      malformed = info;
      malformed_syncs[2].handle = 0;
      malformed.syncs = malformed_syncs;
      if (agx_submit_info_validate(&malformed)) {
         fputs("AGX_SUBMIT_INFO_SMOKE accepted a zero sync handle\n", stderr);
         return 1;
      }

      malformed = info;
      malformed_commands[1].render.attachments = NULL;
      malformed.commands = malformed_commands;
      if (agx_submit_info_validate(&malformed)) {
         fputs("AGX_SUBMIT_INFO_SMOKE accepted missing attachment storage\n",
               stderr);
         return 1;
      }

      malformed = info;
      malformed_encoders[0].bo = NULL;
      malformed.encoders = malformed_encoders;
      if (agx_submit_info_validate(&malformed)) {
         fputs("AGX_SUBMIT_INFO_SMOKE accepted a missing encoder owner\n",
               stderr);
         return 1;
      }

      malformed = info;
      malformed_objects[0].handle = 0;
      malformed.objects = malformed_objects;
      if (agx_submit_info_validate(&malformed)) {
         fputs("AGX_SUBMIT_INFO_SMOKE accepted a zero object handle\n", stderr);
         return 1;
      }

      malformed = info;
      malformed.objects = malformed_duplicate_objects;
      malformed.object_count = 2;
      if (agx_submit_info_validate(&malformed)) {
         fputs("AGX_SUBMIT_INFO_SMOKE accepted duplicate timestamp objects\n",
               stderr);
         return 1;
      }

      malformed = info;
      malformed_resources[2].bo = NULL;
      malformed.resources = malformed_resources;
      if (agx_submit_info_validate(&malformed)) {
         fputs("AGX_SUBMIT_INFO_SMOKE accepted a missing resource owner\n",
               stderr);
         return 1;
      }
   }

   puts("AGX_SUBMIT_INFO_SMOKE passed");
   return 0;
}
