// Copyright 2022 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "./fuzztest/internal/coverage.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <type_traits>

#include "absl/base/attributes.h"
#include "absl/strings/str_format.h"
#include "absl/types/span.h"
#include "./fuzztest/internal/flag_name.h"
#include "./fuzztest/internal/logging.h"
#include "./fuzztest/internal/table_of_recent_compares.h"

// IMPORTANT: Almost all functions (e.g. Update()) in this file will
// be called during coverage instrumentation callbacks.
//
// AVOID LIBRARY FUNCTION CALLS from here:
// Library functions can be instrumented, which cause reentrancy issues.

namespace fuzztest::internal {
namespace {

// We use this function in instrumentation callbacks instead of library
// functions (like `absl::bit_width`) in order to avoid having potentially
// instrumented code in the callback.
constexpr uint8_t BitWidth(uint8_t x) {
  return x == 0 ? 0 : (8 - __builtin_clz(x));
}

}  // namespace

// We want to make the tracing codes as light-weight as possible, so
// we disabled most sanitizers. Some may not be necessary but we don't
// want any one of them in the tracing codes so it's fine.
#define FUZZTEST_INTERNAL_NOSANITIZE \
  ABSL_ATTRIBUTE_NO_SANITIZE_MEMORY  \
  ABSL_ATTRIBUTE_NO_SANITIZE_ADDRESS \
  ABSL_ATTRIBUTE_NO_SANITIZE_UNDEFINED

// TODO(b/311658540):
//
// When integrated with Centipede, the execution coverage is
// created/used by only the mutators in the engine or runner process
// (for auto-dictionary mutation). Since each mutator runs in its
// thread without the need to share the coverage information with
// others, we can make this singleton thread_local, otherwise there
// can be data-races when accessing the instance from multiple
// Centipede shards.
//
// When running without Centipede, the singleton instance is updated
// by test threads during the test execution. Thus we cannot make it
// thread_local as it would skip coverage information in the threads
// other than the thread running the property function, but we then
// suffer from the race conditions.  This issue is hard to fix, but as
// we are fully switching to the Centipede integration soon, we will
// leave the issue as-is.
#ifdef FUZZTEST_USE_CENTIPEDE
thread_local ExecutionCoverage *execution_coverage_instance = nullptr;
#else
ExecutionCoverage *execution_coverage_instance = nullptr;
#endif

void SetExecutionCoverage(ExecutionCoverage *value) {
  execution_coverage_instance = value;
}

ExecutionCoverage* GetExecutionCoverage() {
  return execution_coverage_instance;
}

FUZZTEST_INTERNAL_NOSANITIZE void ExecutionCoverage::UpdateCmpMap(
    size_t index, uint8_t hamming_dist, uint8_t absolute_dist) {
  index %= kCmpCovMapSize;
  // Normalize counter value with log2 to reduce corpus size.
  uint8_t bucketized_counter = BitWidth(++new_cmp_counter_map_[index]);
  if (bucketized_counter > max_cmp_map_[index].counter) {
    max_cmp_map_[index].counter = bucketized_counter;
    max_cmp_map_[index].hamming = hamming_dist;
    max_cmp_map_[index].absolute = absolute_dist;
    new_coverage_.store(true, std::memory_order_relaxed);
  } else if (bucketized_counter == max_cmp_map_[index].counter) {
    if (max_cmp_map_[index].hamming < hamming_dist) {
      new_coverage_.store(true, std::memory_order_relaxed);
      max_cmp_map_[index].hamming = hamming_dist;
    }
    if (max_cmp_map_[index].absolute < absolute_dist) {
      new_coverage_.store(true, std::memory_order_relaxed);
      max_cmp_map_[index].absolute = absolute_dist;
    }
  }
}

void ExecutionCoverage::UpdateMaxStack(uintptr_t PC) {
  auto &stack = test_thread_stack;
  if (!stack ||
      stack->stack_frame_before_calling_property_function == nullptr ||
      stack->allocated_stack_region_size == 0) {
    // No stack info.
    return;
  }

  // Avoid reentrancy here. Code below could trigger reentrancy and if we don't
  // stop it we could easily cause an infinite recursion.
  // We only allow a single call to `UpdateMaxStack` per thread.
  static thread_local bool updating_max_stack = false;
  if (updating_max_stack) {
    // Already updating up the stack.
    return;
  }
  // Mark as updating for nested calls.
  updating_max_stack = true;
  struct Reset {
    ~Reset() { updating_max_stack = false; }
  };
  // Reset back to !updating on any exit path.
  Reset reset;

  const char *this_frame = GetCurrentStackFrame();
  if (this_frame < stack->allocated_stack_region_start ||
      stack->allocated_stack_region_start +
              stack->allocated_stack_region_size <=
          this_frame) {
    // The current stack frame pointer is outside the known thread stack.
    // This is either not the right thread, or we are running under a different
    // stack (eg signal handler in an alt stack).
    return;
  }
  const ptrdiff_t this_stack =
      stack->stack_frame_before_calling_property_function - this_frame;

  // Hash to use more of the map array. The PC is normally aligned which mean
  // the lower bits are zero. By hashing we put some entropy on those bits.
  const auto mix_or = [](uint64_t x) { return x ^ (x >> 32); };
  const size_t index = mix_or(PC * 0x9ddfea08eb382d69) % kMaxStackMapSize;
  if (this_stack > max_stack_map_[index]) {
    max_stack_map_[index] = this_stack;
    // Reaching a new max on any PC is new coverage.
    new_coverage_.store(true, std::memory_order_relaxed);

    // Keep the total max for stats.
    if (this_stack > max_stack_recorded_) {
      max_stack_recorded_ = this_stack;
    }

    if (StackLimit() > 0 && static_cast<size_t>(this_stack) > StackLimit()) {
      absl::FPrintF(GetStderr(),
                    "[!] Code under test used %d bytes of stack. Configured "
                    "limit is %d. You can change the limit by specifying "
                    "--" FUZZTEST_FLAG_PREFIX "stack_limit_kb flag.\n",
                    this_stack, StackLimit());
      std::abort();
    }
  }
}

// Coverage only available in Clang, but only for Linux, macOS, and newer
// versions of Android. Windows might not have what we need.
#if /* Supported compilers */                         \
    defined(__clang__) &&                             \
    (/* Supported platforms */                        \
     (defined(__linux__) && !defined(__ANDROID__)) || \
     (defined(__ANDROID_MIN_SDK_VERSION__) &&         \
      __ANDROID_MIN_SDK_VERSION__ >= 28) ||           \
     defined(__APPLE__))
#define FUZZTEST_COVERAGE_IS_AVAILABLE
#endif

#ifdef FUZZTEST_COVERAGE_IS_AVAILABLE
namespace {
// Use clang's vector extensions. This way it will implement with whatever the
// platform supports.
// Using a large vector size allows the compiler to choose the largest
// vectorized instruction it can for the architecture.
// Eg, it will use 4 xmm's per iteration in westmere, 2 ymm's in haswell, and 1
// zmm when avx512 is enabled.
using Vector = uint8_t __attribute__((vector_size(64)));

constexpr size_t kVectorSize = sizeof(Vector);
FUZZTEST_INTERNAL_NOSANITIZE bool UpdateVectorized(
    const uint8_t *execution_data, uint8_t *corpus_data, size_t size,
    size_t offset_to_align) {
  FUZZTEST_INTERNAL_CHECK(size >= kVectorSize,
                          "size cannot be smaller than block size!");

  // Avoid collapsing the "greater than" vector until the end.
  Vector any_greater{};

  // When aligned, just cast. This generates an aligned instruction.
  // When unaligned, go through memcpy. This generates a slower unaligned
  // instruction.
  const auto read = [](const uint8_t *p, auto aligned)
                        FUZZTEST_INTERNAL_NOSANITIZE {
                          if constexpr (aligned) {
                            return *reinterpret_cast<const Vector *>(p);
                          } else {
                            Vector v;
                            memcpy(&v, p, sizeof(v));
                            return v;
                          }
                        };
  const auto write = [](uint8_t *p, Vector v, auto aligned)
                         FUZZTEST_INTERNAL_NOSANITIZE {
                           if constexpr (aligned) {
                             *reinterpret_cast<Vector *>(p) = v;
                           } else {
                             memcpy(p, &v, sizeof(v));
                           }
                         };
  // We don't care about potential ABI change since all of this has internal
  // linkage. Silence the warning.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunknown-warning-option"
#pragma clang diagnostic ignored "-Wpsabi"
  const auto merge_data = [&](auto aligned) FUZZTEST_INTERNAL_NOSANITIZE {
    const Vector execution_v = read(execution_data, aligned);
    const Vector corpus_v = read(corpus_data, aligned);
    // Normalize counter value with log2 to reduce corpus size.
    // Approximation for `bit_width(execution_v) > bit_width(corpus_v)`:
    // When the above comparison returns true, this comparison may not be
    // true; But when this comparison is true, the above comparison must be
    // true.
    const Vector max_v = execution_v >> 1 >= corpus_v ? execution_v : corpus_v;
    write(corpus_data, max_v, aligned);
    any_greater |= max_v ^ corpus_v;
  };
#pragma clang diagnostic pop

  // Merge every sizeof(Vector) chunks.
  // We read the first and last blocks with unaligned reads.
  // The rest we make sure that memory is properly aligned and use the faster
  // aligned operations. There will be overlap between the two parts, but the
  // merge is idempotent.

  merge_data(std::false_type{});
  execution_data += offset_to_align;
  corpus_data += offset_to_align;
  size -= offset_to_align;

  for (; size > kVectorSize; size -= kVectorSize, execution_data += kVectorSize,
                             corpus_data += kVectorSize) {
    merge_data(std::true_type{});
  }
  execution_data = execution_data + size - kVectorSize;
  corpus_data = corpus_data + size - kVectorSize;
  merge_data(std::false_type{});

  // If any position has a bit on, we updated something.
  for (size_t i = 0; i < sizeof(Vector); ++i) {
    if (any_greater[i]) return true;
  }
  return false;
}
}  // namespace

CorpusCoverage::CorpusCoverage(size_t map_size) {
  size_t alignment = alignof(Vector);
  // Round up to a multiple of alignment.
  map_size += alignment - 1;
  map_size -= map_size % alignment;
  // And allocate an extra step to make sure the alignment logic has the
  // necessary space.
  map_size += alignment;
  corpus_map_size_ = map_size;
  corpus_map_ = static_cast<uint8_t*>(std::aligned_alloc(alignment, map_size));
  std::fill(corpus_map_, corpus_map_ + corpus_map_size_, 0);
}

CorpusCoverage::~CorpusCoverage() { std::free(corpus_map_); }

bool CorpusCoverage::Update(ExecutionCoverage* execution_coverage) {
  absl::Span<uint8_t> execution_map = execution_coverage->GetCounterMap();
  // Note: corpus_map_size_ will be larger than execution_map.size().
  // See the constructor for more details.
  FUZZTEST_INTERNAL_CHECK(execution_map.size() <= corpus_map_size_,
                          "Map size mismatch.");

  // Calculate the offset required to align `p` to alignof(Vector).
  void* p = execution_map.data();
  size_t space = execution_map.size();
  // If we can't align, then the buffer is too small and we don't need to use
  // vectorization.
  if (std::align(alignof(Vector), sizeof(Vector), p, space)) {
    size_t offset_to_align = execution_map.size() - space;

    // Align the corpus to the same alignemnt as execution_data (relative to
    // alignof(Vector)). This makes it simpler to apply the same kinds of
    // reads on both. We skip some bytes in corpus_data, which is fine, since
    // we overallocated for this purpose.
    uint8_t* corpus_data = corpus_map_ + (alignof(Vector) - offset_to_align);

    return UpdateVectorized(execution_map.data(), corpus_data,
                            execution_map.size(), offset_to_align) ||
           execution_coverage->NewCoverageFound();
  }

  bool new_coverage = false;
  for (size_t i = 0; i < execution_map.size(); i++) {
    uint8_t bucketized_counter = BitWidth(execution_map[i]);
    if (bucketized_counter != 0) {
      if (corpus_map_[i] < bucketized_counter) {
        corpus_map_[i] = bucketized_counter;
        new_coverage = true;
      }
    }
  }
  return new_coverage || execution_coverage->NewCoverageFound();
}

#else  // FUZZTEST_COVERAGE_IS_AVAILABLE

// On other compilers we just need it to build, but we know we don't have any
// instrumentation.
CorpusCoverage::CorpusCoverage(size_t map_size)
    : corpus_map_size_(0), corpus_map_(nullptr) {}
CorpusCoverage::~CorpusCoverage() {}
bool CorpusCoverage::Update(ExecutionCoverage* execution_coverage) {
  return false;
}

#endif  // FUZZTEST_COVERAGE_IS_AVAILABLE

}  // namespace fuzztest::internal

