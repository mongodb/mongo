// Copyright 2024 The Centipede Authors.
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

#include "./centipede/resource_pool.h"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include "gtest/gtest.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "./centipede/rusage_stats.h"
#include "./centipede/thread_pool.h"
#include "./common/logging.h"

namespace fuzztest::internal {
namespace {

constexpr RUsageMemory MakeMemRss(MemSize mem_rss) {
  return RUsageMemory{/*mem_vsize=*/0, /*mem_vpeak=*/0, mem_rss};
}

TEST(ResourcePoolTest, InvalidLeaseRequests) {
  constexpr RUsageMemory kQuota = MakeMemRss(1000);
  constexpr RUsageMemory kZero = MakeMemRss(0);
  constexpr RUsageMemory kEpsilon = MakeMemRss(1);
  ResourcePool pool{kQuota};
  {
    ResourcePool<RUsageMemory>::LeaseRequest request;
    request.amount = kZero;
    const auto lease = pool.AcquireLeaseBlocking(std::move(request));
    EXPECT_EQ(lease.status().code(), absl::StatusCode::kInvalidArgument)
        << VV(lease.status());
  }
  {
    ResourcePool<RUsageMemory>::LeaseRequest request;
    request.amount = kQuota - kEpsilon;
    const auto lease = pool.AcquireLeaseBlocking(std::move(request));
    EXPECT_EQ(lease.status().code(), absl::StatusCode::kOk)
        << VV(lease.status());
  }
  {
    ResourcePool<RUsageMemory>::LeaseRequest request;
    request.amount = kQuota;
    const auto lease = pool.AcquireLeaseBlocking(std::move(request));
    EXPECT_EQ(lease.status().code(), absl::StatusCode::kOk)
        << VV(lease.status());
  }
  {
    ResourcePool<RUsageMemory>::LeaseRequest request;
    request.amount = kQuota + kEpsilon;
    const auto lease = pool.AcquireLeaseBlocking(std::move(request));
    EXPECT_EQ(lease.status().code(), absl::StatusCode::kResourceExhausted)
        << VV(lease.status());
  }
}

TEST(ResourcePoolTest, Dynamic) {
  struct TaskSpec {
    std::string_view id;
    RUsageMemory ram_chunk;
    // The times are relative to time zero, when all the tasks roughly start.
    int request_at_secs;
    int timeout_at_secs;
    int release_at_secs;
    absl::StatusCode expected_lease_status;
  };

  constexpr RUsageMemory kRssQuota = MakeMemRss(5);
  constexpr int kNumTasks = 9;
  constexpr std::array<TaskSpec, kNumTasks> kTaskSpecs = {{
      // Can't request 0 amount.
      {"0", /*ram_chunk=*/MakeMemRss(0), 0, 1, 3,
       absl::StatusCode::kInvalidArgument},
      // Exceeds the initial pool capacity.
      {"1", /*ram_chunk=*/MakeMemRss(10), 0, 1, 3,
       absl::StatusCode::kResourceExhausted},
      // "2" gets the resource first.
      {"2", /*ram_chunk=*/MakeMemRss(2), 0, 0, 2, absl::StatusCode::kOk},
      // "1" gets the resource immediately after "2" and runs concurrently.
      {"3", /*ram_chunk=*/MakeMemRss(2), 0, 0, 4, absl::StatusCode::kOk},
      // "4" can't get the resource right away - 1 sec later than "2" and "3" -
      // because they almost exhaust the pool; but it waits long enough for "2"
      // to finish (while "3" is still running) and free up enough of the pool;
      // then "4" gets the resource and runs fine.
      {"4", /*ram_chunk=*/MakeMemRss(1), 1, 3, 4, absl::StatusCode::kOk},
      // "5" starts while "2" and "3", and later on "3" and "4", are still
      // running. They all continuously hold enough of the pool to prevent "5"
      // from ever getting its resource. Eventually, "5" runs out of time.
      {"5", /*ram_chunk=*/MakeMemRss(4), 2, 3, 5,
       absl::StatusCode::kDeadlineExceeded},
      // "6" is like "5", but it waits long enough for "3" and "4" to free up
      // the pool; then "6" gets the resource and runs fine.
      {"6", /*ram_chunk=*/MakeMemRss(4), 2, 5, 6, absl::StatusCode::kOk},
      // "7" is also like "5", but is less greedy, so although it starts 1 sec
      // later, it is allowed in front of "5" and "6" and runs fine, partially
      // sharing the pool with "3" and "4".
      {"7", /*ram_chunk=*/MakeMemRss(1), 3, 3, 5, absl::StatusCode::kOk},
      // "8" starts waiting for the maximum available amount when other
      // consumers already use some of the pool. It waits long enough for all of
      // them to finish, then finally grabs the entire quota and runs.
      {"8", /*ram_chunk=*/MakeMemRss(5), 2, 9, 10, absl::StatusCode::kOk},
  }};
  std::array<absl::Status, kNumTasks> task_lease_statuses;

  {
    ResourcePool pool{kRssQuota};
    ThreadPool threads{kNumTasks};
    for (size_t i = 0; i < kNumTasks; ++i) {
      const auto& t = kTaskSpecs[i];
      auto& lease_status = task_lease_statuses[i];
      threads.Schedule([&t, &pool, &lease_status]() {
        // All the tasks start roughly at the same time (because there are just
        // as many threads, and scheduling is fast), so they are on roughly the
        // same relative timetable.
        absl::SleepFor(absl::Seconds(t.request_at_secs));
        ResourcePool<RUsageMemory>::LeaseRequest request;
        request.id = std::string(t.id);
        request.amount = t.ram_chunk;
        request.timeout = absl::Seconds(t.timeout_at_secs - t.request_at_secs);
        const auto lease = pool.AcquireLeaseBlocking(std::move(request));
        lease_status = lease.status();
        if (lease_status.ok()) {
          absl::SleepFor(absl::Seconds(t.release_at_secs - t.request_at_secs));
        }
      });
    }
  }  // Threads join here.

  for (size_t i = 0; i < kNumTasks; ++i) {
    const auto& task = kTaskSpecs[i];
    auto& lease_status = task_lease_statuses[i];
    EXPECT_EQ(lease_status.code(), task.expected_lease_status)
        << VV(task.id) << VV(lease_status);
  }
}

}  // namespace
}  // namespace fuzztest::internal
