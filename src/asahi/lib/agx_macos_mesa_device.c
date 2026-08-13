/*
 * Copyright 2026 Khronos_AppleICDs contributors
 * SPDX-License-Identifier: MIT
 */

#include "agx_macos_mesa_device.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agx_bo.h"
#include "asahi/compiler/agx_compile.h"
#include "agx_macos_queue.h"
#include "agx_macos_shader_provenance.h"
#include "agx_macos_uabi.h"
#include "util/os_time.h"
#include "util/sparse_array.h"

/* Apple reports native BO GPU VAs on the AGX 16 KiB page grid. Mesa's own
 * Asahi BO path uses the same page size; demanding 64 KiB rejects otherwise
 * valid, distinct native allocations without providing extra safety. */
#define AGX_MACOS_MESA_NATIVE_BO_ALIGNMENT AIL_PAGESIZE

/* One Apple completion can satisfy both the per-batch binary fence and Mesa's
 * flush timeline fence. Keep package ownership in this shared group instead
 * of making either output handle the accidental sole owner. */
enum agx_macos_mesa_sync_type {
   AGX_MACOS_MESA_SYNC_TYPE_UNSET = 0,
   AGX_MACOS_MESA_SYNC_TYPE_BINARY,
   AGX_MACOS_MESA_SYNC_TYPE_TIMELINE,
};

struct agx_macos_mesa_sync;

struct agx_macos_mesa_timeline_point {
   uint64_t value;
   bool complete;
   struct agx_macos_mesa_timeline_point *next;
};

struct agx_macos_mesa_sync_submission_output {
   struct agx_macos_mesa_sync *sync;
   struct agx_macos_mesa_timeline_point *timeline_point;
   enum agx_macos_mesa_sync_type type;
};

struct agx_macos_mesa_sync_submission {
   struct agx_macos_mesa_submission_package package;
   struct agx_macos_mesa_sync_submission *next;
   uint32_t output_count;
   struct agx_macos_mesa_sync_submission_output outputs[];
};

struct agx_macos_mesa_sync {
   uint32_t handle;
   uint32_t references;
   bool signaled;
   enum agx_macos_mesa_sync_type type;
   uint64_t completed_timeline_value;
   struct agx_macos_mesa_timeline_point *timeline_points;
   struct agx_macos_mesa_sync *next;
};

struct agx_macos_mesa_device_state {
   const struct agx_macos_device_session *session;
   struct agx_macos_bo_set *bo_set;
   const struct agx_macos_notification_queue *notification_queue;
   const struct agx_macos_mesa_bo_provider *bo_provider;
   struct agx_macos_uabi_contract uabi_contract;
   uint64_t api_generation;
   pthread_mutex_t sync_lock;
   struct agx_macos_mesa_sync *syncs;
   struct agx_macos_mesa_sync_submission *submissions;
   uint32_t next_sync_handle;
};

struct agx_macos_mesa_bo {
   enum {
      AGX_MACOS_MESA_BO_DIRECT = 0,
      AGX_MACOS_MESA_BO_APPLE_OWNED,
   } backing;
   struct agx_macos_bo native;
   struct agx_macos_mesa_platform_bo platform;
};

static struct agx_macos_mesa_device_state *
agx_macos_mesa_device_state(const struct agx_device *device)
{
   return device ? device->platform_data : NULL;
}

bool
agx_macos_mesa_device_is_current(const struct agx_device *device)
{
   const struct agx_macos_mesa_device_state *state =
      agx_macos_mesa_device_state(device);

   return state && state->session && state->bo_set &&
          agx_macos_device_session_is_current(state->session) &&
          state->api_generation != 0 &&
          state->session->api_generation == state->api_generation &&
          agx_macos_uabi_contract_is_current(&state->uabi_contract,
                                             state->session) &&
          agx_macos_bo_set_is_current(state->bo_set, state->session);
}

bool
agx_macos_mesa_sync_is_supported(const struct agx_device *device)
{
   const struct agx_macos_mesa_device_state *state =
      agx_macos_mesa_device_state(device);

   return agx_macos_mesa_device_is_current(device) && state &&
          agx_macos_uabi_contract_supports(
             &state->uabi_contract,
             AGX_MACOS_UABI_OPERATION_NOTIFICATION_QUEUE |
                AGX_MACOS_UABI_OPERATION_COMPLETION_POLL) &&
          state->notification_queue &&
          agx_macos_notification_queue_is_current(
             state->session, state->notification_queue);
}

uint32_t
agx_macos_mesa_device_capabilities(const struct agx_device *device)
{
   const struct agx_macos_mesa_device_state *state =
      agx_macos_mesa_device_state(device);
   uint32_t capabilities;

   if (!state || !agx_macos_mesa_device_is_current(device))
      return 0;

   capabilities = 0;
   if (agx_macos_uabi_contract_supports(
          &state->uabi_contract, AGX_MACOS_UABI_OPERATION_BO_ALLOCATE)) {
      capabilities |= AGX_MACOS_MESA_DEVICE_CAP_BO_ALLOC;
   }
   if (agx_macos_uabi_contract_supports(
          &state->uabi_contract, AGX_MACOS_UABI_OPERATION_BO_CPU_MAP)) {
      capabilities |= AGX_MACOS_MESA_DEVICE_CAP_BO_MAP;
   }
   if (agx_macos_uabi_contract_supports(
          &state->uabi_contract, AGX_MACOS_UABI_OPERATION_FIXED_VA_BIND)) {
      capabilities |= AGX_MACOS_MESA_DEVICE_CAP_FIXED_BO_BIND;
   }
   if (agx_macos_uabi_contract_supports(
          &state->uabi_contract, AGX_MACOS_UABI_OPERATION_VM_BIND)) {
      capabilities |= AGX_MACOS_MESA_DEVICE_CAP_VM_BIND;
   }
   if (agx_macos_uabi_contract_supports(
          &state->uabi_contract,
          AGX_MACOS_UABI_OPERATION_RESOURCE_BIND |
             AGX_MACOS_UABI_OPERATION_CARRIER_BUILD |
             AGX_MACOS_UABI_OPERATION_BATCH_SUBMIT)) {
      capabilities |= AGX_MACOS_MESA_DEVICE_CAP_SUBMIT;
   }
   if (agx_macos_mesa_sync_is_supported(device))
      capabilities |= AGX_MACOS_MESA_DEVICE_CAP_COMPLETION_SYNC;
   if (agx_macos_uabi_contract_supports(
          &state->uabi_contract, AGX_MACOS_UABI_OPERATION_OBJECT_BIND)) {
      capabilities |= AGX_MACOS_MESA_DEVICE_CAP_OBJECT_BIND;
   }
   if (agx_macos_uabi_contract_supports(
          &state->uabi_contract, AGX_MACOS_UABI_OPERATION_LOW_VA_BIND)) {
      capabilities |= AGX_MACOS_MESA_DEVICE_CAP_LOW_VA_BIND;
   }
   if (agx_macos_uabi_contract_supports(
          &state->uabi_contract, AGX_MACOS_UABI_OPERATION_EXECUTABLE_BO)) {
      capabilities |= AGX_MACOS_MESA_DEVICE_CAP_EXECUTABLE_BO;
   }
   if (agx_macos_uabi_contract_supports(
          &state->uabi_contract,
          AGX_MACOS_UABI_OPERATION_SHADER_CODE_ADMISSION)) {
      capabilities |= AGX_MACOS_MESA_DEVICE_CAP_SHADER_CODE_ADMISSION;
   }

   return capabilities;
}

uint32_t
agx_macos_mesa_device_missing_screen_capabilities(
   const struct agx_device *device)
{
   return AGX_MACOS_MESA_DEVICE_CAP_SCREEN_REQUIRED &
          ~agx_macos_mesa_device_capabilities(device);
}

static bool
agx_macos_mesa_is_screen_ready(struct agx_device *device)
{
   return agx_macos_mesa_device_missing_screen_capabilities(device) == 0;
}

static struct agx_macos_mesa_sync *
agx_macos_mesa_sync_find(struct agx_macos_mesa_device_state *state,
                         uint32_t handle)
{
   for (struct agx_macos_mesa_sync *sync = state ? state->syncs : NULL; sync;
        sync = sync->next) {
      if (sync->handle == handle)
         return sync;
   }

   return NULL;
}

/* A native Mesa fence may consume completions only from the queue that the
 * package was admitted against. Never let a separately initialized queue
 * transfer an in-flight package into this device's sync registry. */
static bool
agx_macos_mesa_sync_package_uses_current_queue(
   const struct agx_macos_mesa_device_state *state,
   const struct agx_macos_mesa_submission_package *package)
{
   const struct agx_macos_submission_lease *lease;

   if (!state || !state->notification_queue || !package)
      return false;

   lease = &package->native.lease;
   return lease->queue_lease_bound &&
          lease->queue_connection == state->notification_queue->connection &&
          lease->bound_queue_id == state->notification_queue->id &&
          lease->bound_queue_api_generation ==
             state->notification_queue->api_generation &&
          lease->bound_queue_api_generation == state->api_generation;
}

static void
agx_macos_mesa_sync_timeline_advance(struct agx_macos_mesa_sync *sync)
{
   struct agx_macos_mesa_timeline_point **link = &sync->timeline_points;

   while (*link && (*link)->complete &&
          (*link)->value == sync->completed_timeline_value + 1) {
      struct agx_macos_mesa_timeline_point *point = *link;

      sync->completed_timeline_value = point->value;
      *link = point->next;
      free(point);
   }

   sync->signaled = sync->completed_timeline_value != 0;
}

static struct agx_macos_mesa_timeline_point *
agx_macos_mesa_sync_timeline_find(
   const struct agx_macos_mesa_sync *sync, uint64_t value)
{
   for (struct agx_macos_mesa_timeline_point *point = sync->timeline_points;
        point; point = point->next) {
      if (point->value == value)
         return point;
   }

   return NULL;
}

