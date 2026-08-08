/*
 * Copyright 2021-2022 Alyssa Rosenzweig
 * SPDX-License-Identifier: MIT
 */
#include <assert.h>
#include <dlfcn.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <IOKit/IODataQueueClient.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/vm_region.h>

#include "util/compiler.h"
#include "util/u_hexdump.h"
#include "decode.h"
#include "dyld_interpose.h"
#include "agx_macos_submission_observation.h"
#include "util.h"

#define HANDLE(x) (x ^ (1 << 29))

/*
 * This section contains the minimal set of definitions to trace the macOS
 * (IOKit) interface to the AGX accelerator.
 * They are not used under Linux.
 *
 * Information is this file was originally determined independently. More
 * recently, names have been augmented via the oob_timestamp code sample from
 * Project Zero [1]
 *
 * [1] https://bugs.chromium.org/p/project-zero/issues/detail?id=1986
 */

#define AGX_SERVICE_TYPE 0x100005

enum agx_selector {
   AGX_SELECTOR_GET_GLOBAL_IDS = 0x6,
   AGX_SELECTOR_SET_API = 0x7,
   /* Historical macOS selector assignments retained for trace decoding only.
    * Current macOS reuses several numbers with different payload contracts. */
   AGX_SELECTOR_CREATE_COMMAND_QUEUE_LEGACY = 0x8,
   AGX_SELECTOR_FREE_COMMAND_QUEUE_LEGACY = 0x9,
   AGX_SELECTOR_ALLOCATE_MEM_LEGACY = 0xA,
   AGX_SELECTOR_FREE_MEM_LEGACY = 0xB,
   AGX_SELECTOR_CREATE_SHMEM_LEGACY = 0xF,
   AGX_SELECTOR_FREE_SHMEM_LEGACY = 0x10,
   AGX_SELECTOR_CREATE_NOTIFICATION_QUEUE_LEGACY = 0x11,
   AGX_SELECTOR_FREE_NOTIFICATION_QUEUE_LEGACY = 0x12,
   AGX_SELECTOR_SUBMIT_COMMAND_BUFFERS_LEGACY = 0x1E,
   AGX_SELECTOR_GET_VERSION_LEGACY = 0x2A,
   AGX_NUM_SELECTORS = 0x33
};

/* Current macOS uses the allocation interface at selector 9. The trace-only
 * decoder keys off its observed request/reply sizes before interpreting it. */
#define AGX_SELECTOR_ALLOCATE_MEM_MODERN 0x9
#define AGX_SELECTOR_COMMAND_PAIR_NO_INPUT_MODERN 0x6
#define AGX_SELECTOR_COMMAND_PAIR_CONFIGURED_MODERN 0xE
#define AGX_ALLOCATE_MEM_ATTR_WRITE_COMBINED (1ull << 42)
#define AGX_ALLOCATE_MEM_STORAGE_PRIVATE 0x2000
#define AGX_TRACE_TRAP_PAYLOAD_LIMIT 4096
#define AGX_TRACE_AUX_PREFIX_LIMIT 256
#define AGX_TRACE_AUX_EXTENDED_LIMIT 4096
#define AGX_TRACE_AUX_POINTER_TARGET_LIMIT 512
#define AGX_TRACE_AUX_POINTER_LIMIT 24
#define AGX_TRACE_MAX_QUEUES 64
#define AGX_TRACE_MAX_NOTIFICATION_PORTS 64
#define AGX_TRACE_MAX_RESOURCES 1024
#define AGX_TRACE_AUX_DIFF_RANGES 12
#define AGX_TRACE_AUX_RESOURCE_REFS 16
#define AGX_TRACE_RESOURCE_SCAN_LIMIT (64 * 1024)
#define AGX_TRACE_RESOURCE_SCAN_REFS 64

/* Fields isolated from controlled Metal buffer allocations. The remaining
 * bytes stay opaque until their UABI semantics are independently verified. */
struct agx_allocate_resource_req_modern {
   uint64_t attributes;
   uint8_t unknown0[12];
   uint32_t storage_flags;
   uint8_t opaque_prefix[0x4a - 0x18];
   /* Measured in direct allocation traces only; not used as a UABI field. */
   uint16_t size_units_candidate;
   uint8_t opaque_suffix[0x68 - 0x4c];
} __attribute__((packed));

_Static_assert(sizeof(struct agx_allocate_resource_req_modern) == 0x68,
               "modern AGX allocation request size");

struct IOAccelCommandQueueSubmitArgs_Command {
   uint32_t command_buffer_shmem_id;
   uint32_t segment_list_shmem_id;
   uint64_t unk1B; // 0, new in 12.x
   uint64_t notify_1;
   uint64_t notify_2;
   uint32_t unk2;
   uint32_t unk3;
} __attribute__((packed));

struct agx_allocate_resource_resp {
   /* Returned GPU virtual address */
   uint64_t gpu_va;

   /* Returned CPU virtual address */
   uint64_t cpu;

   uint32_t unk4[3];

   /* Handle used to identify the resource in the segment list */
   uint32_t handle;

   /* Size of the root resource from which we are allocated. If this is not a
    * suballocation, this is equal to the size.
    */
   uint64_t root_size;

   /* Globally unique identifier for the resource, shown in Instruments */
   uint32_t guid;

   uint32_t unk11[7];

   /* Maximum size of the suballocation. For a suballocation, this equals:
    *
    *    sub_size = root_size - (sub_cpu - root_cpu)
    *
    * For root allocations, this equals the size.
    */
   uint64_t sub_size;
} __attribute__((packed));

struct agx_allocate_resource_resp_modern {
   uint64_t gpu_va;
   uint64_t cpu;
   uint32_t unk4[5];
   uint32_t handle;
   uint64_t root_size;
   uint32_t guid;
   uint32_t unk11[7];
   uint64_t sub_size;
} __attribute__((packed));

_Static_assert(sizeof(struct agx_allocate_resource_resp_modern) == 0x58,
               "modern AGX allocation reply size");

/* Observed while creating Metal command queues on current macOS. This is
 * trace-only: it identifies the completion data queue associated with a
 * returned queue identifier, but does not authorize runtime queue creation. */
struct agx_notification_queue_resp_modern {
   uint64_t data_queue;
   uint32_t queue_id;
   uint32_t reserved;
} __attribute__((packed));

_Static_assert(sizeof(struct agx_notification_queue_resp_modern) == 0x10,
               "modern AGX notification queue reply size");

/* Selectors 6 and 14 return this shape during controlled Metal command
 * infrastructure initialization. The values are intentionally opaque: this
 * decoder records the stable call contract without assigning a private UABI
 * meaning to either field. */
struct agx_command_pair_resp_modern {
   uint64_t value0;
   uint64_t value1;
} __attribute__((packed));

