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

#include "tcmalloc/global_stats.h"

#include <cstddef>
#include <cstdint>
#include <optional>

#include "absl/strings/match.h"
#include "absl/strings/string_view.h"
#include "absl/strings/strip.h"
#include "absl/time/time.h"
#include "absl/types/optional.h"
#include "tcmalloc/central_freelist.h"
#include "tcmalloc/common.h"
#include "tcmalloc/cpu_cache.h"
#include "tcmalloc/experiment.h"
#include "tcmalloc/experiment_config.h"
#include "tcmalloc/guarded_page_allocator.h"
#include "tcmalloc/internal/allocation_guard.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/memory_stats.h"
#include "tcmalloc/internal/optimization.h"
#include "tcmalloc/internal/percpu.h"
#include "tcmalloc/page_allocator.h"
#include "tcmalloc/page_heap_allocator.h"
#include "tcmalloc/pagemap.h"
#include "tcmalloc/parameters.h"
#include "tcmalloc/selsan/selsan.h"
#include "tcmalloc/span.h"
#include "tcmalloc/span_stats.h"
#include "tcmalloc/stack_trace_table.h"
#include "tcmalloc/static_vars.h"
#include "tcmalloc/stats.h"
#include "tcmalloc/system-alloc.h"
#include "tcmalloc/thread_cache.h"
#include "tcmalloc/transfer_cache.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

using subtle::percpu::RseqVcpuMode;

static absl::string_view MadviseString() {
  MadvisePreference pref = Parameters::madvise();

  if (Parameters::madvise_free()) {
    switch (pref) {
      case MadvisePreference::kDontNeed:
        pref = MadvisePreference::kFreeAndDontNeed;
        break;
      case MadvisePreference::kFreeOnly:
        break;
      case MadvisePreference::kFreeAndDontNeed:
        break;
    }
  }

  switch (pref) {
    case MadvisePreference::kDontNeed:
      return "MADVISE_DONTNEED";
    case MadvisePreference::kFreeOnly:
      return "MADVISE_FREE_ONLY";
    case MadvisePreference::kFreeAndDontNeed:
      return "MADVISE_FREE_AND_DONTNEED";
  }

  ABSL_UNREACHABLE();
}

// Get stats into "r".  Also, if class_count != NULL, class_count[k]
// will be set to the total number of objects of size class k in the
// central cache, transfer cache, and per-thread and per-CPU caches.
// If small_spans is non-NULL, it is filled.  Same for large_spans.
// The boolean report_residence determines whether residence information
// should be captured or not. Residence info requires a potentially
// costly OS call, and is not necessary in all situations.
void ExtractStats(TCMallocStats* r, uint64_t* class_count,
                  SpanStats* span_stats, SmallSpanStats* small_spans,
                  LargeSpanStats* large_spans, bool report_residence) {
  r->central_bytes = 0;
  r->transfer_bytes = 0;
  for (int size_class = 0; size_class < kNumClasses; ++size_class) {
    const size_t length = tc_globals.central_freelist(size_class).length();
    const size_t tc_length = tc_globals.transfer_cache().tc_length(size_class);
    const size_t sharded_tc_length =
        tc_globals.sharded_transfer_cache().TotalObjectsOfClass(size_class);
    const size_t cache_overhead =
        tc_globals.central_freelist(size_class).OverheadBytes();
    const size_t size = tc_globals.sizemap().class_to_size(size_class);
    r->central_bytes += (size * length) + cache_overhead;
    r->transfer_bytes += (size * tc_length);
    if (class_count) {
      // Sum the lengths of all per-class freelists, except the per-thread
      // freelists, which get counted when we call GetThreadStats(), below.
      class_count[size_class] = length + tc_length + sharded_tc_length;
      if (UsePerCpuCache(tc_globals)) {
        class_count[size_class] +=
            tc_globals.cpu_cache().TotalObjectsOfClass(size_class);
      }
    }
    if (span_stats) {
      span_stats[size_class] =
          tc_globals.central_freelist(size_class).GetSpanStats();
    }
  }

  // Add stats from per-thread heaps
  r->thread_bytes = 0;
  {  // scope
    PageHeapSpinLockHolder l;
    r->tc_stats = ThreadCache::GetStats(&r->thread_bytes, class_count);
    r->span_stats = tc_globals.span_allocator().stats();
    r->stack_stats = tc_globals.sampledallocation_allocator().stats();
    r->linked_sample_stats = tc_globals.linked_sample_allocator().stats();
    r->metadata_bytes = tc_globals.metadata_bytes();
    r->pagemap_bytes = tc_globals.pagemap().bytes();
    r->pageheap = tc_globals.page_allocator().stats();
    r->peak_stats = tc_globals.page_allocator().peak_stats();
    if (small_spans != nullptr) {
      tc_globals.page_allocator().GetSmallSpanStats(small_spans);
    }
    if (large_spans != nullptr) {
      tc_globals.page_allocator().GetLargeSpanStats(large_spans);
    }

    r->arena = tc_globals.arena().stats();
    if (!report_residence) {
      r->metadata_bytes += r->arena.bytes_nonresident;
    }
  }
  // We can access the pagemap without holding the pageheap_lock since it
  // is static data, and we are only taking address and size which are
  // constants.
  if (report_residence) {
    auto resident_bytes = tc_globals.pagemap_residence();
    r->pagemap_root_bytes_res = resident_bytes;
    TC_ASSERT_GE(r->metadata_bytes, r->pagemap_bytes);
    r->metadata_bytes = r->metadata_bytes - r->pagemap_bytes + resident_bytes;
  } else {
    r->pagemap_root_bytes_res = 0;
  }

  r->per_cpu_bytes = 0;
  r->sharded_transfer_bytes = 0;
  r->percpu_metadata_bytes_res = 0;
  r->percpu_metadata_bytes = 0;
  if (UsePerCpuCache(tc_globals)) {
    r->per_cpu_bytes = tc_globals.cpu_cache().TotalUsedBytes();
    r->sharded_transfer_bytes =
        tc_globals.sharded_transfer_cache().TotalBytes();

    if (report_residence) {
      auto percpu_metadata = tc_globals.cpu_cache().MetadataMemoryUsage();
      r->percpu_metadata_bytes_res = percpu_metadata.resident_size;
      r->percpu_metadata_bytes = percpu_metadata.virtual_size;

      TC_ASSERT_GE(r->metadata_bytes, r->percpu_metadata_bytes);
      r->metadata_bytes = r->metadata_bytes - r->percpu_metadata_bytes +
                          r->percpu_metadata_bytes_res;
    }
  }
}

