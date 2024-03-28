// Copyright 2018 The Abseil Authors.
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

#include "tcmalloc/internal/sampled_allocation_recorder.h"

#include <assert.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/base/thread_annotations.h"
#include "absl/synchronization/notification.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "tcmalloc/internal/explicitly_constructed.h"
#include "tcmalloc/testing/thread_manager.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {
using ::testing::IsEmpty;
using ::testing::UnorderedElementsAre;

struct Info : public Sample<Info> {
 public:
  Info() { PrepareForSampling(); }
  void PrepareForSampling() ABSL_EXCLUSIVE_LOCKS_REQUIRED(lock) {
    initialized = true;
  }
  std::atomic<size_t> size;
  absl::Time create_time;
  bool initialized;
};

class TestAllocator {
 public:
  static Info* New() {
    alloc_count_.fetch_add(1, std::memory_order_relaxed);
    return new Info;
  }
  static void Delete(Info* info) {
    alloc_count_.fetch_sub(1, std::memory_order_relaxed);
    delete info;
  }
  static uint64_t alloc_count() {
    return alloc_count_.load(std::memory_order_relaxed);
  }
  inline static std::atomic<uint64_t> alloc_count_{0};
};

class SampleRecorderTest : public ::testing::Test {
 public:
  SampleRecorderTest() : sample_recorder_(&allocator_) {
    sample_recorder_.Init();
  }

  std::vector<size_t> GetSizes() {
    std::vector<size_t> res;
    // Reserve to avoid reentrant allocations while iterating.
    res.reserve(5);
    sample_recorder_.Iterate([&](const Info& info) {
      res.push_back(info.size.load(std::memory_order_acquire));
    });
    return res;
  }

  Info* Register(size_t size) {
    auto* info = sample_recorder_.Register();
    assert(info != nullptr);
    info->size.store(size);
    return info;
  }

  TestAllocator allocator_;
  SampleRecorder<Info, TestAllocator> sample_recorder_;
};

// In static_vars.cc, we use
// tcmalloc/internal/explicitly_constructed.h to set up the sample
// recorder. Have a test here to verify that it is properly initialized and
// functional through this approach.
TEST_F(SampleRecorderTest, ExplicitlyConstructed) {
  ExplicitlyConstructed<SampleRecorder<Info, TestAllocator>>
      sample_recorder_helper;
  sample_recorder_helper.Construct(&allocator_);
  SampleRecorder<Info, TestAllocator>& sample_recorder =
      sample_recorder_helper.get_mutable();
  sample_recorder.Init();

  Info* info = sample_recorder.Register();
  assert(info != nullptr);
  sample_recorder.Unregister(info);
}

// Check that the state modified by PrepareForSampling() is properly set.
TEST_F(SampleRecorderTest, PrepareForSampling) {
  Info* info1 = Register(1);
  // PrepareForSampling() is invoked in the constructor.
  EXPECT_TRUE(info1->initialized);
  info1->initialized = false;
  sample_recorder_.Unregister(info1);

  Info* info2 = Register(2);
  // We are reusing the sample, PrepareForSampling() is invoked in PopDead();
  EXPECT_TRUE(info2->initialized);
}

TEST_F(SampleRecorderTest, Registration) {
  auto* info1 = Register(1);
  EXPECT_THAT(GetSizes(), UnorderedElementsAre(1));

  auto* info2 = Register(2);
  EXPECT_THAT(GetSizes(), UnorderedElementsAre(1, 2));
  info1->size.store(3);
  EXPECT_THAT(GetSizes(), UnorderedElementsAre(3, 2));

  sample_recorder_.Unregister(info1);
  sample_recorder_.Unregister(info2);
}

TEST_F(SampleRecorderTest, Unregistration) {
  std::vector<Info*> infos;
  for (size_t i = 0; i < 3; ++i) {
    infos.push_back(Register(i));
  }
  EXPECT_THAT(GetSizes(), UnorderedElementsAre(0, 1, 2));

  sample_recorder_.Unregister(infos[1]);
  EXPECT_THAT(GetSizes(), UnorderedElementsAre(0, 2));

  infos.push_back(Register(3));
  infos.push_back(Register(4));
  EXPECT_THAT(GetSizes(), UnorderedElementsAre(0, 2, 3, 4));
  sample_recorder_.Unregister(infos[3]);
  EXPECT_THAT(GetSizes(), UnorderedElementsAre(0, 2, 4));

  sample_recorder_.Unregister(infos[0]);
  sample_recorder_.Unregister(infos[2]);
  sample_recorder_.Unregister(infos[4]);
  EXPECT_THAT(GetSizes(), IsEmpty());

  for (size_t i = 0; i < 10; ++i) {
    Register(i);
  }
  sample_recorder_.UnregisterAll();
  // In a single thread, we expect all samples to be cleaned up.
  EXPECT_THAT(GetSizes(), IsEmpty());
  // UnregisterAll() marks all the live samples as dead. If we register
  // another set of samples, we expect the dead samples are reused and
  // no actual allocation is needed for the new set of samples.
  const uint64_t alloc_count1 = allocator_.alloc_count();
  for (size_t i = 0; i < 10; ++i) {
    Register(i);
  }
  const uint64_t alloc_count2 = allocator_.alloc_count();
  EXPECT_EQ(alloc_count1, alloc_count2);
}

