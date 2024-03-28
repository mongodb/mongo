// Copyright 2022 The TCMalloc Authors
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

// Extra extensions exported by some malloc implementations.  These
// extensions are accessed through a virtual base class so an
// application can link against a malloc that does not implement these
// extensions, and it will get default versions that do nothing.

#include "tcmalloc/malloc_tracing_extension.h"

#include <stddef.h>


#include "gtest/gtest.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"

#ifndef MALLOC_TRACING_EXTENSION_NOT_SUPPORTED
#include "gmock/gmock.h"
#include "absl/cleanup/cleanup.h"
#endif

namespace {

#ifdef MALLOC_TRACING_EXTENSION_NOT_SUPPORTED
TEST(MallocTracingExtension, GetAllocatedAddressRanges) {
  // Act.
  absl::StatusOr<tcmalloc::malloc_tracing_extension::AllocatedAddressRanges>
      allocated =
          tcmalloc::malloc_tracing_extension::GetAllocatedAddressRanges();

  // Assert.
  ASSERT_FALSE(allocated.ok());
  EXPECT_EQ(allocated.status().code(), absl::StatusCode::kUnimplemented);
}
#else

using ::tcmalloc::malloc_tracing_extension::AllocatedAddressRanges;
using ::testing::AllOf;
using ::testing::Each;
using ::testing::Field;
using ::testing::Gt;

bool InRange(const AllocatedAddressRanges::SpanDetails& span, void* obj_start,
             size_t obj_size) {
  return span.start_addr <= (uintptr_t)obj_start &&
         ((uintptr_t)obj_start + obj_size) <= (span.start_addr + span.size);
}

std::optional<AllocatedAddressRanges::SpanDetails> GetSpanDetailsForObject(
    const AllocatedAddressRanges& allocated, void* obj_start, size_t obj_size) {
  for (const AllocatedAddressRanges::SpanDetails& span : allocated.spans) {
    if (InRange(span, obj_start, obj_size)) return span;
  }
  return std::nullopt;
}

TEST(MallocTracingExtension, GetAllocatedAddressRanges) {
  // Arrange.
  const int kArrCount = 3;
  size_t size[] = {2, 1000, 1000000};
  void* arr[kArrCount];
  for (int i = 0; i < kArrCount; i++) {
    arr[i] = ::operator new(size[i]);
  }
  absl::Cleanup cleanup = [arr] {
    for (int i = 0; i < kArrCount; i++) {
      ::operator delete(arr[i]);
    }
  };

  // Act.
  absl::StatusOr<tcmalloc::malloc_tracing_extension::AllocatedAddressRanges>
      allocated =
          tcmalloc::malloc_tracing_extension::GetAllocatedAddressRanges();

  // Assert.
  ASSERT_TRUE(allocated.ok());
  EXPECT_GT(allocated.value().spans.size(), 0);
  EXPECT_THAT(
      allocated.value().spans,
      Each(AllOf(
          Field("start_addr", &AllocatedAddressRanges::SpanDetails::start_addr,
                Gt(0)),
          Field("size", &AllocatedAddressRanges::SpanDetails::size, Gt(0)))));
  for (int i = 0; i < kArrCount; i++) {
    std::optional<AllocatedAddressRanges::SpanDetails> span =
        GetSpanDetailsForObject(allocated.value(), arr[i], size[i]);
    ASSERT_TRUE(span.has_value())
        << " for the " << size[i] << "-byte object at index " << i;
    // Non-sizeclass objects that are larger than kMaxSize will have
    // object_size == 0.
    if (span.value().object_size != 0) {
      EXPECT_GE(span.value().object_size, size[i])
          << " for the " << size[i] << "-byte object at index " << i;
    }
  }
}
#endif

}  // namespace