void ExtractTCMallocStats(TCMallocStats* r, bool report_residence) {
  ExtractStats(r, nullptr, nullptr, nullptr, nullptr, report_residence);
}

// Because different fields of stats are computed from state protected
// by different locks, they may be inconsistent.  Prevent underflow
// when subtracting to avoid gigantic results.
static uint64_t StatSub(uint64_t a, uint64_t b) {
  return (a >= b) ? (a - b) : 0;
}

// Return approximate number of bytes in use by app.
uint64_t InUseByApp(const TCMallocStats& stats) {
  return StatSub(stats.pageheap.system_bytes,
                 stats.thread_bytes + stats.central_bytes +
                     stats.transfer_bytes + stats.per_cpu_bytes +
                     stats.sharded_transfer_bytes + stats.pageheap.free_bytes +
                     stats.pageheap.unmapped_bytes);
}

uint64_t VirtualMemoryUsed(const TCMallocStats& stats) {
  return stats.pageheap.system_bytes + stats.metadata_bytes +
         stats.arena.bytes_unallocated + stats.arena.bytes_unavailable +
         stats.arena.bytes_nonresident;
}

uint64_t UnmappedBytes(const TCMallocStats& stats) {
  return stats.pageheap.unmapped_bytes + stats.arena.bytes_nonresident;
}

uint64_t PhysicalMemoryUsed(const TCMallocStats& stats) {
  return StatSub(VirtualMemoryUsed(stats), UnmappedBytes(stats));
}

// The number of bytes either in use by the app or fragmented so that
// it cannot be (arbitrarily) reused.
uint64_t RequiredBytes(const TCMallocStats& stats) {
  return StatSub(PhysicalMemoryUsed(stats), stats.pageheap.free_bytes);
}

size_t ExternalBytes(const TCMallocStats& stats) {
  return stats.pageheap.free_bytes + stats.central_bytes + stats.per_cpu_bytes +
         stats.sharded_transfer_bytes + stats.transfer_bytes +
         stats.thread_bytes + stats.metadata_bytes +
         stats.arena.bytes_unavailable + stats.arena.bytes_unallocated;
}

size_t HeapSizeBytes(const BackingStats& stats) {
  return StatSub(stats.system_bytes, stats.unmapped_bytes);
}

size_t LocalBytes(const TCMallocStats& stats) {
  return stats.thread_bytes + stats.per_cpu_bytes +
         stats.sharded_transfer_bytes;
}

size_t SlackBytes(const BackingStats& stats) {
  return stats.free_bytes + stats.unmapped_bytes;
}

static int CountAllowedCpus() {
  cpu_set_t allowed_cpus;
  if (sched_getaffinity(0, sizeof(allowed_cpus), &allowed_cpus) != 0) {
    return 0;
  }

  return CPU_COUNT(&allowed_cpus);
}

static absl::string_view SizeClassConfigurationString(
    SizeClassConfiguration config) {
  switch (config) {
    case SizeClassConfiguration::kPow2Below64:
      return "SIZE_CLASS_POW2_BELOW_64";
    case SizeClassConfiguration::kPow2Only:
      return "SIZE_CLASS_POW2_ONLY";
    case SizeClassConfiguration::kLowFrag:
      return "SIZE_CLASS_LOW_FRAG";
    case SizeClassConfiguration::kLegacy:
      // TODO(b/242710633): remove this opt out.
      return "SIZE_CLASS_LEGACY";
  }

  ASSUME(false);
  return "SIZE_CLASS_UNKNOWN";
}

static absl::string_view PerCpuTypeString(RseqVcpuMode mode) {
  switch (mode) {
    case RseqVcpuMode::kNone:
      return "NONE";
  }

  ASSUME(false);
  return "NONE";
}

