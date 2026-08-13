/*
 * Copyright 2026 Khronos_AppleICDs contributors
 * SPDX-License-Identifier: MIT
 */

#include "agx_macos_submission_observation.h"

#include <stdint.h>
#include <string.h>

static uint64_t
agx_macos_submission_fingerprint_bytes(uint64_t fingerprint,
                                       const void *bytes, size_t byte_count)
{
   const uint8_t *data = bytes;

   for (size_t i = 0; i < byte_count; ++i) {
      fingerprint ^= data[i];
      fingerprint *= UINT64_C(0x100000001b3);
   }

   return fingerprint;
}

static uint64_t
agx_macos_submission_carrier_extended_fingerprint(
   const struct agx_macos_submission_carrier_extended_snapshot *snapshot)
{
   const uint64_t auxiliary_offset = snapshot->observation.auxiliary_offset;
   const uint64_t auxiliary_readable_prefix =
      snapshot->observation.auxiliary_readable_prefix;
   uint64_t fingerprint = UINT64_C(0xcbf29ce484222325);

   fingerprint = agx_macos_submission_fingerprint_bytes(
      fingerprint, &snapshot->observation.submission.queue_id,
      sizeof(snapshot->observation.submission.queue_id));
   fingerprint = agx_macos_submission_fingerprint_bytes(
      fingerprint, &snapshot->observation.submission.descriptor,
      sizeof(snapshot->observation.submission.descriptor));
   fingerprint = agx_macos_submission_fingerprint_bytes(
      fingerprint, &auxiliary_offset, sizeof(auxiliary_offset));
   fingerprint = agx_macos_submission_fingerprint_bytes(
      fingerprint, &auxiliary_readable_prefix,
      sizeof(auxiliary_readable_prefix));
   fingerprint = agx_macos_submission_fingerprint_bytes(
      fingerprint, &snapshot->opaque_pointer_slot,
      sizeof(snapshot->opaque_pointer_slot));
   return agx_macos_submission_fingerprint_bytes(
      fingerprint, snapshot->auxiliary_prefix,
      sizeof(snapshot->auxiliary_prefix));
}

bool
agx_macos_submission_observation_decode(
   uint32_t queue_id, const void *bytes, size_t byte_count,
   struct agx_macos_submission_observation *out_observation)
{
   struct agx_macos_submit_descriptor_observed descriptor;

   if (queue_id == 0 || !bytes || !out_observation ||
       byte_count != sizeof(descriptor)) {
      return false;
   }

   memcpy(&descriptor, bytes, sizeof(descriptor));
   if (descriptor.header0 != AGX_MACOS_SUBMISSION_DESCRIPTOR_HEADER0 ||
       descriptor.header1 != AGX_MACOS_SUBMISSION_DESCRIPTOR_HEADER1 ||
       descriptor.completion_tokens[0] == 0 ||
       descriptor.completion_tokens[1] == 0 ||
       descriptor.completion_tokens[0] == descriptor.completion_tokens[1]) {
      return false;
   }

   *out_observation = (struct agx_macos_submission_observation){
      .queue_id = queue_id,
      .descriptor = descriptor,
   };
   return true;
}

bool
agx_macos_submission_carrier_snapshot_capture(
   uint32_t queue_id, const void *descriptor_bytes, size_t descriptor_size,
   const void *auxiliary_bytes, size_t auxiliary_readable_prefix,
   struct agx_macos_submission_carrier_snapshot *out_snapshot)
{
   struct agx_macos_submission_carrier_observation observation;

   if (!out_snapshot ||
       !agx_macos_submission_carrier_observation_decode(
          queue_id, descriptor_bytes, descriptor_size, auxiliary_bytes,
          auxiliary_readable_prefix, &observation)) {
      return false;
   }

   *out_snapshot = (struct agx_macos_submission_carrier_snapshot){
      .observation = observation,
   };
   memcpy(out_snapshot->auxiliary_prefix, auxiliary_bytes,
          sizeof(out_snapshot->auxiliary_prefix));
   return true;
}

bool
agx_macos_submission_carrier_extended_snapshot_capture(
   uint32_t queue_id, const void *descriptor_bytes, size_t descriptor_size,
   const void *auxiliary_bytes, size_t auxiliary_readable_prefix,
   struct agx_macos_submission_carrier_extended_snapshot *out_snapshot)
{
   struct agx_macos_submission_carrier_observation observation;
   uint64_t opaque_pointer_slot;

   if (!out_snapshot ||
       auxiliary_readable_prefix <
          AGX_MACOS_SUBMISSION_CARRIER_EXTENDED_READABLE_PREFIX ||
       !agx_macos_submission_carrier_observation_decode(
          queue_id, descriptor_bytes, descriptor_size, auxiliary_bytes,
          AGX_MACOS_SUBMISSION_CARRIER_EXTENDED_READABLE_PREFIX,
          &observation)) {
      return false;
   }

   memcpy(&opaque_pointer_slot,
          (const uint8_t *)auxiliary_bytes +
             AGX_MACOS_SUBMISSION_CARRIER_OPAQUE_POINTER_SLOT_OFFSET,
          sizeof(opaque_pointer_slot));
   if (opaque_pointer_slot == 0)
      return false;