#if !defined(FUZZTEST_COMPATIBILITY_MODE) && \
    !defined(FUZZTEST_USE_CENTIPEDE) && !defined(FUZZTEST_NO_LEGACY_COVERAGE)
// Sanitizer Coverage hooks.

// The instrumentation runtime calls back the following function at startup,
// where [start,end) is the array of 8-bit counters created for the current DSO.
extern "C" void __sanitizer_cov_8bit_counters_init(uint8_t* start,
                                                   uint8_t* stop) {
  FUZZTEST_INTERNAL_CHECK_PRECONDITION(start != nullptr,
                                       "Invalid counter map address.");
  FUZZTEST_INTERNAL_CHECK_PRECONDITION(start < stop,
                                       "Invalid counter map size.");
  size_t map_size = stop - start;

  // For now, we assume single DSO. This means that this call back should get
  // called only once, or if it gets called multiple times, the arguments should
  // be the same.
  using fuzztest::internal::execution_coverage_instance;
  using fuzztest::internal::ExecutionCoverage;
  if (execution_coverage_instance == nullptr) {
    fprintf(stderr, "[.] Sanitizer coverage enabled. Counter map size: %td",
            map_size);
    size_t cmp_map_size = fuzztest::internal::ExecutionCoverage::kCmpCovMapSize;
    fprintf(stderr, ", Cmp map size: %td\n", cmp_map_size);
    execution_coverage_instance = new fuzztest::internal::ExecutionCoverage(
        absl::Span<uint8_t>(start, map_size));
  } else if (execution_coverage_instance->GetCounterMap() ==
             absl::Span<uint8_t>(start, map_size)) {
    // Nothing to do.
  } else {
    fprintf(fuzztest::internal::GetStderr(),
            "Warning: __sanitizer_cov_8bit_counters_init was called multiple "
            "times with different arguments. Currently, we only support "
            "recording coverage metrics for the first DSO encountered.\n");
  }
}

