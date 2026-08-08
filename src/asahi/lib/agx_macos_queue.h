/*
 * Copyright 2026 Khronos_AppleICDs contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <IOKit/IODataQueueClient.h>
#include <mach/mach.h>

#include "agx_macos_device.h"

struct agx_macos_submission_fence;
struct agx_macos_submission_lease;

enum agx_macos_notification_queue_release_state {
   AGX_MACOS_NOTIFICATION_QUEUE_ACTIVE,
   AGX_MACOS_NOTIFICATION_QUEUE_DESTROY_REQUESTED,
   AGX_MACOS_NOTIFICATION_QUEUE_PORT_RELEASED,
};

/* This is an experimental completion-notification queue only. It is not an
 * AGX command queue and cannot submit GPU work. */
struct agx_macos_notification_queue {
   io_connect_t connection;
   IODataQueueMemory *data_queue;
   mach_port_t notification_port;
   uint32_t id;
   enum agx_macos_notification_queue_release_state release_state;
};

struct agx_macos_notification_queue_state {
   uint32_t capacity;
   uint32_t head;
   uint32_t tail;
};

#define AGX_MACOS_COMPLETION_RECORD_SIZE 40

/* Only the record size is established. Its fields remain opaque until command
 * submission is independently validated. */
struct agx_macos_completion_record {
   uint8_t bytes[AGX_MACOS_COMPLETION_RECORD_SIZE];
};

kern_return_t agx_macos_notification_queue_create(
   const struct agx_macos_device_session *session,
   struct agx_macos_notification_queue *queue);
/* Split teardown follows the no-submit Metal lifecycle: selector 0x08, local
 * port release, then selector 0x11. */
kern_return_t
agx_macos_notification_queue_begin_destroy(struct agx_macos_notification_queue *queue);
kern_return_t
agx_macos_notification_queue_release_port(struct agx_macos_notification_queue *queue);
kern_return_t
agx_macos_notification_queue_finish_destroy(struct agx_macos_notification_queue *queue);
/* Replays the validated no-submit notification-queue teardown. */
kern_return_t
agx_macos_notification_queue_destroy(struct agx_macos_notification_queue *queue);
kern_return_t agx_macos_notification_queue_get_state(
   const struct agx_macos_notification_queue *queue,
   struct agx_macos_notification_queue_state *state);

/* Polls one future completion record without blocking. */
kern_return_t agx_macos_notification_queue_poll(
   const struct agx_macos_notification_queue *queue, void *record,
   uint32_t *record_size);
/* The queue has one user-space consumer. Unknown entry sizes remain queued. */
kern_return_t agx_macos_notification_queue_peek_completion(
   const struct agx_macos_notification_queue *queue,
   struct agx_macos_completion_record *record);
kern_return_t agx_macos_notification_queue_poll_completion(
   const struct agx_macos_notification_queue *queue,
   struct agx_macos_completion_record *record);
/* Consumes a completion only after it matches fence's queue and token. A
 * foreign or malformed entry remains queued for its owner to inspect. */
kern_return_t agx_macos_notification_queue_poll_fence(
   const struct agx_macos_notification_queue *queue,
   struct agx_macos_submission_fence *fence, bool *out_complete);
/* Routes one known completion into a submission lease. The final token retires
 * its pinned BO ranges; unrelated records remain queued. */
kern_return_t agx_macos_notification_queue_poll_lease(
   const struct agx_macos_notification_queue *queue,
   struct agx_macos_submission_lease *lease, bool *out_complete);
/* Drains up to capacity known completion records without blocking. */
kern_return_t agx_macos_notification_queue_drain_completions(
   const struct agx_macos_notification_queue *queue,
   struct agx_macos_completion_record *records, uint32_t capacity,
   uint32_t *count);

/* This relinquishes the local notification port after any incomplete teardown.
 * The caller must close the owning AGX session immediately afterwards. */
kern_return_t agx_macos_notification_queue_release_for_session_close(
   struct agx_macos_notification_queue *queue);
