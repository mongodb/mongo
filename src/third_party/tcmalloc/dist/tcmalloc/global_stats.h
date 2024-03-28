// Copyright 2019 The TCMalloc Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef TCMALLOC_GLOBAL_STATS_H_
#define TCMALLOC_GLOBAL_STATS_H_

#include <cstddef>
#include <cstdint>

#include "tcmalloc/arena.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/page_allocator.h"
#include "tcmalloc/page_heap_allocator.h"
#include "tcmalloc/stats.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

// Extract interesting stats
struct TCMallocStats {
  uint64_t thread_bytes;               // Bytes in thread caches
  uint64_t central_bytes;              // Bytes in central cache
  uint64_t transfer_bytes;             // Bytes in central transfer cache
  uint64_t metadata_bytes;             // Bytes alloced for metadata
  uint64_t sharded_transfer_bytes;     // Bytes in per-CCX cache
  uint64_t per_cpu_bytes;              // Bytes in per-CPU cache
  uint64_t pagemap_root_bytes_res;     // Resident bytes of pagemap root node
  uint64_t percpu_metadata_bytes_res;  // Resident bytes of the per-CPU metadata
  AllocatorStats tc_stats;             // ThreadCache objects
  AllocatorStats span_stats;           // Span objects
  AllocatorStats stack_stats;          // StackTrace objects
  AllocatorStats linked_sample_stats;  // StackTraceTable::LinkedSample objects
  size_t pagemap_bytes;                // included in metadata bytes
  size_t percpu_metadata_bytes;        // included in metadata bytes
  BackingStats pageheap;               // Stats from page heap
  PageAllocator::PeakStats peak_stats;

  ArenaStats arena;  // Stats from the metadata Arena

  // Explicitly declare the ctor to put it in the google_malloc section.
  TCMallocStats() = default;
};

void ExtractTCMallocStats(TCMallocStats* r, bool report_residence);

uint64_t InUseByApp(const TCMallocStats& stats);
uint64_t VirtualMemoryUsed(const TCMallocStats& stats);
uint64_t UnmappedBytes(const TCMallocStats& stats);
uint64_t PhysicalMemoryUsed(const TCMallocStats& stats);
uint64_t RequiredBytes(const TCMallocStats& stats);
size_t ExternalBytes(const TCMallocStats& stats);
size_t HeapSizeBytes(const BackingStats& stats);
size_t LocalBytes(const TCMallocStats& stats);
size_t SlackBytes(const BackingStats& stats);

// WRITE stats to "out"
void DumpStats(Printer* out, int level);
void DumpStatsInPbtxt(Printer* out, int level);

bool GetNumericProperty(const char* name_data, size_t name_size, size_t* value);

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_GLOBAL_STATS_H_