// This function should have no external library dependencies to prevent
// accidental coverage instrumentation.
template <int data_size>
ABSL_ATTRIBUTE_ALWAYS_INLINE      // To make __builtin_return_address(0) work.
    FUZZTEST_INTERNAL_NOSANITIZE  // To skip arg1 - arg2 overflow.
    void
    TraceCmp(uint64_t arg1, uint64_t arg2, uint8_t argsize_bit,
             uintptr_t PC =
                 reinterpret_cast<uintptr_t>(__builtin_return_address(0))) {
  if (fuzztest::internal::execution_coverage_instance == nullptr ||
      !fuzztest::internal::execution_coverage_instance->IsTracing())
    return;
  uint64_t abs = arg1 > arg2 ? arg1 - arg2 : arg2 - arg1;
  fuzztest::internal::execution_coverage_instance->UpdateCmpMap(
      PC, argsize_bit - __builtin_popcount(arg1 ^ arg2),
      255U - (255U > abs ? abs : 255U));
  fuzztest::internal::execution_coverage_instance->GetTablesOfRecentCompares()
      .GetMutable<data_size>()
      .Insert(arg1, arg2);

  fuzztest::internal::execution_coverage_instance->UpdateMaxStack(PC);
}

FUZZTEST_INTERNAL_NOSANITIZE
static size_t InternalStrlen(const char *s1, const char *s2) {
  size_t len = 0;
  while (s1[len] && s2[len]) {
    len++;
  }
  return len;
}