_Static_assert(sizeof(struct agx_command_pair_resp_modern) == 0x10,
               "modern AGX command pair reply size");

/*
 * Wrap IOKit entrypoints to intercept communication between the AGX kernel
 * extension and userspace clients. IOKit prototypes are public from the IOKit
 * source release.
 */

mach_port_t metal_connection = 0;

struct agxdecode_ctx *decode_ctx = NULL;

struct agx_trace_queue {
   uint64_t data_queue;
   uint32_t id;
};

struct agx_trace_notification_port {
   mach_port_t port;
   io_connect_t connection;
   uint32_t queue_id;
   bool bound;
};

struct agx_trace_resource {
   uint64_t gpu_va;
   uint64_t cpu;
   uint64_t size;
   uint32_t handle;
   uint64_t fingerprint;
   bool fingerprint_valid;
};

struct agx_trace_aux_snapshot {
   uint32_t queue_id;
   size_t length;
   bool valid;
   uint8_t data[AGX_TRACE_AUX_PREFIX_LIMIT];
};

static struct agx_trace_queue agx_trace_queues[AGX_TRACE_MAX_QUEUES];
static unsigned agx_trace_queue_count;
static struct agx_trace_notification_port
   agx_trace_notification_ports[AGX_TRACE_MAX_NOTIFICATION_PORTS];
static unsigned agx_trace_notification_port_count;
static struct agx_trace_resource agx_trace_resources[AGX_TRACE_MAX_RESOURCES];
static unsigned agx_trace_resource_count;
static struct agx_trace_aux_snapshot
   agx_trace_aux_snapshots[AGX_TRACE_MAX_QUEUES];

static void
agx_trace_track_queue(uint64_t data_queue, uint32_t id)
{
   for (unsigned i = 0; i < agx_trace_queue_count; ++i) {
      if (agx_trace_queues[i].id == id ||
          agx_trace_queues[i].data_queue == data_queue) {
         agx_trace_queues[i] = (struct agx_trace_queue){data_queue, id};
         return;
      }
   }

   if (agx_trace_queue_count < AGX_TRACE_MAX_QUEUES) {
      agx_trace_queues[agx_trace_queue_count++] =
         (struct agx_trace_queue){data_queue, id};
   }
}

static uint32_t
agx_trace_queue_id(const void *data_queue)
{
   uint64_t address = (uint64_t)(uintptr_t)data_queue;

   for (unsigned i = 0; i < agx_trace_queue_count; ++i) {
      if (agx_trace_queues[i].data_queue == address)
         return agx_trace_queues[i].id;
   }

   return 0;
}

static struct agx_trace_notification_port *
agx_trace_find_notification_port(mach_port_t port)
{
   for (unsigned i = 0; i < agx_trace_notification_port_count; ++i) {
      if (agx_trace_notification_ports[i].port == port)
         return &agx_trace_notification_ports[i];
   }

   return NULL;
}

static void
agx_trace_track_notification_port(mach_port_t port)
{
   if (port == MACH_PORT_NULL || agx_trace_find_notification_port(port))
      return;

   if (agx_trace_notification_port_count == AGX_TRACE_MAX_NOTIFICATION_PORTS)
      return;

   agx_trace_notification_ports[agx_trace_notification_port_count++] =
      (struct agx_trace_notification_port){.port = port};
}

static void
agx_trace_bind_notification_port(io_connect_t connection, mach_port_t port,
                                 uintptr_t reference)
{
   struct agx_trace_notification_port *tracked;

   if (connection != metal_connection || reference > UINT32_MAX)
      return;

   agx_trace_track_notification_port(port);
   tracked = agx_trace_find_notification_port(port);
   if (!tracked)
      return;

   *tracked = (struct agx_trace_notification_port){
      .port = port,
      .connection = connection,
      .queue_id = reference,
      .bound = true,
   };
}

static const struct agx_trace_notification_port *
agx_trace_find_bound_notification_port(io_connect_t connection, uint32_t id)
{
   for (unsigned i = 0; i < agx_trace_notification_port_count; ++i) {
      const struct agx_trace_notification_port *tracked =
         &agx_trace_notification_ports[i];

      if (tracked->bound && tracked->connection == connection &&
          tracked->queue_id == id) {
         return tracked;
      }
   }

   return NULL;
}

static void
agx_trace_report_notification_port_state(io_connect_t connection,
                                         uint32_t selector,
                                         const uint64_t *input,
                                         uint32_t input_count,
                                         const char *phase)
{
   const struct agx_trace_notification_port *tracked;
   const char *trace_notification_ports;
   static const mach_port_right_t rights[] = {
      MACH_PORT_RIGHT_RECEIVE,
      MACH_PORT_RIGHT_SEND,
      MACH_PORT_RIGHT_SEND_ONCE,
   };

   trace_notification_ports = getenv("AGX_TRACE_NOTIFICATION_PORTS");
   if (!trace_notification_ports ||
       strcmp(trace_notification_ports, "1") != 0 || !input ||
       input_count != 1 ||
       (selector != AGX_SELECTOR_CREATE_COMMAND_QUEUE_LEGACY &&
        selector != AGX_SELECTOR_CREATE_NOTIFICATION_QUEUE_LEGACY) ||
       input[0] > UINT32_MAX) {
      return;
   }

   tracked = agx_trace_find_bound_notification_port(connection, input[0]);
   if (!tracked)
      return;

   for (unsigned i = 0; i < ARRAY_SIZE(rights); ++i) {
      mach_port_urefs_t refs = 0;
      kern_return_t ret = mach_port_get_refs(mach_task_self(), tracked->port,
                                             rights[i], &refs);

      printf("MODERN_NOTIFICATION_PORT_STATE phase=%s selector=%u"
             " connect=%X id=%u port=%X right=%d refs=%u result=%#x\n",
             phase, selector, tracked->connection, tracked->queue_id,
             tracked->port, rights[i], refs, ret);
   }
}

static void
agx_trace_track_resource(uint64_t gpu_va, uint64_t cpu, uint64_t size,
                         uint32_t handle)
{
   for (unsigned i = 0; i < agx_trace_resource_count; ++i) {
      if (agx_trace_resources[i].handle == handle) {
         agx_trace_resources[i] =
            (struct agx_trace_resource){gpu_va, cpu, size, handle};
         return;
      }
   }

   if (agx_trace_resource_count < AGX_TRACE_MAX_RESOURCES) {
      agx_trace_resources[agx_trace_resource_count++] =
         (struct agx_trace_resource){gpu_va, cpu, size, handle};
   }
}

/* Preserve the allocation request exactly as observed without assigning
 * meanings to its opaque fields. This is development-only trace output. */
