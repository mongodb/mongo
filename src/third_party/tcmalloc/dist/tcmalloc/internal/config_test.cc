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

#include "tcmalloc/internal/config.h"

#include <fcntl.h>

#include <algorithm>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "google/protobuf/io/gzip_stream.h"
#include "google/protobuf/io/zero_copy_stream_impl.h"
#include "tcmalloc/internal/util.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

TEST(AddressBits, CpuVirtualBits) {
  // Check that kAddressBits is as least as large as either the number of bits
  // in a pointer or as the number of virtual bits handled by the processor.
  // To be effective this test must be run on each processor model.
#ifdef __x86_64__
  const int kPointerBits = 8 * sizeof(void*);

  // LLVM has a miscompile bug around %rbx, see
  // https://bugs.llvm.org/show_bug.cgi?id=17907
  int ret;
  asm("mov %%rbx, %%rdi\n"
      "cpuid\n"
      "xchg %%rdi, %%rbx\n"
      /* inputs */
      : "=a"(ret)
      /* outputs */
      : "a"(0x80000008)
      /* clobbers */
      : "rdi", "ecx", "edx");
  const int kImplementedVirtualBits = (ret >> 8) & ((1 << 8) - 1);
  ASSERT_GE(kAddressBits, std::min(kImplementedVirtualBits, kPointerBits));
#elif __aarch64__
  const int kPointerBits = 8 * sizeof(void*);

  int fd = signal_safe_open("/proc/config.gz", O_RDONLY);
  if (fd < 0) {
    GTEST_SKIP() << "Unable to open kernel config.";
  }

  google::protobuf::io::FileInputStream fs(fd);
  google::protobuf::io::GzipInputStream gs(&fs, google::protobuf::io::GzipInputStream::GZIP);

  std::string config;
  do {
    const void* buf;
    int size;
    if (!gs.Next(&buf, &size)) {
      break;
    }
    if (size < 0) {
      break;
    }

    absl::StrAppend(
        &config, absl::string_view(reinterpret_cast<const char*>(buf), size));
  } while (true);

  constexpr absl::string_view token = "CONFIG_PGTABLE_LEVELS=";
  ASSERT_THAT(config, testing::HasSubstr(token));
  auto position = config.find(token);
  ASSERT_NE(position, std::string::npos);
  position += token.size();
  auto eol = config.find('\n', position);
  ASSERT_NE(eol, std::string::npos);
  ASSERT_NE(eol, position);
  absl::string_view string_levels(&config[position], eol - position);
  int levels;
  ASSERT_TRUE(absl::SimpleAtoi(string_levels, &levels)) << string_levels;

  ASSERT_GE(levels, 3);
  const int kImplementedVirtualBits = 39 + (levels - 3) * 9;
  ASSERT_EQ(kAddressBits, kImplementedVirtualBits);
  ASSERT_GE(kAddressBits, std::min(kImplementedVirtualBits, kPointerBits));
#endif
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
