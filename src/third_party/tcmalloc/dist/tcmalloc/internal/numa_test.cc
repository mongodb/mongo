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

#include "tcmalloc/internal/numa.h"

#include <errno.h>
#include <linux/memfd.h>
#include <sched.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <syscall.h>
#include <unistd.h>

#include <algorithm>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/random/random.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/string_view.h"
#include "absl/types/optional.h"
#include "absl/types/span.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/percpu.h"
#include "tcmalloc/internal/sysinfo.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

int memfd_create(const char* name, unsigned int flags) {
#ifdef __NR_memfd_create
  return syscall(__NR_memfd_create, name, flags);
#else
  errno = ENOSYS;
  return -1;
#endif
}

// A synthetic cpulist that can be read from a file descriptor.
class SyntheticCpuList {
 public:
  explicit SyntheticCpuList(const absl::string_view content) {
    fd_ = memfd_create("cpulist", MFD_CLOEXEC);
    TC_CHECK_NE(fd_, -1);

    TC_CHECK_EQ(write(fd_, content.data(), content.size()), content.size());
    TC_CHECK_EQ(write(fd_, "\n", 1), 1);
    TC_CHECK_EQ(lseek(fd_, 0, SEEK_SET), 0);
  }

  ~SyntheticCpuList() { close(fd_); }

  // Disallow copies, which would make require reference counting to know when
  // we should close fd_.
  SyntheticCpuList(const SyntheticCpuList&) = delete;
  SyntheticCpuList& operator=(const SyntheticCpuList&) = delete;

  // Moves are fine - only one instance at a time holds the fd.
  SyntheticCpuList(SyntheticCpuList&& other)
      : fd_(std::exchange(other.fd_, -1)) {}
  SyntheticCpuList& operator=(SyntheticCpuList&& other) {
    new (this) SyntheticCpuList(std::move(other));
    return *this;
  }

  int fd() const { return fd_; }

 private:
  // The underlying memfd.
  int fd_;
};

class NumaTopologyTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // We use memfd to create synthetic cpulist files, and can't run without
    // it. Skip all affected tests if memfd is not supported (i.e. Linux <
    // 3.17).
    const int fd = memfd_create("test", MFD_CLOEXEC);
    if (fd == -1 && errno == ENOSYS) {
      GTEST_SKIP() << "Test requires memfd support";
    }
    close(fd);

    // If rseq is unavailable the NumaTopology never enables NUMA awareness.
    if (!subtle::percpu::IsFast()) {
      GTEST_SKIP() << "Test requires rseq support";
    }
  }
};

template <size_t NumPartitions, size_t ScaleBy = 1>
NumaTopology<NumPartitions, ScaleBy> CreateNumaTopology(
    const absl::Span<const SyntheticCpuList> cpu_lists) {
  NumaTopology<NumPartitions, ScaleBy> nt;
  nt.InitForTest([&](const size_t node) {
    if (node >= cpu_lists.size()) {
      errno = ENOENT;
      return -1;
    }
    return cpu_lists[node].fd();
  });
  return nt;
}

// Ensure that if we set NumPartitions=1 then NUMA awareness is disabled even
// in the presence of a system with multiple NUMA nodes.
TEST_F(NumaTopologyTest, NoCompileTimeNuma) {
  std::vector<SyntheticCpuList> nodes;
  nodes.emplace_back("0");
  nodes.emplace_back("1");

  const auto nt = CreateNumaTopology<1>(nodes);

  EXPECT_EQ(nt.numa_aware(), false);
  EXPECT_EQ(nt.active_partitions(), 1);
  EXPECT_EQ(nt.GetCurrentPartition(), 0);
}

// Ensure that if we run on a system with no NUMA support at all (i.e. no
// /sys/devices/system/node/nodeX/cpulist files) we correctly disable NUMA
// awareness.
TEST_F(NumaTopologyTest, NoRunTimeNuma) {
  const auto nt = CreateNumaTopology<2>({});

  EXPECT_EQ(nt.numa_aware(), false);
  EXPECT_EQ(nt.active_partitions(), 1);
  EXPECT_EQ(nt.GetCurrentPartition(), 0);
}

// Ensure that if we run on a system with only 1 node then we disable NUMA
// awareness.
TEST_F(NumaTopologyTest, SingleNode) {
  std::vector<SyntheticCpuList> nodes;
  nodes.emplace_back("0-27");

  const auto nt = CreateNumaTopology<4>(nodes);

  EXPECT_EQ(nt.numa_aware(), false);
  EXPECT_EQ(nt.active_partitions(), 1);
}