static void
agx_trace_report_allocation_request(
   const struct agx_allocate_resource_req_modern *req)
{
   if (!getenv("AGX_TRACE_ALLOCATION_REQUESTS"))
      return;

   const uint8_t *bytes = (const uint8_t *)req;
   printf("MODERN_ALLOCATE_REQUEST size_units_candidate=%u\n",
          req->size_units_candidate);
   printf("MODERN_ALLOCATE_REQUEST bytes=");

   for (size_t i = 0; i < sizeof(*req); ++i)
      printf("%02x", bytes[i]);

   printf("\n");
}

static bool
agx_trace_address_in_resource(uint64_t address, uint64_t base, uint64_t size)
{
   return base && address >= base && address - base < size;
}

/* Modern allocation replies may expose overlapping root allocations and
 * suballocations. Prefer an exact base match, otherwise the tightest range,
 * so diagnostic reference reports identify one meaningful owner. */
static const struct agx_trace_resource *
agx_trace_find_gpu_resource(uint64_t address, unsigned source_index)
{
   const struct agx_trace_resource *best = NULL;
   const struct agx_trace_resource *exact = NULL;

   for (unsigned i = 0; i < agx_trace_resource_count; ++i) {
      const struct agx_trace_resource *resource = &agx_trace_resources[i];

      if (i == source_index ||
          !agx_trace_address_in_resource(address, resource->gpu_va,
                                         resource->size)) {
         continue;
      }

      if (address == resource->gpu_va) {
         if (!exact || resource->size < exact->size)
            exact = resource;
         continue;
      }

      if (!best || resource->size < best->size)
         best = resource;
   }

   return exact ? exact : best;
}

static void
agx_trace_report_aux_resource_refs(const uint8_t *data, size_t start,
                                   size_t length)
{
   unsigned reported = 0;

   for (size_t offset = start; offset + sizeof(uint64_t) <= length;
        offset += sizeof(uint64_t)) {
      uint64_t value;

      memcpy(&value, data + offset, sizeof(value));
      if (!value)
         continue;

      const struct agx_trace_resource *resource =
         agx_trace_find_gpu_resource(value, UINT_MAX);
      if (!resource)
         continue;

      printf("MODERN_SUBMIT_AUX_RESOURCE offset=%#zx handle=%u kind=gpu"
             " delta=%" PRIx64 "\n",
             offset, resource->handle, value - resource->gpu_va);
      if (++reported == AGX_TRACE_AUX_RESOURCE_REFS) {
         printf("MODERN_SUBMIT_AUX_RESOURCE truncated\n");
         return;
      }
   }
}

static void
agx_trace_report_aux_diff(uint32_t queue_id, const uint8_t *data, size_t length)
{
   struct agx_trace_aux_snapshot *snapshot = NULL;

   for (unsigned i = 0; i < AGX_TRACE_MAX_QUEUES; ++i) {
      if (agx_trace_aux_snapshots[i].valid &&
          agx_trace_aux_snapshots[i].queue_id == queue_id) {
         snapshot = &agx_trace_aux_snapshots[i];
         break;
      }

      if (!snapshot && !agx_trace_aux_snapshots[i].valid)
         snapshot = &agx_trace_aux_snapshots[i];
   }

   if (!snapshot)
      return;

   if (!snapshot->valid) {
      snapshot->queue_id = queue_id;
      snapshot->length = length;
      snapshot->valid = true;
      memcpy(snapshot->data, data, length);
      printf("MODERN_SUBMIT_AUX_DIFF queue=%u baseline=%zu\n", queue_id,
             length);
      return;
   }

   size_t shared_length = snapshot->length < length ? snapshot->length : length;
   size_t changed = 0;
   unsigned ranges = 0;

   for (size_t offset = 0; offset < shared_length;) {
      if (snapshot->data[offset] == data[offset]) {
         ++offset;
         continue;
      }

      size_t start = offset;
      while (offset < shared_length && snapshot->data[offset] != data[offset]) {
         ++changed;
         ++offset;
      }

      if (ranges++ < AGX_TRACE_AUX_DIFF_RANGES) {
         printf("MODERN_SUBMIT_AUX_DIFF queue=%u range=%#zx-%#zx\n", queue_id,
                start, offset);
      }
   }

   changed += snapshot->length > length ? snapshot->length - length
                                        : length - snapshot->length;
   printf("MODERN_SUBMIT_AUX_DIFF queue=%u changed=%zu bytes=%zu\n", queue_id,
          changed, length);

   snapshot->length = length;
   memcpy(snapshot->data, data, length);
}

static size_t
agx_trace_readable_prefix(const void *pointer, size_t requested)
{
   mach_vm_address_t region = (mach_vm_address_t)(uintptr_t)pointer;
   mach_vm_size_t region_size = 0;
   vm_region_basic_info_data_64_t info = {};
   mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
   mach_port_t object_name = MACH_PORT_NULL;
   kern_return_t ret;

   if (!pointer || !requested)
      return 0;

   ret = mach_vm_region(mach_task_self(), &region, &region_size,
                        VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info,
                        &count, &object_name);
   if (object_name != MACH_PORT_NULL)
      mach_port_deallocate(mach_task_self(), object_name);

   if (ret != KERN_SUCCESS || region > (mach_vm_address_t)(uintptr_t)pointer ||
       !(info.protection & VM_PROT_READ))
      return 0;

   mach_vm_size_t offset = (mach_vm_address_t)(uintptr_t)pointer - region;
   if (offset >= region_size)
      return 0;

   mach_vm_size_t available = region_size - offset;
   return available < requested ? (size_t)available : requested;
}

static uint64_t
agx_trace_fingerprint(const uint8_t *data, size_t length)
{
   uint64_t hash = UINT64_C(1469598103934665603);

   for (size_t i = 0; i < length; ++i) {
      hash ^= data[i];
      hash *= UINT64_C(1099511628211);
   }

   return hash;
}

/* Inspect mapped allocations only in the diagnostic wrapper. A changed mapped
 * allocation containing another allocation's GPU VA is a useful lead for the
 * command-resource ownership investigation, but is not a decoded command ABI. */
