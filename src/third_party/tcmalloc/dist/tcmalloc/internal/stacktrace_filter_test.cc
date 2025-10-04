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

#include "tcmalloc/internal/stacktrace_filter.h"

#include <atomic>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/random/random.h"
#include "tcmalloc/testing/thread_manager.h"

namespace tcmalloc {
namespace tcmalloc_internal {

// Simple functional tests
class StackTraceFilterTest : public testing::Test {
 public:
  void InitializeUniqueStackTrace(absl::flat_hash_set<size_t>& hashes,
                                  absl::flat_hash_set<size_t>& hash_bases,
                                  StackTrace& stacktrace) {
    stacktrace.depth = 1;
    uint64_t pc = 0;
    while (true) {
      ++pc;
      // Checking for wrap around (unique stack trace never found)
      ASSERT_NE(pc, 0);
      stacktrace.stack[0] = reinterpret_cast<void*>(pc);
      auto hash = HashOfStackTrace(stacktrace);
      size_t hash_base = HashBaseOfStackTrace(stacktrace);
      if (!hash_bases.contains(hash_base) && !hashes.contains(hash)) {
        hashes.insert(hash);
        hash_bases.insert(hash_base);
        break;
      }
    }
  }

  void SetUp() override {
    absl::flat_hash_set<size_t> hashes;
    absl::flat_hash_set<size_t> hash_bases;

    InitializeUniqueStackTrace(hashes, hash_bases, stacktrace1_);
    InitializeUniqueStackTrace(hashes, hash_bases, stacktrace2_);
    InitializeUniqueStackTrace(hashes, hash_bases, stacktrace3_);

    // insure no collisions among test set (the initializer above should prove
    // this, but this is additional insurance)
    ASSERT_NE(HashOfStackTrace(stacktrace1_), HashOfStackTrace(stacktrace2_));
    ASSERT_NE(HashBaseOfStackTrace(stacktrace1_),
              HashBaseOfStackTrace(stacktrace2_));
    ASSERT_NE(HashOfStackTrace(stacktrace1_), HashOfStackTrace(stacktrace3_));
    ASSERT_NE(HashBaseOfStackTrace(stacktrace1_),
              HashBaseOfStackTrace(stacktrace3_));
    ASSERT_NE(HashOfStackTrace(stacktrace2_), HashOfStackTrace(stacktrace3_));
    ASSERT_NE(HashBaseOfStackTrace(stacktrace2_),
              HashBaseOfStackTrace(stacktrace3_));
  }

  void InitializeColliderStackTrace() {
    absl::flat_hash_set<size_t> hashes;
    absl::flat_hash_set<size_t> hash_bases;

    // Do not add base of stacktrace1_, because that is the match that is being
    // created.
    hashes.insert(HashOfStackTrace(stacktrace1_));
    hashes.insert(HashOfStackTrace(stacktrace2_));
    hash_bases.insert(HashBaseOfStackTrace(stacktrace2_));
    hashes.insert(HashOfStackTrace(stacktrace3_));
    hash_bases.insert(HashBaseOfStackTrace(stacktrace3_));

    size_t hash1_base = HashBaseOfStackTrace(stacktrace1_);
    collider_stacktrace_.depth = 1;
    uint64_t pc = reinterpret_cast<uint64_t>(stacktrace1_.stack[0]);
    size_t collider_hash;
    size_t collider_hash_base;
    while (true) {
      ++pc;
      // Checking for wrap around
      ASSERT_NE(pc, 0);
      collider_stacktrace_.stack[0] = reinterpret_cast<void*>(pc);
      collider_hash = HashOfStackTrace(collider_stacktrace_);
      collider_hash_base = HashBaseOfStackTrace(collider_stacktrace_);
      // if a possible match, check to avoid collisions with others
      if (hash1_base == collider_hash_base && !hashes.contains(collider_hash) &&
          !hash_bases.contains(collider_hash_base)) {
        break;
      }
    }

    // Double check the work above
    ASSERT_NE(HashOfStackTrace(stacktrace1_),
              HashOfStackTrace(collider_stacktrace_));
    ASSERT_EQ(HashBaseOfStackTrace(stacktrace1_),
              HashBaseOfStackTrace(collider_stacktrace_));
    ASSERT_NE(HashOfStackTrace(stacktrace2_),
              HashOfStackTrace(collider_stacktrace_));
    ASSERT_NE(HashBaseOfStackTrace(stacktrace2_),
              HashBaseOfStackTrace(collider_stacktrace_));
    ASSERT_NE(HashOfStackTrace(stacktrace3_),
              HashOfStackTrace(collider_stacktrace_));
    ASSERT_NE(HashBaseOfStackTrace(stacktrace3_),
              HashBaseOfStackTrace(collider_stacktrace_));
  }