void DumpStats(Printer* out, int level) {
  TCMallocStats stats;
  uint64_t class_count[kNumClasses];
  SpanStats span_stats[kNumClasses];
  if (level >= 2) {
    ExtractStats(&stats, class_count, span_stats, nullptr, nullptr, true);
  } else {
    ExtractTCMallocStats(&stats, true);
  }

  static const double MiB = 1048576.0;

  out->printf(
      "See https://github.com/google/tcmalloc/tree/master/docs/stats.md for an explanation of "
      "this page\n");

  const uint64_t virtual_memory_used = VirtualMemoryUsed(stats);
  const uint64_t physical_memory_used = PhysicalMemoryUsed(stats);
  const uint64_t unmapped_bytes = UnmappedBytes(stats);
  const uint64_t bytes_in_use_by_app = InUseByApp(stats);

#ifdef TCMALLOC_INTERNAL_SMALL_BUT_SLOW
  out->printf("NOTE:  SMALL MEMORY MODEL IS IN USE, PERFORMANCE MAY SUFFER.\n");
#endif
  // clang-format off
  // Avoid clang-format complaining about the way that this text is laid out.
  out->printf(
      "------------------------------------------------\n"
      "MALLOC:   %12u (%7.1f MiB) Bytes in use by application\n"
      "MALLOC: + %12u (%7.1f MiB) Bytes in page heap freelist\n"
      "MALLOC: + %12u (%7.1f MiB) Bytes in central cache freelist\n"
      "MALLOC: + %12u (%7.1f MiB) Bytes in per-CPU cache freelist\n"
      "MALLOC: + %12u (%7.1f MiB) Bytes in Sharded cache freelist\n"
      "MALLOC: + %12u (%7.1f MiB) Bytes in transfer cache freelist\n"
      "MALLOC: + %12u (%7.1f MiB) Bytes in thread cache freelists\n"
      "MALLOC: + %12u (%7.1f MiB) Bytes in malloc metadata\n"
      "MALLOC: + %12u (%7.1f MiB) Bytes in malloc metadata Arena unallocated\n"
      "MALLOC: + %12u (%7.1f MiB) Bytes in malloc metadata Arena unavailable\n"

      "MALLOC:   ------------\n"
      "MALLOC: = %12u (%7.1f MiB) Actual memory used (physical + swap)\n"
      "MALLOC: + %12u (%7.1f MiB) Bytes released to OS (aka unmapped)\n"
      "MALLOC:   ------------\n"
      "MALLOC: = %12u (%7.1f MiB) Virtual address space used\n"
      "MALLOC:\n"
      "MALLOC:   %12u               Spans in use\n"
      "MALLOC:   %12u (%7.1f MiB) Spans created\n"
      "MALLOC:   %12u               Thread heaps in use\n"
      "MALLOC:   %12u (%7.1f MiB) Thread heaps created\n"
      "MALLOC:   %12u               Stack traces in use\n"
      "MALLOC:   %12u (%7.1f MiB) Stack traces created\n"
      "MALLOC:   %12u               Table buckets in use\n"
      "MALLOC:   %12u (%7.1f MiB) Table buckets created\n"
      "MALLOC:   %12u (%7.1f MiB) Pagemap bytes used\n"
      "MALLOC:   %12u (%7.1f MiB) Pagemap root resident bytes\n"
      "MALLOC:   %12u (%7.1f MiB) per-CPU slab bytes used\n"
      "MALLOC:   %12u (%7.1f MiB) per-CPU slab resident bytes\n"
      "MALLOC:   %12u (%7.1f MiB) malloc metadata Arena non-resident bytes\n"
      "MALLOC:   %12u (%7.1f MiB) Actual memory used at peak\n"
      "MALLOC:   %12u (%7.1f MiB) Estimated in-use at peak\n"
      "MALLOC:   %12.4f               Realized fragmentation (%%)\n"
      "MALLOC:   %12u               Tcmalloc page size\n"
      "MALLOC:   %12u               Tcmalloc hugepage size\n"
      "MALLOC:   %12u               CPUs Allowed in Mask\n"
      "MALLOC:   %12u               Arena blocks\n",
      bytes_in_use_by_app, bytes_in_use_by_app / MiB,
      stats.pageheap.free_bytes, stats.pageheap.free_bytes / MiB,
      stats.central_bytes, stats.central_bytes / MiB,
      stats.per_cpu_bytes, stats.per_cpu_bytes / MiB,
      stats.sharded_transfer_bytes, stats.sharded_transfer_bytes / MiB,
      stats.transfer_bytes, stats.transfer_bytes / MiB,
      stats.thread_bytes, stats.thread_bytes / MiB,
      stats.metadata_bytes, stats.metadata_bytes / MiB,
      stats.arena.bytes_unallocated, stats.arena.bytes_unallocated / MiB,
      stats.arena.bytes_unavailable, stats.arena.bytes_unavailable / MiB,
      physical_memory_used, physical_memory_used / MiB,
      unmapped_bytes, unmapped_bytes / MiB,
      virtual_memory_used, virtual_memory_used / MiB,
      uint64_t(stats.span_stats.in_use),
      uint64_t(stats.span_stats.total),
      (stats.span_stats.total * sizeof(Span)) / MiB,
      uint64_t(stats.tc_stats.in_use),
      uint64_t(stats.tc_stats.total),
      (stats.tc_stats.total * sizeof(ThreadCache)) / MiB,
      uint64_t(stats.stack_stats.in_use),
      uint64_t(stats.stack_stats.total),
      (stats.stack_stats.total * sizeof(StackTrace)) / MiB,
      uint64_t(stats.linked_sample_stats.in_use),
      uint64_t(stats.linked_sample_stats.total),
      (stats.linked_sample_stats.total * sizeof(StackTraceTable::LinkedSample)) / MiB,
      uint64_t(stats.pagemap_bytes),
      stats.pagemap_bytes / MiB,
      stats.pagemap_root_bytes_res, stats.pagemap_root_bytes_res / MiB,
      uint64_t(stats.percpu_metadata_bytes),
      stats.percpu_metadata_bytes / MiB,
      stats.percpu_metadata_bytes_res, stats.percpu_metadata_bytes_res / MiB,
      stats.arena.bytes_nonresident, stats.arena.bytes_nonresident / MiB,
      uint64_t(stats.peak_stats.backed_bytes),
      stats.peak_stats.backed_bytes / MiB,
      uint64_t(stats.peak_stats.sampled_application_bytes),
      stats.peak_stats.sampled_application_bytes / MiB,
      100. * safe_div(stats.peak_stats.backed_bytes - stats.peak_stats.sampled_application_bytes, stats.peak_stats.sampled_application_bytes),
      uint64_t(kPageSize),
      uint64_t(kHugePageSize),
      CountAllowedCpus(),
      stats.arena.blocks
  );
  // clang-format on

  out->printf("MALLOC EXPERIMENTS:");
  WalkExperiments([&](absl::string_view name, bool active) {
    const char* value = active ? "1" : "0";
    out->printf(" %s=%s", name, value);
  });
  out->printf("\n");

  out->printf(
      "MALLOC SAMPLED PROFILES: %zu bytes (current), %zu bytes (internal "
      "fragmentation), %zu bytes (peak), %zu count (total)\n",
      static_cast<size_t>(tc_globals.sampled_objects_size_.value()),
      tc_globals.sampled_internal_fragmentation_.value(),
      tc_globals.peak_heap_tracker().CurrentPeakSize(),
      tc_globals.total_sampled_count_.value());

  MemoryStats memstats;
  if (GetMemoryStats(&memstats)) {
    uint64_t rss = memstats.rss;
    uint64_t vss = memstats.vss;
    // clang-format off
    out->printf(
        "\n"
        "Total process stats (inclusive of non-malloc sources):\n"
        "TOTAL: %12u (%7.1f MiB) Bytes resident (physical memory used)\n"
        "TOTAL: %12u (%7.1f MiB) Bytes mapped (virtual memory used)\n",
        rss, rss / MiB, vss, vss / MiB);
    // clang-format on
  }

  out->printf(
      "------------------------------------------------\n"
      "Call ReleaseMemoryToSystem() to release freelist memory to the OS"
      " (via madvise()).\n"
      "Bytes released to the OS take up virtual address space"
      " but no physical memory.\n");
  if (level >= 2) {
    out->printf("------------------------------------------------\n");
    out->printf("Total size of freelists for per-thread and per-CPU caches,\n");
    out->printf("transfer cache, and central cache, as well as number of\n");
    out->printf("live pages, returned/requested spans by size class\n");
    out->printf("------------------------------------------------\n");

    uint64_t cumulative = 0;
    for (int size_class = 1; size_class < kNumClasses; ++size_class) {
      uint64_t class_bytes = class_count[size_class] *
                             tc_globals.sizemap().class_to_size(size_class);

      cumulative += class_bytes;
      out->printf(
          // clang-format off
          "class %3d [ %8zu bytes ] : %8u objs; %5.1f MiB; %6.1f cum MiB; "
          "%8u live pages; spans: %10zu ret / %10zu req = %5.4f;\n",
          // clang-format on
          size_class, tc_globals.sizemap().class_to_size(size_class),
          class_count[size_class], class_bytes / MiB, cumulative / MiB,
          span_stats[size_class].num_live_spans() *
              tc_globals.sizemap().class_to_pages(size_class),
          span_stats[size_class].num_spans_returned,
          span_stats[size_class].num_spans_requested,
          span_stats[size_class].prob_returned());
    }

#ifndef TCMALLOC_INTERNAL_SMALL_BUT_SLOW
    out->printf("------------------------------------------------\n");
    out->printf("Central cache freelist: Span utilization histogram\n");
    out->printf("Non-cumulative number of spans with allocated objects < N\n");
    out->printf("------------------------------------------------\n");
    for (int size_class = 1; size_class < kNumClasses; ++size_class) {
      tc_globals.central_freelist(size_class).PrintSpanUtilStats(out);
    }
#endif

    tc_globals.transfer_cache().Print(out);
    tc_globals.sharded_transfer_cache().Print(out);

    if (UsePerCpuCache(tc_globals)) {
      tc_globals.cpu_cache().Print(out);
    }

    tc_globals.page_allocator().Print(out, MemoryTag::kNormal);
    if (tc_globals.numa_topology().active_partitions() > 1) {
      tc_globals.page_allocator().Print(out, MemoryTag::kNormalP1);
    }
    tc_globals.page_allocator().Print(out, MemoryTag::kSampled);
    tc_globals.page_allocator().Print(out, MemoryTag::kCold);
    tc_globals.guardedpage_allocator().Print(out);
    selsan::PrintTextStats(out);

    uint64_t soft_limit_bytes =
        tc_globals.page_allocator().limit(PageAllocator::kSoft);
    uint64_t hard_limit_bytes =
        tc_globals.page_allocator().limit(PageAllocator::kHard);
    out->printf("PARAMETER desired_usage_limit_bytes %u\n", soft_limit_bytes);
    out->printf("PARAMETER hard_usage_limit_bytes %u\n", hard_limit_bytes);
    out->printf("Number of times soft limit was hit: %lld\n",
                tc_globals.page_allocator().limit_hits(PageAllocator::kSoft));
    out->printf("Number of times hard limit was hit: %lld\n",
                tc_globals.page_allocator().limit_hits(PageAllocator::kHard));
    out->printf("Number of times memory shrank below soft limit: %lld\n",
                tc_globals.page_allocator().successful_shrinks_after_limit_hit(
                    PageAllocator::kSoft));
    out->printf("Number of times memory shrank below hard limit: %lld\n",
                tc_globals.page_allocator().successful_shrinks_after_limit_hit(
                    PageAllocator::kHard));
    out->printf("PARAMETER tcmalloc_per_cpu_caches %d\n",
                Parameters::per_cpu_caches() ? 1 : 0);
    out->printf("PARAMETER tcmalloc_max_per_cpu_cache_size %d\n",
                Parameters::max_per_cpu_cache_size());
    out->printf("PARAMETER tcmalloc_max_total_thread_cache_bytes %lld\n",
                Parameters::max_total_thread_cache_bytes());
    out->printf("PARAMETER malloc_release_bytes_per_sec %llu\n",
                Parameters::background_release_rate());
    out->printf(
        "PARAMETER tcmalloc_skip_subrelease_interval %s\n",
        absl::FormatDuration(Parameters::filler_skip_subrelease_interval()));
    out->printf("PARAMETER tcmalloc_skip_subrelease_short_interval %s\n",
                absl::FormatDuration(
                    Parameters::filler_skip_subrelease_short_interval()));
    out->printf("PARAMETER tcmalloc_skip_subrelease_long_interval %s\n",
                absl::FormatDuration(
                    Parameters::filler_skip_subrelease_long_interval()));
    out->printf("PARAMETER tcmalloc_release_partial_alloc_pages %d\n",
                Parameters::release_partial_alloc_pages() ? 1 : 0);
    out->printf("PARAMETER tcmalloc_huge_region_demand_based_release %d\n",
                Parameters::huge_region_demand_based_release() ? 1 : 0);
    out->printf("PARAMETER tcmalloc_release_pages_from_huge_region %d\n",
                Parameters::release_pages_from_huge_region() ? 1 : 0);
    out->printf("PARAMETER flat vcpus %d\n",
                subtle::percpu::UsingFlatVirtualCpus() ? 1 : 0);
    out->printf(
        "PARAMETER tcmalloc_separate_allocs_for_few_and_many_objects_spans "
        "%d\n",
        Parameters::separate_allocs_for_few_and_many_objects_spans());
    out->printf("PARAMETER tcmalloc_filler_chunks_per_alloc %d\n",
                Parameters::chunks_per_alloc());
    out->printf("PARAMETER tcmalloc_use_wider_slabs %d\n",
                tc_globals.cpu_cache().UseWiderSlabs() ? 1 : 0);
    out->printf("PARAMETER tcmalloc_configure_size_class_max_capacity %d\n",
                tc_globals.cpu_cache().ConfigureSizeClassMaxCapacity() ? 1 : 0);
    out->printf(
        "PARAMETER tcmalloc_use_all_buckets_for_few_object_spans %d\n",
        Parameters::use_all_buckets_for_few_object_spans_in_cfl() ? 1 : 0);

    out->printf(
        "PARAMETER size_class_config %s\n",
        SizeClassConfigurationString(tc_globals.size_class_configuration()));
    out->printf("PARAMETER percpu_vcpu_type %s\n",
                PerCpuTypeString(subtle::percpu::GetRseqVcpuMode()));
    out->printf("PARAMETER max_span_cache_size %d\n",
                Parameters::max_span_cache_size());
    out->printf("PARAMETER madvise %s\n", MadviseString());
  }
}

