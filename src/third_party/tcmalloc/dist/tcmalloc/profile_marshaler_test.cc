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

#include "tcmalloc/profile_marshaler.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "tcmalloc/internal/profile.pb.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/memory/memory.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "google/protobuf/io/coded_stream.h"
#include "google/protobuf/io/gzip_stream.h"
#include "google/protobuf/io/zero_copy_stream_impl_lite.h"
#include "tcmalloc/internal/fake_profile.h"
#include "tcmalloc/internal_malloc_extension.h"
#include "tcmalloc/malloc_extension.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

TEST(ProfileMarshalTest, Smoke) {
  constexpr absl::Duration kDuration = absl::Milliseconds(1500);

  auto fake_profile = std::make_unique<FakeProfile>();
  fake_profile->SetType(ProfileType::kAllocations);
  fake_profile->SetDuration(kDuration);

  std::vector<Profile::Sample> samples;
  {
    auto& sample = samples.emplace_back();

    sample.sum = 1234;
    sample.count = 2;
  }
  fake_profile->SetSamples(std::move(samples));

  Profile profile =
      tcmalloc_internal::ProfileAccessor::MakeProfile(std::move(fake_profile));

  absl::StatusOr<std::string> encoded_or = Marshal(profile);
  ASSERT_TRUE(encoded_or.ok());

  const absl::string_view encoded = *encoded_or;

  google::protobuf::io::ArrayInputStream stream(encoded.data(), encoded.size());
  google::protobuf::io::GzipInputStream gzip_stream(&stream);
  google::protobuf::io::CodedInputStream coded_stream(&gzip_stream);

  perftools::profiles::Profile converted;
  ASSERT_TRUE(converted.ParseFromCodedStream(&coded_stream));

  // The conversion of tcmalloc::Profile to perftools::profiles::Profile is more
  // extensively tested in tcmalloc/testing/profile_test.cc.  We do
  // limited tests here to verify the proto likely roundtripped correctly.

  EXPECT_EQ(converted.period(), 0);  // Not set
  EXPECT_EQ(converted.string_table(converted.period_type().type()), "space");
  EXPECT_EQ(converted.string_table(converted.period_type().unit()), "bytes");
  EXPECT_THAT(converted.string_table(converted.drop_frames()),
              testing::HasSubstr("TCMallocInternalNew"));
  EXPECT_EQ(converted.duration_nanos(), absl::ToInt64Nanoseconds(kDuration));
  EXPECT_EQ(converted.string_table(converted.default_sample_type()), "objects");
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