  static size_t filter_hash_count_limit() {
    return StackTraceFilter::kHashCountLimit;
  }

  size_t HashOfStackTrace(const StackTrace& stacktrace) const {
    return filter_.HashOfStackTrace(stacktrace);
  }

  size_t HashBaseOfStackTrace(const StackTrace& stacktrace) const {
    return filter_.HashOfStackTrace(stacktrace) % StackTraceFilter::kSize;
  }

  void Reset() { filter_.Reset(); }

  size_t count(const StackTrace& stacktrace) const {
    return filter_.stack_hashes_with_count_[HashBaseOfStackTrace(stacktrace)]
               .load(std::memory_order_relaxed) &
           StackTraceFilter::kMask;
  }

  StackTraceFilter filter_;
  StackTrace stacktrace1_{0};
  StackTrace stacktrace2_{0};
  StackTrace stacktrace3_{0};
  StackTrace collider_stacktrace_{0};
};

namespace {

// This test proves that class can be owned by a constexpr constructor class.
// This is required as the class will be instantiated within
// tcmalloc::tcmalloc_internal::Static.
TEST_F(StackTraceFilterTest, ConstexprConstructor) {
  class Wrapper {
   public:
    constexpr Wrapper() = default;
    StackTraceFilter filter_;
  };

  // Instantiate
  [[maybe_unused]] Wrapper wrapper;
}

TEST_F(StackTraceFilterTest, CountNew) {
  EXPECT_EQ(0, filter_.Count(stacktrace1_));
}

TEST_F(StackTraceFilterTest, CountDifferent) {
  InitializeColliderStackTrace();
  filter_.Add(stacktrace1_);
  EXPECT_EQ(1, filter_.max_slots_used());
  EXPECT_EQ(0, filter_.replacement_inserts());
  EXPECT_EQ(0, filter_.Count(collider_stacktrace_));
}

TEST_F(StackTraceFilterTest, Add) {
  filter_.Add(stacktrace1_);
  EXPECT_EQ(1, filter_.max_slots_used());
  EXPECT_EQ(0, filter_.replacement_inserts());
  EXPECT_EQ(1, filter_.Count(stacktrace1_));
  filter_.Add(stacktrace1_);
  EXPECT_EQ(1, filter_.max_slots_used());
  EXPECT_EQ(0, filter_.replacement_inserts());
  EXPECT_EQ(2, filter_.Count(stacktrace1_));
}

TEST_F(StackTraceFilterTest, AddCountLimitReached) {
  while (count(stacktrace1_) < filter_hash_count_limit()) {
    filter_.Add(stacktrace1_);
  }
  EXPECT_EQ(1, filter_.max_slots_used());
  EXPECT_EQ(0, filter_.replacement_inserts());
  EXPECT_EQ(filter_hash_count_limit(), filter_.Count(stacktrace1_));
  filter_.Add(stacktrace1_);
  EXPECT_EQ(1, filter_.max_slots_used());
  EXPECT_EQ(0, filter_.replacement_inserts());
  EXPECT_EQ(filter_hash_count_limit(), filter_.Count(stacktrace1_));
}

TEST_F(StackTraceFilterTest, AddReplace) {
  InitializeColliderStackTrace();
  filter_.Add(stacktrace1_);
  EXPECT_EQ(1, filter_.max_slots_used());
  EXPECT_EQ(0, filter_.replacement_inserts());
  EXPECT_EQ(1, filter_.Count(stacktrace1_));
  filter_.Add(stacktrace1_);
  EXPECT_EQ(1, filter_.max_slots_used());
  EXPECT_EQ(0, filter_.replacement_inserts());
  EXPECT_EQ(2, filter_.Count(stacktrace1_));
  filter_.Add(collider_stacktrace_);
  EXPECT_EQ(1, filter_.max_slots_used());
  EXPECT_EQ(1, filter_.replacement_inserts());
  EXPECT_EQ(0, filter_.Count(stacktrace1_));
  EXPECT_EQ(1, filter_.Count(collider_stacktrace_));
}

TEST_F(StackTraceFilterTest, Reset) {
  filter_.Add(stacktrace1_);
  EXPECT_EQ(1, filter_.Count(stacktrace1_));
  filter_.Add(stacktrace1_);
  EXPECT_EQ(2, filter_.Count(stacktrace1_));
  Reset();
  EXPECT_EQ(0, filter_.Count(stacktrace1_));
}

}  // namespace

// A collection of threaded tests which are useful for demonstrating
// correct functioning in a threaded environment.
class StackTraceFilterThreadedTest : public testing::Test {
 protected:
  class FilterExerciser {
   public:
    FilterExerciser(StackTraceFilter& filter, int stacktrace_count,
                    int colliding_stacktrace_count,
                    int evaluate_calls_requested, int add_calls_requested)
        : filter_(filter),
          stacktraces_(stacktrace_count),
          colliding_stacktrace_count_(colliding_stacktrace_count),
          evaluate_calls_requested_(evaluate_calls_requested),
          add_calls_requested_(add_calls_requested) {}

