/*
 * Copyright 2026 Khronos_AppleICDs contributors
 * SPDX-License-Identifier: MIT
 */

#include "agx_macos_queue.h"

#include "agx_macos_submission_observation.h"
#include "agx_macos_submission_lease.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

#define AGX_MACOS_SELECTOR_CREATE_NOTIFICATION_QUEUE 0x10
#define AGX_MACOS_SELECTOR_ACTIVATE_NOTIFICATION_QUEUE 0x1c
#define AGX_MACOS_SELECTOR_DESTROY_QUEUE 0x08
#define AGX_MACOS_SELECTOR_RELEASE_QUEUE_NOTIFICATION 0x11
#define AGX_MACOS_NOTIFICATION_QUEUE_CONFIG_SIZE 0x100
#define AGX_MACOS_NOTIFICATION_QUEUE_CONFIG_ENTRY_SIZE 0x28

struct agx_macos_notification_queue_response {
   uint64_t data_queue;
   uint32_t id;
   uint32_t reserved;
} __attribute__((packed));

_Static_assert(sizeof(struct agx_macos_notification_queue_response) == 0x10,
               "modern AGX notification queue response size");

static kern_return_t
agx_macos_notification_queue_call_release_selector(io_connect_t connection,
                                                    uint32_t selector,
                                                    uint32_t id)
{
   const uint64_t input = id;

   return IOConnectCallMethod(connection, selector, &input, 1, NULL, 0, NULL,
                              NULL, NULL, NULL);
}

static kern_return_t
agx_macos_notification_port_release(mach_port_t notification_port)
{
   return mach_port_mod_refs(mach_task_self(), notification_port,
                             MACH_PORT_RIGHT_RECEIVE, -1);
}

static void
agx_macos_notification_queue_cleanup_partial(io_connect_t connection, uint32_t id,
                                             mach_port_t notification_port)
{
   (void)agx_macos_notification_queue_call_release_selector(
      connection, AGX_MACOS_SELECTOR_DESTROY_QUEUE, id);
   if (notification_port != MACH_PORT_NULL)
      (void)agx_macos_notification_port_release(notification_port);
   (void)agx_macos_notification_queue_call_release_selector(
      connection, AGX_MACOS_SELECTOR_RELEASE_QUEUE_NOTIFICATION, id);

   /* The final kernel teardown selector is not validated for direct queues.
     * The caller must close the owning session after a partial-create failure. */
}

static bool
agx_macos_notification_queue_is_active(
   const struct agx_macos_notification_queue *queue)
{
   return queue && queue->connection != IO_OBJECT_NULL && queue->data_queue &&
          queue->notification_port != MACH_PORT_NULL && queue->id != 0 &&
          queue->api_generation != 0 &&
          queue->release_state == AGX_MACOS_NOTIFICATION_QUEUE_ACTIVE;
}

bool
agx_macos_notification_queue_is_current(
   const struct agx_macos_device_session *session,
   const struct agx_macos_notification_queue *queue)
{
   return agx_macos_device_session_is_current(session) && queue &&
          agx_macos_notification_queue_is_active(queue) &&
          queue->connection == session->device.connection &&
          queue->api_generation == session->api_generation;
}

kern_return_t
agx_macos_notification_queue_create(
   const struct agx_macos_device_session *session,
   struct agx_macos_notification_queue *queue)
{
   const uint64_t create_input[] = {
      AGX_MACOS_NOTIFICATION_QUEUE_CONFIG_SIZE,
      AGX_MACOS_NOTIFICATION_QUEUE_CONFIG_ENTRY_SIZE,
   };
   struct agx_macos_notification_queue_response response = {0};
   size_t response_size = sizeof(response);
   uint64_t activate_input[2];
   mach_port_t notification_port;
   kern_return_t result;

   if (!agx_macos_device_session_is_current(session) || !queue ||
       queue->connection != IO_OBJECT_NULL) {
      return kIOReturnBadArgument;
   }

   result = IOConnectCallMethod(session->device.connection,
                                AGX_MACOS_SELECTOR_CREATE_NOTIFICATION_QUEUE,
                                create_input, 2, NULL, 0, NULL, NULL, &response,
                                &response_size);
   if (result != KERN_SUCCESS)
      return result;

   if (response_size != sizeof(response) || response.data_queue == 0 ||
       response.id == 0 || response.reserved != 0) {
      if (response.id)
         agx_macos_notification_queue_cleanup_partial(
            session->device.connection, response.id, MACH_PORT_NULL);
      return kIOReturnBadArgument;
   }