static void
agx_macos_mesa_sync_timeline_insert(
   struct agx_macos_mesa_sync *sync,
   struct agx_macos_mesa_timeline_point *point)
{
   struct agx_macos_mesa_timeline_point **link = &sync->timeline_points;

   while (*link && (*link)->value < point->value)
      link = &(*link)->next;

   point->next = *link;
   *link = point;
}

static bool
agx_macos_mesa_sync_has_pending_submission(
   const struct agx_macos_mesa_device_state *state,
   const struct agx_macos_mesa_sync *sync)
{
   for (const struct agx_macos_mesa_sync_submission *submission =
           state->submissions;
        submission; submission = submission->next) {
      for (uint32_t i = 0; i < submission->output_count; ++i) {
         if (submission->outputs[i].sync == sync)
            return true;
      }
   }

   return false;
}

static void
agx_macos_mesa_sync_submission_signal(
   struct agx_macos_mesa_sync_submission *submission)
{
   for (uint32_t i = 0; i < submission->output_count; ++i) {
      struct agx_macos_mesa_sync_submission_output *output =
         &submission->outputs[i];

      if (output->type == AGX_MACOS_MESA_SYNC_TYPE_TIMELINE) {
         output->timeline_point->complete = true;
         agx_macos_mesa_sync_timeline_advance(output->sync);
      } else {
         output->sync->signaled = true;
      }
   }
}

static void
agx_macos_mesa_sync_submission_retire(
   struct agx_macos_mesa_device_state *state,
   struct agx_macos_mesa_sync_submission *submission)
{
   struct agx_macos_mesa_sync_submission **link = &state->submissions;

   while (*link && *link != submission)
      link = &(*link)->next;

   if (*link != submission)
      return;

   agx_macos_mesa_sync_submission_signal(submission);
   *link = submission->next;
   free(submission);
}

/* A record is consumed only if one live Mesa-owned package recognizes its
 * queue/token pair. This keeps a different context's completion visible to
 * its owner instead of silently dropping it during a fence wait. */
static kern_return_t
agx_macos_mesa_sync_poll_one(struct agx_macos_mesa_device_state *state)
{
   struct agx_macos_completion_record raw;
   struct agx_macos_completion_record_observed completion;
   kern_return_t result;

   result = agx_macos_notification_queue_peek_completion(
      state->session, state->notification_queue, &raw);
   if (result != KERN_SUCCESS)
      return result;

   memcpy(&completion, raw.bytes, sizeof(completion));
   for (struct agx_macos_mesa_sync_submission *submission =
           state->submissions;
        submission; submission = submission->next) {
      struct agx_macos_submission_lease *lease;
      unsigned token_index;
      bool complete = false;

      lease = &submission->package.native.lease;
      if (!agx_macos_submission_observation_matches_completion(
             &lease->fence.observation, state->notification_queue->id,
             &completion, &token_index) ||
          (lease->fence.completed_token_mask & (1u << token_index))) {
         continue;
      }

      result = agx_macos_mesa_submission_package_poll_notification_queue(
         state->session, state->notification_queue, &submission->package,
         &complete);
      if (result != KERN_SUCCESS)
         return result;

      if (complete)
         agx_macos_mesa_sync_submission_retire(state, submission);

      return KERN_SUCCESS;
   }

   return kIOReturnBadArgument;
}

int
agx_macos_mesa_sync_create(struct agx_device *device, uint32_t flags,
                           uint32_t *out_handle)
{
   struct agx_macos_mesa_device_state *state =
      agx_macos_mesa_device_state(device);
   struct agx_macos_mesa_sync *sync;
   uint32_t handle;

   if (!out_handle || !agx_macos_mesa_sync_is_supported(device))
      return -ENOTSUP;

   sync = calloc(1, sizeof(*sync));
   if (!sync)
      return -ENOMEM;

   pthread_mutex_lock(&state->sync_lock);
   do {
      handle = ++state->next_sync_handle;
   } while (handle == 0 || agx_macos_mesa_sync_find(state, handle));

   *sync = (struct agx_macos_mesa_sync){
      .handle = handle,
      .references = 1,
      .signaled = (flags & 1u) != 0,
      .next = state->syncs,
   };
   state->syncs = sync;
   pthread_mutex_unlock(&state->sync_lock);

   *out_handle = handle;
   return 0;
}

int
agx_macos_mesa_sync_reference(struct agx_device *device, uint32_t handle)
{
   struct agx_macos_mesa_device_state *state =
      agx_macos_mesa_device_state(device);
   struct agx_macos_mesa_sync *sync;

   if (handle == 0 || !agx_macos_mesa_sync_is_supported(device))
      return -ENOTSUP;

   pthread_mutex_lock(&state->sync_lock);
   sync = agx_macos_mesa_sync_find(state, handle);
   if (!sync || sync->references == UINT32_MAX) {
      pthread_mutex_unlock(&state->sync_lock);
      return -EINVAL;
   }

   ++sync->references;
   pthread_mutex_unlock(&state->sync_lock);
   return 0;
}

static bool
agx_macos_mesa_sync_output_is_adoptable(
   const struct agx_macos_mesa_device_state *state,
   const struct agx_macos_mesa_sync *sync,
   const struct agx_submit_sync *output)
{
   enum agx_macos_mesa_sync_type type;

   if (!state || !sync || !output)
      return false;

   switch (output->type) {
   case AGX_SUBMIT_SYNC_BINARY:
      type = AGX_MACOS_MESA_SYNC_TYPE_BINARY;
      /* Mesa recycles batch sync handles after a completed batch. Adopting a
       * new binary output is the explicit reset transition. */
      if (output->timeline_value != 0)
         return false;
      break;
   case AGX_SUBMIT_SYNC_TIMELINE:
      type = AGX_MACOS_MESA_SYNC_TYPE_TIMELINE;
      if (output->timeline_value == 0 ||
          output->timeline_value <= sync->completed_timeline_value ||
          agx_macos_mesa_sync_timeline_find(sync, output->timeline_value)) {
         return false;
      }
      break;
   default:
      return false;
   }

   return (sync->type == AGX_MACOS_MESA_SYNC_TYPE_UNSET ||
           sync->type == type) &&
          (type == AGX_MACOS_MESA_SYNC_TYPE_TIMELINE ||
           !agx_macos_mesa_sync_has_pending_submission(state, sync));
}

int
agx_macos_mesa_sync_adopt_submission_outputs(
   struct agx_device *device, const struct agx_submit_sync *outputs,
   uint32_t output_count,
   struct agx_macos_mesa_submission_package *package)
{
   struct agx_macos_mesa_device_state *state =
      agx_macos_mesa_device_state(device);
   struct agx_macos_mesa_sync_submission *submission;

   if (!outputs || output_count == 0 || !package ||
       !agx_macos_mesa_sync_is_supported(device) ||
       !agx_macos_mesa_submission_package_is_intact(package) ||
       package->device != device ||
       package->native.lease.state != AGX_MACOS_SUBMISSION_LEASE_IN_FLIGHT ||
       !agx_macos_mesa_sync_package_uses_current_queue(state, package)) {
      return -EINVAL;
   }

   submission = calloc(1, sizeof(*submission) +
                            output_count * sizeof(submission->outputs[0]));
   if (!submission)
      return -ENOMEM;

   pthread_mutex_lock(&state->sync_lock);
   for (uint32_t i = 0; i < output_count; ++i) {
      struct agx_macos_mesa_sync *sync =
         agx_macos_mesa_sync_find(state, outputs[i].handle);

      if (!agx_macos_mesa_sync_output_is_adoptable(state, sync, &outputs[i])) {
         pthread_mutex_unlock(&state->sync_lock);
         free(submission);
         return -EINVAL;
      }

      for (uint32_t j = 0; j < i; ++j) {
         if (outputs[j].handle == outputs[i].handle) {
            pthread_mutex_unlock(&state->sync_lock);
            free(submission);
            return -EINVAL;
         }
      }
   }

   submission->package = *package;
   submission->output_count = output_count;
   for (uint32_t i = 0; i < output_count; ++i) {
      struct agx_macos_mesa_sync *sync =
         agx_macos_mesa_sync_find(state, outputs[i].handle);
      enum agx_macos_mesa_sync_type type =
         outputs[i].type == AGX_SUBMIT_SYNC_BINARY
            ? AGX_MACOS_MESA_SYNC_TYPE_BINARY
            : AGX_MACOS_MESA_SYNC_TYPE_TIMELINE;
      struct agx_macos_mesa_timeline_point *point = NULL;

      if (type == AGX_MACOS_MESA_SYNC_TYPE_TIMELINE) {
         point = calloc(1, sizeof(*point));
         if (!point) {
            for (uint32_t j = 0; j < i; ++j)
               free(submission->outputs[j].timeline_point);
            pthread_mutex_unlock(&state->sync_lock);
            free(submission);
            return -ENOMEM;
         }
         point->value = outputs[i].timeline_value;
      }

      submission->outputs[i] = (struct agx_macos_mesa_sync_submission_output){
         .sync = sync,
         .timeline_point = point,
         .type = type,
      };
   }

   for (uint32_t i = 0; i < output_count; ++i) {
      struct agx_macos_mesa_sync_submission_output *output =
         &submission->outputs[i];

      output->sync->type = output->type;
      output->sync->signaled = false;
      if (output->timeline_point) {
         agx_macos_mesa_sync_timeline_insert(output->sync,
                                              output->timeline_point);
      }
   }
   submission->next = state->submissions;
   state->submissions = submission;
   *package = (struct agx_macos_mesa_submission_package){0};
   pthread_mutex_unlock(&state->sync_lock);
   return 0;
}

int
agx_macos_mesa_sync_adopt_submission_group(
   struct agx_device *device, const uint32_t *handles, uint32_t handle_count,
   struct agx_macos_mesa_submission_package *package)
{
   struct agx_submit_sync *outputs;
   int result;

   if (!handles || handle_count == 0) {
      return -EINVAL;
   }

   outputs = calloc(handle_count, sizeof(*outputs));
   if (!outputs)
      return -ENOMEM;