static void
agx_trace_report_mapped_resource_refs(void)
{
   unsigned scanned = 0;
   unsigned changed = 0;
   unsigned reported = 0;

   for (unsigned source_index = 0; source_index < agx_trace_resource_count;
        ++source_index) {
      struct agx_trace_resource *source = &agx_trace_resources[source_index];
      size_t requested =
         source->size < AGX_TRACE_RESOURCE_SCAN_LIMIT ? source->size
                                                       : AGX_TRACE_RESOURCE_SCAN_LIMIT;
      size_t readable = agx_trace_readable_prefix(
         (const void *)(uintptr_t)source->cpu, requested);

      if (!readable)
         continue;

      const uint8_t *data = (const void *)(uintptr_t)source->cpu;
      uint64_t fingerprint = agx_trace_fingerprint(data, readable);
      bool source_changed = !source->fingerprint_valid ||
                            source->fingerprint != fingerprint;

      source->fingerprint = fingerprint;
      source->fingerprint_valid = true;
      ++scanned;

      if (!source_changed)
         continue;

      ++changed;
      for (size_t offset = 0; offset + sizeof(uint64_t) <= readable;
           offset += sizeof(uint64_t)) {
         uint64_t value;
         memcpy(&value, data + offset, sizeof(value));

         const struct agx_trace_resource *target =
            agx_trace_find_gpu_resource(value, source_index);
         if (!target)
            continue;

         if (reported < AGX_TRACE_RESOURCE_SCAN_REFS) {
            printf("MODERN_RESOURCE_REFERENCE source_handle=%u"
                   " source_offset=%#zx target_handle=%u gpu_delta=%" PRIx64
                   "\n",
                   source->handle, offset, target->handle,
                   value - target->gpu_va);
         }
         ++reported;
      }
   }

   printf("MODERN_RESOURCE_SCAN mapped=%u changed=%u references=%u"
          " bytes_per_resource=%u\n",
          scanned, changed, reported, AGX_TRACE_RESOURCE_SCAN_LIMIT);
   if (reported > AGX_TRACE_RESOURCE_SCAN_REFS)
      printf("MODERN_RESOURCE_REFERENCE truncated\n");
}

static void
agx_trace_report_aux_indirect_resource_refs(const uint8_t *data, size_t length,
                                            size_t origin_offset)
{
   for (size_t offset = 0; offset + sizeof(uint64_t) <= length;
        offset += sizeof(uint64_t)) {
      uint64_t value;

      memcpy(&value, data + offset, sizeof(value));
      if (!value)
         continue;

      for (unsigned i = 0; i < agx_trace_resource_count; ++i) {
         const struct agx_trace_resource *resource = &agx_trace_resources[i];
         const char *kind = NULL;
         uint64_t base = 0;

         if (agx_trace_address_in_resource(value, resource->gpu_va,
                                           resource->size)) {
            kind = "gpu";
            base = resource->gpu_va;
         } else if (agx_trace_address_in_resource(value, resource->cpu,
                                                  resource->size)) {
            kind = "cpu";
            base = resource->cpu;
         }

         if (kind) {
            printf("MODERN_SUBMIT_AUX_INDIRECT_RESOURCE origin=%#zx"
                   " target_offset=%#zx handle=%u kind=%s delta=%" PRIx64
                   "\n",
                   origin_offset, offset, resource->handle, kind, value - base);
         }
      }
   }
}

static void
agx_trace_follow_aux_pointers(const uint8_t *data, size_t length)
{
   uintptr_t data_address = (uintptr_t)data;
   unsigned followed = 0;

   for (size_t offset = 0; offset + sizeof(uint64_t) <= length;
        offset += sizeof(uint64_t)) {
      uint64_t value;

      memcpy(&value, data + offset, sizeof(value));
      if (!value || (value & (sizeof(uint64_t) - 1)))
         continue;

      if (value >= data_address && value - data_address < length)
         continue;

      size_t readable = agx_trace_readable_prefix(
         (const void *)(uintptr_t)value, AGX_TRACE_AUX_POINTER_TARGET_LIMIT);
      if (!readable)
         continue;

      printf("MODERN_SUBMIT_AUX_POINTER offset=%#zx pointer=%p readable=%zu\n",
             offset, (const void *)(uintptr_t)value, readable);
      agx_trace_report_aux_indirect_resource_refs(
         (const void *)(uintptr_t)value, readable, offset);

      if (++followed == AGX_TRACE_AUX_POINTER_LIMIT) {
         printf("MODERN_SUBMIT_AUX_POINTER truncated\n");
         return;
      }
   }
}

static void
agx_trace_configure_output(void)
{
   static bool configured;

   if (!configured && getenv("AGX_TRACE_UNBUFFERED")) {
      /* Markers from controlled probes go to stderr. Make wrapper output
       * ordered with them when a trace needs lifecycle correlation. */
      setvbuf(stdout, NULL, _IONBF, 0);
      configured = true;
   }
}

