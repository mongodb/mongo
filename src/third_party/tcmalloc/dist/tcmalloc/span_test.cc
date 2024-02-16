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

#include "tcmalloc/span.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "absl/base/internal/spinlock.h"
#include "absl/container/flat_hash_set.h"
#include "absl/random/random.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/static_vars.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

class RawSpan {
 public:
  void Init(size_t size_class) {
    size_t size = tc_globals.sizemap().class_to_size(size_class);
    auto npages = Length(tc_globals.sizemap().class_to_pages(size_class));
    size_t objects_per_span = npages.in_bytes() / size;

    int res = posix_memalign(&mem_, kPageSize, npages.in_bytes());
    CHECK_CONDITION(res == 0);
    span_.Init(PageIdContaining(mem_), npages);
    span_.BuildFreelist(size, objects_per_span, nullptr, 0);
  }

  ~RawSpan() { free(mem_); }

  Span& span() { return span_; }

 private:
  void* mem_ = nullptr;
  Span span_;
};

class SpanTest : public testing::TestWithParam<size_t> {
 protected:
  size_t size_class_;
  size_t size_;
  size_t npages_;
  size_t batch_size_;
  size_t objects_per_span_;
  RawSpan raw_span_;

 private:
  void SetUp() override {
    size_class_ = GetParam();
    size_ = tc_globals.sizemap().class_to_size(size_class_);
    if (size_ == 0) {
      GTEST_SKIP() << "Skipping empty size class.";
    }

    npages_ = tc_globals.sizemap().class_to_pages(size_class_);
    batch_size_ = tc_globals.sizemap().num_objects_to_move(size_class_);
    objects_per_span_ = npages_ * kPageSize / size_;

    raw_span_.Init(size_class_);
  }

  void TearDown() override {}
};

TEST_P(SpanTest, FreelistBasic) {
  Span& span_ = raw_span_.span();

  EXPECT_FALSE(span_.FreelistEmpty(size_));
  void* batch[kMaxObjectsToMove];
  size_t popped = 0;
  size_t want = 1;
  char* start = static_cast<char*>(span_.start_address());
  std::vector<bool> objects(objects_per_span_);
  for (size_t x = 0; x < 2; ++x) {
    // Pop all objects in batches of varying size and ensure that we've got
    // all objects.
    for (;;) {
      size_t n = span_.FreelistPopBatch(batch, want, size_);
      popped += n;
      EXPECT_NEAR(
          span_.Fragmentation(size_),
          static_cast<double>(objects_per_span_) / static_cast<double>(popped) -
              1.,
          1e-5);
      EXPECT_EQ(span_.FreelistEmpty(size_), popped == objects_per_span_);
      for (size_t i = 0; i < n; ++i) {
        void* p = batch[i];
        uintptr_t off = reinterpret_cast<char*>(p) - start;
        EXPECT_LT(off, span_.bytes_in_span());
        EXPECT_EQ(off % size_, 0);
        size_t idx = off / size_;
        EXPECT_FALSE(objects[idx]);
        objects[idx] = true;
      }
      if (n < want) {
        break;
      }
      ++want;
      if (want > batch_size_) {
        want = 1;
      }
    }
    EXPECT_TRUE(span_.FreelistEmpty(size_));
    EXPECT_EQ(span_.FreelistPopBatch(batch, 1, size_), 0);
    EXPECT_EQ(popped, objects_per_span_);

    // Push all objects back except the last one (which would not be pushed).
    for (size_t idx = 0; idx < objects_per_span_ - 1; ++idx) {
      EXPECT_TRUE(objects[idx]);
      bool ok = span_.FreelistPush(start + idx * size_, size_);
      EXPECT_TRUE(ok);
      EXPECT_FALSE(span_.FreelistEmpty(size_));
      objects[idx] = false;
      --popped;
    }
    // On the last iteration we can actually push the last object.
    if (x == 1) {
      bool ok =
          span_.FreelistPush(start + (objects_per_span_ - 1) * size_, size_);
      EXPECT_FALSE(ok);
    }
  }
}

TEST_P(SpanTest, FreelistRandomized) {
  Span& span_ = raw_span_.span();

  char* start = static_cast<char*>(span_.start_address());

  // Do a bunch of random pushes/pops with random batch size.
  absl::BitGen rng;
  absl::flat_hash_set<void*> objects;
  void* batch[kMaxObjectsToMove];
  for (size_t x = 0; x < 10000; ++x) {
    if (!objects.empty() && absl::Bernoulli(rng, 1.0 / 2)) {
      void* p = *objects.begin();
      if (span_.FreelistPush(p, size_)) {
        objects.erase(objects.begin());
      } else {
        EXPECT_EQ(objects.size(), 1);
      }
      EXPECT_EQ(span_.FreelistEmpty(size_), objects_per_span_ == 1);
    } else {
      size_t want = absl::Uniform<int32_t>(rng, 0, batch_size_) + 1;
      size_t n = span_.FreelistPopBatch(batch, want, size_);
      if (n < want) {
        EXPECT_TRUE(span_.FreelistEmpty(size_));
      }
      for (size_t i = 0; i < n; ++i) {
        EXPECT_TRUE(objects.insert(batch[i]).second);
      }
    }
  }
  // Now pop everything what's there.
  for (;;) {
    size_t n = span_.FreelistPopBatch(batch, batch_size_, size_);
    for (size_t i = 0; i < n; ++i) {
      EXPECT_TRUE(objects.insert(batch[i]).second);
    }
    if (n < batch_size_) {
      break;
    }
  }
  // Check that we have collected all objects.
  EXPECT_EQ(objects.size(), objects_per_span_);
  for (void* p : objects) {
    uintptr_t off = reinterpret_cast<char*>(p) - start;
    EXPECT_LT(off, span_.bytes_in_span());
    EXPECT_EQ(off % size_, 0);
  }
}

INSTANTIATE_TEST_SUITE_P(All, SpanTest, testing::Range(size_t(1), kNumClasses));

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