   for (uint32_t i = 0; i < handle_count; ++i) {
      outputs[i] = (struct agx_submit_sync){
         .type = AGX_SUBMIT_SYNC_BINARY,
         .handle = handles[i],
      };
   }

   result = agx_macos_mesa_sync_adopt_submission_outputs(
      device, outputs, handle_count, package);
   free(outputs);
   return result;
}

int
agx_macos_mesa_sync_adopt_submission(
   struct agx_device *device, uint32_t handle,
   struct agx_macos_mesa_submission_package *package)
{
   return agx_macos_mesa_sync_adopt_submission_group(device, &handle, 1,
                                                      package);
}

int
agx_macos_mesa_sync_wait(struct agx_device *device,
                         const uint32_t *handles, uint32_t handle_count,
                         uint64_t absolute_timeout, uint32_t flags,
                         uint32_t *first_signaled)
{
   struct agx_macos_mesa_device_state *state =
      agx_macos_mesa_device_state(device);
   const bool wait_all = (flags & 1u) != 0;

   if (!handles || handle_count == 0 || !agx_macos_mesa_sync_is_supported(device))
      return -ENOTSUP;

   for (;;) {
      bool any_signaled = false;
      bool all_signaled = true;
      uint32_t signaled_index = UINT32_MAX;
      kern_return_t poll_result;

      pthread_mutex_lock(&state->sync_lock);
      poll_result = agx_macos_mesa_sync_poll_one(state);
      if (poll_result != KERN_SUCCESS && poll_result != kIOReturnUnderrun) {
         pthread_mutex_unlock(&state->sync_lock);
         return -EIO;
      }

      for (uint32_t i = 0; i < handle_count; ++i) {
         struct agx_macos_mesa_sync *sync =
            agx_macos_mesa_sync_find(state, handles[i]);

         if (!sync) {
            pthread_mutex_unlock(&state->sync_lock);
            return -EINVAL;
         }

         any_signaled |= sync->signaled;
         all_signaled &= sync->signaled;
         if (sync->signaled && signaled_index == UINT32_MAX)
            signaled_index = i;
      }
      pthread_mutex_unlock(&state->sync_lock);

      if ((wait_all && all_signaled) || (!wait_all && any_signaled)) {
         if (first_signaled)
            *first_signaled = signaled_index;
         return 0;
      }

      if (absolute_timeout != UINT64_MAX &&
          (uint64_t)os_time_get_nano() >= absolute_timeout) {
         return -ETIME;
      }

      /* The notification queue is the proven completion source. Its Mach port
       * protocol is not yet a Mesa wait ABI, so use bounded polling rather
       * than consuming an unvalidated notification message. */
      os_time_sleep(1000);
   }
}

int
agx_macos_mesa_sync_destroy(struct agx_device *device, uint32_t handle)
{
   struct agx_macos_mesa_device_state *state =
      agx_macos_mesa_device_state(device);
   struct agx_macos_mesa_sync **link;
   struct agx_macos_mesa_sync *sync;

   if (handle == 0 || !agx_macos_mesa_sync_is_supported(device))
      return -ENOTSUP;

   pthread_mutex_lock(&state->sync_lock);
   for (link = &state->syncs; *link && (*link)->handle != handle;
        link = &(*link)->next)
      ;
   sync = *link;
   if (!sync || sync->references == 0) {
      pthread_mutex_unlock(&state->sync_lock);
      return -EINVAL;
   }
   if (sync->references == 1 &&
       (sync->timeline_points ||
        agx_macos_mesa_sync_has_pending_submission(state, sync))) {
      pthread_mutex_unlock(&state->sync_lock);
      return -EBUSY;
   }
   if (--sync->references != 0) {
      pthread_mutex_unlock(&state->sync_lock);
      return 0;
   }

   *link = sync->next;
   pthread_mutex_unlock(&state->sync_lock);
   free(sync);
   return 0;
}

static bool
agx_macos_mesa_bo_provider_supports_flags(
   const struct agx_macos_mesa_bo_provider *provider, enum agx_bo_flags flags)
{
   uint32_t required = AGX_MACOS_MESA_PLATFORM_BO_CAP_CPU_MAPPABLE;

   if (flags & AGX_BO_LOW_VA)
      required |= AGX_MACOS_MESA_PLATFORM_BO_CAP_LOW_VA;
   if (flags & AGX_BO_EXEC)
      required |= AGX_MACOS_MESA_PLATFORM_BO_CAP_EXECUTABLE;

   return provider && (provider->capabilities & required) == required;
}

/* Provider-wide capability is intentionally not enough for shader BOs. The
 * individual allocation must match the Apple-owned compiler-result lifecycle
 * that authorized its code resource and low-VA mapping. */
static bool
agx_macos_mesa_platform_bo_matches_flags(
   const struct agx_macos_mesa_device_state *state,
   const struct agx_macos_mesa_platform_bo *platform, uint64_t requested_size,
   enum agx_bo_flags flags)
{
   const struct agx_macos_shader_provenance *provenance;

   if (!state || !platform || platform->size < requested_size)
      return false;

   if (!(flags & AGX_BO_LOW_VA))
      return platform->shader_provenance == NULL;

   /* The only low-VA lifecycle evidenced so far is Apple-owned executable
    * code residency. Do not let a future data BO reuse that aperture. */
   if (!(flags & AGX_BO_EXEC))
      return false;

   provenance = platform->shader_provenance;
   return provenance &&
          agx_macos_shader_provenance_matches_range(
             provenance, state->session, platform->gpu_va, requested_size) &&
          platform->size == provenance->mapping_size;
}

static bool
agx_macos_mesa_allocation_flags_supported(
   const struct agx_macos_mesa_device_state *state, enum agx_bo_flags flags)
{
   /* Direct shared/write-combined BO allocations are observed. Cross-process,
    * shareable, and read-only mappings need separate UABI work. Low VA and
    * executable memory are explicit screen gates because Mesa's linker uses
    * AGX_BO_EXEC | AGX_BO_LOW_VA for every shader binary. */
   if (!state || (flags & (AGX_BO_SHARED | AGX_BO_SHAREABLE | AGX_BO_READONLY)))
      return false;

   if ((flags & AGX_BO_LOW_VA) &&
       !agx_macos_uabi_contract_supports(
          &state->uabi_contract, AGX_MACOS_UABI_OPERATION_LOW_VA_BIND)) {
      return false;
   }

   if ((flags & AGX_BO_EXEC) &&
       !agx_macos_uabi_contract_supports(
          &state->uabi_contract, AGX_MACOS_UABI_OPERATION_EXECUTABLE_BO)) {
      return false;
   }

   if ((flags & (AGX_BO_LOW_VA | AGX_BO_EXEC)) &&
       !agx_macos_uabi_contract_supports(
          &state->uabi_contract,
          AGX_MACOS_UABI_OPERATION_SHADER_CODE_ADMISSION)) {
      return false;
   }

   /* Mesa itself requires executable allocations to use the USC low-VA
    * region. Reject an impossible partial contract before allocation. */
   if ((flags & AGX_BO_EXEC) && !(flags & AGX_BO_LOW_VA))
      return false;

   /* Low VA is only evidenced for the executable code-resource path. */
   if ((flags & AGX_BO_LOW_VA) && !(flags & AGX_BO_EXEC))
      return false;

   /* A public data-buffer provider may not silently satisfy a future UABI
    * low-VA or executable bit. It must have independently proven support for
    * the exact allocation class Mesa requested. */
   return !state->bo_provider ||
          agx_macos_mesa_bo_provider_supports_flags(state->bo_provider, flags);
}

static struct agx_bo *
agx_macos_mesa_bo_wrap_native(struct agx_device *device,
                              const struct agx_macos_bo *native,
                              enum agx_bo_flags flags)
{
   struct agx_macos_mesa_device_state *state =
      agx_macos_mesa_device_state(device);
   struct agx_macos_mesa_bo *macos_bo;
   struct agx_macos_bo tracked = {.connection = IO_OBJECT_NULL};
   struct agx_bo *bo;
   struct agx_va *va;
   uint32_t handle;

   if (!agx_macos_mesa_device_is_current(device) || !native ||
       !native->managed_by_set || !native->cpu || native->size == 0 ||
       (native->gpu_va & (AGX_MACOS_MESA_NATIVE_BO_ALIGNMENT - 1)) != 0 ||
       agx_macos_bo_set_lookup_handle(state->bo_set, native->handle,
                                      &tracked) != KERN_SUCCESS ||
       tracked.connection != native->connection ||
       tracked.handle != native->handle || tracked.gpu_va != native->gpu_va ||
       tracked.cpu != native->cpu || tracked.size != native->size ||
       tracked.api_generation != native->api_generation) {
      errno = EINVAL;
      return NULL;
   }

   macos_bo = calloc(1, sizeof(*macos_bo));
   if (!macos_bo) {
      errno = ENOMEM;
      return NULL;
   }

   pthread_mutex_lock(&device->bo_map_lock);
   if (device->max_handle == UINT32_MAX) {
      pthread_mutex_unlock(&device->bo_map_lock);
      free(macos_bo);
      errno = EOVERFLOW;
      return NULL;
   }

   handle = ++device->max_handle;
   bo = agx_lookup_bo(device, handle);
   pthread_mutex_unlock(&device->bo_map_lock);
   if (memcmp(bo, &(struct agx_bo){0}, sizeof(*bo)) != 0) {
      free(macos_bo);
      errno = EBUSY;
      return NULL;
   }

   va = calloc(1, sizeof(*va));
   if (!va) {
      free(macos_bo);
      errno = ENOMEM;
      return NULL;
   }

