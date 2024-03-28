// Copyright 2020 The TCMalloc Authors
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

#include "tcmalloc/new_extension.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <algorithm>
#include <limits>
#include <new>
#include <vector>

#include "benchmark/benchmark.h"
#include "gtest/gtest.h"
#include "absl/base/optimization.h"
#include "absl/numeric/bits.h"
#include "absl/random/random.h"
#include "tcmalloc/internal/page_size.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/testing/testutil.h"

namespace tcmalloc {
namespace {

TEST(HotColdNew, InvalidSizeFails) {
#if defined(ABSL_HAVE_ADDRESS_SANITIZER) || \
    defined(ABSL_HAVE_MEMORY_SANITIZER) || defined(ABSL_HAVE_THREAD_SANITIZER)
  GTEST_SKIP() << "skipping large allocation tests on sanitizers";
#endif
  constexpr size_t kBadSize = std::numeric_limits<size_t>::max();
  EXPECT_DEATH(::operator new (kBadSize, hot_cold_t{0}), ".*");
  EXPECT_DEATH(::operator new (kBadSize, hot_cold_t{128}), ".*");
  EXPECT_DEATH(::operator new (kBadSize, hot_cold_t{255}), ".*");
  EXPECT_DEATH(::operator new[](kBadSize, hot_cold_t{0}), ".*");
  EXPECT_DEATH(::operator new[](kBadSize, hot_cold_t{128}), ".*");
  EXPECT_DEATH(::operator new[](kBadSize, hot_cold_t{255}), ".*");
}

TEST(HotColdNew, InvalidSizeNothrow) {
  constexpr size_t kBadSize = std::numeric_limits<size_t>::max();
  EXPECT_EQ(::operator new (kBadSize, std::nothrow, hot_cold_t{0}), nullptr);
  EXPECT_EQ(::operator new (kBadSize, std::nothrow, hot_cold_t{128}), nullptr);
  EXPECT_EQ(::operator new (kBadSize, std::nothrow, hot_cold_t{255}), nullptr);
  EXPECT_EQ(::operator new[](kBadSize, std::nothrow, hot_cold_t{0}), nullptr);
  EXPECT_EQ(::operator new[](kBadSize, std::nothrow, hot_cold_t{128}), nullptr);
  EXPECT_EQ(::operator new[](kBadSize, std::nothrow, hot_cold_t{255}), nullptr);
}

struct SizedPtr {
  void* ptr;
  size_t size;
};

TEST(HotColdNew, OperatorNew) {
  // Scan densely.
  for (size_t size = 0; size < 1024; ++size) {
    for (auto hotcold : {0, 128, 255}) {
      void* ret = ::operator new(size, static_cast<hot_cold_t>(hotcold));
      ASSERT_NE(ret, nullptr);
      benchmark::DoNotOptimize(memset(ret, 0xBF, size));
      ::operator delete(ret);
    }
  }

  // Try a single allocation using the new hot_cold_t type from the global
  // namespace.
  void* ret = ::operator new(1, static_cast<__hot_cold_t>(0));
  ASSERT_NE(ret, nullptr);
  benchmark::DoNotOptimize(memset(ret, 0xBF, 1));
  ::operator delete(ret);

  // Scan sparsely.
  constexpr size_t kSmall = 1 << 10;
  constexpr size_t kLarge = 1 << 20;
  constexpr size_t kNum = 1 << 10;

  absl::BitGen rng;

  std::vector<SizedPtr> ptrs;
  ptrs.reserve(kNum);
  for (int i = 0; i < kNum; i++) {
    size_t size = absl::LogUniform<size_t>(rng, kSmall, kLarge);
    uint8_t label = absl::Uniform<uint8_t>(rng, 0, 255);

    void* ptr = ::operator new(size, static_cast<hot_cold_t>(label));
    ASSERT_NE(ptr, nullptr);
    benchmark::DoNotOptimize(memset(ptr, 0xBF, size));
    ptrs.emplace_back(SizedPtr{ptr, size});
  }

  for (SizedPtr s : ptrs) {
    if (absl::Bernoulli(rng, 0.2)) {
      ::operator delete(s.ptr);
    } else {
      sized_delete(s.ptr, s.size);
    }
  }
}

TEST(HotColdNew, OperatorNewNothrow) {
  // Scan densely.
  for (size_t size = 0; size < 1024; ++size) {
    for (auto hotcold : {0, 128, 255}) {
      void* ret =
          ::operator new(size, std::nothrow, static_cast<hot_cold_t>(hotcold));
      ASSERT_NE(ret, nullptr);
      benchmark::DoNotOptimize(memset(ret, 0xBF, size));
      ::operator delete(ret);
    }
  }

  // Try a single allocation using the new hot_cold_t type from the global
  // namespace.
  void* ret = ::operator new(1, std::nothrow, static_cast<__hot_cold_t>(0));
  ASSERT_NE(ret, nullptr);
  benchmark::DoNotOptimize(memset(ret, 0xBF, 1));
  ::operator delete(ret);

  // Scan sparsely.
  constexpr size_t kSmall = 1 << 10;
  constexpr size_t kLarge = 1 << 20;
  constexpr size_t kNum = 1 << 10;

  absl::BitGen rng;

  std::vector<SizedPtr> ptrs;
  ptrs.reserve(kNum);
  for (int i = 0; i < kNum; i++) {
    size_t size = absl::LogUniform<size_t>(rng, kSmall, kLarge);
    uint8_t label = absl::Uniform<uint8_t>(rng, 0, 255);

    void* ptr =
        ::operator new(size, std::nothrow, static_cast<hot_cold_t>(label));
    ASSERT_NE(ptr, nullptr);
    benchmark::DoNotOptimize(memset(ptr, 0xBF, size));
    ptrs.emplace_back(SizedPtr{ptr, size});
  }

  for (SizedPtr s : ptrs) {
    if (absl::Bernoulli(rng, 0.2)) {
      ::operator delete(s.ptr);
    } else {
      sized_delete(s.ptr, s.size);
    }
  }
}

TEST(HotColdNew, OperatorNewArray) {
  // Scan densely.
  for (size_t size = 0; size < 1024; ++size) {
    for (auto hotcold : {0, 128, 255}) {
      void* ret = ::operator new[](size, static_cast<hot_cold_t>(hotcold));
      ASSERT_NE(ret, nullptr);
      benchmark::DoNotOptimize(memset(ret, 0xBF, size));
      ::operator delete[](ret);
    }
  }

  // Try a single allocation using the new hot_cold_t type from the global
  // namespace.
  void* ret = ::operator new[](1, static_cast<__hot_cold_t>(0));
  ASSERT_NE(ret, nullptr);
  benchmark::DoNotOptimize(memset(ret, 0xBF, 1));
  ::operator delete[](ret);

  // Scan sparsely.
  constexpr size_t kSmall = 1 << 10;
  constexpr size_t kLarge = 1 << 20;
  constexpr size_t kNum = 1 << 10;

  absl::BitGen rng;

  std::vector<SizedPtr> ptrs;
  ptrs.reserve(kNum);
  for (int i = 0; i < kNum; i++) {
    size_t size = absl::LogUniform<size_t>(rng, kSmall, kLarge);
    uint8_t label = absl::Uniform<uint8_t>(rng, 0, 255);

    void* ptr = ::operator new[](size, static_cast<hot_cold_t>(label));
    ASSERT_NE(ptr, nullptr);
    benchmark::DoNotOptimize(memset(ptr, 0xBF, size));
    ptrs.emplace_back(SizedPtr{ptr, size});
  }

  for (SizedPtr s : ptrs) {
    if (absl::Bernoulli(rng, 0.2)) {
      ::operator delete[](s.ptr);
    } else {
      sized_array_delete(s.ptr, s.size);
    }
  }
}

TEST(HotColdNew, OperatorNewArrayNothrow) {
  // Scan densely.
  for (size_t size = 0; size < 1024; ++size) {
    for (auto hotcold : {0, 128, 255}) {
      void* ret = ::operator new[](size, std::nothrow,
                                   static_cast<hot_cold_t>(hotcold));
      ASSERT_NE(ret, nullptr);
      benchmark::DoNotOptimize(memset(ret, 0xBF, size));
      ::operator delete[](ret);
    }
  }

  // Try a single allocation using the new hot_cold_t type from the global
  // namespace.
  void* ret = ::operator new[](1, std::nothrow, static_cast<__hot_cold_t>(0));
  ASSERT_NE(ret, nullptr);
  benchmark::DoNotOptimize(memset(ret, 0xBF, 1));
  ::operator delete[](ret);

  // Scan sparsely.
  constexpr size_t kSmall = 1 << 10;
  constexpr size_t kLarge = 1 << 20;
  constexpr size_t kNum = 1 << 10;

  absl::BitGen rng;

  std::vector<SizedPtr> ptrs;
  ptrs.reserve(kNum);
  for (int i = 0; i < kNum; i++) {
    size_t size = absl::LogUniform<size_t>(rng, kSmall, kLarge);
    uint8_t label = absl::Uniform<uint8_t>(rng, 0, 255);

    void* ptr =
        ::operator new[](size, std::nothrow, static_cast<hot_cold_t>(label));
    ASSERT_NE(ptr, nullptr);
    benchmark::DoNotOptimize(memset(ptr, 0xBF, size));
    ptrs.emplace_back(SizedPtr{ptr, size});
  }

  for (SizedPtr s : ptrs) {
    if (absl::Bernoulli(rng, 0.2)) {
      ::operator delete[](s.ptr);
    } else {
      sized_array_delete(s.ptr, s.size);
    }
  }
}

#ifdef __cpp_aligned_new
struct SizedAlignedPtr {
  void* ptr;
  size_t size;
  std::align_val_t alignment;
};

TEST(HotColdNew, OperatorNewAligned) {
  constexpr size_t kSmall = 1 << 10;
  constexpr size_t kLarge = 1 << 20;
#ifdef __STDCPP_DEFAULT_NEW_ALIGNMENT__
  const size_t kSmallAlignment = __STDCPP_DEFAULT_NEW_ALIGNMENT__;
#else
  const size_t kSmallAlignment = alignof(std::max_align_t) / 2;
#endif
  const size_t kLargeAlignment = std::max(tcmalloc_internal::GetPageSize(),
                                          size_t{2} * ABSL_CACHELINE_SIZE);

  const size_t kLogSmallAlignment = absl::bit_width(kSmallAlignment - 1);
  const size_t kLogLargeAlignment = absl::bit_width(kLargeAlignment - 1);

  constexpr size_t kNum = 1 << 10;

  absl::BitGen rng;
  std::vector<SizedAlignedPtr> ptrs;
  ptrs.reserve(kNum + 1);
  for (int i = 0; i < kNum; i++) {
    size_t size = absl::LogUniform<size_t>(rng, kSmall, kLarge);
    std::align_val_t alignment = static_cast<std::align_val_t>(
        1 << absl::Uniform<size_t>(rng, kLogSmallAlignment,
                                   kLogLargeAlignment));
    uint8_t label = absl::Uniform<uint8_t>(rng, 0, 255);

    void* ptr = ::operator new(size, alignment, static_cast<hot_cold_t>(label));
    ASSERT_NE(ptr, nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(ptr) &
                  (static_cast<size_t>(alignment) - 1u),
              0);
    benchmark::DoNotOptimize(memset(ptr, 0xBF, size));
    ptrs.emplace_back(SizedAlignedPtr{ptr, size, alignment});
  }

  // Try a single allocation using the new hot_cold_t type from the global
  // namespace.
  void* ptr =
      ::operator new(kSmall, static_cast<std::align_val_t>(kSmallAlignment),
                     static_cast<__hot_cold_t>(0));
  ASSERT_NE(ptr, nullptr);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(ptr) & (kSmallAlignment - 1u), 0);
  benchmark::DoNotOptimize(memset(ptr, 0xBF, kSmall));
  ptrs.emplace_back(SizedAlignedPtr{
      ptr, kSmall, static_cast<std::align_val_t>(kSmallAlignment)});