   notification_port = IODataQueueAllocateNotificationPort();
   if (notification_port == MACH_PORT_NULL) {
      agx_macos_notification_queue_cleanup_partial(
         session->device.connection, response.id, MACH_PORT_NULL);
      return kIOReturnNoResources;
   }

   result = IOConnectSetNotificationPort(session->device.connection, 0,
                                         notification_port, response.id);
   if (result != KERN_SUCCESS)
      goto fail;

   activate_input[0] = response.id;
   activate_input[1] = response.id;
   result = IOConnectCallMethod(session->device.connection,
                                AGX_MACOS_SELECTOR_ACTIVATE_NOTIFICATION_QUEUE,
                                activate_input, 2, NULL, 0, NULL, NULL, NULL,
                                NULL);
   if (result != KERN_SUCCESS)
      goto fail;

   *queue = (struct agx_macos_notification_queue){
      .connection = session->device.connection,
      .data_queue = (IODataQueueMemory *)(uintptr_t)response.data_queue,
      .notification_port = notification_port,
      .id = response.id,
      .api_generation = session->api_generation,
      .release_state = AGX_MACOS_NOTIFICATION_QUEUE_ACTIVE,
   };
   return KERN_SUCCESS;

fail:
   agx_macos_notification_queue_cleanup_partial(session->device.connection,
                                                 response.id, notification_port);
   return result;
}

kern_return_t
agx_macos_notification_queue_begin_destroy(struct agx_macos_notification_queue *queue)
{
   kern_return_t result;

   if (!queue || queue->connection == IO_OBJECT_NULL || queue->id == 0) {
      return kIOReturnBadArgument;
   }

   if (queue->in_flight_submission_count != 0)
      return kIOReturnBusy;

   if (queue->release_state == AGX_MACOS_NOTIFICATION_QUEUE_ACTIVE) {
      result = agx_macos_notification_queue_call_release_selector(
         queue->connection, AGX_MACOS_SELECTOR_DESTROY_QUEUE, queue->id);
      if (result != KERN_SUCCESS)
         return result;
      queue->release_state = AGX_MACOS_NOTIFICATION_QUEUE_DESTROY_REQUESTED;
   }

   return queue->release_state ==
             AGX_MACOS_NOTIFICATION_QUEUE_DESTROY_REQUESTED
             ? KERN_SUCCESS
             : kIOReturnBadArgument;
}

kern_return_t
agx_macos_notification_queue_release_port(struct agx_macos_notification_queue *queue)
{
   kern_return_t result;

   if (!queue || queue->connection == IO_OBJECT_NULL ||
       queue->notification_port == MACH_PORT_NULL || queue->id == 0 ||
       queue->release_state !=
          AGX_MACOS_NOTIFICATION_QUEUE_DESTROY_REQUESTED) {
      return kIOReturnBadArgument;
   }

   result = agx_macos_notification_port_release(queue->notification_port);
   if (result != KERN_SUCCESS)
      return result;

   queue->notification_port = MACH_PORT_NULL;
   queue->release_state = AGX_MACOS_NOTIFICATION_QUEUE_PORT_RELEASED;
   return KERN_SUCCESS;
}

kern_return_t
agx_macos_notification_queue_finish_destroy(struct agx_macos_notification_queue *queue)
{
   kern_return_t result;

   if (!queue || queue->connection == IO_OBJECT_NULL || queue->id == 0 ||
       queue->release_state != AGX_MACOS_NOTIFICATION_QUEUE_PORT_RELEASED) {
      return kIOReturnBadArgument;
   }

   result = agx_macos_notification_queue_call_release_selector(
      queue->connection, AGX_MACOS_SELECTOR_RELEASE_QUEUE_NOTIFICATION,
      queue->id);
   if (result != KERN_SUCCESS)
      return result;

   *queue = (struct agx_macos_notification_queue){
      .connection = IO_OBJECT_NULL,
      .notification_port = MACH_PORT_NULL,
   };
   return KERN_SUCCESS;
}