   macos_bo->native = tracked;
   *bo = (struct agx_bo){
      .dev = device,
      .flags = flags,
      .size = tracked.size,
      .align = AGX_MACOS_MESA_NATIVE_BO_ALIGNMENT,
      .va = va,
      ._map = tracked.cpu,
      .handle = handle,
      .uapi_handle = tracked.handle,
      .prime_fd = -1,
      .platform_data = macos_bo,
   };
   *va = (struct agx_va){
      .addr = tracked.gpu_va,
      .size_B = tracked.size,
   };
   /* Public adoption returns an owned Mesa BO rather than passing through
    * agx_bo_create(), so establish the same initial reference explicitly. */
   p_atomic_set(&bo->refcnt, 1);
   return bo;
}

static struct agx_bo *
agx_macos_mesa_bo_wrap_platform(
   struct agx_device *device, uint64_t size, enum agx_bo_flags flags)
{
   struct agx_macos_mesa_device_state *state =
      agx_macos_mesa_device_state(device);
   const struct agx_macos_mesa_bo_provider *provider =
      state ? state->bo_provider : NULL;
   struct agx_macos_mesa_platform_bo platform = {0};
   struct agx_macos_mesa_bo *macos_bo;
   struct agx_bo *bo;
   struct agx_va *va;
   uint32_t handle;

   if (!provider || !provider->create || !provider->is_current ||
       !provider->destroy ||
       !agx_macos_mesa_bo_provider_supports_flags(provider, flags) ||
       !provider->create(provider->context, size, flags, &platform) ||
       !provider->is_current(provider->context, &platform) || !platform.cpu ||
       !agx_macos_mesa_platform_bo_matches_flags(state, &platform, size, flags) ||
       (platform.gpu_va & (AGX_MACOS_MESA_NATIVE_BO_ALIGNMENT - 1)) != 0) {
      if (platform.owner)
         provider->destroy(provider->context, &platform);
      return NULL;
   }

   macos_bo = calloc(1, sizeof(*macos_bo));
   va = calloc(1, sizeof(*va));
   if (!macos_bo || !va) {
      free(va);
      free(macos_bo);
      provider->destroy(provider->context, &platform);
      errno = ENOMEM;
      return NULL;
   }

   pthread_mutex_lock(&device->bo_map_lock);
   if (device->max_handle == UINT32_MAX) {
      pthread_mutex_unlock(&device->bo_map_lock);
      free(va);
      free(macos_bo);
      provider->destroy(provider->context, &platform);
      errno = EOVERFLOW;
      return NULL;
   }

   handle = ++device->max_handle;
   bo = agx_lookup_bo(device, handle);
   pthread_mutex_unlock(&device->bo_map_lock);
   if (memcmp(bo, &(struct agx_bo){0}, sizeof(*bo)) != 0) {
      free(va);
      free(macos_bo);
      provider->destroy(provider->context, &platform);
      errno = EBUSY;
      return NULL;
   }

   macos_bo->backing = AGX_MACOS_MESA_BO_APPLE_OWNED;
   macos_bo->platform = platform;
   *bo = (struct agx_bo){
      .dev = device,
      .flags = flags,
      .size = platform.size,
      .align = AGX_MACOS_MESA_NATIVE_BO_ALIGNMENT,
      .va = va,
      ._map = platform.cpu,
      .handle = handle,
      /* This is a local Mesa identity, never an Apple resource handle. */
      .uapi_handle = handle,
      .prime_fd = -1,
      .platform_data = macos_bo,
   };
   *va = (struct agx_va){
      .addr = platform.gpu_va,
      .size_B = platform.size,
   };
   p_atomic_set(&bo->refcnt, 1);
   return bo;
}

struct agx_bo *
agx_macos_mesa_device_adopt_bo(struct agx_device *device,
                               const struct agx_macos_bo *native,
                               enum agx_bo_flags flags)
{
   struct agx_macos_mesa_device_state *state =
      agx_macos_mesa_device_state(device);

   if (!agx_macos_mesa_allocation_flags_supported(state, flags)) {
      errno = ENOTSUP;
      return NULL;
   }

   return agx_macos_mesa_bo_wrap_native(device, native, flags);
}

static struct agx_bo *
agx_macos_mesa_bo_alloc(struct agx_device *device, size_t size, size_t align,
                        enum agx_bo_flags flags)
{
   struct agx_macos_mesa_device_state *state =
      agx_macos_mesa_device_state(device);
   struct agx_macos_bo native = {.connection = IO_OBJECT_NULL};
   struct agx_bo *bo;

   if (!agx_macos_mesa_device_is_current(device) ||
       !agx_macos_mesa_allocation_flags_supported(state, flags) || size == 0 ||
       size > AGX_MACOS_BO_SHARED_512K_SIZE ||
       align > AGX_MACOS_MESA_NATIVE_BO_ALIGNMENT) {
      return NULL;
   }

   if (state->bo_provider)
      return agx_macos_mesa_bo_wrap_platform(device, size, flags);

   if (agx_macos_bo_set_create_at_least(
          state->bo_set,
          flags & AGX_BO_WRITEBACK ? AGX_MACOS_BO_STORAGE_SHARED
                                   : AGX_MACOS_BO_STORAGE_WRITE_COMBINED,
          size, AGX_MACOS_MESA_NATIVE_BO_ALIGNMENT, &native) != KERN_SUCCESS) {
      return NULL;
   }

   bo = agx_macos_mesa_bo_wrap_native(device, &native, flags);
   if (!bo)
      (void)agx_macos_bo_set_destroy(state->bo_set, &native);

   return bo;
}

static void
agx_macos_mesa_bo_mmap(struct agx_device *device, struct agx_bo *bo,
                       void *fixed_address)
{
   struct agx_macos_mesa_bo *macos_bo = bo ? bo->platform_data : NULL;

   (void)fixed_address;
   if (!agx_macos_mesa_device_is_current(device) || !macos_bo || bo->_map ||
       !macos_bo->native.cpu) {
      return;
   }

   bo->_map = macos_bo->native.cpu;
}

static ssize_t
agx_macos_mesa_get_params(struct agx_device *device, void *out, size_t size)
{
   if (!agx_macos_mesa_device_is_current(device) || !out ||
       size < sizeof(device->params)) {
      return -EINVAL;
   }

   memcpy(out, &device->params, sizeof(device->params));
   return sizeof(device->params);
}

static int
agx_macos_mesa_bind_fixed_bo(struct agx_device *device,
                             struct drm_asahi_gem_bind_op *ops,
                             uint32_t count)
{
   const uint32_t read_write = DRM_ASAHI_BIND_READ | DRM_ASAHI_BIND_WRITE;

   if (!agx_macos_mesa_device_is_current(device) || !ops || count == 0)
      return -EINVAL;

   for (uint32_t i = 0; i < count; ++i) {
      struct agx_bo *bo = NULL;

      if (ops[i].flags & DRM_ASAHI_BIND_UNBIND)
         return -ENOTSUP;
      if (ops[i].flags != read_write || ops[i].handle == 0 ||
          ops[i].offset != 0 || ops[i].range == 0 ||
          (ops[i].addr & (AGX_MACOS_MESA_NATIVE_BO_ALIGNMENT - 1)) != 0 ||
          (ops[i].range & (AGX_MACOS_MESA_NATIVE_BO_ALIGNMENT - 1)) != 0) {
         return -EINVAL;
      }

      pthread_mutex_lock(&device->bo_map_lock);
      for (uint64_t index = 1; index <= device->max_handle; ++index) {
         struct agx_bo *candidate = agx_lookup_bo(device, (uint32_t)index);

         if (candidate->platform_data &&
             candidate->uapi_handle == ops[i].handle) {
            bo = candidate;
            break;
         }
      }
      pthread_mutex_unlock(&device->bo_map_lock);

      /* The direct allocator has already established this exact mapping. It
       * must never authorize a replacement VA or partial resource range. */
      if (!bo || !bo->va || ops[i].addr != bo->va->addr ||
          ops[i].range != bo->size) {
         return -EINVAL;
      }
   }

   return 0;
}

static bool
agx_macos_mesa_gpu_pointer_is_owned(
   struct agx_macos_mesa_device_state *state, uint64_t gpu_va)
{
   struct agx_macos_bo unused_bo;

   return gpu_va == 0 ||
          agx_macos_bo_set_lookup_gpu_va_range(state->bo_set, gpu_va, 1,
                                               &unused_bo) == KERN_SUCCESS;
}

static bool
agx_macos_mesa_gpu_range_is_owned(
   struct agx_macos_mesa_device_state *state, uint64_t gpu_va, uint64_t size)
{
   struct agx_macos_bo unused_bo;

   return gpu_va != 0 && size != 0 &&
          agx_macos_bo_set_lookup_gpu_va_range(state->bo_set, gpu_va, size,
                                               &unused_bo) == KERN_SUCCESS;
}

static bool
agx_macos_mesa_encoder_is_owned(
   struct agx_macos_mesa_device_state *state, struct agx_device *device,
   const struct agx_submit_encoder *encoder)
{
   struct agx_macos_bo native_bo;
   uint64_t size;

   if (!encoder || !encoder->bo || encoder->bo->dev != device ||
       !encoder->bo->va || encoder->gpu_va == 0) {
      return false;
   }

   size = encoder->size_B ? encoder->size_B : 1;
   return agx_macos_bo_set_lookup_gpu_va_range(state->bo_set, encoder->gpu_va,
                                                size, &native_bo) ==
             KERN_SUCCESS &&
          native_bo.handle == encoder->bo->uapi_handle;
}

static bool
agx_macos_mesa_compute_stream_is_owned(
   struct agx_macos_mesa_device_state *state,
   const struct agx_submit_compute *compute)
{
   if (compute->cdm_ctrl_stream_base == 0 ||
       compute->cdm_ctrl_stream_end <= compute->cdm_ctrl_stream_base) {
      return false;
   }

   return agx_macos_mesa_gpu_range_is_owned(
             state, compute->cdm_ctrl_stream_base,
             compute->cdm_ctrl_stream_end - compute->cdm_ctrl_stream_base) &&
          agx_macos_mesa_gpu_pointer_is_owned(state, compute->sampler_heap) &&
          agx_macos_mesa_gpu_pointer_is_owned(state, compute->helper.data);
}