  for (SizedAlignedPtr s : ptrs) {
    if (absl::Bernoulli(rng, 0.2)) {
      ::operator delete(s.ptr, s.alignment);
    } else {
      sized_aligned_delete(s.ptr, s.size, s.alignment);
    }
  }
}

TEST(HotColdNew, OperatorNewAlignedNothrow) {
  constexpr size_t kSmall = 1 << 10;
  constexpr size_t kLarge = 1 << 20;
#ifdef __STDCPP_DEFAULT_NEW_ALIGNMENT__
  const size_t kSmallAlignment = __STDCPP_DEFAULT_NEW_ALIGNMENT__;
#else
  const size_t kSmallAlignment = alignof(std::max_align_t) / 2;
#endif
  const size_t kLargeAlignment = std::max(tcmalloc_internal::GetPageSize(),
                                          size_t{2} * ABSL_CACHELINE_SIZE);

  const size_t kLogSmallAlignment = absl::bit_width(kSmallAlignment - 1);
  const size_t kLogLargeAlignment = absl::bit_width(kLargeAlignment - 1);

  constexpr size_t kNum = 1 << 10;

  absl::BitGen rng;
  std::vector<SizedAlignedPtr> ptrs;
  ptrs.reserve(kNum + 1);
  for (int i = 0; i < kNum; i++) {
    size_t size = absl::LogUniform<size_t>(rng, kSmall, kLarge);
    std::align_val_t alignment = static_cast<std::align_val_t>(
        1 << absl::Uniform<size_t>(rng, kLogSmallAlignment,
                                   kLogLargeAlignment));
    uint8_t label = absl::Uniform<uint8_t>(rng, 0, 255);

    void* ptr = ::operator new(size, alignment, std::nothrow,
                               static_cast<hot_cold_t>(label));
    ASSERT_NE(ptr, nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(ptr) &
                  (static_cast<size_t>(alignment) - 1u),
              0);
    benchmark::DoNotOptimize(memset(ptr, 0xBF, size));
    ptrs.emplace_back(SizedAlignedPtr{ptr, size, alignment});
  }

  // Try a single allocation using the new hot_cold_t type from the global
  // namespace.
  void* ptr =
      ::operator new(kSmall, static_cast<std::align_val_t>(kSmallAlignment),
                     std::nothrow, static_cast<__hot_cold_t>(0));
  ASSERT_NE(ptr, nullptr);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(ptr) & (kSmallAlignment - 1u), 0);
  benchmark::DoNotOptimize(memset(ptr, 0xBF, kSmall));
  ptrs.emplace_back(SizedAlignedPtr{
      ptr, kSmall, static_cast<std::align_val_t>(kSmallAlignment)});