   *out_snapshot = (struct agx_macos_submission_carrier_extended_snapshot){
      .observation = observation,
      .opaque_pointer_slot = opaque_pointer_slot,
   };
   memcpy(out_snapshot->auxiliary_prefix, auxiliary_bytes,
          sizeof(out_snapshot->auxiliary_prefix));
   out_snapshot->integrity_fingerprint =
      agx_macos_submission_carrier_extended_fingerprint(out_snapshot);
   return true;
}

bool
agx_macos_submission_carrier_extended_snapshot_is_intact(
   const struct agx_macos_submission_carrier_extended_snapshot *snapshot)
{
   return snapshot && snapshot->observation.submission.queue_id != 0 &&
          snapshot->observation.auxiliary_offset ==
             AGX_MACOS_SUBMISSION_CARRIER_AUXILIARY_OFFSET &&
          snapshot->observation.auxiliary_readable_prefix ==
             AGX_MACOS_SUBMISSION_CARRIER_EXTENDED_READABLE_PREFIX &&
          snapshot->opaque_pointer_slot != 0 &&
          snapshot->integrity_fingerprint ==
             agx_macos_submission_carrier_extended_fingerprint(snapshot);
}

bool
agx_macos_submission_carrier_observation_decode(
   uint32_t queue_id, const void *descriptor_bytes, size_t descriptor_size,
   const void *auxiliary_bytes, size_t auxiliary_readable_prefix,
   struct agx_macos_submission_carrier_observation *out_observation)
{
   struct agx_macos_submission_observation submission;
   uintptr_t descriptor_address;
   uintptr_t auxiliary_address;

   if (!auxiliary_bytes || !out_observation ||
       auxiliary_readable_prefix <
          AGX_MACOS_SUBMISSION_CARRIER_MIN_READABLE_PREFIX ||
       !agx_macos_submission_observation_decode(
          queue_id, descriptor_bytes, descriptor_size, &submission)) {
      return false;
   }

   descriptor_address = (uintptr_t)descriptor_bytes;
   auxiliary_address = (uintptr_t)auxiliary_bytes;
   if (auxiliary_address < descriptor_address ||
       auxiliary_address - descriptor_address !=
          AGX_MACOS_SUBMISSION_CARRIER_AUXILIARY_OFFSET) {
      return false;
   }

   *out_observation = (struct agx_macos_submission_carrier_observation){
      .submission = submission,
      .auxiliary_offset = AGX_MACOS_SUBMISSION_CARRIER_AUXILIARY_OFFSET,
      .auxiliary_readable_prefix = auxiliary_readable_prefix,
   };
   return true;
}

bool
agx_macos_submission_trap_observation_decode(
   uint32_t trap_index, uintptr_t queue_value, uintptr_t descriptor_size,
   uintptr_t descriptor_address, uintptr_t auxiliary_address,
   size_t auxiliary_readable_prefix,
   struct agx_macos_submission_trap_observation *out_observation)
{
   struct agx_macos_submission_carrier_observation carrier;

   if (!out_observation || trap_index != 0 || queue_value == 0 ||
       queue_value > UINT32_MAX ||
       descriptor_size != sizeof(struct agx_macos_submit_descriptor_observed) ||
       descriptor_address == 0 || auxiliary_address == 0 ||
       !agx_macos_submission_carrier_observation_decode(
          (uint32_t)queue_value, (const void *)descriptor_address,
          (size_t)descriptor_size, (const void *)auxiliary_address,
          auxiliary_readable_prefix, &carrier)) {
      return false;
   }

   *out_observation = (struct agx_macos_submission_trap_observation){
      .carrier = carrier,
      .trap_index = trap_index,
      .descriptor_size = (size_t)descriptor_size,
   };
   return true;
}

bool
agx_macos_submission_observation_matches_completion(
   const struct agx_macos_submission_observation *observation,
   uint32_t completion_queue_id,
   const struct agx_macos_completion_record_observed *record,
   unsigned *out_token_index)
{
   if (!observation || !record || completion_queue_id == 0 ||
       observation->queue_id != completion_queue_id) {
      return false;
   }

   for (unsigned i = 0; i < 2; ++i) {
      if (record->token == observation->descriptor.completion_tokens[i]) {
         if (out_token_index)
            *out_token_index = i;
         return true;
      }
   }

   return false;
}

bool
agx_macos_submission_fence_init(
   uint32_t queue_id, const void *descriptor_bytes, size_t descriptor_size,
   struct agx_macos_submission_fence *out_fence)
{
   struct agx_macos_submission_observation observation;

   if (!out_fence ||
       !agx_macos_submission_observation_decode(queue_id, descriptor_bytes,
                                                 descriptor_size, &observation)) {
      return false;
   }

   *out_fence = (struct agx_macos_submission_fence){
      .observation = observation,
   };
   return true;
}

bool
agx_macos_submission_fence_record_completion(
   struct agx_macos_submission_fence *fence, uint32_t completion_queue_id,
   const struct agx_macos_completion_record_observed *record)
{
   unsigned token_index;
   uint8_t token_bit;

   if (!fence || !agx_macos_submission_observation_matches_completion(
                    &fence->observation, completion_queue_id, record,
                    &token_index)) {
      return false;
   }

   token_bit = 1u << token_index;
   if (fence->completed_token_mask & token_bit)
      return false;

   fence->completed_token_mask |= token_bit;
   return true;
}

bool
agx_macos_submission_fence_is_complete(
   const struct agx_macos_submission_fence *fence)
{
   return fence && fence->completed_token_mask == 0x3;
}
