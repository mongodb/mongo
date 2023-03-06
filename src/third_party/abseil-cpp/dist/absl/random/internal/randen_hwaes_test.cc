// Copyright 2017 The Abseil Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "absl/random/internal/randen_hwaes.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/base/internal/raw_logging.h"
#include "absl/random/internal/platform.h"
#include "absl/random/internal/randen_detect.h"
#include "absl/random/internal/randen_traits.h"
#include "absl/strings/str_format.h"

namespace {

using absl::random_internal::RandenHwAes;
using absl::random_internal::RandenTraits;

// Local state parameters.
constexpr size_t kSeedBytes =
    RandenTraits::kStateBytes - RandenTraits::kCapacityBytes;
constexpr size_t kStateSizeT = RandenTraits::kStateBytes / sizeof(uint64_t);
constexpr size_t kSeedSizeT = kSeedBytes / sizeof(uint32_t);

struct alignas(16) randen {
  uint64_t state[kStateSizeT];
  uint32_t seed[kSeedSizeT];
};

TEST(RandenHwAesTest, Default) {
  EXPECT_TRUE(absl::random_internal::CPUSupportsRandenHwAes());

  constexpr uint64_t kGolden[] = {
      0x6c6534090ee6d3ee, 0x044e2b9b9d5333c6, 0xc3c14f134e433977,
      0xdda9f47cd90410ee, 0x887bf3087fd8ca10, 0xf0b780f545c72912,
      0x15dbb1d37696599f, 0x30ec63baff3c6d59, 0xb29f73606f7f20a6,
      0x02808a316f49a54c, 0x3b8feaf9d5c8e50e, 0x9cbf605e3fd9de8a,
      0xc970ae1a78183bbb, 0xd8b2ffd356301ed5, 0xf4b327fe0fc73c37,
      0xcdfd8d76eb8f9a19, 0xc3a506eb91420c9d, 0xd5af05dd3eff9556,
      0x48db1bb78f83c4a1, 0x7023920e0d6bfe8c, 0x58d3575834956d42,
      0xed1ef4c26b87b840, 0x8eef32a23e0b2df3, 0x497cabf3431154fc,
      0x4e24370570029a8b, 0xd88b5749f090e5ea, 0xc651a582a970692f,
      0x78fcec2cbb6342f5, 0x463cb745612f55db, 0x352ee4ad1816afe3,
      0x026ff374c101da7e, 0x811ef0821c3de851,
  };

  alignas(16) randen d;
  memset(d.state, 0, sizeof(d.state));
  RandenHwAes::Generate(RandenHwAes::GetKeys(), d.state);

  uint64_t* id = d.state;
  for (const auto& elem : kGolden) {
    auto a = absl::StrFormat("%#x", elem);
    auto b = absl::StrFormat("%#x", *id++);
    EXPECT_EQ(a, b);
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  testing::InitGoogleTest(&argc, argv);

  ABSL_RAW_LOG(INFO, "ABSL_HAVE_ACCELERATED_AES=%d", ABSL_HAVE_ACCELERATED_AES);
  ABSL_RAW_LOG(INFO, "ABSL_RANDOM_INTERNAL_AES_DISPATCH=%d",
               ABSL_RANDOM_INTERNAL_AES_DISPATCH);

#if defined(ABSL_ARCH_X86_64)
  ABSL_RAW_LOG(INFO, "ABSL_ARCH_X86_64");
#elif defined(ABSL_ARCH_X86_32)
  ABSL_RAW_LOG(INFO, "ABSL_ARCH_X86_32");
#elif defined(ABSL_ARCH_AARCH64)
  ABSL_RAW_LOG(INFO, "ABSL_ARCH_AARCH64");
#elif defined(ABSL_ARCH_ARM)
  ABSL_RAW_LOG(INFO, "ABSL_ARCH_ARM");
#elif defined(ABSL_ARCH_PPC)
  ABSL_RAW_LOG(INFO, "ABSL_ARCH_PPC");
#else
  ABSL_RAW_LOG(INFO, "ARCH Unknown");
#endif

  int x = absl::random_internal::HasRandenHwAesImplementation();
  ABSL_RAW_LOG(INFO, "HasRandenHwAesImplementation = %d", x);

  int y = absl::random_internal::CPUSupportsRandenHwAes();
  ABSL_RAW_LOG(INFO, "CPUSupportsRandenHwAes = %d", x);

  if (!x || !y) {
    ABSL_RAW_LOG(INFO, "Skipping Randen HWAES tests.");
    return 0;
  }
  return RUN_ALL_TESTS();
}
