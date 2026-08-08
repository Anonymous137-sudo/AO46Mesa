/*
 * Copyright 2026 Khronos_AppleICDs contributors
 * SPDX-License-Identifier: MIT
 */

#include "agx_macos_submission_observation.h"

#include <stdio.h>
#include <string.h>

int
main(void)
{
   const struct agx_macos_submit_descriptor_observed input = {
      .header0 = 2,
      .header1 = 1,
      .completion_tokens = {0x0102030405060708ull, 0x1112131415161718ull},
   };
   struct agx_macos_submission_observation observation = {0};
   struct agx_macos_submission_fence fence = {0};
   struct agx_macos_submission_carrier_observation carrier_observation = {0};
   struct agx_macos_submission_carrier_snapshot carrier_snapshot = {0};
   struct agx_macos_submission_carrier_extended_snapshot
      extended_snapshot = {0};
   struct agx_macos_submit_descriptor_observed invalid_input = input;
   struct agx_macos_completion_record_observed completion = {
      .token = input.completion_tokens[1],
   };
   uint8_t carrier[AGX_MACOS_SUBMISSION_CARRIER_AUXILIARY_OFFSET +
                   AGX_MACOS_SUBMISSION_CARRIER_EXTENDED_READABLE_PREFIX] = {0};
   const uint64_t opaque_pointer_slot = UINT64_C(0x1ff800000);
   unsigned token_index = 99;

   if (!agx_macos_submission_observation_decode(7, &input, sizeof(input),
                                                 &observation) ||
       observation.queue_id != 7 ||
       observation.descriptor.header0 != input.header0 ||
       observation.descriptor.header1 != input.header1 ||
       observation.descriptor.completion_tokens[0] != input.completion_tokens[0] ||
       observation.descriptor.completion_tokens[1] != input.completion_tokens[1]) {
      fputs("AGX_MACOS_SUBMISSION_OBSERVATION_SMOKE decode failed\n", stderr);
      return 1;
   }

   memcpy(carrier, &input, sizeof(input));
   memset(carrier + AGX_MACOS_SUBMISSION_CARRIER_AUXILIARY_OFFSET, 0xa5,
          AGX_MACOS_SUBMISSION_CARRIER_EXTENDED_READABLE_PREFIX);
   memcpy(carrier + AGX_MACOS_SUBMISSION_CARRIER_AUXILIARY_OFFSET +
             AGX_MACOS_SUBMISSION_CARRIER_OPAQUE_POINTER_SLOT_OFFSET,
          &opaque_pointer_slot, sizeof(opaque_pointer_slot));
   if (!agx_macos_submission_carrier_observation_decode(
          7, carrier, sizeof(input),
          carrier + AGX_MACOS_SUBMISSION_CARRIER_AUXILIARY_OFFSET,
          AGX_MACOS_SUBMISSION_CARRIER_MIN_READABLE_PREFIX,
          &carrier_observation) ||
       carrier_observation.submission.queue_id != 7 ||
       carrier_observation.auxiliary_offset !=
          AGX_MACOS_SUBMISSION_CARRIER_AUXILIARY_OFFSET ||
       carrier_observation.auxiliary_readable_prefix !=
          AGX_MACOS_SUBMISSION_CARRIER_MIN_READABLE_PREFIX ||
       agx_macos_submission_carrier_observation_decode(
          7, carrier, sizeof(input),
          carrier + AGX_MACOS_SUBMISSION_CARRIER_AUXILIARY_OFFSET + 1,
          AGX_MACOS_SUBMISSION_CARRIER_MIN_READABLE_PREFIX,
          &carrier_observation) ||
       agx_macos_submission_carrier_observation_decode(
          7, carrier, sizeof(input),
          carrier + AGX_MACOS_SUBMISSION_CARRIER_AUXILIARY_OFFSET,
          AGX_MACOS_SUBMISSION_CARRIER_MIN_READABLE_PREFIX - 1,
          &carrier_observation) ||
       agx_macos_submission_carrier_observation_decode(
          7, carrier, sizeof(input), NULL,
          AGX_MACOS_SUBMISSION_CARRIER_MIN_READABLE_PREFIX,
          &carrier_observation) ||
       !agx_macos_submission_carrier_snapshot_capture(
          7, carrier, sizeof(input),
          carrier + AGX_MACOS_SUBMISSION_CARRIER_AUXILIARY_OFFSET,
          AGX_MACOS_SUBMISSION_CARRIER_MIN_READABLE_PREFIX,
          &carrier_snapshot) ||
       carrier_snapshot.observation.submission.queue_id != 7 ||
       carrier_snapshot.auxiliary_prefix[0] != 0xa5 ||
       carrier_snapshot.auxiliary_prefix[
          AGX_MACOS_SUBMISSION_CARRIER_MIN_READABLE_PREFIX - 1] != 0xa5) {
      fputs("AGX_MACOS_SUBMISSION_OBSERVATION_SMOKE carrier validation failed\n",
            stderr);
      return 1;
   }

   if (!agx_macos_submission_carrier_extended_snapshot_capture(
          7, carrier, sizeof(input),
          carrier + AGX_MACOS_SUBMISSION_CARRIER_AUXILIARY_OFFSET,
          AGX_MACOS_SUBMISSION_CARRIER_EXTENDED_READABLE_PREFIX,
          &extended_snapshot) ||
       extended_snapshot.observation.auxiliary_readable_prefix !=
          AGX_MACOS_SUBMISSION_CARRIER_EXTENDED_READABLE_PREFIX ||
       extended_snapshot.opaque_pointer_slot != opaque_pointer_slot ||
       agx_macos_submission_carrier_extended_snapshot_capture(
          7, carrier, sizeof(input),
          carrier + AGX_MACOS_SUBMISSION_CARRIER_AUXILIARY_OFFSET,
          AGX_MACOS_SUBMISSION_CARRIER_EXTENDED_READABLE_PREFIX - 1,
          &extended_snapshot)) {
      fputs("AGX_MACOS_SUBMISSION_OBSERVATION_SMOKE extended carrier validation failed\n",
            stderr);
      return 1;
   }

   carrier[AGX_MACOS_SUBMISSION_CARRIER_AUXILIARY_OFFSET] = 0;
   memset(carrier + AGX_MACOS_SUBMISSION_CARRIER_AUXILIARY_OFFSET +
             AGX_MACOS_SUBMISSION_CARRIER_OPAQUE_POINTER_SLOT_OFFSET,
          0, sizeof(opaque_pointer_slot));
   invalid_input.header0 = 0;
   if (carrier_snapshot.auxiliary_prefix[0] != 0xa5 ||
       extended_snapshot.opaque_pointer_slot != opaque_pointer_slot ||
       agx_macos_submission_carrier_snapshot_capture(
          7, carrier, sizeof(input),
          carrier + AGX_MACOS_SUBMISSION_CARRIER_AUXILIARY_OFFSET,
          AGX_MACOS_SUBMISSION_CARRIER_MIN_READABLE_PREFIX - 1,
          &carrier_snapshot) ||
       agx_macos_submission_carrier_extended_snapshot_capture(
          7, carrier, sizeof(input),
          carrier + AGX_MACOS_SUBMISSION_CARRIER_AUXILIARY_OFFSET,
          AGX_MACOS_SUBMISSION_CARRIER_EXTENDED_READABLE_PREFIX,
          &extended_snapshot) ||
       agx_macos_submission_observation_decode(7, &invalid_input,
                                                sizeof(invalid_input),
                                                &observation)) {
      fputs("AGX_MACOS_SUBMISSION_OBSERVATION_SMOKE snapshot isolation failed\n",
            stderr);
      return 1;
   }

   if (!agx_macos_submission_observation_matches_completion(
          &observation, 7, &completion, &token_index) || token_index != 1) {
      fputs("AGX_MACOS_SUBMISSION_OBSERVATION_SMOKE token match failed\n",
            stderr);
      return 1;
   }

   if (!agx_macos_submission_fence_init(7, &input, sizeof(input), &fence) ||
       agx_macos_submission_fence_is_complete(&fence) ||
       !agx_macos_submission_fence_record_completion(&fence, 7, &completion) ||
       agx_macos_submission_fence_is_complete(&fence) ||
       agx_macos_submission_fence_record_completion(&fence, 7, &completion)) {
      fputs("AGX_MACOS_SUBMISSION_OBSERVATION_SMOKE fence first completion failed\n",
            stderr);
      return 1;
   }

   completion.token = input.completion_tokens[0];
   if (agx_macos_submission_fence_record_completion(&fence, 8, &completion) ||
       !agx_macos_submission_fence_record_completion(&fence, 7, &completion) ||
       !agx_macos_submission_fence_is_complete(&fence)) {
      fputs("AGX_MACOS_SUBMISSION_OBSERVATION_SMOKE fence completion state failed\n",
            stderr);
      return 1;
   }

   if (agx_macos_submission_observation_matches_completion(&observation, 8,
                                                            &completion, NULL)) {
      fputs("AGX_MACOS_SUBMISSION_OBSERVATION_SMOKE queue match failed\n",
            stderr);
      return 1;
   }

   if (agx_macos_submission_fence_record_completion(&fence, 7, &completion)) {
      fputs("AGX_MACOS_SUBMISSION_OBSERVATION_SMOKE accepted a completed fence\n",
            stderr);
      return 1;
   }

   completion.token = 0;
   if (agx_macos_submission_observation_matches_completion(&observation, 7,
                                                            &completion, NULL) ||
       agx_macos_submission_observation_decode(0, &input, sizeof(input),
                                                &observation) ||
       agx_macos_submission_observation_decode(7, &input, sizeof(input) - 1,
                                                &observation)) {
      fputs("AGX_MACOS_SUBMISSION_OBSERVATION_SMOKE accepted invalid input\n",
            stderr);
      return 1;
   }

   puts("AGX_MACOS_SUBMISSION_OBSERVATION_SMOKE complete");
   return 0;
}