// Basic sanity test modelling a simple 2 node system.
TEST_F(NumaTopologyTest, TwoNode) {
  std::vector<SyntheticCpuList> nodes;
  nodes.emplace_back("0-5");
  nodes.emplace_back("6-11");

  const auto nt = CreateNumaTopology<2>(nodes);

  EXPECT_EQ(nt.numa_aware(), true);
  EXPECT_EQ(nt.active_partitions(), 2);

  for (int cpu = 0; cpu <= 5; cpu++) {
    EXPECT_EQ(nt.GetCpuPartition(cpu), 0);
  }
  for (int cpu = 6; cpu <= 11; cpu++) {
    EXPECT_EQ(nt.GetCpuPartition(cpu), 1);
  }
}

// Confirm that an empty node parses correctly (b/212827142).
TEST_F(NumaTopologyTest, EmptyNode) {
  std::vector<SyntheticCpuList> nodes;
  nodes.emplace_back("0-5");
  nodes.emplace_back("");
  nodes.emplace_back("6-11");

  const auto nt = CreateNumaTopology<3>(nodes);

  EXPECT_EQ(nt.numa_aware(), true);
  EXPECT_EQ(nt.active_partitions(), 3);

  for (int cpu = 0; cpu <= 5; cpu++) {
    EXPECT_EQ(nt.GetCpuPartition(cpu), 0);
  }
  for (int cpu = 6; cpu <= 11; cpu++) {
    EXPECT_EQ(nt.GetCpuPartition(cpu), 2);
  }
}

TEST_F(NumaTopologyTest, IsLocalToCpuPartition) {
  std::vector<SyntheticCpuList> nodes;
  nodes.emplace_back("0-1");
  nodes.emplace_back("2-3");

  const auto nt = CreateNumaTopology<2, 2>(nodes);

  EXPECT_EQ(nt.numa_aware(), true);
  EXPECT_TRUE(nt.IsLocalToCpuPartition(/*size_class=*/0, /*cpu=*/0));
  EXPECT_TRUE(nt.IsLocalToCpuPartition(/*size_class=*/2, /*cpu=*/2));
  EXPECT_FALSE(nt.IsLocalToCpuPartition(/*size_class=*/0, /*cpu=*/2));
  EXPECT_FALSE(nt.IsLocalToCpuPartition(/*size_class=*/2, /*cpu=*/0));
}

// Test that cpulists too long to fit into the 16 byte buffer used by
// InitNumaTopology() parse successfully.
TEST_F(NumaTopologyTest, LongCpuLists) {
  std::vector<SyntheticCpuList> nodes;

  // Content from here onwards lies   |
  // beyond the 16 byte buffer.       |
  //                                  v
  nodes.emplace_back("0-1,2-3,4-5,6-7,8");        // Right after a comma
  nodes.emplace_back("9,10,11,12,13,14,15-19");   // Right before a comma
  nodes.emplace_back("20-21,22-23,24-25,26-29");  // Within range end
  nodes.emplace_back("30-32,33,34,35,36-38,39");  // Within range start
  nodes.emplace_back("40-43,44,45-49");

  const auto nt = CreateNumaTopology<3>(nodes);

  EXPECT_EQ(nt.numa_aware(), true);
  EXPECT_EQ(nt.active_partitions(), 3);

  for (int cpu = 0; cpu <= 8; cpu++) {
    EXPECT_EQ(nt.GetCpuPartition(cpu), 0);
  }
  for (int cpu = 9; cpu <= 19; cpu++) {
    EXPECT_EQ(nt.GetCpuPartition(cpu), 1);
  }
  for (int cpu = 20; cpu <= 29; cpu++) {
    EXPECT_EQ(nt.GetCpuPartition(cpu), 2);
  }
  for (int cpu = 30; cpu <= 39; cpu++) {
    EXPECT_EQ(nt.GetCpuPartition(cpu), 0);
  }
  for (int cpu = 40; cpu <= 49; cpu++) {
    EXPECT_EQ(nt.GetCpuPartition(cpu), 1);
  }
}

// Ensure we can initialize using the host system's real NUMA topology
// information.
TEST_F(NumaTopologyTest, Host) {
  NumaTopology<4> nt;
  nt.Init();

  const size_t active_partitions = nt.active_partitions();

  // We don't actually know anything about the host, so there's not much more
  // we can do beyond checking that we didn't crash.
  for (int cpu = 0, n = NumCPUs(); cpu < n; ++cpu) {
    size_t partition = nt.GetCpuPartition(cpu);
    EXPECT_LT(partition, active_partitions) << cpu;
  }
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