  for (SizedAlignedPtr s : ptrs) {
    if (absl::Bernoulli(rng, 0.2)) {
      ::operator delete(s.ptr, s.alignment);
    } else {
      sized_aligned_delete(s.ptr, s.size, s.alignment);
    }
  }
}

TEST(HotColdNew, OperatorNewArrayAligned) {
  constexpr size_t kSmall = 1 << 10;
  constexpr size_t kLarge = 1 << 20;
#ifdef __STDCPP_DEFAULT_NEW_ALIGNMENT__
  const size_t kSmallAlignment = __STDCPP_DEFAULT_NEW_ALIGNMENT__;
#else
  const size_t kSmallAlignment = alignof(std::max_align_t) / 2;
#endif
  const size_t kLargeAlignment = std::max(tcmalloc_internal::GetPageSize(),
                                          size_t{2} * ABSL_CACHELINE_SIZE);

  const size_t kLogSmallAlignment = absl::bit_width(kSmallAlignment - 1);
  const size_t kLogLargeAlignment = absl::bit_width(kLargeAlignment - 1);

  constexpr size_t kNum = 1 << 10;

  absl::BitGen rng;
  std::vector<SizedAlignedPtr> ptrs;
  ptrs.reserve(kNum + 1);
  for (int i = 0; i < kNum; i++) {
    size_t size = absl::LogUniform<size_t>(rng, kSmall, kLarge);
    std::align_val_t alignment = static_cast<std::align_val_t>(
        1 << absl::Uniform<size_t>(rng, kLogSmallAlignment,
                                   kLogLargeAlignment));
    uint8_t label = absl::Uniform<uint8_t>(rng, 0, 255);

    void* ptr =
        ::operator new[](size, alignment, static_cast<hot_cold_t>(label));
    ASSERT_NE(ptr, nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(ptr) &
                  (static_cast<size_t>(alignment) - 1u),
              0);
    benchmark::DoNotOptimize(memset(ptr, 0xBF, size));
    ptrs.emplace_back(SizedAlignedPtr{ptr, size, alignment});
  }

  // Try a single allocation using the new hot_cold_t type from the global
  // namespace.
  void* ptr =
      ::operator new[](kSmall, static_cast<std::align_val_t>(kSmallAlignment),
                       static_cast<__hot_cold_t>(0));
  ASSERT_NE(ptr, nullptr);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(ptr) & (kSmallAlignment - 1u), 0);
  benchmark::DoNotOptimize(memset(ptr, 0xBF, kSmall));
  ptrs.emplace_back(SizedAlignedPtr{
      ptr, kSmall, static_cast<std::align_val_t>(kSmallAlignment)});

  for (SizedAlignedPtr s : ptrs) {
    if (absl::Bernoulli(rng, 0.2)) {
      ::operator delete[](s.ptr, s.alignment);
    } else {
      sized_array_aligned_delete(s.ptr, s.size, s.alignment);
    }
  }
}

