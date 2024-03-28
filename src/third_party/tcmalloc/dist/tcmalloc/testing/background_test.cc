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

#include <stddef.h>
#include <sys/types.h>

#include <atomic>
#include <thread>

#include "gtest/gtest.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/testing/test_allocator_harness.h"
#include "tcmalloc/testing/testutil.h"
#include "tcmalloc/testing/thread_manager.h"

namespace tcmalloc {
namespace {

TEST(BackgroundTest, Defaults) {
  EXPECT_TRUE(MallocExtension::GetBackgroundProcessActionsEnabled());
  EXPECT_EQ(MallocExtension::GetBackgroundProcessSleepInterval(),
            absl::Seconds(1));
}

TEST(BackgroundTest, Stress) {
  // Process background actions by setting a custom sleep interval.
  struct ProcessActions {
    static void Go() {
      constexpr absl::Duration kSleepTime = absl::Milliseconds(10);
      ScopedBackgroundProcessSleepInterval sleep_time(kSleepTime);
      MallocExtension::ProcessBackgroundActions();
    }
  };

  // Make sure that background acions are indeed enabled.
  EXPECT_TRUE(MallocExtension::GetBackgroundProcessActionsEnabled());

  std::thread background(ProcessActions::Go);

  constexpr int kThreads = 10;
  ThreadManager mgr;
  AllocatorHarness harness(kThreads);

  mgr.Start(kThreads, [&](int thread_id) { harness.Run(thread_id); });

  absl::SleepFor(absl::Seconds(5));

  mgr.Stop();

  ScopedBackgroundProcessActionsEnabled background_process_enabled(
      /*value=*/false);
  background.join();
}

}  // namespace
}  // namespace tcmalloc

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
