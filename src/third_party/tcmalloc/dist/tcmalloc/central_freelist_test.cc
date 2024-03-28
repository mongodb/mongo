// Copyright 2019 The TCMalloc Authors
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

#include "tcmalloc/central_freelist.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

#include "benchmark/benchmark.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/algorithm/container.h"
#include "absl/base/internal/spinlock.h"
#include "absl/base/thread_annotations.h"
#include "absl/container/fixed_array.h"
#include "absl/container/flat_hash_map.h"
#include "absl/memory/memory.h"
#include "absl/numeric/bits.h"
#include "absl/random/random.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/mock_static_forwarder.h"
#include "tcmalloc/pagemap.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/size_class_info.h"
#include "tcmalloc/sizemap.h"
#include "tcmalloc/span.h"
#include "tcmalloc/span_stats.h"
#include "tcmalloc/static_vars.h"
#include "tcmalloc/stats.h"
#include "tcmalloc/testing/thread_manager.h"

namespace tcmalloc {
namespace tcmalloc_internal {

namespace central_freelist_internal {

class StaticForwarderTest : public testing::TestWithParam<
                                std::tuple<AccessDensityPrediction, size_t>> {
 protected:
  size_t size_class_;
  size_t object_size_;
  Length pages_per_span_;
  size_t batch_size_;
  size_t objects_per_span_;
  uint32_t size_reciprocal_;
  AccessDensityPrediction density_;

 private:
  void SetUp() override {
    density_ = std::get<0>(GetParam());
    size_class_ = std::get<1>(GetParam());
    if (IsExpandedSizeClass(size_class_)) {
#if ABSL_HAVE_THREAD_SANITIZER
      GTEST_SKIP() << "Skipping test under sanitizers that conflict with "
                      "address placement";
#endif

      if (!ColdFeatureActive()) {
        // If !ColdFeatureActive(), we will use the normal page heap, which will
        // keep us from seeing memory get the expected tags.
        GTEST_SKIP()
            << "Skipping expanded size classes without cold experiment";
      }
    }
    object_size_ = tc_globals.sizemap().class_to_size(size_class_);
    if (object_size_ == 0) {
      GTEST_SKIP() << "Skipping empty size class.";
    }

    pages_per_span_ = Length(tc_globals.sizemap().class_to_pages(size_class_));
    batch_size_ = tc_globals.sizemap().num_objects_to_move(size_class_);
    objects_per_span_ = pages_per_span_.in_bytes() / object_size_;
    size_reciprocal_ = Span::CalcReciprocal(object_size_);
  }
};

TEST_P(StaticForwarderTest, Simple) {
  SpanAllocInfo alloc_info = {.objects_per_span = objects_per_span_,
                              .density = density_};
  Span* span =
      StaticForwarder::AllocateSpan(size_class_, alloc_info, pages_per_span_);
  ASSERT_NE(span, nullptr);

  absl::FixedArray<void*> batch(objects_per_span_);
  const uint32_t max_span_cache_size = StaticForwarder::max_span_cache_size();
  size_t allocated =
      span->BuildFreelist(object_size_, objects_per_span_, &batch[0],
                          objects_per_span_, max_span_cache_size);
  ASSERT_EQ(allocated, objects_per_span_);

  EXPECT_EQ(size_class_, tc_globals.pagemap().sizeclass(span->first_page()));
  EXPECT_EQ(size_class_, tc_globals.pagemap().sizeclass(span->last_page()));

  // span_test.cc provides test coverage for Span, but we need to obtain several
  // objects to confirm we can map back to the Span pointer from the PageMap.
  for (void* ptr : batch) {
    Span* got;
    StaticForwarder::MapObjectsToSpans({&ptr, 1}, &got);
    EXPECT_EQ(span, got);
  }

  for (void* ptr : batch) {
    span->FreelistPush(ptr, object_size_, size_reciprocal_,
                       max_span_cache_size);
  }

  StaticForwarder::DeallocateSpans(size_class_, objects_per_span_,
                                   absl::MakeSpan(&span, 1));
}

class StaticForwarderEnvironment {
  struct SpanData {
    Span* span;
    void* batch[kMaxObjectsToMove];
  };

 public:
  StaticForwarderEnvironment(int size_class, size_t object_size,
                             size_t objects_per_span, Length pages_per_span,
                             int batch_size, AccessDensityPrediction density)
      : size_class_(size_class),
        object_size_(object_size),
        objects_per_span_(objects_per_span),
        pages_per_span_(pages_per_span),
        batch_size_(batch_size),
        density_(density) {}