kern_return_t
wrap_Method(mach_port_t connection, uint32_t selector, const uint64_t *input,
            uint32_t inputCnt, const void *inputStruct, size_t inputStructCnt,
            uint64_t *output, uint32_t *outputCnt, void *outputStruct,
            size_t *outputStructCntP)
{
   agx_trace_configure_output();

   if (!decode_ctx) {
      decode_ctx = agxdecode_new_context(0);
   }

   /* Heuristic guess which connection is Metal, skip over I/O from everything
    * else. This is technically wrong but it works in practice, and reduces the
    * surface area we need to wrap.
    */
   if (selector == AGX_SELECTOR_SET_API) {
      metal_connection = connection;
   } else if (metal_connection != connection &&
              !getenv("AGX_TRACE_ALL_CONNECTIONS")) {
      return IOConnectCallMethod(connection, selector, input, inputCnt,
                                 inputStruct, inputStructCnt, output, outputCnt,
                                 outputStruct, outputStructCntP);
   }

   printf("Selector %u, %X, %X\n", selector, connection, metal_connection);

   /* Check the arguments make sense */
   assert((input != NULL) == (inputCnt != 0));
   assert((inputStruct != NULL) == (inputStructCnt != 0));
   assert((output != NULL) == (outputCnt != 0));
   assert((outputStruct != NULL) == (outputStructCntP != 0));

   if (getenv("AGX_TRACE_CALL_SHAPES")) {
      printf("AGX_CALL selector=%u scalar_in=%u struct_in=%zu "
             "scalar_out_capacity=%u struct_out_capacity=%zu\n",
             selector, inputCnt, inputStructCnt, outputCnt ? *outputCnt : 0,
             outputStructCntP ? *outputStructCntP : 0);
   }

   /* Dump inputs */
   switch (selector) {
   case AGX_SELECTOR_SET_API:
      /* The SET_API payload changed across macOS releases. Trace it without
       * assuming the older 16-byte, NUL-terminated form. */
      printf("%X: SET_API", connection);
      if (inputStruct && inputStructCnt) {
         printf("(%.*s)", (int)inputStructCnt, (const char *)inputStruct);
      }
      printf("\n");
      if (getenv("AGX_TRACE_SET_API_PAYLOAD")) {
         printf("SET_API_PAYLOAD bytes=%zu\n", inputStructCnt);
         u_hexdump(stdout, inputStruct, inputStructCnt, true);
      }
      break;

   case AGX_SELECTOR_SUBMIT_COMMAND_BUFFERS_LEGACY: {
      if (!input || inputCnt < 1 || !inputStruct ||
          inputStructCnt < sizeof(struct IOAccelCommandQueueSubmitArgs_Command)) {
         printf("%X: SUBMIT_COMMAND_BUFFERS with an unrecognized payload\n",
                connection);
         break;
      }

      printf("%X: SUBMIT_COMMAND_BUFFERS command queue id:%llx %p\n",
             connection, input[0], inputStruct);

      u_hexdump(stdout, inputStruct, inputStructCnt, true);
      const struct IOAccelCommandQueueSubmitArgs_Command *cmds =
         (void *)(inputStruct + 0);

      //  for (unsigned i = 0; i < hdr->count; ++i) {
      const struct IOAccelCommandQueueSubmitArgs_Command *req = &cmds[0];
      agxdecode_cmdstream(decode_ctx, HANDLE(req->command_buffer_shmem_id),
                          HANDLE(req->segment_list_shmem_id), true);
      // }

      agxdecode_next_frame();
      FALLTHROUGH;
   }

   default:
      printf("%X: call %X (out %p, %zu)", connection, selector,
             outputStructCntP, outputStructCntP ? *outputStructCntP : 0);

      for (uint64_t u = 0; u < inputCnt; ++u)
         printf(" %llx", input[u]);

      if (inputStructCnt) {
         printf(", struct:\n");
         u_hexdump(stdout, inputStruct, inputStructCnt, true);
      } else {
         printf("\n");
      }

      break;
   }

   agx_trace_report_notification_port_state(connection, selector, input,
                                            inputCnt, "before");

   /* Invoke the real method */
   kern_return_t ret = IOConnectCallMethod(
      connection, selector, input, inputCnt, inputStruct, inputStructCnt,
      output, outputCnt, outputStruct, outputStructCntP);

   agx_trace_report_notification_port_state(connection, selector, input,
                                            inputCnt, "after");

   if (getenv("AGX_TRACE_CALL_SHAPES")) {
      printf("AGX_RESULT selector=%u status=%#x scalar_out=%u struct_out=%zu\n",
             selector, ret, outputCnt ? *outputCnt : 0,
             outputStructCntP ? *outputStructCntP : 0);
   }

   if (ret != 0)
      printf("return %u\n", ret);

   /* Track allocations for later analysis (dumping, disassembly, etc) */
   switch (selector) {
   case AGX_SELECTOR_COMMAND_PAIR_NO_INPUT_MODERN:
   case AGX_SELECTOR_COMMAND_PAIR_CONFIGURED_MODERN: {
      if (!getenv("AGX_TRACE_COMMAND_DETAILS"))
         break;

      if (ret != KERN_SUCCESS)
         break;

      bool expected_input =
         selector == AGX_SELECTOR_COMMAND_PAIR_NO_INPUT_MODERN ? inputCnt == 0
                                                               : inputCnt == 2;
      if (!expected_input || inputStruct || output || !outputStruct ||
          !outputStructCntP ||
          *outputStructCntP != sizeof(struct agx_command_pair_resp_modern)) {
         printf("MODERN_COMMAND_PAIR selector=%u unrecognized payload\n",
                selector);
         break;
      }

      const struct agx_command_pair_resp_modern *resp = outputStruct;
      printf("MODERN_COMMAND_PAIR selector=%u", selector);
      for (uint32_t i = 0; i < inputCnt; ++i)
         printf(" input%u=%" PRIx64, i, input[i]);
      printf(" value0=%" PRIx64 " value1=%" PRIx64 "\n", resp->value0,
             resp->value1);
      break;
   }

   case AGX_SELECTOR_CREATE_COMMAND_QUEUE_LEGACY:
   case AGX_SELECTOR_CREATE_NOTIFICATION_QUEUE_LEGACY:
      if (input && inputCnt == 1 && !inputStruct && !output && !outputStruct) {
         /* These selector values are queue teardown events on the profiled
          * macOS interface. Their individual semantics remain opaque. */
         printf("MODERN_QUEUE_LIFECYCLE selector=%u id=%" PRIu64 "\n", selector,
                input[0]);
      }
      break;

   case AGX_SELECTOR_CREATE_SHMEM_LEGACY: {
      if (input && inputCnt == 1 && !inputStruct && !output && !outputStruct) {
         printf("MODERN_OPAQUE_LIFECYCLE selector=%u value=%" PRIu64 "\n",
                selector, input[0]);
         break;
      }

      if (!input || inputCnt != 2 || !outputStructCntP ||
          *outputStructCntP != 0x10) {
         printf("LEGACY_CREATE_SHMEM returned an unrecognized payload\n");
         break;
      }
      uint64_t *inp = (uint64_t *)input;

      uint8_t type = inp[1];

      assert(type <= 2);
      if (type == 2)
         printf("(cmdbuf with error reporting)\n");

      uint64_t *ptr = (uint64_t *)outputStruct;
      uint32_t *words = (uint32_t *)(ptr + 1);

      /* Construct a synthetic GEM handle for the shmem */
      agxdecode_track_alloc(decode_ctx, &(struct agx_bo){
                                           .handle = HANDLE(words[1]),
                                           ._map = (void *)*ptr,
                                           .size = words[0],
                                        });

      break;
   }

   case AGX_SELECTOR_ALLOCATE_MEM_LEGACY: {
      if (!inputStruct || !outputStructCntP || *outputStructCntP != 0x50) {
         printf("ALLOCATE_MEM returned an unrecognized payload\n");
         break;
      }
      struct agx_allocate_resource_resp *resp = outputStruct;

      struct agx_va *va = malloc(sizeof(struct agx_va));
      va->addr = resp->gpu_va;
      va->size_B = resp->sub_size;

      agxdecode_track_alloc_or_replace(decode_ctx, &(struct agx_bo){
                                                      .size = resp->sub_size,
                                                      .handle = resp->handle,
                                                      .va = va,
                                                      ._map = (void *)resp->cpu,
                                                   });

      break;
   }

   case AGX_SELECTOR_ALLOCATE_MEM_MODERN: {
      if (!inputStruct || inputStructCnt != 0x68 || !outputStructCntP ||
          *outputStructCntP != 0x58) {
         printf("MODERN_ALLOCATE_MEM returned an unrecognized payload\n");
         break;
      }

      const struct agx_allocate_resource_req_modern *req = inputStruct;
      const struct agx_allocate_resource_resp_modern *resp = outputStruct;

      struct agx_va *va = malloc(sizeof(*va));
      va->addr = resp->gpu_va;
      va->size_B = resp->sub_size;

      bool reused = agxdecode_track_alloc_or_replace(decode_ctx,
                                                      &(struct agx_bo){
                                                         .size = resp->sub_size,
                                                         .handle = resp->handle,
                                                         .va = va,
                                                         ._map = (void *)resp->cpu,
                                                      });

      agx_trace_track_resource(resp->gpu_va, resp->cpu, resp->sub_size,
                               resp->handle);

      printf("MODERN_ALLOCATE_MEM handle=%u gpu=%" PRIx64 " cpu=%" PRIx64
             " size=%" PRIx64 " attributes=%" PRIx64 " storage=%#x"
             " write_combined=%u private=%u reused=%u\n",
             resp->handle, resp->gpu_va, resp->cpu, resp->sub_size,
             req->attributes, req->storage_flags,
             (req->attributes & AGX_ALLOCATE_MEM_ATTR_WRITE_COMBINED) != 0,
             (req->storage_flags & AGX_ALLOCATE_MEM_STORAGE_PRIVATE) != 0,
             reused);
      agx_trace_report_allocation_request(req);

      break;
   }

   case AGX_SELECTOR_FREE_MEM_LEGACY: {
      if (!input || inputCnt != 1 || inputStruct || output || outputStruct) {
         printf("FREE_MEM returned an unrecognized payload\n");
         break;
      }

      agxdecode_track_free(decode_ctx, &(struct agx_bo){.handle = input[0]});

      break;
   }

   case AGX_SELECTOR_FREE_SHMEM_LEGACY: {
      if (input && inputCnt == 2 && !inputStruct && !output && outputStruct &&
          outputStructCntP &&
          *outputStructCntP == sizeof(struct agx_notification_queue_resp_modern)) {
         const struct agx_notification_queue_resp_modern *resp = outputStruct;

         agx_trace_track_queue(resp->data_queue, resp->queue_id);

         printf("MODERN_NOTIFICATION_QUEUE data_queue=%" PRIx64
                " id=%u config=%" PRIx64 "/%" PRIx64 "\n",
                resp->data_queue, resp->queue_id, input[0], input[1]);
         break;
      }

      if (!input || inputCnt != 1 || inputStruct || output || outputStruct) {
         printf("LEGACY_FREE_SHMEM returned an unrecognized payload\n");
         if (outputStruct && outputStructCntP && *outputStructCntP) {
            printf("opaque selector %u reply:\n", selector);
            u_hexdump(stdout, outputStruct, *outputStructCntP, true);
         }
         break;
      }

      agxdecode_track_free(decode_ctx,
                           &(struct agx_bo){.handle = HANDLE(input[0])});

      break;
   }

   default:
      /* Dump the outputs */
      if (outputCnt) {
         printf("%u scalars: ", *outputCnt);

         for (uint64_t u = 0; u < *outputCnt; ++u)
            printf("%llx ", output[u]);

         printf("\n");
      }

      if (outputStructCntP) {
         printf(" struct\n");
         u_hexdump(stdout, outputStruct, *outputStructCntP, true);

         if (selector == 2) {
            /* Dump linked buffer as well */
            void **o = outputStruct;
            u_hexdump(stdout, *o, 64, true);
         }
      }

      printf("\n");
      break;
   }

   return ret;
}