void DumpStatsInPbtxt(Printer* out, int level) {
  TCMallocStats stats;
  uint64_t class_count[kNumClasses];
  SpanStats span_stats[kNumClasses];
  if (level >= 2) {
    ExtractStats(&stats, class_count, span_stats, nullptr, nullptr, true);
  } else {
    ExtractTCMallocStats(&stats, true);
  }

  const uint64_t bytes_in_use_by_app = InUseByApp(stats);
  const uint64_t virtual_memory_used = VirtualMemoryUsed(stats);
  const uint64_t physical_memory_used = PhysicalMemoryUsed(stats);
  const uint64_t unmapped_bytes = UnmappedBytes(stats);

  PbtxtRegion region(out, kTop);
  region.PrintI64("in_use_by_app", bytes_in_use_by_app);
  region.PrintI64("page_heap_freelist", stats.pageheap.free_bytes);
  region.PrintI64("central_cache_freelist", stats.central_bytes);
  region.PrintI64("per_cpu_cache_freelist", stats.per_cpu_bytes);
  region.PrintI64("sharded_transfer_cache_freelist",
                  stats.sharded_transfer_bytes);
  region.PrintI64("transfer_cache_freelist", stats.transfer_bytes);
  region.PrintI64("thread_cache_freelists", stats.thread_bytes);
  region.PrintI64("malloc_metadata", stats.metadata_bytes);
  region.PrintI64("malloc_metadata_arena_unavailable",
                  stats.arena.bytes_unavailable);
  region.PrintI64("malloc_metadata_arena_unallocated",
                  stats.arena.bytes_unallocated);
  region.PrintI64("actual_mem_used", physical_memory_used);
  region.PrintI64("unmapped", unmapped_bytes);
  region.PrintI64("virtual_address_space_used", virtual_memory_used);
  region.PrintI64("num_spans", uint64_t(stats.span_stats.in_use));
  region.PrintI64("num_spans_created", uint64_t(stats.span_stats.total));
  region.PrintI64("num_thread_heaps", uint64_t(stats.tc_stats.in_use));
  region.PrintI64("num_thread_heaps_created", uint64_t(stats.tc_stats.total));
  region.PrintI64("num_stack_traces", uint64_t(stats.stack_stats.in_use));
  region.PrintI64("num_stack_traces_created",
                  uint64_t(stats.stack_stats.total));
  region.PrintI64("num_table_buckets",
                  uint64_t(stats.linked_sample_stats.in_use));
  region.PrintI64("num_table_buckets_created",
                  uint64_t(stats.linked_sample_stats.total));
  region.PrintI64("pagemap_size", uint64_t(stats.pagemap_bytes));
  region.PrintI64("pagemap_root_residence", stats.pagemap_root_bytes_res);
  region.PrintI64("percpu_slab_size", stats.percpu_metadata_bytes);
  region.PrintI64("percpu_slab_residence", stats.percpu_metadata_bytes_res);
  region.PrintI64("peak_backed", stats.peak_stats.backed_bytes);
  region.PrintI64("peak_application_demand",
                  stats.peak_stats.sampled_application_bytes);
  region.PrintI64("tcmalloc_page_size", uint64_t(kPageSize));
  region.PrintI64("tcmalloc_huge_page_size", uint64_t(kHugePageSize));
  region.PrintI64("cpus_allowed", CountAllowedCpus());
  region.PrintI64("arena_blocks", stats.arena.blocks);

  {
    auto sampled_profiles = region.CreateSubRegion("sampled_profiles");
    sampled_profiles.PrintI64("current_bytes",
                              tc_globals.sampled_objects_size_.value());
    sampled_profiles.PrintI64(
        "current_fragmentation_bytes",
        tc_globals.sampled_internal_fragmentation_.value());
    sampled_profiles.PrintI64("peak_bytes",
                              tc_globals.peak_heap_tracker().CurrentPeakSize());
  }

  // Print total process stats (inclusive of non-malloc sources).
  MemoryStats memstats;
  if (GetMemoryStats(&memstats)) {
    region.PrintI64("total_resident", uint64_t(memstats.rss));
    region.PrintI64("total_mapped", uint64_t(memstats.vss));
  }

  region.PrintI64("total_sampled_count",
                  tc_globals.total_sampled_count_.value());

  if (level >= 2) {
    {
#ifndef TCMALLOC_INTERNAL_SMALL_BUT_SLOW
      for (int size_class = 1; size_class < kNumClasses; ++size_class) {
        uint64_t class_bytes = class_count[size_class] *
                               tc_globals.sizemap().class_to_size(size_class);
        PbtxtRegion entry = region.CreateSubRegion("freelist");
        entry.PrintI64("sizeclass",
                       tc_globals.sizemap().class_to_size(size_class));
        entry.PrintI64("bytes", class_bytes);
        entry.PrintI64("num_spans_requested",
                       span_stats[size_class].num_spans_requested);
        entry.PrintI64("num_spans_returned",
                       span_stats[size_class].num_spans_returned);
        entry.PrintI64("obj_capacity", span_stats[size_class].obj_capacity);
        tc_globals.central_freelist(size_class)
            .PrintSpanUtilStatsInPbtxt(&entry);
      }
#endif
    }

    tc_globals.transfer_cache().PrintInPbtxt(&region);
    tc_globals.sharded_transfer_cache().PrintInPbtxt(&region);

    if (UsePerCpuCache(tc_globals)) {
      tc_globals.cpu_cache().PrintInPbtxt(&region);
    }
  }
  tc_globals.page_allocator().PrintInPbtxt(&region, MemoryTag::kNormal);
  if (tc_globals.numa_topology().active_partitions() > 1) {
    tc_globals.page_allocator().PrintInPbtxt(&region, MemoryTag::kNormalP1);
  }
  tc_globals.page_allocator().PrintInPbtxt(&region, MemoryTag::kSampled);
  tc_globals.page_allocator().PrintInPbtxt(&region, MemoryTag::kCold);
  // We do not collect tracking information in pbtxt.

  size_t soft_limit_bytes =
      tc_globals.page_allocator().limit(PageAllocator::kSoft);
  size_t hard_limit_bytes =
      tc_globals.page_allocator().limit(PageAllocator::kHard);

  region.PrintI64("desired_usage_limit_bytes", soft_limit_bytes);
  region.PrintI64("hard_usage_limit_bytes", hard_limit_bytes);
  region.PrintI64("soft_limit_hits",
                  tc_globals.page_allocator().limit_hits(PageAllocator::kSoft));
  region.PrintI64("hard_limit_hits",
                  tc_globals.page_allocator().limit_hits(PageAllocator::kHard));
  region.PrintI64(
      "successful_shrinks_after_soft_limit_hit",
      tc_globals.page_allocator().successful_shrinks_after_limit_hit(
          PageAllocator::kSoft));
  region.PrintI64(
      "successful_shrinks_after_hard_limit_hit",
      tc_globals.page_allocator().successful_shrinks_after_limit_hit(
          PageAllocator::kHard));
  {
    auto gwp_asan = region.CreateSubRegion("gwp_asan");
    tc_globals.guardedpage_allocator().PrintInPbtxt(&gwp_asan);
  }
  selsan::PrintPbtxtStats(&region);

  region.PrintI64("memory_release_failures", SystemReleaseErrors());

  region.PrintBool("tcmalloc_per_cpu_caches", Parameters::per_cpu_caches());
  region.PrintI64("tcmalloc_max_per_cpu_cache_size",
                  Parameters::max_per_cpu_cache_size());
  region.PrintI64("tcmalloc_max_total_thread_cache_bytes",
                  Parameters::max_total_thread_cache_bytes());
  region.PrintI64("malloc_release_bytes_per_sec",
                  static_cast<int64_t>(Parameters::background_release_rate()));
  region.PrintI64(
      "tcmalloc_skip_subrelease_interval_ns",
      absl::ToInt64Nanoseconds(Parameters::filler_skip_subrelease_interval()));
  region.PrintI64("tcmalloc_skip_subrelease_short_interval_ns",
                  absl::ToInt64Nanoseconds(
                      Parameters::filler_skip_subrelease_short_interval()));
  region.PrintI64("tcmalloc_skip_subrelease_long_interval_ns",
                  absl::ToInt64Nanoseconds(
                      Parameters::filler_skip_subrelease_long_interval()));
  region.PrintBool("tcmalloc_release_partial_alloc_pages",
                   Parameters::release_partial_alloc_pages());
  region.PrintBool("tcmalloc_huge_region_demand_based_release",
                   Parameters::huge_region_demand_based_release());
  region.PrintBool("tcmalloc_release_pages_from_huge_region",
                   Parameters::release_pages_from_huge_region());
  region.PrintI64("profile_sampling_rate", Parameters::profile_sampling_rate());
  region.PrintRaw("percpu_vcpu_type",
                  PerCpuTypeString(subtle::percpu::GetRseqVcpuMode()));
  region.PrintI64("separate_allocs_for_few_and_many_objects_spans",
                  Parameters::separate_allocs_for_few_and_many_objects_spans());
  region.PrintI64("tcmalloc_filler_chunks_per_alloc",
                  Parameters::chunks_per_alloc());
  region.PrintI64("tcmalloc_use_wider_slabs",
                  tc_globals.cpu_cache().UseWiderSlabs());
  region.PrintBool("tcmalloc_configure_size_class_max_capacity",
                   tc_globals.cpu_cache().ConfigureSizeClassMaxCapacity());
  region.PrintI64("tcmalloc_use_all_buckets_for_few_object_spans",
                  Parameters::use_all_buckets_for_few_object_spans_in_cfl());
  region.PrintI64("span_max_cache_size", Parameters::max_span_cache_size());

  region.PrintRaw(
      "size_class_config",
      SizeClassConfigurationString(tc_globals.size_class_configuration()));
  region.PrintRaw("madvise", MadviseString());
}