kern_return_t
agx_macos_notification_queue_destroy(struct agx_macos_notification_queue *queue)
{
   kern_return_t result;

   if (!queue || queue->connection == IO_OBJECT_NULL || queue->id == 0)
      return kIOReturnBadArgument;

   if (queue->release_state == AGX_MACOS_NOTIFICATION_QUEUE_ACTIVE ||
       queue->release_state == AGX_MACOS_NOTIFICATION_QUEUE_DESTROY_REQUESTED) {
      result = agx_macos_notification_queue_begin_destroy(queue);
      if (result != KERN_SUCCESS)
         return result;
   }

   if (queue->release_state ==
       AGX_MACOS_NOTIFICATION_QUEUE_DESTROY_REQUESTED) {
      result = agx_macos_notification_queue_release_port(queue);
      if (result != KERN_SUCCESS)
         return result;
   }

   return agx_macos_notification_queue_finish_destroy(queue);
}

kern_return_t
agx_macos_notification_queue_get_state(
   const struct agx_macos_device_session *session,
   const struct agx_macos_notification_queue *queue,
   struct agx_macos_notification_queue_state *state)
{
   uint32_t capacity;
   uint32_t head;
   uint32_t tail;

   if (!agx_macos_notification_queue_is_current(session, queue) || !state) {
      return kIOReturnBadArgument;
   }

   capacity = __atomic_load_n(&queue->data_queue->queueSize, __ATOMIC_ACQUIRE);
   head = __atomic_load_n(&queue->data_queue->head, __ATOMIC_ACQUIRE);
   tail = __atomic_load_n(&queue->data_queue->tail, __ATOMIC_ACQUIRE);
   if (capacity == 0 || head >= capacity || tail >= capacity)
      return kIOReturnBadArgument;

   *state = (struct agx_macos_notification_queue_state){
      .capacity = capacity,
      .head = head,
      .tail = tail,
   };
   return KERN_SUCCESS;
}

kern_return_t
agx_macos_notification_queue_poll(
   const struct agx_macos_device_session *session,
   const struct agx_macos_notification_queue *queue, void *record,
   uint32_t *record_size)
{
   if (!agx_macos_notification_queue_is_current(session, queue) || !record ||
       !record_size || *record_size == 0) {
      return kIOReturnBadArgument;
   }

   return IODataQueueDequeue(queue->data_queue, record, record_size);
}

kern_return_t
agx_macos_notification_queue_poll_completion(
   const struct agx_macos_device_session *session,
   const struct agx_macos_notification_queue *queue,
   struct agx_macos_completion_record *record)
{
   uint32_t record_size = sizeof(*record);
   kern_return_t result =
      agx_macos_notification_queue_peek_completion(session, queue, record);

   if (result != KERN_SUCCESS)
      return result;

   result = agx_macos_notification_queue_poll(session, queue, record,
                                              &record_size);
   if (result != KERN_SUCCESS)
      return result;
   if (record_size != sizeof(*record))
      return kIOReturnBadArgument;

   return KERN_SUCCESS;
}

kern_return_t
agx_macos_notification_queue_bind_fence(
   const struct agx_macos_device_session *session,
   const struct agx_macos_notification_queue *queue,
   struct agx_macos_submission_fence *fence)
{
   if (!agx_macos_notification_queue_is_current(session, queue) || !fence ||
       fence->observation.queue_id != queue->id ||
       fence->completed_token_mask != 0 || fence->queue_api_generation != 0) {
      return kIOReturnBadArgument;
   }

   fence->queue_api_generation = queue->api_generation;
   return KERN_SUCCESS;
}

kern_return_t
agx_macos_notification_queue_poll_fence(
   const struct agx_macos_device_session *session,
   const struct agx_macos_notification_queue *queue,
   struct agx_macos_submission_fence *fence, bool *out_complete)
{
   struct agx_macos_completion_record raw;
   struct agx_macos_completion_record_observed completion;
   unsigned token_index;
   kern_return_t result;

   if (!agx_macos_notification_queue_is_current(session, queue) || !fence ||
       !out_complete || fence->observation.queue_id != queue->id ||
       fence->queue_api_generation != queue->api_generation)
      return kIOReturnBadArgument;

   *out_complete = agx_macos_submission_fence_is_complete(fence);
   if (*out_complete)
      return KERN_SUCCESS;

   result = agx_macos_notification_queue_peek_completion(session, queue, &raw);
   if (result != KERN_SUCCESS)
      return result;

   memcpy(&completion, raw.bytes, sizeof(completion));
   if (!agx_macos_submission_observation_matches_completion(
          &fence->observation, queue->id, &completion, &token_index) ||
       (fence->completed_token_mask & (1u << token_index))) {
      return kIOReturnBadArgument;
   }

   result = agx_macos_notification_queue_poll_completion(session, queue, &raw);
   if (result != KERN_SUCCESS)
      return result;