kern_return_t
wrap_AsyncMethod(mach_port_t connection, uint32_t selector,
                 mach_port_t wakePort, uint64_t *reference,
                 uint32_t referenceCnt, const uint64_t *input,
                 uint32_t inputCnt, const void *inputStruct,
                 size_t inputStructCnt, uint64_t *output, uint32_t *outputCnt,
                 void *outputStruct, size_t *outputStructCntP)
{
   /* Check the arguments make sense */
   assert((input != NULL) == (inputCnt != 0));
   assert((inputStruct != NULL) == (inputStructCnt != 0));
   assert((output != NULL) == (outputCnt != 0));
   assert((outputStruct != NULL) == (outputStructCntP != 0));

   printf("%X: call %X, wake port %X (out %p, %zu)", connection, selector,
          wakePort, outputStructCntP, outputStructCntP ? *outputStructCntP : 0);

   for (uint64_t u = 0; u < inputCnt; ++u)
      printf(" %llx", input[u]);

   if (inputStructCnt) {
      printf(", struct:\n");
      u_hexdump(stdout, inputStruct, inputStructCnt, true);
   } else {
      printf("\n");
   }

   printf(", references: ");
   for (unsigned i = 0; i < referenceCnt; ++i)
      printf(" %llx", reference[i]);
   printf("\n");

   kern_return_t ret = IOConnectCallAsyncMethod(
      connection, selector, wakePort, reference, referenceCnt, input, inputCnt,
      inputStruct, inputStructCnt, output, outputCnt, outputStruct,
      outputStructCntP);

   printf("return %u", ret);

   if (outputCnt) {
      printf("%u scalars: ", *outputCnt);

      for (uint64_t u = 0; u < *outputCnt; ++u)
         printf("%llx ", output[u]);

      printf("\n");
   }

   if (outputStructCntP) {
      printf(" struct\n");
      u_hexdump(stdout, outputStruct, *outputStructCntP, true);

      if (selector == 2) {
         /* Dump linked buffer as well */
         void **o = outputStruct;
         u_hexdump(stdout, *o, 64, true);
      }
   }

   printf("\n");
   return ret;
}

kern_return_t
wrap_StructMethod(mach_port_t connection, uint32_t selector,
                  const void *inputStruct, size_t inputStructCnt,
                  void *outputStruct, size_t *outputStructCntP)
{
   return wrap_Method(connection, selector, NULL, 0, inputStruct,
                      inputStructCnt, NULL, NULL, outputStruct,
                      outputStructCntP);
}

kern_return_t
wrap_AsyncStructMethod(mach_port_t connection, uint32_t selector,
                       mach_port_t wakePort, uint64_t *reference,
                       uint32_t referenceCnt, const void *inputStruct,
                       size_t inputStructCnt, void *outputStruct,
                       size_t *outputStructCnt)
{
   return wrap_AsyncMethod(connection, selector, wakePort, reference,
                           referenceCnt, NULL, 0, inputStruct, inputStructCnt,
                           NULL, NULL, outputStruct, outputStructCnt);
}

kern_return_t
wrap_ScalarMethod(mach_port_t connection, uint32_t selector,
                  const uint64_t *input, uint32_t inputCnt, uint64_t *output,
                  uint32_t *outputCnt)
{
   return wrap_Method(connection, selector, input, inputCnt, NULL, 0, output,
                      outputCnt, NULL, NULL);
}

kern_return_t
wrap_AsyncScalarMethod(mach_port_t connection, uint32_t selector,
                       mach_port_t wakePort, uint64_t *reference,
                       uint32_t referenceCnt, const uint64_t *input,
                       uint32_t inputCnt, uint64_t *output, uint32_t *outputCnt)
{
   return wrap_AsyncMethod(connection, selector, wakePort, reference,
                           referenceCnt, input, inputCnt, NULL, 0, output,
                           outputCnt, NULL, NULL);
}

mach_port_t
wrap_DataQueueAllocateNotificationPort()
{
   mach_port_t ret = IODataQueueAllocateNotificationPort();

   if (metal_connection != MACH_PORT_NULL)
      agx_trace_track_notification_port(ret);

   printf("Allocated notif port %X\n", ret);
   return ret;
}