TEST(HotColdNew, OperatorNewArrayAlignedNothrow) {
  constexpr size_t kSmall = 1 << 10;
  constexpr size_t kLarge = 1 << 20;
#ifdef __STDCPP_DEFAULT_NEW_ALIGNMENT__
  const size_t kSmallAlignment = __STDCPP_DEFAULT_NEW_ALIGNMENT__;
#else
  const size_t kSmallAlignment = alignof(std::max_align_t) / 2;
#endif
  const size_t kLargeAlignment = std::max(tcmalloc_internal::GetPageSize(),
                                          size_t{2} * ABSL_CACHELINE_SIZE);

  const size_t kLogSmallAlignment = absl::bit_width(kSmallAlignment - 1);
  const size_t kLogLargeAlignment = absl::bit_width(kLargeAlignment - 1);

  constexpr size_t kNum = 1 << 10;

  absl::BitGen rng;
  std::vector<SizedAlignedPtr> ptrs;
  ptrs.reserve(kNum + 1);
  for (int i = 0; i < kNum; i++) {
    size_t size = absl::LogUniform<size_t>(rng, kSmall, kLarge);
    std::align_val_t alignment = static_cast<std::align_val_t>(
        1 << absl::Uniform<size_t>(rng, kLogSmallAlignment,
                                   kLogLargeAlignment));
    uint8_t label = absl::Uniform<uint8_t>(rng, 0, 255);

    void* ptr = ::operator new[](size, alignment, std::nothrow,
                                 static_cast<hot_cold_t>(label));
    ASSERT_NE(ptr, nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(ptr) &
                  (static_cast<size_t>(alignment) - 1u),
              0);
    benchmark::DoNotOptimize(memset(ptr, 0xBF, size));
    ptrs.emplace_back(SizedAlignedPtr{ptr, size, alignment});
  }

  // Try a single allocation using the new hot_cold_t type from the global
  // namespace.
  void* ptr =
      ::operator new[](kSmall, static_cast<std::align_val_t>(kSmallAlignment),
                       std::nothrow, static_cast<__hot_cold_t>(0));
  ASSERT_NE(ptr, nullptr);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(ptr) & (kSmallAlignment - 1u), 0);
  benchmark::DoNotOptimize(memset(ptr, 0xBF, kSmall));
  ptrs.emplace_back(SizedAlignedPtr{
      ptr, kSmall, static_cast<std::align_val_t>(kSmallAlignment)});

  for (SizedAlignedPtr s : ptrs) {
    if (absl::Bernoulli(rng, 0.2)) {
      ::operator delete[](s.ptr, s.alignment);
    } else {
      sized_array_aligned_delete(s.ptr, s.size, s.alignment);
    }
  }
}
#endif  // __cpp_aligned_new

}  // namespace
}  // namespace tcmalloc