TEST_F(SampleRecorderTest, MultiThreaded) {
  absl::Notification stop;
  ThreadManager threads;
  const int kThreads = 10;
  threads.Start(kThreads, [&](int) {
    std::random_device rd;
    std::mt19937 gen(rd());

    std::vector<Info*> infoz;
    while (!stop.HasBeenNotified()) {
      if (infoz.empty()) {
        infoz.push_back(sample_recorder_.Register());
      }
      switch (std::uniform_int_distribution<>(0, 2)(gen)) {
        case 0: {
          infoz.push_back(sample_recorder_.Register());
          break;
        }
        case 1: {
          size_t p = std::uniform_int_distribution<>(0, infoz.size() - 1)(gen);
          Info* info = infoz[p];
          infoz[p] = infoz.back();
          infoz.pop_back();
          sample_recorder_.Unregister(info);
          break;
        }
        case 2: {
          absl::Duration oldest = absl::ZeroDuration();
          sample_recorder_.Iterate([&](const Info& info) {
            oldest = std::max(oldest, absl::Now() - info.create_time);
            ASSERT_TRUE(info.initialized);
          });
          ASSERT_GE(oldest, absl::ZeroDuration());
          break;
        }
      }
    }
  });

  // Another `SampleRecorder` instance to test `UnregisterAll()`, which does not
  // work well with the setup above since `infoz` might find itself storing dead
  // objects as `UnregisterAll()` is running concurrently. And `Unregister()`
  // assumes the object it is going to mark dead is still alive.
  SampleRecorder<Info, TestAllocator> sample_recorder{&allocator_};
  sample_recorder.Init();
  threads.Start(kThreads, [&](int) { sample_recorder.Register(); });
  threads.Start(kThreads, [&](int) { sample_recorder.UnregisterAll(); });
  threads.Start(kThreads, [&](int) {
    sample_recorder.Iterate(
        [&](const Info& info) { ASSERT_TRUE(info.initialized); });
  });

  // The threads will hammer away.  Give it a little bit of time for tsan to
  // spot errors.
  absl::SleepFor(absl::Seconds(3));
  stop.Notify();
  threads.Stop();
}

TEST_F(SampleRecorderTest, Callback) {
  auto* info1 = Register(1);
  auto* info2 = Register(2);

  static const Info* expected;

  auto callback = [](const Info& info) {
    // We can't use `info` outside of this callback because the object will be
    // disposed as soon as we return from here.
    EXPECT_EQ(&info, expected);
  };

  // Set the callback.
  EXPECT_EQ(sample_recorder_.SetDisposeCallback(callback), nullptr);
  expected = info1;
  sample_recorder_.Unregister(info1);

  // Unset the callback.
  EXPECT_EQ(callback, sample_recorder_.SetDisposeCallback(nullptr));
  expected = nullptr;  // no more calls.
  sample_recorder_.Unregister(info2);
}

// Similar to Sample<Info> above but requires parameter(s) at initialization.
struct InfoWithParam : public Sample<InfoWithParam> {
 public:
  // Default constructor to initialize |graveyard_|.
  InfoWithParam() = default;
  explicit InfoWithParam(size_t size) { PrepareForSampling(size); }
  void PrepareForSampling(size_t size) ABSL_EXCLUSIVE_LOCKS_REQUIRED(lock) {
    info_size = size;
    initialized = true;
  }
  size_t info_size;
  bool initialized;
};

class InfoAllocator {
 public:
  static InfoWithParam* New(size_t size) { return new InfoWithParam(size); }
  static void Delete(InfoWithParam* infoWithParam) { delete infoWithParam; }
};

TEST(SampleRecorderWithParamTest, RegisterWithParam) {
  InfoAllocator allocator;
  SampleRecorder<InfoWithParam, InfoAllocator> sample_recorder{&allocator};
  sample_recorder.Init();
  // Register() goes though New().
  InfoWithParam* info = sample_recorder.Register(1);
  EXPECT_THAT(info->info_size, 1);
  EXPECT_TRUE(info->initialized);
  // Set these values to something else.
  info->info_size = 0;
  info->initialized = false;
  sample_recorder.Unregister(info);
  // |info| is not deleted, just marked as dead. Here, Register() would invoke
  // PopDead(), revive the same object, with its fields populated by PopDead().
  sample_recorder.Register(2);
  EXPECT_THAT(info->info_size, 2);
  EXPECT_TRUE(info->initialized);
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