  ~StaticForwarderEnvironment() { Drain(); }

  void RandomlyPoke() {
    absl::BitGen rng;
    double coin = absl::Uniform(rng, 0.0, 1.0);

    if (coin < 0.5) {
      Grow();
    } else if (coin < 0.9) {
      // Deallocate Spans.  We may deallocate more than 1 span, so we bias
      // towards allocating Spans more often than we deallocate.
      Shrink();
    } else {
      Shuffle(rng);
    }
  }

  void Drain() {
    std::vector<std::unique_ptr<SpanData>> spans;

    {
      absl::MutexLock l(&mu_);
      if (data_.empty()) {
        return;
      }

      spans = std::move(data_);
      data_.clear();
    }

    // Check mappings.
    std::vector<Span*> free_spans;
    for (const auto& data : spans) {
      EXPECT_EQ(size_class_,
                tc_globals.pagemap().sizeclass(data->span->first_page()));
      EXPECT_EQ(size_class_,
                tc_globals.pagemap().sizeclass(data->span->last_page()));
      // Confirm we can map at least one object back.
      Span* got;
      StaticForwarder::MapObjectsToSpans({&data->batch[0], 1}, &got);
      EXPECT_EQ(data->span, got);

      free_spans.push_back(data->span);
    }

    StaticForwarder::DeallocateSpans(size_class_, objects_per_span_,
                                     absl::MakeSpan(free_spans));
  }

  void Grow() {
    // Allocate a Span
    SpanAllocInfo alloc_info = {.objects_per_span = objects_per_span_,
                                .density = density_};
    Span* span =
        StaticForwarder::AllocateSpan(size_class_, alloc_info, pages_per_span_);
    ASSERT_NE(span, nullptr);

    auto d = std::make_unique<SpanData>();
    d->span = span;

    size_t allocated = span->BuildFreelist(
        object_size_, objects_per_span_, d->batch, batch_size_,
        StaticForwarder::max_span_cache_size());
    EXPECT_LE(allocated, objects_per_span_);

    EXPECT_EQ(size_class_, tc_globals.pagemap().sizeclass(span->first_page()));
    EXPECT_EQ(size_class_, tc_globals.pagemap().sizeclass(span->last_page()));
    // Confirm we can map at least one object back.
    Span* got;
    StaticForwarder::MapObjectsToSpans({&d->batch[0], 1}, &got);
    EXPECT_EQ(span, got);

    absl::MutexLock l(&mu_);
    spans_allocated_++;
    data_.push_back(std::move(d));
  }

  void Shrink() {
    absl::BitGen rng;
    std::vector<std::unique_ptr<SpanData>> spans;

    {
      absl::MutexLock l(&mu_);
      if (data_.empty()) {
        return;
      }

      size_t count = absl::LogUniform<size_t>(rng, 1, data_.size());
      spans.reserve(count);

      for (int i = 0; i < count; i++) {
        spans.push_back(std::move(data_.back()));
        data_.pop_back();
      }
    }

    // Check mappings.
    std::vector<Span*> free_spans;
    for (auto& data : spans) {
      EXPECT_EQ(size_class_,
                tc_globals.pagemap().sizeclass(data->span->first_page()));
      EXPECT_EQ(size_class_,
                tc_globals.pagemap().sizeclass(data->span->last_page()));
      // Confirm we can map at least one object back.
      Span* got;
      StaticForwarder::MapObjectsToSpans({&data->batch[0], 1}, &got);
      EXPECT_EQ(data->span, got);

      free_spans.push_back(data->span);
    }

    StaticForwarder::DeallocateSpans(size_class_, objects_per_span_,
                                     absl::MakeSpan(free_spans));
  }

  void Shuffle(absl::BitGen& rng) {
    // Shuffle the shared vector.
    absl::MutexLock l(&mu_);
    absl::c_shuffle(data_, rng);
  }

  int64_t BytesAllocated() {
    absl::MutexLock l(&mu_);
    return pages_per_span_.in_bytes() * spans_allocated_;
  }

 private:
  int size_class_;
  size_t object_size_;
  size_t objects_per_span_;
  Length pages_per_span_;
  int batch_size_;
  AccessDensityPrediction density_;