static bool
agx_macos_mesa_render_stream_is_owned(
   struct agx_macos_mesa_device_state *state,
   const struct agx_submit_render *render)
{
   const uint64_t pointers[] = {
      render->vdm_ctrl_stream_base,
      render->sampler_heap,
      render->vertex_helper.data,
      render->fragment_helper.data,
      render->isp_scissor_base,
      render->isp_dbias_base,
      render->isp_oclqry_base,
      render->depth.base,
      render->depth.compression_base,
      render->stencil.base,
      render->stencil.compression_base,
   };

   if (render->vdm_ctrl_stream_base == 0 ||
       !agx_macos_mesa_gpu_pointer_is_owned(state, pointers[0])) {
      return false;
   }

   for (uint32_t i = 1; i < sizeof(pointers) / sizeof(pointers[0]); ++i) {
      if (!agx_macos_mesa_gpu_pointer_is_owned(state, pointers[i]))
         return false;
   }

   for (uint32_t i = 0; i < render->attachment_count; ++i) {
      if (!agx_macos_mesa_gpu_range_is_owned(
             state, render->attachments[i].gpu_va,
             render->attachments[i].size_B)) {
         return false;
      }
   }

   return true;
}

static bool
agx_macos_mesa_submit_info_contains_range(const struct agx_submit_info *info,
                                          uint64_t gpu_va, uint64_t size)
{
   if (gpu_va == 0)
      return true;

   if (size == 0)
      size = 1;

   for (uint32_t i = 0; i < info->resource_count; ++i) {
      const struct agx_submit_resource *resource = &info->resources[i];
      uint64_t offset;

      if (gpu_va < resource->gpu_va)
         continue;

      offset = gpu_va - resource->gpu_va;
      if (offset <= resource->size_B && size <= resource->size_B - offset)
         return true;
   }

   return false;
}

static bool
agx_macos_mesa_submit_info_contains_bo(const struct agx_submit_info *info,
                                       const struct agx_bo *bo)
{
   for (uint32_t i = 0; i < info->resource_count; ++i) {
      if (info->resources[i].bo == bo)
         return true;
   }

   return false;
}

static bool
agx_macos_mesa_compute_stream_is_declared(const struct agx_submit_info *info,
                                          const struct agx_submit_compute *compute)
{
   return agx_macos_mesa_submit_info_contains_range(
             info, compute->cdm_ctrl_stream_base,
             compute->cdm_ctrl_stream_end - compute->cdm_ctrl_stream_base) &&
          agx_macos_mesa_submit_info_contains_range(info, compute->sampler_heap,
                                                     1) &&
          agx_macos_mesa_submit_info_contains_range(info, compute->helper.data,
                                                     1);
}

static bool
agx_macos_mesa_render_stream_is_declared(const struct agx_submit_info *info,
                                         const struct agx_submit_render *render)
{
   const uint64_t pointers[] = {
      render->vdm_ctrl_stream_base,
      render->sampler_heap,
      render->vertex_helper.data,
      render->fragment_helper.data,
      render->isp_scissor_base,
      render->isp_dbias_base,
      render->isp_oclqry_base,
      render->depth.base,
      render->depth.compression_base,
      render->stencil.base,
      render->stencil.compression_base,
   };

   for (uint32_t i = 0; i < sizeof(pointers) / sizeof(pointers[0]); ++i) {
      if (!agx_macos_mesa_submit_info_contains_range(info, pointers[i], 1))
         return false;
   }

   for (uint32_t i = 0; i < render->attachment_count; ++i) {
      if (!agx_macos_mesa_submit_info_contains_range(
             info, render->attachments[i].gpu_va, render->attachments[i].size_B)) {
         return false;
      }
   }

   return true;
}

static bool
agx_macos_mesa_submit_info_ranges_are_owned(
   struct agx_macos_mesa_device_state *state, struct agx_device *device,
   const struct agx_submit_info *info)
{
   if (info->encoder_count != info->command_count)
      return false;

   for (uint32_t i = 0; i < info->command_count; ++i) {
      const struct agx_submit_command *command = &info->commands[i];

      if (!agx_macos_mesa_encoder_is_owned(state, device,
                                            &info->encoders[i]) ||
          !agx_macos_mesa_submit_info_contains_bo(info, info->encoders[i].bo) ||
          (command->type == AGX_SUBMIT_COMMAND_COMPUTE &&
           (!agx_macos_mesa_compute_stream_is_owned(state, &command->compute) ||
            !agx_macos_mesa_compute_stream_is_declared(info,
                                                        &command->compute))) ||
          (command->type == AGX_SUBMIT_COMMAND_RENDER &&
           (!agx_macos_mesa_render_stream_is_owned(state, &command->render) ||
            !agx_macos_mesa_render_stream_is_declared(info,
                                                       &command->render)))) {
         return false;
      }
   }

   return true;
}

/* The platform-neutral batch description carries Mesa sync handles. Before a
 * native carrier can consume it, prove that every handle belongs to this
 * device's registry instead of accepting a caller-supplied integer. */
static bool
agx_macos_mesa_submit_info_syncs_are_owned(
   struct agx_macos_mesa_device_state *state,
   const struct agx_submit_info *info)
{
   uint32_t sync_count = info->in_sync_count + info->out_sync_count;
   bool valid = true;

   pthread_mutex_lock(&state->sync_lock);
   for (uint32_t i = 0; i < sync_count; ++i) {
      const struct agx_submit_sync *submit_sync = &info->syncs[i];

      if (!agx_macos_mesa_sync_find(state, submit_sync->handle) ||
          (submit_sync->type == AGX_SUBMIT_SYNC_BINARY &&
           submit_sync->timeline_value != 0)) {
         valid = false;
         break;
      }
   }
   pthread_mutex_unlock(&state->sync_lock);

   return valid;
}

/* The carrier's single completion must own every output exactly once. Input
 * waits may share a handle with other work, but two output entries cannot
 * safely race to retire the same native package. */
static bool
agx_macos_mesa_submit_info_outputs_are_adoptable(
   struct agx_macos_mesa_device_state *state,
   const struct agx_submit_info *info)
{
   bool valid = true;

   pthread_mutex_lock(&state->sync_lock);
   for (uint32_t i = 0; i < info->out_sync_count; ++i) {
      const struct agx_submit_sync *output =
         &info->syncs[info->in_sync_count + i];
      struct agx_macos_mesa_sync *sync =
         agx_macos_mesa_sync_find(state, output->handle);

      if (!agx_macos_mesa_sync_output_is_adoptable(state, sync, output)) {
         valid = false;
         break;
      }

      for (uint32_t j = 0; j < i; ++j) {
         if (info->syncs[info->in_sync_count + j].handle == output->handle) {
            valid = false;
            break;
         }
      }

      if (!valid)
         break;
   }
   pthread_mutex_unlock(&state->sync_lock);

   return valid;
}

static bool
agx_macos_mesa_submit_info_objects_are_owned(
   struct agx_macos_mesa_device_state *state, struct agx_device *device,
   const struct agx_submit_info *info)
{
   for (uint32_t i = 0; i < info->object_count; ++i) {
      const struct agx_submit_object *object = &info->objects[i];
      struct agx_macos_bo native_bo;

      if (object->type != AGX_SUBMIT_OBJECT_TIMESTAMPS || !object->bo ||
          object->bo->dev != device || !object->bo->va ||
          object->bo->va->addr == 0 || object->bo->size == 0 ||
          !agx_macos_mesa_submit_info_contains_bo(info, object->bo) ||
          agx_macos_bo_set_lookup_gpu_va_range(
             state->bo_set, object->bo->va->addr, object->bo->size,
             &native_bo) != KERN_SUCCESS ||
          native_bo.handle != object->bo->uapi_handle) {
         return false;
      }
   }

   return true;
}

static bool
agx_macos_mesa_timestamp_is_owned(const struct agx_submit_info *info,
                                  struct agx_submit_timestamp timestamp)
{
   if (timestamp.object == 0)
      return true;

   for (uint32_t i = 0; i < info->object_count; ++i) {
      const struct agx_submit_object *object = &info->objects[i];

      if (object->type == AGX_SUBMIT_OBJECT_TIMESTAMPS &&
          object->handle == timestamp.object) {
         /* The timestamp write width belongs to the AGX command ABI, but its
          * starting offset must always lie within the retained Mesa BO. */
         return object->bo && timestamp.offset_B < object->bo->size;
      }
   }

   return false;
}

static bool
agx_macos_mesa_submit_info_timestamps_are_owned(
   const struct agx_submit_info *info)
{
   for (uint32_t i = 0; i < info->command_count; ++i) {
      const struct agx_submit_command *command = &info->commands[i];

      if (command->type == AGX_SUBMIT_COMMAND_COMPUTE) {
         if (!agx_macos_mesa_timestamp_is_owned(
                info, command->compute.timestamps.start) ||
             !agx_macos_mesa_timestamp_is_owned(
                info, command->compute.timestamps.end)) {
            return false;
         }
      } else if (command->type == AGX_SUBMIT_COMMAND_RENDER) {
         if (!agx_macos_mesa_timestamp_is_owned(
                info, command->render.ts_vtx.start) ||
             !agx_macos_mesa_timestamp_is_owned(
                info, command->render.ts_vtx.end) ||
             !agx_macos_mesa_timestamp_is_owned(
                info, command->render.ts_frag.start) ||
             !agx_macos_mesa_timestamp_is_owned(
                info, command->render.ts_frag.end)) {
            return false;
         }
      }
   }

   return true;
}

static bool
agx_macos_mesa_submit_info_resources_are_owned(
   struct agx_macos_mesa_device_state *state, struct agx_device *device,
   const struct agx_submit_info *info)
{
   for (uint32_t i = 0; i < info->resource_count; ++i) {
      const struct agx_submit_resource *resource = &info->resources[i];
      struct agx_macos_bo native_bo;

      if (!resource->bo || resource->bo->dev != device || !resource->bo->va ||
          resource->gpu_va != resource->bo->va->addr ||
          resource->size_B != resource->bo->size ||
          agx_macos_bo_set_lookup_gpu_va_range(
             state->bo_set, resource->gpu_va, resource->size_B,
             &native_bo) != KERN_SUCCESS ||
          native_bo.handle != resource->bo->uapi_handle) {
         return false;
      }
   }

   return true;
}