bool GetNumericProperty(const char* name_data, size_t name_size,
                        size_t* value) {
  // LINT.IfChange
  TC_ASSERT_NE(name_data, nullptr);
  TC_ASSERT_NE(value, nullptr);
  const absl::string_view name(name_data, name_size);

  // This is near the top since ReleasePerCpuMemoryToOS() calls it frequently.
  if (name == "tcmalloc.per_cpu_caches_active") {
    *value = tc_globals.CpuCacheActive();
    return true;
  }

  if (name == "generic.virtual_memory_used") {
    TCMallocStats stats;
    ExtractTCMallocStats(&stats, false);
    *value = VirtualMemoryUsed(stats);
    return true;
  }

  if (name == "generic.physical_memory_used") {
    TCMallocStats stats;
    ExtractTCMallocStats(&stats, false);
    *value = PhysicalMemoryUsed(stats);
    return true;
  }

  if (name == "generic.current_allocated_bytes" ||
      name == "generic.bytes_in_use_by_app") {
    TCMallocStats stats;
    ExtractTCMallocStats(&stats, false);
    *value = InUseByApp(stats);
    return true;
  }

  if (name == "generic.peak_memory_usage") {
    TCMallocStats stats;
    ExtractTCMallocStats(&stats, false);
    *value = static_cast<uint64_t>(stats.peak_stats.sampled_application_bytes);
    return true;
  }

  if (name == "generic.realized_fragmentation") {
    TCMallocStats stats;
    ExtractTCMallocStats(&stats, false);
    *value = static_cast<uint64_t>(
        100. * safe_div(stats.peak_stats.backed_bytes -
                            stats.peak_stats.sampled_application_bytes,
                        stats.peak_stats.sampled_application_bytes));

    return true;
  }

  if (name == "generic.heap_size") {
    PageHeapSpinLockHolder l;
    BackingStats stats = tc_globals.page_allocator().stats();
    *value = HeapSizeBytes(stats);
    return true;
  }

  if (name == "tcmalloc.central_cache_free") {
    TCMallocStats stats;
    ExtractTCMallocStats(&stats, false);
    *value = stats.central_bytes;
    return true;
  }

  if (name == "tcmalloc.cpu_free") {
    TCMallocStats stats;
    ExtractTCMallocStats(&stats, false);
    *value = stats.per_cpu_bytes;
    return true;
  }

  if (name == "tcmalloc.sharded_transfer_cache_free") {
    TCMallocStats stats;
    ExtractTCMallocStats(&stats, false);
    *value = stats.sharded_transfer_bytes;
    return true;
  }

  if (name == "tcmalloc.slack_bytes") {
    // Kept for backwards compatibility.  Now defined externally as:
    //    pageheap_free_bytes + pageheap_unmapped_bytes.
    PageHeapSpinLockHolder l;
    BackingStats stats = tc_globals.page_allocator().stats();
    *value = SlackBytes(stats);
    return true;
  }

  if (name == "tcmalloc.pageheap_free_bytes" ||
      name == "tcmalloc.page_heap_free") {
    PageHeapSpinLockHolder l;
    *value = tc_globals.page_allocator().stats().free_bytes;
    return true;
  }

  if (name == "tcmalloc.pageheap_unmapped_bytes" ||
      name == "tcmalloc.page_heap_unmapped") {
    PageHeapSpinLockHolder l;
    // Arena non-resident bytes aren't on the page heap, but they are unmapped.
    *value = tc_globals.page_allocator().stats().unmapped_bytes +
             tc_globals.arena().stats().bytes_nonresident;
    return true;
  }

  if (name == "tcmalloc.sampled_internal_fragmentation") {
    *value = tc_globals.sampled_internal_fragmentation_.value();
    return true;
  }

  if (name == "tcmalloc.page_algorithm") {
    PageHeapSpinLockHolder l;
    *value = tc_globals.page_allocator().algorithm();
    return true;
  }

  if (name == "tcmalloc.max_total_thread_cache_bytes") {
    PageHeapSpinLockHolder l;
    *value = ThreadCache::overall_thread_cache_size();
    return true;
  }

  if (name == "tcmalloc.current_total_thread_cache_bytes" ||
      name == "tcmalloc.thread_cache_free") {
    TCMallocStats stats;
    ExtractTCMallocStats(&stats, false);
    *value = stats.thread_bytes;
    return true;
  }

  if (name == "tcmalloc.thread_cache_count") {
    TCMallocStats stats;
    ExtractTCMallocStats(&stats, false);
    *value = stats.tc_stats.in_use;
    return true;
  }

  if (name == "tcmalloc.local_bytes") {
    TCMallocStats stats;
    ExtractTCMallocStats(&stats, false);
    *value = LocalBytes(stats);
    return true;
  }

  if (name == "tcmalloc.external_fragmentation_bytes") {
    TCMallocStats stats;
    ExtractTCMallocStats(&stats, false);
    *value = ExternalBytes(stats);
    return true;
  }

  if (name == "tcmalloc.metadata_bytes") {
    TCMallocStats stats;
    ExtractTCMallocStats(&stats, true);
    *value = stats.metadata_bytes;
    return true;
  }

  if (name == "tcmalloc.transfer_cache_free") {
    TCMallocStats stats;
    ExtractTCMallocStats(&stats, false);
    *value = stats.transfer_bytes;
    return true;
  }

  auto limit_kind = (name == "tcmalloc.hard_usage_limit_bytes")
                        ? PageAllocator::kHard
                        : PageAllocator::kSoft;
  if (limit_kind == PageAllocator::kHard ||
      name == "tcmalloc.desired_usage_limit_bytes") {
    *value = tc_globals.page_allocator().limit(limit_kind);
    return true;
  }
  if (name == "tcmalloc.soft_limit_hits") {
    *value = tc_globals.page_allocator().limit_hits(PageAllocator::kSoft);
    return true;
  }
  if (name == "tcmalloc.hard_limit_hits") {
    *value = tc_globals.page_allocator().limit_hits(PageAllocator::kHard);
    return true;
  }
  if (name == "tcmalloc.successful_shrinks_after_soft_limit_hit") {
    *value = tc_globals.page_allocator().successful_shrinks_after_limit_hit(
        PageAllocator::kSoft);
    return true;
  }
  if (name == "tcmalloc.successful_shrinks_after_hard_limit_hit") {
    *value = tc_globals.page_allocator().successful_shrinks_after_limit_hit(
        PageAllocator::kHard);
    return true;
  }
  if (name == "tcmalloc.required_bytes") {
    TCMallocStats stats;
    ExtractTCMallocStats(&stats, false);
    *value = RequiredBytes(stats);
    return true;
  }

  const absl::string_view kExperimentPrefix = "tcmalloc.experiment.";
  if (absl::StartsWith(name, kExperimentPrefix)) {
    std::optional<Experiment> exp =
        FindExperimentByName(absl::StripPrefix(name, kExperimentPrefix));
    if (exp.has_value()) {
      *value = IsExperimentActive(*exp) ? 1 : 0;
      return true;
    }
  }

  // LINT.ThenChange(//depot/google3/tcmalloc/testing/malloc_extension_test.cc)
  return false;
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