FUZZTEST_INTERNAL_NOSANITIZE
static void TraceMemCmp(const uint8_t *s1, const uint8_t *s2, size_t n,
                        int result) {
  // Non-interesting cases.
  if (n <= 1 || result == 0) return;
  if (fuzztest::internal::execution_coverage_instance == nullptr ||
      !fuzztest::internal::execution_coverage_instance->IsTracing())
    return;
  fuzztest::internal::execution_coverage_instance->GetTablesOfRecentCompares()
      .GetMutable<0>()
      .Insert(s1, s2, n);
}

extern "C" {
void __sanitizer_cov_trace_const_cmp1(uint8_t Arg1, uint8_t Arg2) {
  TraceCmp<1>(Arg1, Arg2, sizeof(Arg1) * 8);
}
void __sanitizer_cov_trace_const_cmp2(uint16_t Arg1, uint16_t Arg2) {
  TraceCmp<2>(Arg1, Arg2, sizeof(Arg1) * 8);
}
void __sanitizer_cov_trace_const_cmp4(uint32_t Arg1, uint32_t Arg2) {
  TraceCmp<4>(Arg1, Arg2, sizeof(Arg1) * 8);
}
void __sanitizer_cov_trace_const_cmp8(uint64_t Arg1, uint64_t Arg2) {
  TraceCmp<8>(Arg1, Arg2, sizeof(Arg1) * 8);
}
void __sanitizer_cov_trace_cmp1(uint8_t Arg1, uint8_t Arg2) {
  TraceCmp<1>(Arg1, Arg2, sizeof(Arg1) * 8);
}
void __sanitizer_cov_trace_cmp2(uint16_t Arg1, uint16_t Arg2) {
  TraceCmp<2>(Arg1, Arg2, sizeof(Arg1) * 8);
}
void __sanitizer_cov_trace_cmp4(uint32_t Arg1, uint32_t Arg2) {
  TraceCmp<4>(Arg1, Arg2, sizeof(Arg1) * 8);
}
void __sanitizer_cov_trace_cmp8(uint64_t Arg1, uint64_t Arg2) {
  TraceCmp<8>(Arg1, Arg2, sizeof(Arg1) * 8);
}

FUZZTEST_INTERNAL_NOSANITIZE
void __sanitizer_cov_trace_switch(uint64_t Val, uint64_t *Cases) {
  uintptr_t PC = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
  switch (Cases[0]) {
    case 8:
      for (uint64_t i = 0; i < Cases[0]; i++) {
        TraceCmp<1>(Val, Cases[2 + i], Cases[1], PC + i);
      }
      break;
    case 16:
      for (uint64_t i = 0; i < Cases[0]; i++) {
        TraceCmp<2>(Val, Cases[2 + i], Cases[1], PC + i);
      }
      break;
    case 32:
      for (uint64_t i = 0; i < Cases[0]; i++) {
        TraceCmp<4>(Val, Cases[2 + i], Cases[1], PC + i);
      }
      break;
    case 64:
      for (uint64_t i = 0; i < Cases[0]; i++) {
        TraceCmp<8>(Val, Cases[2 + i], Cases[1], PC + i);
      }
      break;
    default:
      break;
  }
}

FUZZTEST_INTERNAL_NOSANITIZE
void __sanitizer_weak_hook_strcasecmp(void *, const char *s1, const char *s2,
                                      int result) {
  if (s1 == nullptr || s2 == nullptr) return;
  size_t n = InternalStrlen(s1, s2);
  TraceMemCmp(reinterpret_cast<const uint8_t *>(s1),
              reinterpret_cast<const uint8_t *>(s2), n, result);
}

FUZZTEST_INTERNAL_NOSANITIZE
void __sanitizer_weak_hook_memcmp(void *, const void *s1, const void *s2,
                                  size_t n, int result) {
  if (s1 == nullptr || s2 == nullptr) return;
  TraceMemCmp(reinterpret_cast<const uint8_t *>(s1),
              reinterpret_cast<const uint8_t *>(s2), n, result);
}

FUZZTEST_INTERNAL_NOSANITIZE
void __sanitizer_weak_hook_strncmp(void *, const char *s1, const char *s2,
                                   size_t n, int result) {
  if (s1 == nullptr || s2 == nullptr) return;
  size_t len = 0;
  while (len < n && s1[len] && s2[len]) ++len;
  TraceMemCmp(reinterpret_cast<const uint8_t *>(s1),
              reinterpret_cast<const uint8_t *>(s2), len, result);
}

FUZZTEST_INTERNAL_NOSANITIZE
void __sanitizer_weak_hook_strcmp(void *, const char *s1, const char *s2,
                                  int result) {
  if (s1 == nullptr || s2 == nullptr) return;
  size_t n = InternalStrlen(s1, s2);
  TraceMemCmp(reinterpret_cast<const uint8_t *>(s1),
              reinterpret_cast<const uint8_t *>(s2), n, result);
}

FUZZTEST_INTERNAL_NOSANITIZE
void __sanitizer_weak_hook_strncasecmp(void *caller_pc, const char *s1,
                                       const char *s2, size_t n, int result) {
  if (s1 == nullptr || s2 == nullptr) return;
  return __sanitizer_weak_hook_strncmp(caller_pc, s1, s2, n, result);
}
}

#endif  // !defined(FUZZTEST_COMPATIBILITY_MODE) &&
        // !defined(FUZZTEST_USE_CENTIPEDE) &&
        // !defined(FUZZTEST_NO_LEGACY_COVERAGE)