kern_return_t
wrap_SetNotificationPort(io_connect_t connect, uint32_t type, mach_port_t port,
                         uintptr_t reference)
{
   kern_return_t ret;

   printf(
      "Set noficiation port connect=%X, type=%X, port=%X, reference=%" PRIx64
      "\n",
      connect, type, port, (uint64_t)reference);

   ret = IOConnectSetNotificationPort(connect, type, port, reference);
   if (ret == KERN_SUCCESS)
      agx_trace_bind_notification_port(connect, port, reference);

   return ret;
}

kern_return_t
wrap_AddClient(io_connect_t connect, io_connect_t client)
{
   kern_return_t ret = IOConnectAddClient(connect, client);
   printf("Add client connect=%X, client=%X, result=%X\n", connect, client, ret);
   return ret;
}

kern_return_t
wrap_MapMemory64(io_connect_t connect, uint32_t type, mach_port_t task,
                 mach_vm_address_t *address, mach_vm_size_t *size,
                 IOOptionBits options)
{
   kern_return_t ret =
      IOConnectMapMemory64(connect, type, task, address, size, options);
   printf("Map memory connect=%X, type=%X, address=%llX, size=%llX, result=%X\n",
          connect, type, (unsigned long long)(address ? *address : 0),
          (unsigned long long)(size ? *size : 0), ret);
   return ret;
}

kern_return_t
wrap_UnmapMemory64(io_connect_t connect, uint32_t type, mach_port_t task,
                   mach_vm_address_t address)
{
   kern_return_t ret = IOConnectUnmapMemory64(connect, type, task, address);
   printf("Unmap memory connect=%X, type=%X, address=%llX, result=%X\n", connect,
          type, (unsigned long long)address, ret);
   return ret;
}

kern_return_t
wrap_Trap0(io_connect_t connect, uint32_t index)
{
   kern_return_t ret = IOConnectTrap0(connect, index);
   printf("Trap0 connect=%X, index=%X, result=%X\n", connect, index, ret);
   return ret;
}

kern_return_t
wrap_Trap1(io_connect_t connect, uint32_t index, uintptr_t p1)
{
   kern_return_t ret = IOConnectTrap1(connect, index, p1);
   printf("Trap1 connect=%X, index=%X, p1=%" PRIxPTR ", result=%X\n", connect,
          index, p1, ret);
   return ret;
}

kern_return_t
wrap_Trap2(io_connect_t connect, uint32_t index, uintptr_t p1, uintptr_t p2)
{
   kern_return_t ret = IOConnectTrap2(connect, index, p1, p2);
   printf("Trap2 connect=%X, index=%X, p1=%" PRIxPTR ", p2=%" PRIxPTR
          ", result=%X\n",
          connect, index, p1, p2, ret);
   return ret;
}

kern_return_t
wrap_Trap3(io_connect_t connect, uint32_t index, uintptr_t p1, uintptr_t p2,
           uintptr_t p3)
{
   kern_return_t ret = IOConnectTrap3(connect, index, p1, p2, p3);
   printf("Trap3 connect=%X, index=%X, p1=%" PRIxPTR ", p2=%" PRIxPTR
          ", p3=%" PRIxPTR ", result=%X\n",
          connect, index, p1, p2, p3, ret);
   return ret;
}

kern_return_t
wrap_Trap4(io_connect_t connect, uint32_t index, uintptr_t p1, uintptr_t p2,
           uintptr_t p3, uintptr_t p4)
{
   const char *trace_payloads = getenv("AGX_TRACE_TRAP_PAYLOADS");

   /* Metal submits through a fast IOKit trap on current macOS. Keep this as a
    * trace-only capture: p2 is the observed payload size, p3 is its address,
    * and p4 is a second opaque pointer. The cap prevents malformed inputs from
    * turning a diagnostic trace into an unbounded process-memory read. */
   if (trace_payloads && strcmp(trace_payloads, "1") == 0 &&
       connect == metal_connection && index == 0 && p3 &&
       p2 <= AGX_TRACE_TRAP_PAYLOAD_LIMIT) {
      printf("AGX_TRAP4_SUBMIT payload_size=%" PRIxPTR " payload=%p aux=%p\n",
             p2, (void *)p3, (void *)p4);
      u_hexdump(stdout, (const void *)p3, p2, true);
   }

   if (getenv("AGX_TRACE_SUBMISSION_DETAILS") && connect == metal_connection &&
       index == 0 && p3 &&
       p2 == sizeof(struct agx_macos_submit_descriptor_observed)) {
      const struct agx_macos_submit_descriptor_observed *desc = (const void *)p3;

      printf("MODERN_SUBMIT queue=%" PRIuPTR " header=%u/%u"
             " completion0=%" PRIx64 " completion1=%" PRIx64 "\n",
             p1, desc->header0, desc->header1, desc->completion_tokens[0],
             desc->completion_tokens[1]);

      if (getenv("AGX_TRACE_RESOURCE_REFS"))
         agx_trace_report_mapped_resource_refs();

      if (getenv("AGX_TRACE_TRAP_AUX")) {
         size_t readable =
            agx_trace_readable_prefix((const void *)p4, AGX_TRACE_AUX_PREFIX_LIMIT);
         intptr_t descriptor_offset = (intptr_t)p4 - (intptr_t)p3;
         struct agx_macos_submission_carrier_snapshot snapshot;

         printf("MODERN_SUBMIT_AUX pointer=%p descriptor_offset=%" PRIdPTR
                " readable_prefix=%zu\n",
                (const void *)p4, descriptor_offset, readable);

         if (p1 <= UINT32_MAX &&
             agx_macos_submission_carrier_snapshot_capture(
                (uint32_t)p1, (const void *)p3, p2, (const void *)p4, readable,
                &snapshot)) {
            printf("MODERN_SUBMIT_CARRIER queue=%" PRIuPTR
                   " offset=%zu prefix=%zu fingerprint=%" PRIx64 "\n",
                   p1, snapshot.observation.auxiliary_offset,
                   snapshot.observation.auxiliary_readable_prefix,
                   agx_trace_fingerprint(snapshot.auxiliary_prefix,
                                         sizeof(snapshot.auxiliary_prefix)));
         } else {
            printf("MODERN_SUBMIT_CARRIER unverified\n");
         }

         if (readable)
            u_hexdump(stdout, (const void *)p4, readable, true);

         if (readable && getenv("AGX_TRACE_AUX_ANALYSIS")) {
            agx_trace_report_aux_diff((uint32_t)p1, (const void *)p4, readable);
            agx_trace_report_aux_resource_refs((const void *)p4, 0, readable);

            if (getenv("AGX_TRACE_AUX_EXTENDED")) {
               size_t extended = agx_trace_readable_prefix(
                  (const void *)p4, AGX_TRACE_AUX_EXTENDED_LIMIT);

               if (extended > readable) {
                  struct agx_macos_submission_carrier_extended_snapshot
                     extended_snapshot;

                  printf("MODERN_SUBMIT_AUX_EXTENDED bytes=%zu\n", extended);
                  agx_trace_report_aux_resource_refs((const void *)p4,
                                                     readable, extended);

                  if (p1 <= UINT32_MAX &&
                      agx_macos_submission_carrier_extended_snapshot_capture(
                         (uint32_t)p1, (const void *)p3, p2,
                         (const void *)p4, extended, &extended_snapshot)) {
                     printf("MODERN_SUBMIT_CARRIER_EXTENDED queue=%" PRIuPTR
                            " prefix=%zu opaque_slot=%#" PRIx64 "\n",
                            p1,
                            extended_snapshot.observation
                               .auxiliary_readable_prefix,
                            extended_snapshot.opaque_pointer_slot);
                  } else {
                     printf("MODERN_SUBMIT_CARRIER_EXTENDED unverified\n");
                  }

                  if (getenv("AGX_TRACE_AUX_POINTERS"))
                     agx_trace_follow_aux_pointers((const void *)p4, extended);
               }
            }
         }
      }
   }

   kern_return_t ret = IOConnectTrap4(connect, index, p1, p2, p3, p4);
   printf("Trap4 connect=%X, index=%X, p1=%" PRIxPTR ", p2=%" PRIxPTR
          ", p3=%" PRIxPTR ", p4=%" PRIxPTR ", result=%X\n",
          connect, index, p1, p2, p3, p4, ret);
   fflush(stdout);
   return ret;
}

