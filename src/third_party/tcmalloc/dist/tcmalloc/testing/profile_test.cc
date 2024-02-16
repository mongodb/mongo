// Copyright 2021 The TCMalloc Authors
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

#include <stddef.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "tcmalloc/internal/profile.pb.h"
#include "gtest/gtest.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "google/protobuf/io/coded_stream.h"
#include "google/protobuf/io/gzip_stream.h"
#include "google/protobuf/io/zero_copy_stream_impl_lite.h"
#include "tcmalloc/internal/profile_builder.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/profile_marshaler.h"
#include "tcmalloc/testing/testutil.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

TEST(ProfileTest, HeapProfile) {
#if ABSL_HAVE_ADDRESS_SANITIZER || ABSL_HAVE_MEMORY_SANITIZER || \
    ABSL_HAVE_THREAD_SANITIZER
  GTEST_SKIP() << "Skipping heap profile test under sanitizers.";
#endif

  constexpr int64_t kSamplingRate = 1024 * 1024;
  ScopedProfileSamplingRate s(kSamplingRate);

  // Tweak alloc_size to make it more likely we can distinguish it from others.
  constexpr int kAllocs = 32;
  const size_t alloc_size = 64 * kSamplingRate + 123;

  auto deleter = [](void* ptr) { ::operator delete(ptr); };
  std::vector<std::unique_ptr<void, decltype(deleter)>> allocs;
  for (int i = 0; i < kAllocs; i++) {
    allocs.emplace_back(::operator new(alloc_size), deleter);
    allocs.emplace_back(tcmalloc_size_returning_operator_new(alloc_size).p,
                        deleter);
  }

  // Grab profile, encode, then decode to look for the allocations.
  Profile profile = MallocExtension::SnapshotCurrent(ProfileType::kHeap);
  absl::StatusOr<std::string> encoded_or = Marshal(profile);
  ASSERT_TRUE(encoded_or.ok());

  const absl::string_view encoded = *encoded_or;

  google::protobuf::io::ArrayInputStream stream(encoded.data(), encoded.size());
  google::protobuf::io::GzipInputStream gzip_stream(&stream);
  google::protobuf::io::CodedInputStream coded(&gzip_stream);

  perftools::profiles::Profile converted;
  ASSERT_TRUE(converted.ParseFromCodedStream(&coded));

  // Look for "request" and "size_returning" string in string table.
  std::optional<int> request_id, size_returning_id;
  for (int i = 0, n = converted.string_table().size(); i < n; ++i) {
    if (converted.string_table(i) == "request") {
      request_id = i;
    } else if (converted.string_table(i) == "size_returning") {
      size_returning_id = i;
    }
  }

  EXPECT_TRUE(request_id.has_value());
  EXPECT_TRUE(size_returning_id.has_value());

  size_t count = 0, bytes = 0, samples = 0, size_returning_samples = 0;
  for (const auto& sample : converted.sample()) {
    count += sample.value(0);
    bytes += sample.value(1);

    // Count the number of times we saw an alloc_size-sized allocation.
    bool alloc_sized = false;
    for (const auto& label : sample.label()) {
      if (label.key() == request_id && label.num() == alloc_size) {
        alloc_sized = true;
        samples++;
      }
    }

    if (alloc_sized) {
      for (const auto& label : sample.label()) {
        if (label.key() == size_returning_id && label.num() > 0) {
          size_returning_samples++;
        }
      }
    }
  }

  EXPECT_GT(count, 0);
  EXPECT_GE(bytes, 2 * alloc_size * kAllocs);
  // To minimize the size of profiles, we expect to coalesce similar allocations
  // (same call stack, size, alignment, etc.) during generation of the
  // profile.proto.  Since all of the calls to operator new(alloc_size) are
  // similar in these dimensions, we expect to see only 2 samples, one for
  // ::operator new and one for tcmalloc_size_returning_operator_new.
#ifndef __aarch64__
  EXPECT_EQ(samples, 2);
  EXPECT_EQ(size_returning_samples, 1);
#else
  // TODO(b/246562683):  We can see two distinct size-returning new callstacks
  // on AArch64.
  EXPECT_GE(samples, 2);
  EXPECT_GE(size_returning_samples, 1);
#endif

  absl::flat_hash_set<int> mapping_ids;
  mapping_ids.reserve(converted.mapping().size());
  for (const auto& mapping : converted.mapping()) {
    ASSERT_TRUE(mapping_ids.insert(mapping.id()).second);
  }

  // Every location should have a mapping.
  int mappings_seen = 0;
  for (const auto& location : converted.location()) {
    const int mapping_id = location.mapping_id();
    EXPECT_NE(mapping_id, 0);
    mappings_seen += mapping_ids.contains(mapping_id);
  }
  EXPECT_EQ(mappings_seen, converted.location().size());
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