static int
agx_macos_mesa_submit_info(struct agx_device *device,
                           const struct agx_submit_info *info,
                           struct agx_submit_virt *virt)
{
   struct agx_macos_mesa_device_state *state =
      agx_macos_mesa_device_state(device);

   (void)virt;

   /* Native Mesa BO ownership is checked before the unavailable carrier
    * import boundary. This consumer never serializes a DRM packet. */
   if (!agx_macos_mesa_device_is_current(device) ||
       !agx_submit_info_validate(info) ||
       !state->notification_queue ||
       !agx_macos_notification_queue_is_current(state->session,
                                                 state->notification_queue) ||
       !agx_macos_mesa_submit_info_syncs_are_owned(state, info) ||
       !agx_macos_mesa_submit_info_outputs_are_adoptable(state, info) ||
       !agx_macos_mesa_submit_info_resources_are_owned(state, device, info) ||
       !agx_macos_mesa_submit_info_objects_are_owned(state, device, info) ||
       !agx_macos_mesa_submit_info_timestamps_are_owned(info) ||
       !agx_macos_mesa_submit_info_ranges_are_owned(state, device, info)) {
      return -EINVAL;
   }

   return -ENOTSUP;
}

static int
agx_macos_mesa_unsupported_bind_object(
   struct agx_device *device, struct drm_asahi_gem_bind_object *bind)
{
   (void)device;
   (void)bind;
   return -ENOTSUP;
}

static int
agx_macos_mesa_unsupported_unbind_object(struct agx_device *device,
                                         uint32_t object_handle)
{
   (void)device;
   (void)object_handle;
   return -ENOTSUP;
}

bool
agx_macos_mesa_device_init(
   struct agx_device *out_device,
   const struct agx_macos_device_session *session,
   struct agx_macos_bo_set *bo_set,
   const struct agx_macos_notification_queue *notification_queue)
{
   struct agx_macos_mesa_device_state *state;
   struct agx_macos_uabi_contract uabi_contract;

   if (!out_device || !agx_macos_bo_set_is_current(bo_set, session) ||
       (notification_queue &&
        !agx_macos_notification_queue_is_current(session, notification_queue))) {
      return false;
   }

   if (agx_macos_uabi_contract_init(session, &uabi_contract) != KERN_SUCCESS)
      return false;

   state = calloc(1, sizeof(*state));
   if (!state)
      return false;

   *state = (struct agx_macos_mesa_device_state){
      .session = session,
      .bo_set = bo_set,
      .notification_queue = notification_queue,
      .uabi_contract = uabi_contract,
      .api_generation = session->api_generation,
   };
   pthread_mutex_init(&state->sync_lock, NULL);
   *out_device = (struct agx_device){
      .fd = -1,
      /* A notification queue carries completions only. Never expose its ID as
       * an Asahi execution queue before the separate native queue contract is
       * proven. */
      .queue_id = 0,
      .params = {
         .gpu_generation = session->info.gpu_generation,
         .gpu_variant = session->info.variant[0],
         .chip_id = session->info.chip_id,
         .num_clusters_total = session->info.cluster_count,
         .num_cores_per_cluster = session->info.cores_per_cluster,
      },
      .ops = {
         .bo_alloc = agx_macos_mesa_bo_alloc,
         .bo_bind = agx_macos_mesa_bind_fixed_bo,
         .bo_mmap = agx_macos_mesa_bo_mmap,
         .get_params = agx_macos_mesa_get_params,
         .submit_info = agx_macos_mesa_submit_info,
         .bo_bind_object = agx_macos_mesa_unsupported_bind_object,
         .bo_unbind_object = agx_macos_mesa_unsupported_unbind_object,
         .is_screen_ready = agx_macos_mesa_is_screen_ready,
      },
      .platform_data = state,
   };
   memcpy(out_device->params.core_masks, session->info.core_masks,
          sizeof(out_device->params.core_masks));
   snprintf(out_device->name, sizeof(out_device->name), "Apple %s (G%u)",
            session->info.variant, session->info.gpu_generation);

   util_sparse_array_init(&out_device->bo_map, sizeof(struct agx_bo), 512);
   pthread_mutex_init(&out_device->bo_map_lock, NULL);
   simple_mtx_init(&out_device->bo_cache.lock, mtx_plain);
   list_inithead(&out_device->bo_cache.lru);
   for (unsigned i = 0; i < ARRAY_SIZE(out_device->bo_cache.buckets); ++i)
      list_inithead(&out_device->bo_cache.buckets[i]);

   return true;
}

bool
agx_macos_mesa_device_attach_notification_queue(
   struct agx_device *device,
   const struct agx_macos_notification_queue *notification_queue)
{
   struct agx_macos_mesa_device_state *state =
      agx_macos_mesa_device_state(device);

   if (!state || !agx_macos_mesa_device_is_current(device) ||
       !notification_queue || state->notification_queue ||
       !agx_macos_notification_queue_is_current(state->session,
                                                 notification_queue)) {
      return false;
   }

   state->notification_queue = notification_queue;
   return true;
}

bool
agx_macos_mesa_device_attach_bo_provider(
   struct agx_device *device,
   const struct agx_macos_mesa_bo_provider *provider)
{
   struct agx_macos_mesa_device_state *state =
      agx_macos_mesa_device_state(device);

   if (!state || !agx_macos_mesa_device_is_current(device) ||
       !provider || !provider->context || !provider->create ||
       !provider->is_current || !provider->destroy || state->bo_provider ||
       !(provider->capabilities & AGX_MACOS_MESA_PLATFORM_BO_CAP_CPU_MAPPABLE) ||
       device->max_handle != 0) {
      return false;
   }

   state->bo_provider = provider;
   return true;
}

bool
agx_macos_mesa_bo_get_platform_backing(
   const struct agx_device *device, const struct agx_bo *bo,
   const struct agx_macos_mesa_bo_provider *expected_provider,
   struct agx_macos_mesa_platform_bo *out_backing)
{
   const struct agx_macos_mesa_device_state *state =
      agx_macos_mesa_device_state(device);
   const struct agx_macos_mesa_bo *macos_bo = bo ? bo->platform_data : NULL;
   const struct agx_macos_mesa_bo_provider *provider =
      state ? state->bo_provider : NULL;

   if (!out_backing)
      return false;
   *out_backing = (struct agx_macos_mesa_platform_bo){0};

   if (!agx_macos_mesa_device_is_current(device) || !bo || bo->dev != device ||
       !macos_bo || macos_bo->backing != AGX_MACOS_MESA_BO_APPLE_OWNED ||
       !provider || provider != expected_provider || !provider->is_current ||
       !provider->is_current(provider->context, &macos_bo->platform) ||
       !bo->va || bo->va->addr != macos_bo->platform.gpu_va ||
       bo->va->size_B != macos_bo->platform.size ||
       bo->size != macos_bo->platform.size || bo->_map != macos_bo->platform.cpu ||
       bo->uapi_handle != bo->handle) {
      return false;
   }

   *out_backing = macos_bo->platform;
   return true;
}

bool
agx_macos_mesa_bo_get_carrier_resource_binding(
   const struct agx_device *device, const struct agx_bo *bo,
   const void **out_binding)
{
   struct agx_macos_mesa_platform_bo platform = {0};
   const struct agx_macos_mesa_device_state *state =
      agx_macos_mesa_device_state(device);

   if (!out_binding)
      return false;
   *out_binding = NULL;

   return state && state->bo_provider &&
          agx_macos_mesa_bo_get_platform_backing(device, bo,
                                                  state->bo_provider,
                                                  &platform) &&
          platform.carrier_resource_binding &&
          ((*out_binding = platform.carrier_resource_binding) != NULL);
}

static kern_return_t
agx_macos_mesa_bo_range_resolve(
   struct agx_device *device, const struct agx_macos_mesa_bo_range *range,
   uint64_t *out_gpu_va, uint8_t **out_cpu)
{
   struct agx_macos_mesa_device_state *state =
      agx_macos_mesa_device_state(device);
   struct agx_macos_mesa_bo *macos_bo;
   struct agx_macos_bo tracked = {0};
   uint64_t gpu_va;

   if (!agx_macos_mesa_device_is_current(device) || !range || !range->bo ||
       !out_gpu_va || range->size == 0 || range->bo->dev != device ||
       !range->bo->platform_data || !range->bo->va ||
       range->offset > range->bo->size ||
       range->size > range->bo->size - range->offset) {
      return kIOReturnBadArgument;
   }

   macos_bo = range->bo->platform_data;
   if (!macos_bo->native.managed_by_set ||
       macos_bo->native.handle != range->bo->uapi_handle ||
       macos_bo->native.gpu_va != range->bo->va->addr ||
       macos_bo->native.size != range->bo->size ||
       range->bo->va->size_B != range->bo->size ||
       range->offset > UINT64_MAX - macos_bo->native.gpu_va) {
      return kIOReturnBadArgument;
   }

   if (agx_macos_bo_set_lookup_handle(state->bo_set, macos_bo->native.handle,
                                       &tracked) != KERN_SUCCESS ||
       tracked.connection != macos_bo->native.connection ||
       tracked.gpu_va != macos_bo->native.gpu_va ||
       tracked.cpu != macos_bo->native.cpu ||
       tracked.size != macos_bo->native.size ||
       tracked.api_generation != macos_bo->native.api_generation) {
      return kIOReturnBadArgument;
   }

   gpu_va = macos_bo->native.gpu_va + range->offset;
   if (out_cpu) {
      if (!macos_bo->native.cpu || range->bo->_map != macos_bo->native.cpu) {
         return kIOReturnBadArgument;
      }

      *out_cpu = (uint8_t *)macos_bo->native.cpu + range->offset;
   }

   *out_gpu_va = gpu_va;
   return KERN_SUCCESS;
}