    void Initialize() {
      ASSERT_LE(colliding_stacktrace_count_, stacktraces_.size());
      absl::flat_hash_set<size_t> hashes;
      absl::flat_hash_set<size_t> hash_bases;

      // Create stack traces
      for (auto& stacktrace : stacktraces_) {
        stacktrace.depth = absl::Uniform(
            bitgen_, 4, tcmalloc::tcmalloc_internal::kMaxStackDepth);
        for (int stack_index = 0; stack_index < stacktrace.depth;
             ++stack_index) {
          stacktrace.stack[stack_index] = reinterpret_cast<void*>(
              absl::Uniform(bitgen_, static_cast<size_t>(0),
                            std::numeric_limits<size_t>::max()));
        }
        hashes.insert(HashOfStackTrace(stacktrace));
        hash_bases.insert(HashBaseOfStackTrace(stacktrace));
      }

      // Create colliding stack traces
      for (int stacktrace_index = 0;
           stacktrace_index < colliding_stacktrace_count_; ++stacktrace_index) {
        StackTrace colliding_stacktrace;
        InitializeColliderStackTrace(hashes, hash_bases,
                                     stacktraces_[stacktrace_index],
                                     colliding_stacktrace);
        stacktraces_.push_back(colliding_stacktrace);
      }
    }