   if (!agx_macos_submission_fence_record_completion(fence, queue->id,
                                                     &completion)) {
      return kIOReturnBadArgument;
   }

   *out_complete = agx_macos_submission_fence_is_complete(fence);
   return KERN_SUCCESS;
}

kern_return_t
agx_macos_notification_queue_bind_lease(
   const struct agx_macos_device_session *session,
   struct agx_macos_notification_queue *queue,
   struct agx_macos_submission_lease *lease)
{
   kern_return_t result;

   if (!lease || !lease->active ||
       lease->state != AGX_MACOS_SUBMISSION_LEASE_ADMITTED ||
       lease->queue_lease_bound)
      return kIOReturnBadArgument;

   result = agx_macos_notification_queue_bind_fence(session, queue,
                                                     &lease->fence);
   if (result != KERN_SUCCESS)
      return result;

   lease->queue_connection = queue->connection;
   lease->bound_queue = queue;
   lease->bound_queue_id = queue->id;
   lease->bound_queue_api_generation = queue->api_generation;
   lease->queue_lease_bound = true;
   return KERN_SUCCESS;
}

kern_return_t
agx_macos_notification_queue_admit_lease_submission(
   struct agx_macos_notification_queue *queue,
   struct agx_macos_submission_lease *lease)
{
   if (!agx_macos_notification_queue_is_active(queue) || !lease ||
       !lease->active || !lease->queue_lease_bound ||
       lease->bound_queue != queue ||
       lease->queue_connection != queue->connection ||
       lease->bound_queue_id != queue->id ||
       lease->bound_queue_api_generation != queue->api_generation ||
       lease->fence.observation.queue_id != queue->id ||
       lease->fence.queue_api_generation != queue->api_generation ||
       lease->queue_submission_serial != 0) {
      return kIOReturnBadArgument;
   }

   if (queue->next_submission_serial == UINT64_MAX)
      return kIOReturnNoResources;

   lease->queue_submission_serial = ++queue->next_submission_serial;
   ++queue->in_flight_submission_count;
   return KERN_SUCCESS;
}

bool
agx_macos_notification_queue_lease_can_retire_submission(
   const struct agx_macos_notification_queue *queue,
   const struct agx_macos_submission_lease *lease)
{
   return agx_macos_notification_queue_is_active(queue) && lease &&
          lease->active && lease->bound_queue == queue &&
          lease->queue_connection == queue->connection &&
          lease->bound_queue_id == queue->id &&
          lease->bound_queue_api_generation == queue->api_generation &&
          lease->fence.observation.queue_id == queue->id &&
          lease->fence.queue_api_generation == queue->api_generation &&
          lease->state == AGX_MACOS_SUBMISSION_LEASE_IN_FLIGHT &&
          lease->queue_submission_serial != 0 &&
          queue->in_flight_submission_count != 0 &&
          queue->retired_submission_serial != UINT64_MAX &&
          lease->queue_submission_serial ==
             queue->retired_submission_serial + 1;
}

kern_return_t
agx_macos_notification_queue_retire_lease_submission(
   struct agx_macos_notification_queue *queue,
   struct agx_macos_submission_lease *lease)
{
   if (!agx_macos_notification_queue_lease_can_retire_submission(queue,
                                                                   lease)) {
      return kIOReturnNotPermitted;
   }

   queue->retired_submission_serial = lease->queue_submission_serial;
   --queue->in_flight_submission_count;
   return KERN_SUCCESS;
}

kern_return_t
agx_macos_notification_queue_abandon_lease_submission(
   struct agx_macos_notification_queue *queue,
   struct agx_macos_submission_lease *lease)
{
   if (!queue || !lease || !lease->active || lease->bound_queue != queue ||
       lease->queue_connection != queue->connection ||
       lease->bound_queue_id != queue->id ||
       lease->bound_queue_api_generation != queue->api_generation ||
       lease->fence.observation.queue_id != queue->id ||
       lease->fence.queue_api_generation != queue->api_generation ||
       lease->queue_submission_serial == 0 ||
       queue->in_flight_submission_count == 0) {
      return kIOReturnBadArgument;
   }

   /* Device loss makes all outstanding order evidence unusable. The owning
    * session invalidates this queue by generation, so a new queue is required
    * before another lease can enter flight. */
   queue->retired_submission_serial = queue->next_submission_serial;
   --queue->in_flight_submission_count;
   return KERN_SUCCESS;
}