kern_return_t
agx_macos_mesa_encoder_range_resolve(
   struct agx_device *device,
   const struct agx_macos_mesa_encoder_range *encoder_range,
   struct agx_macos_mesa_bo_range *out_range)
{
   struct agx_macos_mesa_bo_range range;
   uintptr_t map;
   uintptr_t begin;
   uintptr_t end;
   uint64_t unused_gpu_va;

   if (!device || !encoder_range || !out_range || !encoder_range->bo ||
       !encoder_range->begin || !encoder_range->end ||
       !encoder_range->bo->_map) {
      return kIOReturnBadArgument;
   }

   map = (uintptr_t)encoder_range->bo->_map;
   begin = (uintptr_t)encoder_range->begin;
   end = (uintptr_t)encoder_range->end;
   if (begin < map || end <= begin || begin - map > encoder_range->bo->size ||
       end - begin > encoder_range->bo->size - (begin - map)) {
      return kIOReturnBadArgument;
   }

   range = (struct agx_macos_mesa_bo_range){
      .bo = encoder_range->bo,
      .offset = begin - map,
      .size = end - begin,
   };
   if (agx_macos_mesa_bo_range_resolve(device, &range, &unused_gpu_va, NULL) !=
       KERN_SUCCESS) {
      return kIOReturnBadArgument;
   }

   *out_range = range;
   return KERN_SUCCESS;
}

static bool
agx_macos_mesa_range_contains(const struct agx_macos_submission_range *outer,
                              uint64_t inner_gpu_va, uint64_t inner_size)
{
   uint64_t outer_end;

   if (!outer || outer->size == 0 || inner_size == 0 ||
       outer->gpu_va > UINT64_MAX - outer->size ||
       inner_gpu_va > UINT64_MAX - inner_size || inner_gpu_va < outer->gpu_va) {
      return false;
   }

   outer_end = outer->gpu_va + outer->size;
   return inner_gpu_va + inner_size <= outer_end;
}

static bool
agx_macos_mesa_submission_add_bo(struct agx_bo **bo_references,
                                 uint32_t *bo_reference_count,
                                 struct agx_bo *bo)
{
   if (!bo_references || !bo_reference_count || !bo)
      return false;

   for (uint32_t i = 0; i < *bo_reference_count; ++i) {
      if (bo_references[i] == bo)
         return true;
   }

   if (*bo_reference_count == AGX_MACOS_MESA_SUBMISSION_MAX_BOS)
      return false;

   bo_references[(*bo_reference_count)++] = bo;
   return true;
}

static void
agx_macos_mesa_submission_release_bo_references(
   struct agx_macos_mesa_submission_package *package)
{
   if (!package || !package->device)
      return;

   for (uint32_t i = 0; i < package->bo_reference_count; ++i) {
      if (package->bo_references[i])
         agx_bo_unreference(package->device, package->bo_references[i]);
   }
}

bool
agx_macos_mesa_submission_package_is_intact(
   const struct agx_macos_mesa_submission_package *package)
{
   if (!package || !package->active || !package->device ||
       package->bo_reference_count == 0 ||
       !agx_macos_submission_package_is_intact(&package->native)) {
      return false;
   }

   for (uint32_t i = 0; i < package->bo_reference_count; ++i) {
      if (!package->bo_references[i] ||
          package->bo_references[i]->dev != package->device) {
         return false;
      }
   }

   return true;
}

kern_return_t
agx_macos_mesa_submission_package_admit(
   struct agx_macos_mesa_submission_package *out_package,
   struct agx_device *device, uint32_t queue_id,
   const void *descriptor_bytes, size_t descriptor_size,
   const void *auxiliary_bytes, size_t auxiliary_readable_prefix,
   const struct agx_macos_mesa_bo_range *command_ranges,
   uint32_t command_range_count,
   const struct agx_macos_mesa_bo_range *resource_record_range,
   enum agx_macos_resource_record_kind resource_record_kind,
   const struct agx_macos_mesa_bo_range *resource_ranges,
   uint32_t resource_range_count)
{
   struct agx_macos_resource_record_layout layout;
   struct agx_macos_submission_range native_command_ranges
      [AGX_MACOS_SUBMISSION_LEASE_MAX_RANGES];
   struct agx_macos_resource_binding native_resource_bindings
      [AGX_MACOS_RESOURCE_RECORD_MAX_BINDINGS];
   struct agx_bo *bo_references[AGX_MACOS_MESA_SUBMISSION_MAX_BOS] = {0};
   uint8_t *resource_record;
   uint64_t resource_record_gpu_va;
   uint32_t bo_reference_count = 0;
   bool record_is_pinned = false;
   kern_return_t result;

   if (!out_package || out_package->active || out_package->native.active ||
       !agx_macos_mesa_device_is_current(device) || !command_ranges ||
       command_range_count == 0 || !resource_record_range || !resource_ranges ||
       !agx_macos_resource_record_layout_get(resource_record_kind, &layout) ||
       resource_range_count != layout.binding_count ||
       command_range_count > AGX_MACOS_SUBMISSION_LEASE_MAX_RANGES -
                                 resource_range_count) {
      return kIOReturnBadArgument;
   }

   for (uint32_t i = 0; i < command_range_count; ++i) {
      uint64_t gpu_va;

      result = agx_macos_mesa_bo_range_resolve(
         device, &command_ranges[i], &gpu_va, NULL);
      if (result != KERN_SUCCESS ||
          !agx_macos_mesa_submission_add_bo(bo_references, &bo_reference_count,
                                             command_ranges[i].bo)) {
         return kIOReturnBadArgument;
      }

      native_command_ranges[i] = (struct agx_macos_submission_range){
         .gpu_va = gpu_va,
         .size = command_ranges[i].size,
      };
   }

   result = agx_macos_mesa_bo_range_resolve(
      device, resource_record_range, &resource_record_gpu_va, &resource_record);
   if (result != KERN_SUCCESS || resource_record_range->size < layout.minimum_record_size) {
      return kIOReturnBadArgument;
   }

   for (uint32_t i = 0; i < command_range_count; ++i) {
      record_is_pinned |= agx_macos_mesa_range_contains(
         &native_command_ranges[i], resource_record_gpu_va,
         resource_record_range->size) &&
                          command_ranges[i].bo == resource_record_range->bo;
   }
   if (!record_is_pinned)
      return kIOReturnBadArgument;

   for (uint32_t i = 0; i < resource_range_count; ++i) {
      result = agx_macos_mesa_bo_range_resolve(
         device, &resource_ranges[i], &native_resource_bindings[i].gpu_va, NULL);
      if (result != KERN_SUCCESS ||
          !agx_macos_mesa_submission_add_bo(bo_references, &bo_reference_count,
                                             resource_ranges[i].bo)) {
         return kIOReturnBadArgument;
      }

      native_resource_bindings[i].byte_size = resource_ranges[i].size;
   }

   /* Hold Mesa's objects before the native lease starts pinning their backing
    * allocations. This prevents agx_bo_free from racing package retirement. */
   for (uint32_t i = 0; i < bo_reference_count; ++i)
      agx_bo_reference(bo_references[i]);

   result = agx_macos_submission_package_admit(
      &out_package->native,
      agx_macos_mesa_device_state(device)->bo_set, queue_id, descriptor_bytes,
      descriptor_size, auxiliary_bytes, auxiliary_readable_prefix,
      native_command_ranges, command_range_count, resource_record,
      resource_record_range->size, resource_record_kind, native_resource_bindings,
      resource_range_count);
   if (result != KERN_SUCCESS) {
      for (uint32_t i = 0; i < bo_reference_count; ++i)
         agx_bo_unreference(device, bo_references[i]);
      return result;
   }

   out_package->device = device;
   memcpy(out_package->bo_references, bo_references, sizeof(bo_references));
   out_package->bo_reference_count = bo_reference_count;
   out_package->active = true;
   return KERN_SUCCESS;
}

kern_return_t
agx_macos_mesa_submission_package_admit_encoders(
   struct agx_macos_mesa_submission_package *out_package,
   struct agx_device *device, uint32_t queue_id,
   const void *descriptor_bytes, size_t descriptor_size,
   const void *auxiliary_bytes, size_t auxiliary_readable_prefix,
   const struct agx_macos_mesa_encoder_range *encoder_ranges,
   uint32_t encoder_range_count,
   const struct agx_macos_mesa_bo_range *resource_record_range,
   enum agx_macos_resource_record_kind resource_record_kind,
   const struct agx_macos_mesa_bo_range *resource_ranges,
   uint32_t resource_range_count)
{
   struct agx_macos_mesa_bo_range command_ranges
      [AGX_MACOS_SUBMISSION_LEASE_MAX_RANGES];

   if (!encoder_ranges || encoder_range_count == 0 ||
       encoder_range_count > AGX_MACOS_SUBMISSION_LEASE_MAX_RANGES) {
      return kIOReturnBadArgument;
   }

   for (uint32_t i = 0; i < encoder_range_count; ++i) {
      if (agx_macos_mesa_encoder_range_resolve(device, &encoder_ranges[i],
                                                &command_ranges[i]) !=
          KERN_SUCCESS) {
         return kIOReturnBadArgument;
      }
   }

   return agx_macos_mesa_submission_package_admit(
      out_package, device, queue_id, descriptor_bytes, descriptor_size,
      auxiliary_bytes, auxiliary_readable_prefix, command_ranges,
      encoder_range_count,
      resource_record_range, resource_record_kind, resource_ranges,
      resource_range_count);
}

