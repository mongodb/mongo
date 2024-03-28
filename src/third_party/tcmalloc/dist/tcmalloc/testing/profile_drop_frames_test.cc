// Copyright 2023 The TCMalloc Authors
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

#include <malloc.h>
#include <stddef.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <regex>  // NOLINT(build/c++11)
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "absl/base/attributes.h"
#include "absl/container/flat_hash_map.h"
#include "absl/debugging/symbolize.h"
#include "absl/strings/match.h"
#include "absl/strings/str_replace.h"
#include "tcmalloc/internal/profile_builder.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/new_extension.h"

#if !defined(__STDC_VERSION_STDLIB_H__) || __STDC_VERSION_STDLIB_H__ < 202311L
// free_sized is a sized free function introduced in C23.
extern "C" void free_sized(void* ptr, size_t size) noexcept;
// free_aligned_sized is an overaligned sized free function introduced in C23.
extern "C" void free_aligned_sized(void* ptr, size_t align,
                                   size_t size) noexcept;
#endif

namespace {

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void wrap_delete(void* ptr, size_t size) {
  operator delete(ptr);
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void wrap_delete_array(void* ptr,
                                                           size_t size) {
  operator delete[](ptr);
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void wrap_sized_delete(void* ptr,
                                                           size_t size) {
#ifdef __cpp_sized_deallocation
  operator delete(ptr, size);
#else
  operator delete(ptr);
#endif
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void wrap_sized_delete_array(void* ptr,
                                                                 size_t size) {
#ifdef __cpp_sized_deallocation
  operator delete[](ptr, size);
#else
  operator delete[](ptr);
#endif
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void* wrap_new_cold(size_t size) {
  return operator new[](size, tcmalloc::hot_cold_t(0));
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void* wrap_new_array_aligned_cold_nothrow(
    size_t size) {
  return operator new[](size, std::align_val_t(64), std::nothrow,
                        tcmalloc::hot_cold_t(0));
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void* wrap_new_aligned(size_t size) {
  return operator new(size, std::align_val_t(64));
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void wrap_delete_aligned(void* ptr,
                                                             size_t size) {
#ifdef __cpp_sized_deallocation
  operator delete(ptr, size, std::align_val_t(64));
#else
  operator delete[](ptr);
#endif
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void* wrap_new_nothrow(size_t size) {
  return operator new(size, std::nothrow);
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void wrap_delete_nothrow(void* ptr,
                                                             size_t size) {
  operator delete(ptr, std::nothrow);
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void* wrap_calloc(size_t size) {
  return calloc(size, 1);
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void wrap_free(void* ptr, size_t size) {
  free(ptr);
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void wrap_free_aligned_sized(void* ptr,
                                                                 size_t size) {
  free_aligned_sized(ptr, 64, size);
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void wrap_sdallocx(void* ptr, size_t size) {
  sdallocx(ptr, size, 0);
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void* wrap_realloc(size_t size) {
  return realloc(malloc(1), size);
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void* wrap_reallocarray(size_t size) {
  return reallocarray(malloc(1), size, 1);
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void* wrap_posix_memalign(size_t size) {
  void* res;
  if (posix_memalign(&res, 64, size)) {
    abort();
  }
  return res;
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void* wrap_aligned_alloc(size_t size) {
  return aligned_alloc(64, size);
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void* wrap_memalign(size_t size) {
  return memalign(64, size);
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void* wrap_size_returning_operator_new(
    size_t size) {
  return tcmalloc_size_returning_operator_new(size).p;
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void*
wrap_size_returning_operator_new_nothrow(size_t size) {
  return tcmalloc_size_returning_operator_new_nothrow(size).p;
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void*
wrap_size_returning_operator_new_hot_cold(size_t size) {
  return tcmalloc_size_returning_operator_new_hot_cold(size,
                                                       tcmalloc::hot_cold_t(0))
      .p;
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void*
wrap_size_returning_operator_new_hot_cold_nothrow(size_t size) {
  return tcmalloc_size_returning_operator_new_hot_cold_nothrow(
             size, tcmalloc::hot_cold_t(0))
      .p;
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void*
wrap_size_returning_operator_new_aligned(size_t size) {
  return tcmalloc_size_returning_operator_new_aligned(size,
                                                      std::align_val_t(64))
      .p;
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void*
wrap_size_returning_operator_new_aligned_nothrow(size_t size) {
  return tcmalloc_size_returning_operator_new_aligned_nothrow(
             size, std::align_val_t(64))
      .p;
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void*
wrap_size_returning_operator_new_aligned_hot_cold(size_t size) {
  return tcmalloc_size_returning_operator_new_aligned_hot_cold(
             size, std::align_val_t(64), tcmalloc::hot_cold_t(0))
      .p;
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void*
wrap_size_returning_operator_new_aligned_hot_cold_nothrow(size_t size) {
  return tcmalloc_size_returning_operator_new_aligned_hot_cold_nothrow(
             size, std::align_val_t(64), tcmalloc::hot_cold_t(0))
      .p;
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void* wrap_std_allocator_allocate(
    size_t size) {
  return std::allocator<char>().allocate(size);
}

#if defined(_LIBCPP_VERSION) && _LIBCPP_VERSION >= 150000
inline ABSL_ATTRIBUTE_ALWAYS_INLINE void*
wrap_std_allocator_allocate_at_least_internal(size_t size) {
  std::allocator<char> a;
  return std::__allocate_at_least(a, size).ptr;
}
#endif

#if defined(__cpp_lib_allocate_at_least) && \
    __cpp_lib_allocate_at_least >= 202302L
inline ABSL_ATTRIBUTE_ALWAYS_INLINE void* wrap_std_allocator_allocate_at_least(
    size_t size) {
  return std::allocate_at_least(std::allocator<char>(), size).ptr;
}
#endif

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void wrap_std_allocator_deallocate(
    void* ptr, size_t size) {
  std::allocator<char>().deallocate(static_cast<char*>(ptr), size);
}

template <void*(alloc)(size_t), void(free)(void*, size_t)>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE void test() {
  constexpr size_t kLargeSize = 3 << 20;
  // Larger than the smallest size class b/c wrap_realloc first allocates
  // 1 byte and then reallocs to kSmallSize and we want it to free the old
  // object inside of realloc.
  constexpr size_t kSmallSize = 16;
  {
    void* volatile ptr = alloc(kLargeSize);
    free(ptr, kLargeSize);
  }
  for (size_t i = 0; i < kLargeSize / kSmallSize; i++) {
    void* volatile ptr = alloc(kSmallSize);
    free(ptr, kSmallSize);
  }
}

ABSL_ATTRIBUTE_NOINLINE
void profile_test_top_func() {
  test<operator new, wrap_delete>();
  test<operator new, wrap_sized_delete>();
  test<operator new[], wrap_sized_delete_array>();
  test<operator new[], wrap_delete_array>();
  test<wrap_new_cold, wrap_sized_delete>();
  test<wrap_new_array_aligned_cold_nothrow, wrap_delete_aligned>();
  test<wrap_new_aligned, wrap_delete_aligned>();
  test<wrap_new_nothrow, wrap_delete_nothrow>();
  test<wrap_size_returning_operator_new, wrap_sized_delete>();
  test<wrap_size_returning_operator_new_nothrow, wrap_sized_delete>();
  test<wrap_size_returning_operator_new_hot_cold, wrap_sized_delete>();
  test<wrap_size_returning_operator_new_hot_cold_nothrow, wrap_sized_delete>();
  test<wrap_size_returning_operator_new_aligned, wrap_delete_aligned>();
  test<wrap_size_returning_operator_new_aligned_nothrow, wrap_delete_aligned>();
  test<wrap_size_returning_operator_new_aligned_hot_cold,
       wrap_delete_aligned>();
  test<wrap_size_returning_operator_new_aligned_hot_cold_nothrow,
       wrap_delete_aligned>();
  test<wrap_std_allocator_allocate, wrap_std_allocator_deallocate>();
#if defined(__cpp_lib_allocate_at_least) && \
    __cpp_lib_allocate_at_least >= 202302L
  test<wrap_std_allocator_allocate_at_least, wrap_std_allocator_deallocate>();
#endif
#if defined(_LIBCPP_VERSION) && _LIBCPP_VERSION >= 150000
  test<wrap_std_allocator_allocate_at_least_internal,
       wrap_std_allocator_deallocate>();
#endif
  test<malloc, wrap_free>();
  test<malloc, free_sized>();
  test<valloc, wrap_free>();
  test<malloc, wrap_sdallocx>();
  test<wrap_realloc, wrap_free>();
  test<wrap_reallocarray, wrap_free>();
  test<wrap_calloc, wrap_free>();
  test<wrap_posix_memalign, wrap_free>();
  test<wrap_aligned_alloc, wrap_free>();
  test<wrap_aligned_alloc, wrap_free_aligned_sized>();
  test<wrap_memalign, wrap_free>();
#if defined(__x86_64__)
  // pvalloc is not present in some versions of libc.
  test<pvalloc, wrap_free>();
#endif
}

// Test that kProfileDropFrames properly marks all internal frames
// from all possible allocation/deallocation functions.
TEST(AllocationSampleTest, ProfileDropFrames) {
  auto alloc_profiler = tcmalloc::MallocExtension::StartAllocationProfiling();
  auto lifetime_profiler = tcmalloc::MallocExtension::StartLifetimeProfiling();
  profile_test_top_func();
  auto alloc_profile = std::move(alloc_profiler).Stop();
  auto lifetime_profile = std::move(lifetime_profiler).Stop();

  std::regex drop_frames(
      tcmalloc::tcmalloc_internal::kProfileDropFrames.begin(),
      tcmalloc::tcmalloc_internal::kProfileDropFrames.end());
  absl::flat_hash_map<void*, std::string> symbol_cache;
  absl::flat_hash_map<std::string, bool> drop_cache;
  std::vector<char> symbol_name(4 << 10);
  bool failed = false;
  int got_stacks = 0;
  constexpr absl::string_view kOurFunction =
      "(anonymous namespace)::profile_test_top_func";
  auto check = [&](const tcmalloc::Profile& profile) {
    profile.Iterate([&](const tcmalloc::Profile::Sample& sample) {
      std::vector<std::string> stack;
      bool our = false;
      ssize_t last_matched = -1;
      for (int i = 0; i < sample.depth; i++) {
        void* pc = sample.stack[i];
        if (symbol_cache[pc].empty()) {
          char buf[1024];
          if (!absl::Symbolize(pc, buf, sizeof(buf))) {
            snprintf(buf, sizeof(buf), "%p", pc);
          }
          const char* symb = buf;
          // Remove file:line part;
          if (strstr(symb, ".cc:") || strstr(symb, ".h:")) {
            symb = strchr(symb, ' ');
            ASSERT_NE(symb, nullptr);
          }
          std::string str(symb);
          // Remove <>/().
          // absl::Symbolize adds them for templates/functions,
          // but github.com/ianlancetaylor/demangle that pprof uses
          // does not add them, so remove them to match what pprof will see.
          absl::StrReplaceAll({{"<>", ""}, {"()", ""}}, &str);
          symbol_cache[pc] = str;
        }
        const auto& symb = symbol_cache[pc];
        stack.push_back(symb);
        if (drop_cache.find(symb) == drop_cache.end()) {
          drop_cache[symb] =
              std::regex_search(symb.begin(), symb.end(), drop_frames);
        }
        if (drop_cache[symb]) {
          last_matched = i;
        }
        // Somehow we get stacks from testing::InitGUnit() in our profiles,
        // filter them out.
        our |= absl::StrContains(symb, kOurFunction);
      }
      if (!our) {
        return;
      }
      got_stacks++;
      if (last_matched + 1 < sample.depth &&
          absl::StrContains(stack[last_matched + 1], kOurFunction)) {
        return;
      }
      failed = true;
      fprintf(stderr, "bad stack trace:\n");
      for (size_t i = 0; i < stack.size(); i++) {
        fprintf(stderr, "  #%zu: %p %s%s\n", i, sample.stack[i],
                stack[i].c_str(), last_matched == i ? " ***" : "");
      }
    });
  };
  check(alloc_profile);
  EXPECT_GT(got_stacks, 20);
  got_stacks = 0;
  check(lifetime_profile);
  EXPECT_GT(got_stacks, 20);
  EXPECT_FALSE(failed);
}

}  // namespace
