/*
 * Copyright 2026 Khronos_AppleICDs contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "util/u_dynarray.h"

struct agx_device;
struct agx_bo;
struct agx_submit_virt;

/* This is the platform boundary between Gallium batch finalization and a
 * submission transport. It describes AGX work without exposing Linux DRM UAPI
 * structures to a non-Linux winsys. */
enum agx_submit_sync_type {
   AGX_SUBMIT_SYNC_BINARY = 0,
   AGX_SUBMIT_SYNC_TIMELINE,
};

struct agx_submit_sync {
   enum agx_submit_sync_type type;
   uint32_t handle;
   uint64_t timeline_value;
};

struct agx_submit_attachment {
   uint64_t gpu_va;
   uint64_t size_B;
};

struct agx_submit_zls_buffer {
   uint64_t base;
   uint64_t compression_base;
   uint32_t stride_B;
   uint32_t compression_stride_B;
};

struct agx_submit_timestamp {
   uint32_t object;
   uint32_t offset_B;
};

struct agx_submit_timestamps {
   struct agx_submit_timestamp start;
   struct agx_submit_timestamp end;
};

struct agx_submit_helper {
   uint32_t binary;
   uint32_t config;
   uint64_t data;
};

struct agx_submit_bg_eot {
   uint32_t usc;
   uint32_t resource_specifier;
};

struct agx_submit_render {
   uint32_t flags;
   uint32_t isp_zls_pixels;
   uint64_t vdm_ctrl_stream_base;
   struct agx_submit_helper vertex_helper;
   struct agx_submit_helper fragment_helper;
   uint64_t isp_scissor_base;
   uint64_t isp_dbias_base;
   uint64_t isp_oclqry_base;
   struct agx_submit_zls_buffer depth;
   struct agx_submit_zls_buffer stencil;
   uint64_t zls_ctrl;
   uint64_t ppp_multisamplectl;
   uint64_t sampler_heap;
   uint32_t ppp_ctrl;
   uint16_t width_px;
   uint16_t height_px;
   uint16_t layers;
   uint16_t sampler_count;
   uint8_t utile_width_px;
   uint8_t utile_height_px;
   uint8_t samples;
   uint8_t sample_size_B;
   uint32_t isp_merge_upper_x;
   uint32_t isp_merge_upper_y;
   struct agx_submit_bg_eot bg;
   struct agx_submit_bg_eot eot;
   struct agx_submit_bg_eot partial_bg;
   struct agx_submit_bg_eot partial_eot;
   uint32_t isp_bgobjdepth;
   uint32_t isp_bgobjvals;
   struct agx_submit_timestamps ts_vtx;
   struct agx_submit_timestamps ts_frag;
   const struct agx_submit_attachment *attachments;
   uint32_t attachment_count;
};

struct agx_submit_compute {
   uint32_t flags;
   uint32_t sampler_count;
   uint64_t cdm_ctrl_stream_base;
   uint64_t cdm_ctrl_stream_end;
   uint64_t sampler_heap;
   struct agx_submit_helper helper;
   struct agx_submit_timestamps timestamps;
};

enum agx_submit_command_type {
   AGX_SUBMIT_COMMAND_RENDER = 0,
   AGX_SUBMIT_COMMAND_COMPUTE,
};

/* Identifies the Mesa BO that owns an AGX encoder stream. Render streams are
 * self-terminating, so their size may be zero when only the stream base is
 * known at submission finalization. */
enum agx_submit_encoder_type {
   AGX_SUBMIT_ENCODER_RENDER = 0,
   AGX_SUBMIT_ENCODER_COMPUTE,
};

struct agx_submit_encoder {
   enum agx_submit_encoder_type type;
   struct agx_bo *bo;
   uint64_t gpu_va;
   uint64_t size_B;
};

/* A command-visible object retains the Mesa BO from which its native
 * counterpart will eventually be created. This is not a Linux GEM-bind
 * request. */
enum agx_submit_object_type {
   AGX_SUBMIT_OBJECT_TIMESTAMPS = 0,
};

struct agx_submit_object {
   enum agx_submit_object_type type;
   uint32_t handle;
   struct agx_bo *bo;
};

/* Retains the complete Asahi batch dependency set. The platform transport can
 * use this table for residency and lifetime without reinterpreting command
 * bytes or Linux DRM handles. */
struct agx_submit_resource {
   struct agx_bo *bo;
   uint64_t gpu_va;
   uint64_t size_B;
};

#define AGX_SUBMIT_BARRIER_NONE UINT16_MAX

struct agx_submit_command {
   enum agx_submit_command_type type;
   uint16_t vdm_barrier;
   uint16_t cdm_barrier;

   union {
      struct agx_submit_render render;
      struct agx_submit_compute compute;
   };
};

struct agx_submit_info {
   uint32_t queue_id;
   const struct agx_submit_sync *syncs;
   uint32_t in_sync_count;
   uint32_t out_sync_count;
   const struct agx_submit_command *commands;
   uint32_t command_count;
   const struct agx_submit_encoder *encoders;
   uint32_t encoder_count;
   const struct agx_submit_object *objects;
   uint32_t object_count;
   const struct agx_submit_resource *resources;
   uint32_t resource_count;
};

/* Validates the platform-neutral ownership shape before a transport consumes
 * the finalized batch. It deliberately does not interpret GPU addresses. */
bool agx_submit_info_validate(const struct agx_submit_info *info);

/* The Linux transport serializer is intentionally the only place that turns
 * this neutral description into drm_asahi_submit data. */
int agx_submit_info_submit_linux(struct agx_device *dev,
                                 const struct agx_submit_info *info,
                                 struct agx_submit_virt *virt);

/* Produces a temporary Linux packet only for Linux diagnostics and transport.
 * The caller owns the initialized dynarray and must fini it. */
bool agx_submit_info_encode_drm_commands(const struct agx_submit_info *info,
                                         struct util_dynarray *cmdbuf);