  absl::Mutex mu_;
  int64_t spans_allocated_ ABSL_GUARDED_BY(mu_) = 0;
  std::vector<std::unique_ptr<SpanData>> data_ ABSL_GUARDED_BY(mu_);
};

static BackingStats PageHeapStats() {
  PageHeapSpinLockHolder l;
  return tc_globals.page_allocator().stats();
}

TEST_P(StaticForwarderTest, Fuzz) {
#if ABSL_HAVE_THREAD_SANITIZER
  // TODO(b/193887621):  Enable this test under TSan after addressing benign
  // true positives.
  GTEST_SKIP() << "Skipping test under Thread Sanitizer.";
#endif  // ABSL_HAVE_THREAD_SANITIZER

  const auto page_heap_before = PageHeapStats();

  StaticForwarderEnvironment env(size_class_, object_size_, objects_per_span_,
                                 pages_per_span_, batch_size_, density_);
  ThreadManager threads;
  threads.Start(10, [&](int) { env.RandomlyPoke(); });

  absl::SleepFor(absl::Seconds(0.2));

  threads.Stop();

  const auto page_heap_after = PageHeapStats();
  // Confirm we did not leak Spans by ensuring the page heap did not grow nearly
  // 1:1 by the total number of Spans we ever allocated.
  //
  // Since we expect to allocate a significant number of spans, we apply a
  // factor of 1/2 (which is unlikely to be flaky) to avoid false negatives
  // if/when a background thread triggers a deallocation.
  const int64_t bytes_allocated = env.BytesAllocated();
  EXPECT_GT(bytes_allocated, 0);
  EXPECT_LE(static_cast<int64_t>(page_heap_after.system_bytes) -
                static_cast<int64_t>(page_heap_before.system_bytes),
            bytes_allocated / 2);
}

INSTANTIATE_TEST_SUITE_P(
    All, StaticForwarderTest,
    testing::Combine(testing::Values(AccessDensityPrediction::kSparse,
                                     AccessDensityPrediction::kDense),
                     testing::Range(size_t(1), kNumClasses)));

}  // namespace central_freelist_internal

namespace {

using central_freelist_internal::kNumLists;
using TypeParam = FakeCentralFreeListEnvironment<
    central_freelist_internal::CentralFreeList<MockStaticForwarder>>;
using CentralFreeListTest =
    ::testing::TestWithParam<std::tuple<SizeClassInfo, bool, bool>>;

TEST_P(CentralFreeListTest, IsolatedSmoke) {
  TypeParam e(std::get<0>(GetParam()).size, std::get<0>(GetParam()).pages,
              std::get<0>(GetParam()).num_to_move, std::get<1>(GetParam()),
              std::get<2>(GetParam()));
  EXPECT_CALL(e.forwarder(), AllocateSpan).Times(1);

  absl::FixedArray<void*> batch(e.batch_size());
  int allocated = e.central_freelist().RemoveRange(&batch[0], e.batch_size());
  ASSERT_GT(allocated, 0);
  EXPECT_LE(allocated, e.batch_size());

  // We should observe span's utilization captured in the histogram. The number
  // of spans in rest of the buckets should be zero.
  const int bitwidth = absl::bit_width(static_cast<unsigned>(allocated));
  for (int i = 1; i <= absl::bit_width(e.objects_per_span()); ++i) {
    // Skip the check for objects_per_span = 1 since such spans skip most of the
    // central freelist's logic.
    if (i == bitwidth && e.objects_per_span() != 1) {
      EXPECT_EQ(e.central_freelist().NumSpansWith(i), 1);
    } else {
      EXPECT_EQ(e.central_freelist().NumSpansWith(i), 0);
    }
  }

  EXPECT_CALL(e.forwarder(), MapObjectsToSpans).Times(1);
  EXPECT_CALL(e.forwarder(), DeallocateSpans).Times(1);

  // Skip the check for objects_per_span = 1 since such spans skip most of the
  // central freelist's logic.
  SpanStats stats = e.central_freelist().GetSpanStats();
  if (e.objects_per_span() != 1) {
    EXPECT_EQ(stats.num_spans_requested, 1);
    EXPECT_EQ(stats.num_spans_returned, 0);
    EXPECT_EQ(stats.obj_capacity, e.objects_per_span());
  }

  e.central_freelist().InsertRange(absl::MakeSpan(&batch[0], allocated));
  // Skip the check for objects_per_span = 1 since such spans skip most of the
  // central freelist's logic.
  if (e.objects_per_span() != 1) {
    SpanStats stats = e.central_freelist().GetSpanStats();
    EXPECT_EQ(stats.num_spans_requested, 1);
    EXPECT_EQ(stats.num_spans_returned, 1);
    EXPECT_EQ(stats.obj_capacity, 0);
  }

  // Span captured in the histogram with the earlier utilization should have
  // been removed.
  for (int i = 1; i <= absl::bit_width(e.objects_per_span()); ++i) {
    EXPECT_EQ(e.central_freelist().NumSpansWith(i), 0);
  }
}

TEST_P(CentralFreeListTest, SpanUtilizationHistogram) {
  TypeParam e(std::get<0>(GetParam()).size, std::get<0>(GetParam()).pages,
              std::get<0>(GetParam()).num_to_move, std::get<1>(GetParam()),
              std::get<2>(GetParam()));
  constexpr size_t kNumSpans = 10;

  // Request kNumSpans spans.
  void* batch[kMaxObjectsToMove];
  const int num_objects_to_fetch = kNumSpans * e.objects_per_span();
  int total_fetched = 0;
  // Tracks object and corresponding span from which it was allocated.
  std::vector<std::pair<void*, Span*>> object_to_span;
  // Tracks number of objects allocated per span.
  absl::flat_hash_map<Span*, size_t> allocated_per_span;
  int span_idx = 0;

  while (total_fetched < num_objects_to_fetch) {
    size_t n = num_objects_to_fetch - total_fetched;
    int got =
        e.central_freelist().RemoveRange(batch, std::min(n, e.batch_size()));
    total_fetched += got;

    // Increment span_idx if current objects have been fetched from the new
    // span.
    if (total_fetched > (span_idx + 1) * e.objects_per_span()) {
      ++span_idx;
    }
    // Record fetched object and the associated span.
    for (int i = 0; i < got; ++i) {
      Span* s = e.forwarder().MapObjectToSpan(batch[i]);
      object_to_span.emplace_back(batch[i], s);
      allocated_per_span[s] += 1;
    }
    TC_ASSERT_LT(span_idx, kNumSpans);
  }

  // Make sure that we have fetched exactly from kNumSpans spans.
  EXPECT_EQ(span_idx + 1, kNumSpans);

  // We should have kNumSpans spans in the histogram with number of allocated
  // objects equal to e.objects_per_span() (i.e. in the last bucket).
  // Rest of the buckets should be empty.
  const int expected_bitwidth = absl::bit_width(e.objects_per_span());
  // Skip the check when objects_per_span = 1 as those spans skip most of the
  // central freelist's logic.
  if (e.objects_per_span() != 1) {
    EXPECT_EQ(e.central_freelist().NumSpansWith(expected_bitwidth), kNumSpans);
  }
  for (int i = 1; i < expected_bitwidth; ++i) {
    EXPECT_EQ(e.central_freelist().NumSpansWith(i), 0);
  }

  // Shuffle.
  absl::BitGen rng;
  std::shuffle(object_to_span.begin(), object_to_span.end(), rng);

  // Return objects, a fraction at a time, each time checking that histogram is
  // correct.
  int total_returned = 0;
  const int last_bucket = absl::bit_width(e.objects_per_span()) - 1;
  while (total_returned < num_objects_to_fetch) {
    uint64_t size_to_pop =
        std::min(object_to_span.size() - total_returned, e.batch_size());

    for (int i = 0; i < size_to_pop; ++i) {
      const auto [ptr, span] = object_to_span[i + total_returned];
      batch[i] = ptr;
      --allocated_per_span[span];
    }
    total_returned += size_to_pop;
    e.central_freelist().InsertRange({batch, size_to_pop});

    // Calculate expected histogram.
    std::vector<size_t> expected(absl::bit_width(e.objects_per_span()), 0);
    for (const auto& span_and_count : allocated_per_span) {
      // If span has non-zero allocated objects, include it in the histogram.
      if (span_and_count.second > 0) {
        const size_t bucket = absl::bit_width(span_and_count.second) - 1;
        TC_ASSERT_LE(bucket, last_bucket);
        ++expected[bucket];
      }
    }

    // Fetch number of spans logged in the histogram and compare it with the
    // expected histogram that we calculated using the tracked allocated
    // objects per span.
    for (int i = 1; i <= last_bucket; ++i) {
      EXPECT_EQ(e.central_freelist().NumSpansWith(i), expected[i - 1]);
    }
  }

  // Since no span is live here, histogram must be empty.
  for (int i = 1; i <= last_bucket; ++i) {
    EXPECT_EQ(e.central_freelist().NumSpansWith(i), 0);
  }
}

// Confirms that a call to RemoveRange returns at most kObjectsPerSpan objects
// in cases when there are no non-empty spans in the central freelist. This
// makes sure that we populate, and subsequently allocate from a single span.
// This avoids memory regression due to multiple Populate calls observed in
// b/225880278.
TEST_P(CentralFreeListTest, SinglePopulate) {
  // Make sure that we allocate up to kObjectsPerSpan objects in both the span
  // prioritization states.
  TypeParam e(std::get<0>(GetParam()).size, std::get<0>(GetParam()).pages,
              std::get<0>(GetParam()).num_to_move, std::get<1>(GetParam()),
              std::get<2>(GetParam()));
  // Try to fetch sufficiently large number of objects at startup.
  const int num_objects_to_fetch = 10 * e.objects_per_span();
  std::vector<void*> objects(num_objects_to_fetch, nullptr);
  const size_t got =
      e.central_freelist().RemoveRange(objects.data(), num_objects_to_fetch);
  // Confirm we allocated at most kObjectsPerSpan number of objects.
  EXPECT_GT(got, 0);
  EXPECT_LE(got, e.objects_per_span());
  size_t returned = 0;
  while (returned < got) {
    const size_t to_return = std::min(got - returned, e.batch_size());
    e.central_freelist().InsertRange({&objects[returned], to_return});
    returned += to_return;
  }
}

// Tests whether the index generated by the input indexing function matches the
// index of the span on which allocations and deallocation operations are
// carried out.  The test first allocates objects and deallocates them.  After
// each operation, the actual index is matched against the expected one.
template <typename IndexingFunc>
void TestIndexing(TypeParam& e, IndexingFunc f) {
  TC_ASSERT_GT(kNumLists, 0);
  const int num_objects_to_fetch = e.objects_per_span();
  std::vector<void*> objects(num_objects_to_fetch);
  size_t fetched = 0;
  int expected_idx = kNumLists - 1;

  // Fetch one object at a time from a span and confirm that the span is moved
  // through the nonempty_ lists as we allocate more objects from it.
  while (fetched < num_objects_to_fetch) {
    // Try to fetch one object from the span.
    int got = e.central_freelist().RemoveRange(&objects[fetched], 1);
    fetched += got;
    TC_ASSERT(fetched);
    if (fetched % num_objects_to_fetch == 0) {
      // Span should have been removed from nonempty_ lists because we have
      // allocated all the objects from it.
      EXPECT_EQ(e.central_freelist().NumSpansInList(expected_idx), 0);
    } else {
      expected_idx = f(fetched);
      TC_ASSERT_GE(expected_idx, 0);
      TC_ASSERT_LT(expected_idx, kNumLists);
      // Check that the span exists in the corresponding nonempty_ list.
      EXPECT_EQ(e.central_freelist().NumSpansInList(expected_idx), 1);
    }
  }

  // Similar to our previous test, we now make sure that the span is moved
  // through the nonempty_ lists when we deallocate objects back to it.
  size_t remaining = fetched;
  while (--remaining > 0) {
    // Return objects back to the span one at a time.
    e.central_freelist().InsertRange({&objects[remaining], 1});
    TC_ASSERT(remaining);
    // When allocated objects are more than the threshold, the span is indexed
    // to nonempty_ list 0.
    expected_idx = f(remaining);
    EXPECT_LT(expected_idx, kNumLists);
    EXPECT_EQ(e.central_freelist().NumSpansInList(expected_idx), 1);
  }

  // When the last object is returned, we release the span to the page heap. So,
  // nonempty_[0] should also be empty.
  e.central_freelist().InsertRange({&objects[remaining], 1});
  EXPECT_EQ(e.central_freelist().NumSpansInList(0), 0);
}

TEST_P(CentralFreeListTest, BitwidthIndexedNonEmptyLists) {
  TypeParam e(std::get<0>(GetParam()).size, std::get<0>(GetParam()).pages,
              std::get<0>(GetParam()).num_to_move, std::get<1>(GetParam()),
              std::get<2>(GetParam()));
  if (e.objects_per_span() <= 2 * kNumLists &&
      e.use_all_buckets_for_few_object_spans() == true) {
    GTEST_SKIP()
        << "Skipping test as one hot encoding used for few object spans.";
  }
  auto bitwidth_indexing = [](size_t allocated) {
    size_t bitwidth = absl::bit_width(allocated);
    return kNumLists - std::min(bitwidth, kNumLists);
  };
  TestIndexing(e, bitwidth_indexing);
}

TEST_P(CentralFreeListTest, DirectIndexedEncodedNonEmptyLists) {
  TypeParam e(std::get<0>(GetParam()).size, std::get<0>(GetParam()).pages,
              std::get<0>(GetParam()).num_to_move, std::get<1>(GetParam()),
              std::get<2>(GetParam()));
  if (e.objects_per_span() > 2 * kNumLists ||
      e.use_all_buckets_for_few_object_spans() == false) {
    GTEST_SKIP() << "Skipping test as one hot encoding not required.";
  }
  auto direct_indexing = [](int allocated) {
    if (allocated <= kNumLists) return kNumLists - allocated;
    return 0UL;
  };
  TestIndexing(e, direct_indexing);
}

// Checks if we are indexing a span in the nonempty_ lists as expected. We also
// check if the spans are correctly being prioritized. That is, we create a
// scenario where we have two live spans, and one span has more allocated
// objects than the other span. On subsequent allocations, we confirm that the
// objects are allocated from the span with a higher number of allocated objects
// as enforced by our prioritization scheme.
TEST_P(CentralFreeListTest, SpanPriority) {
  TypeParam e(std::get<0>(GetParam()).size, std::get<0>(GetParam()).pages,
              std::get<0>(GetParam()).num_to_move, std::get<1>(GetParam()),
              std::get<2>(GetParam()));

  // If the number of objects per span is less than 2, we do not use more than
  // one nonempty_ lists. So, we can not prioritize the spans based on how many
  // objects were allocated from them.
  const int objects_per_span = e.objects_per_span();
  if (objects_per_span < 3 || kNumLists < 2) return;

  constexpr int kNumSpans = 2;
  // Track objects allocated per span.
  absl::FixedArray<std::vector<void*>> objects(kNumSpans);
  void* batch[kMaxObjectsToMove];

  const size_t to_fetch = objects_per_span;
  // Allocate all objects from kNumSpans.
  for (int span = 0; span < kNumSpans; ++span) {
    size_t fetched = 0;
    while (fetched < to_fetch) {
      const size_t n = to_fetch - fetched;
      int got =
          e.central_freelist().RemoveRange(batch, std::min(n, e.batch_size()));
      for (int i = 0; i < got; ++i) {
        objects[span].push_back(batch[i]);
      }
      fetched += got;
    }
  }

  // Perform deallocations so that each span contains only two objects.
  size_t to_release = to_fetch - 2;
  for (int span = 0; span < kNumSpans; ++span) {
    size_t released = 0;
    while (released < to_release) {
      uint64_t n = std::min(to_release - released, e.batch_size());
      for (int i = 0; i < n; ++i) {
        batch[i] = objects[span][i + released];
      }
      released += n;
      e.central_freelist().InsertRange({batch, n});
    }
    objects[span].erase(objects[span].begin(),
                        objects[span].begin() + released);
  }

  // Make sure we have kNumSpans in the expected second-last nonempty_ list.
  EXPECT_EQ(e.central_freelist().NumSpansInList(kNumLists - 2), kNumSpans);

  // Release an additional object from all but one spans so that they are
  // deprioritized for subsequent allocations.
  to_release = 1;
  for (int span = 1; span < kNumSpans; ++span) {
    size_t released = 0;
    while (released < to_release) {
      uint64_t n = std::min(to_release - released, e.batch_size());
      for (int i = 0; i < n; ++i) {
        batch[i] = objects[span][i + released];
      }
      released += n;
      e.central_freelist().InsertRange({batch, n});
    }
    objects[span].erase(objects[span].begin(),
                        objects[span].begin() + released);
  }

  // Make sure we have kNumSpans-1 spans in the last nonempty_ list and just one
  // span in the second-last list.
  EXPECT_EQ(e.central_freelist().NumSpansInList(kNumLists - 1), kNumSpans - 1);
  EXPECT_EQ(e.central_freelist().NumSpansInList(kNumLists - 2), 1);

  // Allocate one object to ensure that it is being allocated from the span with
  // the highest number of allocated objects.
  int got = e.central_freelist().RemoveRange(batch, 1);
  EXPECT_EQ(got, 1);
  // Number of spans in the last nonempty_ list should be unchanged (i.e.
  // kNumSpans-1).
  EXPECT_EQ(e.central_freelist().NumSpansInList(kNumLists - 1), kNumSpans - 1);
  if (e.objects_per_span() == 3) {
    // Since we allocated another object from the span that had two objects
    // allocated from it, so the span would no longer be there in the span list.
    for (int i = kNumLists - 2; i >= 0; --i) {
      EXPECT_EQ(e.central_freelist().NumSpansInList(i), 0);
    }
  } else if (e.objects_per_span() <= 2 * kNumLists &&
             e.use_all_buckets_for_few_object_spans() == true) {
    // We should have only one span in the third-last nonempty_ list; this is
    // the span from which we should have allocated the last object.
    EXPECT_EQ(e.central_freelist().NumSpansInList(kNumLists - 3), 1);
  } else {
    // We should have only one span in the second-last nonempty_ list; this is
    // the span from which we should have allocated the last object.
    EXPECT_EQ(e.central_freelist().NumSpansInList(kNumLists - 2), 1);
  }
  // Return previously allocated object.
  e.central_freelist().InsertRange({batch, 1});

  // Return rest of the objects.
  for (int span = 0; span < kNumSpans; ++span) {
    for (int i = 0; i < objects[span].size(); ++i) {
      e.central_freelist().InsertRange({&objects[span][i], 1});
    }
  }
}

TEST_P(CentralFreeListTest, MultipleSpans) {
  TypeParam e(std::get<0>(GetParam()).size, std::get<0>(GetParam()).pages,
              std::get<0>(GetParam()).num_to_move, std::get<1>(GetParam()),
              std::get<2>(GetParam()));
  std::vector<void*> all_objects;
  constexpr size_t kNumSpans = 10;

  // Request kNumSpans spans.
  void* batch[kMaxObjectsToMove];
  ASSERT_GT(e.objects_per_span(), 0);
  const int num_objects_to_fetch = kNumSpans * e.objects_per_span();
  int total_fetched = 0;
  while (total_fetched < num_objects_to_fetch) {
    size_t n = num_objects_to_fetch - total_fetched;
    int got =
        e.central_freelist().RemoveRange(batch, std::min(n, e.batch_size()));
    for (int i = 0; i < got; ++i) {
      all_objects.push_back(batch[i]);
    }
    total_fetched += got;
  }

  // We should have kNumSpans spans in the histogram with number of
  // allocated objects equal to e.objects_per_span() (i.e. in the last
  // bucket). Rest of the buckets should be empty.
  const int expected_bitwidth = absl::bit_width(e.objects_per_span());
  // Skip the check for objects_per_span = 1 since such spans skip most of the
  // central freelist's logic.
  if (e.objects_per_span() != 1) {
    EXPECT_EQ(e.central_freelist().NumSpansWith(expected_bitwidth), kNumSpans);
  }
  for (int i = 1; i < expected_bitwidth; ++i) {
    EXPECT_EQ(e.central_freelist().NumSpansWith(i), 0);
  }

  // Skip the check for objects_per_span = 1 since such spans skip most of the
  // central freelist's logic.
  if (e.objects_per_span() != 1) {
    SpanStats stats = e.central_freelist().GetSpanStats();
    EXPECT_EQ(stats.num_spans_requested, kNumSpans);
    EXPECT_EQ(stats.num_spans_returned, 0);
  }

  EXPECT_EQ(all_objects.size(), num_objects_to_fetch);

  // Shuffle
  absl::BitGen rng;
  std::shuffle(all_objects.begin(), all_objects.end(), rng);

  // Return all
  int total_returned = 0;
  while (total_returned < num_objects_to_fetch) {
    uint64_t size_to_pop =
        std::min(all_objects.size() - total_returned, e.batch_size());
    for (int i = 0; i < size_to_pop; ++i) {
      batch[i] = all_objects[i + total_returned];
    }
    total_returned += size_to_pop;
    e.central_freelist().InsertRange({batch, size_to_pop});
    // sanity check
    if (e.objects_per_span() != 1 && total_returned < num_objects_to_fetch) {
      SpanStats stats = e.central_freelist().GetSpanStats();
      EXPECT_GT(stats.num_spans_requested, stats.num_spans_returned);
      EXPECT_NE(stats.obj_capacity, 0);
      // Total spans recorded in the histogram must be equal to the number of
      // live spans.
      size_t spans_in_histogram = 0;
      for (int i = 1; i <= absl::bit_width(e.objects_per_span()); ++i) {
        spans_in_histogram += e.central_freelist().NumSpansWith(i);
      }
      EXPECT_EQ(spans_in_histogram, stats.num_live_spans());
    }
  }

  SpanStats stats = e.central_freelist().GetSpanStats();
  EXPECT_EQ(stats.num_spans_requested, stats.num_spans_returned);
  // Since no span is live, histogram must be empty.
  for (int i = 1; i <= absl::bit_width(e.objects_per_span()); ++i) {
    EXPECT_EQ(e.central_freelist().NumSpansWith(i), 0);
  }
  EXPECT_EQ(stats.obj_capacity, 0);
}

TEST_P(CentralFreeListTest, PassSpanDensityToPageheap) {
  TypeParam e(std::get<0>(GetParam()).size, std::get<0>(GetParam()).pages,
              std::get<0>(GetParam()).num_to_move, std::get<1>(GetParam()),
              std::get<2>(GetParam()));
  ASSERT_GE(e.objects_per_span(), 1);
  auto test_function = [&](size_t num_objects,
                           AccessDensityPrediction density) {
    std::vector<void*> objects(e.objects_per_span());
    EXPECT_CALL(e.forwarder(), AllocateSpan(testing::_, testing::_, testing::_))
        .Times(1);
    const size_t to_fetch = std::min(e.objects_per_span(), e.batch_size());
    const size_t fetched =
        e.central_freelist().RemoveRange(&objects[0], to_fetch);
    size_t returned = 0;
    while (returned < fetched) {
      EXPECT_CALL(e.forwarder(),
                  DeallocateSpans(testing::_, testing::_, testing::_))
          .Times(1);
      const size_t to_return = std::min(fetched - returned, e.batch_size());
      e.central_freelist().InsertRange({&objects[returned], to_return});
      returned += to_return;
    }
  };
  test_function(1, AccessDensityPrediction::kDense);
  test_function(e.objects_per_span(), AccessDensityPrediction::kDense);
}

TEST_P(CentralFreeListTest, SpanFragmentation) {
  // This test is primarily exercising Span itself to model how tcmalloc.cc uses
  // it, but this gives us a self-contained (and sanitizable) implementation of
  // the CentralFreeList.
  TypeParam e(std::get<0>(GetParam()).size, std::get<0>(GetParam()).pages,
              std::get<0>(GetParam()).num_to_move, std::get<1>(GetParam()),
              std::get<2>(GetParam()));
  // Allocate one object from the CFL to allocate a span.
  void* initial;
  int got = e.central_freelist().RemoveRange(&initial, 1);
  ASSERT_EQ(got, 1);

  Span* const span = e.central_freelist().forwarder().MapObjectToSpan(initial);
  const size_t object_size =
      e.central_freelist().forwarder().class_to_size(TypeParam::kSizeClass);

  ThreadManager fragmentation;
  fragmentation.Start(1, [&](int) {
    if (e.objects_per_span() != 1) {
      benchmark::DoNotOptimize(span->Fragmentation(object_size));
    }
  });

  ThreadManager cfl;
  cfl.Start(1, [&](int) {
    void* next;
    int got = e.central_freelist().RemoveRange(&next, 1);
    e.central_freelist().InsertRange(absl::MakeSpan(&next, got));
  });

  absl::SleepFor(absl::Seconds(0.1));

  fragmentation.Stop();
  cfl.Stop();

  e.central_freelist().InsertRange(absl::MakeSpan(&initial, 1));
}

INSTANTIATE_TEST_SUITE_P(
    CentralFreeList, CentralFreeListTest,
    testing::Combine(
        // We skip the first size class since it is set to 0.
        testing::ValuesIn(kSizeClasses.classes.begin() + 1,
                          kSizeClasses.classes.end()),
        /*use_all_buckets_for_few_object_spans=*/testing::Values(false, true),
        /*use_large_spans=*/testing::Values(false, true)));

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