kern_return_t
wrap_Trap5(io_connect_t connect, uint32_t index, uintptr_t p1, uintptr_t p2,
           uintptr_t p3, uintptr_t p4, uintptr_t p5)
{
   kern_return_t ret = IOConnectTrap5(connect, index, p1, p2, p3, p4, p5);
   printf("Trap5 connect=%X, index=%X, p1=%" PRIxPTR ", p2=%" PRIxPTR
          ", p3=%" PRIxPTR ", p4=%" PRIxPTR ", p5=%" PRIxPTR
          ", result=%X\n",
          connect, index, p1, p2, p3, p4, p5, ret);
   return ret;
}

kern_return_t
wrap_Trap6(io_connect_t connect, uint32_t index, uintptr_t p1, uintptr_t p2,
           uintptr_t p3, uintptr_t p4, uintptr_t p5, uintptr_t p6)
{
   kern_return_t ret = IOConnectTrap6(connect, index, p1, p2, p3, p4, p5, p6);
   printf("Trap6 connect=%X, index=%X, p1=%" PRIxPTR ", p2=%" PRIxPTR
          ", p3=%" PRIxPTR ", p4=%" PRIxPTR ", p5=%" PRIxPTR
          ", p6=%" PRIxPTR ", result=%X\n",
          connect, index, p1, p2, p3, p4, p5, p6, ret);
   return ret;
}

IOReturn
wrap_DataQueueWaitForAvailableData(IODataQueueMemory *dataQueue,
                                   mach_port_t notificationPort)
{
   printf("Waiting for data queue at notif port %X\n", notificationPort);
   IOReturn ret = IODataQueueWaitForAvailableData(dataQueue, notificationPort);
   printf("ret=%X\n", ret);
   return ret;
}

IODataQueueEntry *
wrap_DataQueuePeek(IODataQueueMemory *dataQueue)
{
   printf("Peeking data queue\n");
   return IODataQueuePeek(dataQueue);
}

IOReturn
wrap_DataQueueDequeue(IODataQueueMemory *dataQueue, void *data,
                      uint32_t *dataSize)
{
   printf("Dequeueing (dataQueue=%p, data=%p, buffer %u)\n", dataQueue, data,
          *dataSize);
   IOReturn ret = IODataQueueDequeue(dataQueue, data, dataSize);
   printf("Return \"%s\", got %u bytes\n", mach_error_string(ret), *dataSize);

   if (ret == kIOReturnSuccess && getenv("AGX_TRACE_SUBMISSION_DETAILS") &&
       data && *dataSize >= sizeof(uint64_t)) {
      uint64_t token;
      uint32_t queue_id = agx_trace_queue_id(dataQueue);

      memcpy(&token, data, sizeof(token));
      printf("MODERN_COMPLETION queue=%u token=%" PRIx64 " bytes=%u\n",
             queue_id, token, *dataSize);
   }

   if (ret == kIOReturnSuccess) {
      uint8_t *data8 = data;
      for (unsigned i = 0; i < *dataSize; ++i) {
         printf("%02X ", data8[i]);
      }
      printf("\n");
   }

   return ret;
}

DYLD_INTERPOSE(wrap_Method, IOConnectCallMethod);
DYLD_INTERPOSE(wrap_AsyncMethod, IOConnectCallAsyncMethod);
DYLD_INTERPOSE(wrap_StructMethod, IOConnectCallStructMethod);
DYLD_INTERPOSE(wrap_AsyncStructMethod, IOConnectCallAsyncStructMethod);
DYLD_INTERPOSE(wrap_ScalarMethod, IOConnectCallScalarMethod);
DYLD_INTERPOSE(wrap_AsyncScalarMethod, IOConnectCallAsyncScalarMethod);
DYLD_INTERPOSE(wrap_AddClient, IOConnectAddClient);
DYLD_INTERPOSE(wrap_MapMemory64, IOConnectMapMemory64);
DYLD_INTERPOSE(wrap_UnmapMemory64, IOConnectUnmapMemory64);
DYLD_INTERPOSE(wrap_Trap0, IOConnectTrap0);
DYLD_INTERPOSE(wrap_Trap1, IOConnectTrap1);
DYLD_INTERPOSE(wrap_Trap2, IOConnectTrap2);
DYLD_INTERPOSE(wrap_Trap3, IOConnectTrap3);
DYLD_INTERPOSE(wrap_Trap4, IOConnectTrap4);
DYLD_INTERPOSE(wrap_Trap5, IOConnectTrap5);
DYLD_INTERPOSE(wrap_Trap6, IOConnectTrap6);
DYLD_INTERPOSE(wrap_SetNotificationPort, IOConnectSetNotificationPort);
DYLD_INTERPOSE(wrap_DataQueueAllocateNotificationPort,
               IODataQueueAllocateNotificationPort);
DYLD_INTERPOSE(wrap_DataQueueWaitForAvailableData,
               IODataQueueWaitForAvailableData);
DYLD_INTERPOSE(wrap_DataQueuePeek, IODataQueuePeek);
DYLD_INTERPOSE(wrap_DataQueueDequeue, IODataQueueDequeue);