kern_return_t
agx_macos_mesa_submission_package_admit_encoder(
   struct agx_macos_mesa_submission_package *out_package,
   struct agx_device *device, uint32_t queue_id,
   const void *descriptor_bytes, size_t descriptor_size,
   const void *auxiliary_bytes, size_t auxiliary_readable_prefix,
   const struct agx_macos_mesa_encoder_range *encoder_range,
   const struct agx_macos_mesa_bo_range *resource_record_range,
   enum agx_macos_resource_record_kind resource_record_kind,
   const struct agx_macos_mesa_bo_range *resource_ranges,
   uint32_t resource_range_count)
{
   return agx_macos_mesa_submission_package_admit_encoders(
      out_package, device, queue_id, descriptor_bytes, descriptor_size,
      auxiliary_bytes, auxiliary_readable_prefix, encoder_range, 1,
      resource_record_range, resource_record_kind, resource_ranges,
      resource_range_count);
}

kern_return_t
agx_macos_mesa_submission_package_bind_notification_queue(
   const struct agx_macos_device_session *session,
   struct agx_macos_notification_queue *queue,
   struct agx_macos_mesa_submission_package *package)
{
   if (!agx_macos_mesa_submission_package_is_intact(package) ||
       !agx_macos_mesa_device_is_current(package->device)) {
      return kIOReturnBadArgument;
   }

   return agx_macos_submission_package_bind_notification_queue(
      session, queue, &package->native);
}

kern_return_t
agx_macos_mesa_submission_package_mark_submitted(
   struct agx_macos_mesa_submission_package *package)
{
   if (!agx_macos_mesa_submission_package_is_intact(package) ||
       !agx_macos_mesa_device_is_current(package->device)) {
      return kIOReturnBadArgument;
   }

   return agx_macos_submission_package_mark_submitted(&package->native);
}

kern_return_t
agx_macos_mesa_submission_package_record_completion(
   struct agx_macos_mesa_submission_package *package,
   uint32_t completion_queue_id,
   const struct agx_macos_completion_record_observed *record,
   bool *out_complete)
{
   kern_return_t result;

   if (!agx_macos_mesa_submission_package_is_intact(package))
      return kIOReturnBadArgument;

   result = agx_macos_submission_package_record_completion(
      &package->native, completion_queue_id, record, out_complete);
   if (result != KERN_SUCCESS || !*out_complete)
      return result;

   agx_macos_mesa_submission_release_bo_references(package);
   *package = (struct agx_macos_mesa_submission_package){0};
   return KERN_SUCCESS;
}

kern_return_t
agx_macos_mesa_submission_package_poll_notification_queue(
   const struct agx_macos_device_session *session,
   const struct agx_macos_notification_queue *queue,
   struct agx_macos_mesa_submission_package *package, bool *out_complete)
{
   kern_return_t result;

   if (!agx_macos_mesa_submission_package_is_intact(package) ||
       !agx_macos_mesa_device_is_current(package->device) || !out_complete) {
      return kIOReturnBadArgument;
   }

   result = agx_macos_submission_package_poll_notification_queue(
      session, queue, &package->native, out_complete);
   if (result != KERN_SUCCESS || !*out_complete)
      return result;

   agx_macos_mesa_submission_release_bo_references(package);
   *package = (struct agx_macos_mesa_submission_package){0};
   return KERN_SUCCESS;
}

kern_return_t
agx_macos_mesa_submission_package_release(
   struct agx_macos_mesa_submission_package *package)
{
   kern_return_t result;

   if (!agx_macos_mesa_submission_package_is_intact(package))
      return kIOReturnBadArgument;

   result = agx_macos_submission_package_release(&package->native);
   if (result != KERN_SUCCESS)
      return result;

   agx_macos_mesa_submission_release_bo_references(package);
   *package = (struct agx_macos_mesa_submission_package){0};
   return KERN_SUCCESS;
}

void
agx_bo_free(struct agx_device *device, struct agx_bo *bo)
{
   struct agx_macos_mesa_device_state *state =
      agx_macos_mesa_device_state(device);
   struct agx_macos_mesa_bo *macos_bo = bo ? bo->platform_data : NULL;

   if (!state || !bo || !macos_bo)
      return;

   if (macos_bo->backing == AGX_MACOS_MESA_BO_DIRECT) {
      if (agx_macos_bo_set_destroy(state->bo_set, &macos_bo->native) !=
          KERN_SUCCESS) {
         fprintf(stderr, "macOS AGX BO release refused for Mesa handle %u\n",
                 bo->handle);
         return;
      }
   } else if (macos_bo->backing == AGX_MACOS_MESA_BO_APPLE_OWNED &&
              state->bo_provider && state->bo_provider->destroy) {
      state->bo_provider->destroy(state->bo_provider->context,
                                  &macos_bo->platform);
   } else {
      return;
   }

   free(bo->va);
   free(macos_bo);
   *bo = (struct agx_bo){0};
}

void
agx_close_device(struct agx_device *device)
{
   struct agx_macos_mesa_device_state *state =
      agx_macos_mesa_device_state(device);

   if (!state)
      return;

   if (state->syncs)
      return;

   agx_bo_cache_evict_all(device);
   util_sparse_array_finish(&device->bo_map);
   pthread_mutex_destroy(&device->bo_map_lock);
   pthread_mutex_destroy(&state->sync_lock);
   free(state);
   *device = (struct agx_device){.fd = -1};
}

bool
agx_macos_mesa_device_destroy(struct agx_device *device)
{
   struct agx_macos_mesa_device_state *state;

   /* A failed initialization leaves an all-zero agx_device. Treat that as an
    * already-destroyed object rather than reading an uninitialized mutex. */
   if (!device || !device->platform_data)
      return true;

   state = agx_macos_mesa_device_state(device);

   if (state) {
      pthread_mutex_lock(&state->sync_lock);
      if (state->syncs) {
         pthread_mutex_unlock(&state->sync_lock);
         return false;
      }
      pthread_mutex_unlock(&state->sync_lock);
   }

   agx_close_device(device);
   return true;
}

bool
agx_open_device(void *memctx, struct agx_device *device)
{
   (void)memctx;
   (void)device;
   return false;
}

int
agx_bo_bind(struct agx_device *device, struct agx_bo *bo, uint64_t address,
            size_t size, uint64_t offset, uint32_t flags)
{
   struct drm_asahi_gem_bind_op op;

   /* The native allocator establishes the BO's only valid GPU mapping. Route
    * Mesa's common bind helper through the validated fixed-map operation, but
    * never treat it as permission for remap, partial bind, or unbind. */
   if (!agx_macos_mesa_device_is_current(device) || !device->ops.bo_bind ||
       !bo || bo->dev != device || !bo->platform_data || !bo->va ||
       size == 0 ||
       (address & (AGX_MACOS_MESA_NATIVE_BO_ALIGNMENT - 1)) != 0 ||
       (size & (AGX_MACOS_MESA_NATIVE_BO_ALIGNMENT - 1)) != 0 ||
       (offset & (AGX_MACOS_MESA_NATIVE_BO_ALIGNMENT - 1)) != 0 ||
       offset > bo->size || size > bo->size - offset) {
      return -EINVAL;
   }

   if (flags & DRM_ASAHI_BIND_UNBIND)
      return -ENOTSUP;

   op = (struct drm_asahi_gem_bind_op){
      .flags = flags,
      .handle = bo->uapi_handle,
      .offset = offset,
      .range = size,
      .addr = address,
   };
   return device->ops.bo_bind(device, &op, 1);
}

int
agx_bind_timestamps(struct agx_device *device, struct agx_bo *bo,
                    uint32_t *out_handle)
{
   (void)device;
   (void)bo;
   if (out_handle)
      *out_handle = 0;
   return -ENOTSUP;
}

uint32_t
agx_create_command_queue(struct agx_device *device,
                         enum drm_asahi_priority priority)
{
   (void)device;
   (void)priority;
   return 0;
}

int
agx_destroy_command_queue(struct agx_device *device, uint32_t queue_id)
{
   (void)device;
   (void)queue_id;
   return -ENOTSUP;
}

int
agx_import_sync_file(struct agx_device *device, struct agx_bo *bo, int fd)
{
   (void)device;
   (void)bo;
   (void)fd;
   return -ENOTSUP;
}

int
agx_export_sync_file(struct agx_device *device, struct agx_bo *bo)
{
   (void)device;
   (void)bo;
   return -ENOTSUP;
}

void
agx_debug_fault(struct agx_device *device, uint64_t address)
{
   (void)device;
   (void)address;
}

uint64_t
agx_get_gpu_timestamp(struct agx_device *device)
{
   (void)device;
   return 0;
}

void
agx_get_device_uuid(const struct agx_device *device, void *uuid)
{
   uint8_t *bytes = uuid;

   if (!bytes)
      return;

   memset(bytes, 0, 16);
   if (device) {
      memcpy(bytes, &device->params.chip_id, sizeof(device->params.chip_id));
      memcpy(bytes + 4, &device->params.gpu_generation,
             sizeof(device->params.gpu_generation));
   }
}

void
agx_get_driver_uuid(void *uuid)
{
   static const uint8_t driver_uuid[16] = {
      0x4b, 0x68, 0x72, 0x6f, 0x6e, 0x6f, 0x73, 0x41,
      0x4f, 0x34, 0x36, 0x41, 0x47, 0x58, 0x00, 0x01,
   };

   if (uuid)
      memcpy(uuid, driver_uuid, sizeof(driver_uuid));
}

unsigned
agx_get_num_cores(const struct agx_device *device)
{
   unsigned cores = 0;

   if (!device)
      return 0;

   for (unsigned i = 0; i < device->params.num_clusters_total; ++i) {
      uint64_t mask = device->params.core_masks[i];
      while (mask) {
         cores += mask & 1;
         mask >>= 1;
      }
   }

   return cores;
}

struct agx_device_key
agx_gather_device_key(struct agx_device *device)
{
   bool g13x_coherency = device &&
      ((device->params.gpu_generation == 13 &&
        device->params.num_clusters_total > 1) ||
       device->params.num_dies > 1);

   return (struct agx_device_key){
      .needs_g13x_coherency = u_tristate_make(g13x_coherency),
      .soft_fault = false,
   };
}