    void Run() {
      if (hasrun()) {
        return;
      }
      absl::flat_hash_map<int, int> evaluate_calls_count;
      absl::flat_hash_map<int, int> add_calls_counts;
      for (int stacktrace_index = 0; stacktrace_index < stacktraces_.size();
           ++stacktrace_index) {
        evaluate_calls_count[stacktrace_index] = evaluate_calls_requested_;
        add_calls_counts[stacktrace_index] = add_calls_requested_;
      }

      int evaluate_calls = 0;
      int add_calls = 0;
      while (!evaluate_calls_count.empty() || !add_calls_counts.empty()) {
        bool do_evaluate_call = absl::Uniform(bitgen_, 0, 2);
        if (do_evaluate_call && !evaluate_calls_count.empty()) {
          auto iter = evaluate_calls_count.begin();
          std::advance(
              iter, absl::Uniform(bitgen_, 0UL, evaluate_calls_count.size()));
          size_t stacktrace_index = iter->first;
          filter_.Count(stacktraces_[stacktrace_index]);
          ++evaluate_calls;
          if (--evaluate_calls_count[stacktrace_index] == 0) {
            evaluate_calls_count.erase(iter);
          }
        } else if (!do_evaluate_call && !add_calls_counts.empty()) {
          auto iter = add_calls_counts.begin();
          std::advance(iter,
                       absl::Uniform(bitgen_, 0UL, add_calls_counts.size()));
          size_t stacktrace_index = iter->first;
          filter_.Add(stacktraces_[stacktrace_index]);
          ++add_calls;
          if (--add_calls_counts[stacktrace_index] <= 0) {
            add_calls_counts.erase(iter);
          }
        }
      }

      EXPECT_EQ(evaluate_calls,
                evaluate_calls_requested_ * stacktraces_.size());
      EXPECT_EQ(add_calls, add_calls_requested_ * stacktraces_.size());

      hasrun_.store(true, std::memory_order_relaxed);
    }

    size_t HashOfStackTrace(const StackTrace& stacktrace) const {
      return filter_.HashOfStackTrace(stacktrace);
    }

    size_t HashBaseOfStackTrace(const StackTrace& stacktrace) const {
      return filter_.HashOfStackTrace(stacktrace) % StackTraceFilter::kSize;
    }

    const std::vector<StackTrace>& stacktraces() const { return stacktraces_; }
    bool hasrun() { return hasrun_.load(std::memory_order_relaxed); }

   private:
    StackTraceFilter& filter_;
    std::vector<StackTrace> stacktraces_;
    int colliding_stacktrace_count_;
    // Each exerciser must have its own, as BitGen is not thread safe.
    absl::BitGen bitgen_;
    int evaluate_calls_requested_;
    int add_calls_requested_;
    std::atomic<bool> hasrun_{false};

    void InitializeColliderStackTrace(absl::flat_hash_set<size_t>& hashes,
                                      absl::flat_hash_set<size_t>& hash_bases,
                                      const StackTrace& target_stacktrace,
                                      StackTrace& stacktrace) {
      stacktrace = target_stacktrace;
      size_t target_hash_base = HashBaseOfStackTrace(target_stacktrace);
      hash_bases.erase(target_hash_base);

      uint64_t pc = 0;
      while (true) {
        ++pc;
        // Checking for wrap around (unique stack trace never found).
        ASSERT_NE(pc, 0);
        stacktrace.stack[0] = reinterpret_cast<void*>(pc);
        auto hash = HashOfStackTrace(stacktrace);
        size_t hash_base = HashBaseOfStackTrace(stacktrace);
        if (hash_base == target_hash_base && !hash_bases.contains(hash_base) &&
            !hashes.contains(hash)) {
          hashes.insert(hash);
          hash_bases.insert(hash_base);
          break;
        }
      }
    }
  };