kern_return_t
agx_macos_notification_queue_poll_lease(
   const struct agx_macos_device_session *session,
   const struct agx_macos_notification_queue *queue,
   struct agx_macos_submission_lease *lease, bool *out_complete)
{
   struct agx_macos_completion_record raw;
   struct agx_macos_completion_record_observed completion;
   unsigned token_index;
   kern_return_t result;

   if (!agx_macos_notification_queue_is_current(session, queue) || !lease ||
       !lease->active || !out_complete ||
       lease->state != AGX_MACOS_SUBMISSION_LEASE_IN_FLIGHT ||
       !lease->queue_lease_bound ||
       lease->bound_queue != queue || lease->queue_submission_serial == 0 ||
       lease->queue_connection != queue->connection ||
       lease->bound_queue_id != queue->id ||
       lease->bound_queue_api_generation != queue->api_generation ||
       lease->fence.observation.queue_id != queue->id ||
       lease->fence.queue_api_generation != queue->api_generation)
      return kIOReturnBadArgument;

   *out_complete = agx_macos_submission_fence_is_complete(&lease->fence);
   if (!lease->active)
      return *out_complete ? KERN_SUCCESS : kIOReturnBadArgument;

   result = agx_macos_notification_queue_peek_completion(session, queue, &raw);
   if (result != KERN_SUCCESS)
      return result;

   memcpy(&completion, raw.bytes, sizeof(completion));
   if (!agx_macos_submission_observation_matches_completion(
          &lease->fence.observation, queue->id, &completion, &token_index) ||
       (lease->fence.completed_token_mask & (1u << token_index))) {
      return kIOReturnBadArgument;
   }

   if ((lease->fence.completed_token_mask | (1u << token_index)) == 0x3 &&
       !agx_macos_notification_queue_lease_can_retire_submission(queue,
                                                                    lease)) {
      return kIOReturnNotPermitted;
   }

   result = agx_macos_notification_queue_poll_completion(session, queue, &raw);
   if (result != KERN_SUCCESS)
      return result;

   return agx_macos_submission_lease_record_completion(
      lease, queue->id, &completion, out_complete);
}

kern_return_t
agx_macos_notification_queue_peek_completion(
   const struct agx_macos_device_session *session,
   const struct agx_macos_notification_queue *queue,
   struct agx_macos_completion_record *record)
{
   IODataQueueEntry *entry;
   uint32_t size;

   if (!agx_macos_notification_queue_is_current(session, queue) || !record) {
      return kIOReturnBadArgument;
   }

   entry = IODataQueuePeek(queue->data_queue);
   if (!entry)
      return kIOReturnUnderrun;

   size = __atomic_load_n(&entry->size, __ATOMIC_ACQUIRE);
   if (size != sizeof(*record))
      return kIOReturnBadArgument;

   memcpy(record->bytes, entry->data, sizeof(record->bytes));
   return KERN_SUCCESS;
}

kern_return_t
agx_macos_notification_queue_drain_completions(
   const struct agx_macos_device_session *session,
   const struct agx_macos_notification_queue *queue,
   struct agx_macos_completion_record *records, uint32_t capacity,
   uint32_t *count)
{
   if (!agx_macos_notification_queue_is_current(session, queue) || !count ||
       (capacity && !records))
      return kIOReturnBadArgument;

   *count = 0;
   for (; *count < capacity; ++*count) {
      kern_return_t result =
         agx_macos_notification_queue_poll_completion(session, queue,
                                                       &records[*count]);

      if (result == kIOReturnUnderrun)
         return KERN_SUCCESS;
      if (result != KERN_SUCCESS)
         return result;
   }

   return KERN_SUCCESS;
}

kern_return_t
agx_macos_notification_queue_release_for_session_close(
   struct agx_macos_notification_queue *queue)
{
   kern_return_t result;

   if (!queue || queue->connection == IO_OBJECT_NULL ||
       queue->notification_port == MACH_PORT_NULL || queue->id == 0 ||
       queue->in_flight_submission_count != 0 ||
       (queue->release_state != AGX_MACOS_NOTIFICATION_QUEUE_ACTIVE &&
        queue->release_state != AGX_MACOS_NOTIFICATION_QUEUE_DESTROY_REQUESTED)) {
      return kIOReturnBadArgument;
   }

   result = agx_macos_notification_port_release(queue->notification_port);
   if (result != KERN_SUCCESS)
      return result;

   *queue = (struct agx_macos_notification_queue){
      .connection = IO_OBJECT_NULL,
      .notification_port = MACH_PORT_NULL,
   };
   return KERN_SUCCESS;
}