  void Exercise(
      std::vector<std::unique_ptr<FilterExerciser>>& filter_exercisers) {
    for (auto& filter_exerciser : filter_exercisers) {
      filter_exerciser->Initialize();
    }
    thread_manager_.Start(filter_exercisers.size(), [&](int thread) {
      filter_exercisers[thread]->Run();
    });
    // Make sure they all run.
    for (auto& filter_exerciser : filter_exercisers) {
      while (!filter_exerciser->hasrun()) {
        sleep(1);
      }
    }
    thread_manager_.Stop();

    // Test is complete at this point.
    // The following simply validates the state of the filter.

    // Find all the unique stacks and colliding stacks across threads.
    absl::flat_hash_map<size_t, StackTrace> unique_stacktraces;
    absl::flat_hash_map<size_t, absl::flat_hash_set<size_t>>
        colliding_base_to_stacktrace_hashes;
    absl::flat_hash_set<size_t> hash_bases;
    for (auto& filter_exerciser : filter_exercisers) {
      for (const auto& stacktrace : filter_exerciser->stacktraces()) {
        auto hash_base =
            filter_.HashOfStackTrace(stacktrace) % StackTraceFilter::kSize;
        if (hash_bases.contains(hash_base)) {
          colliding_base_to_stacktrace_hashes[hash_base].insert(
              filter_.HashOfStackTrace(stacktrace));
          if (unique_stacktraces.contains(hash_base)) {
            colliding_base_to_stacktrace_hashes[hash_base].insert(
                filter_.HashOfStackTrace(unique_stacktraces[hash_base]));
            unique_stacktraces.erase(hash_base);
          }
        } else {
          unique_stacktraces[hash_base] = stacktrace;
          hash_bases.insert(hash_base);
        }
      }
    }

    // Check the counts for unique stack traces (they should be roughly equal
    // to the add calls).
    for (const auto& [hash_base, stacktrace] : unique_stacktraces) {
      auto hash = filter_.HashOfStackTrace(stacktrace);
      EXPECT_EQ(hash & ~StackTraceFilter::kMask,
                filter_.stack_hashes_with_count_[hash_base].load(
                    std::memory_order_relaxed) &
                    ~StackTraceFilter::kMask);
      EXPECT_GE(filter_.stack_hashes_with_count_[hash_base].load(
                    std::memory_order_relaxed) &
                    StackTraceFilter::kMask,
                0);
    }

    // For colliding stacks find at least one in the filter.  Count can not be
    // evaluated based on unknown sequencing of threads.
    for (const auto& [hash_base, stacktrace_hashes] :
         colliding_base_to_stacktrace_hashes) {
      EXPECT_TRUE(stacktrace_hashes.contains(
          (filter_.stack_hashes_with_count_[hash_base].load(
               std::memory_order_relaxed) &
           ~StackTraceFilter::kMask) |
          hash_base));
    }
  }

  StackTraceFilter filter_;
  ThreadManager thread_manager_;
};

namespace {

TEST_F(StackTraceFilterThreadedTest, SingleEntry) {
  std::vector<std::unique_ptr<FilterExerciser>> filter_exercisers;
  for (int exerciser_index = 0; exerciser_index < 10; ++exerciser_index) {
    filter_exercisers.push_back(
        std::make_unique<FilterExerciser>(filter_, 1, 0, 100, 100));
  }
  Exercise(filter_exercisers);
}

TEST_F(StackTraceFilterThreadedTest, SingleEntryWithCollider) {
  std::vector<std::unique_ptr<FilterExerciser>> filter_exercisers;
  for (int exerciser_index = 0; exerciser_index < 10; ++exerciser_index) {
    filter_exercisers.push_back(
        std::make_unique<FilterExerciser>(filter_, 1, 1, 100, 100));
  }
  Exercise(filter_exercisers);
}

TEST_F(StackTraceFilterThreadedTest, MultipleEntries) {
  std::vector<std::unique_ptr<FilterExerciser>> filter_exercisers;
  for (int exerciser_index = 0; exerciser_index < 100; ++exerciser_index) {
    filter_exercisers.push_back(
        std::make_unique<FilterExerciser>(filter_, 100, 0, 1000, 1000));
  }
  Exercise(filter_exercisers);
}

TEST_F(StackTraceFilterThreadedTest, MultipleEntriesWithColliders) {
  std::vector<std::unique_ptr<FilterExerciser>> filter_exercisers;
  for (int exerciser_index = 0; exerciser_index < 100; ++exerciser_index) {
    filter_exercisers.push_back(
        std::make_unique<FilterExerciser>(filter_, 100, 10, 1000, 1000));
  }
  Exercise(filter_exercisers);
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
