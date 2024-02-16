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

#ifndef TCMALLOC_MOCK_STATIC_FORWARDER_H_
#define TCMALLOC_MOCK_STATIC_FORWARDER_H_

#include <map>
#include <new>

#include "gmock/gmock.h"
#include "absl/synchronization/mutex.h"
#include "absl/types/span.h"
#include "tcmalloc/mock_transfer_cache.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/span.h"

namespace tcmalloc {
namespace tcmalloc_internal {

class FakeStaticForwarder {
 public:
  static constexpr size_t class_to_size(int size_class) { return kClassSize; }
  static constexpr Length class_to_pages(int size_class) { return Length(1); }

  Span* MapObjectToSpan(const void* object) {
    const PageId page = PageIdContaining(object);

    absl::MutexLock l(&mu_);
    auto it = map_.lower_bound(page);
    if (it->first != page && it != map_.begin()) {
      --it;
    }

    if (it->first <= page && page <= it->second.span->last_page()) {
      return it->second.span;
    }

    return nullptr;
  }

  Span* AllocateSpan(int, size_t objects_per_span, Length pages_per_span) {
    void* backing =
        ::operator new(pages_per_span.in_bytes(), std::align_val_t(kPageSize));
    PageId page = PageIdContaining(backing);

    auto* span = new Span();
    span->Init(page, pages_per_span);

    absl::MutexLock l(&mu_);
    SpanInfo info;
    info.span = span;
    info.objects_per_span = objects_per_span;
    map_.emplace(page, info);
    return span;
  }

  void DeallocateSpans(int, size_t objects_per_span,
                       absl::Span<Span*> free_spans) {
    {
      absl::MutexLock l(&mu_);
      for (Span* span : free_spans) {
        auto it = map_.find(span->first_page());
        EXPECT_NE(it, map_.end());
        EXPECT_EQ(it->second.objects_per_span, objects_per_span);
        map_.erase(it);
      }
    }

    for (Span* span : free_spans) {
      ::operator delete(span->start_address(), std::align_val_t(kPageSize));
      delete span;
    }
  }

 private:
  struct SpanInfo {
    Span* span;
    size_t objects_per_span;
  };

  absl::Mutex mu_;
  std::map<PageId, SpanInfo> map_ ABSL_GUARDED_BY(mu_);
};

class RawMockStaticForwarder : public FakeStaticForwarder {
 public:
  RawMockStaticForwarder() {
    ON_CALL(*this, MapObjectToSpan).WillByDefault([this](const void* object) {
      return static_cast<FakeStaticForwarder*>(this)->MapObjectToSpan(object);
    });
    ON_CALL(*this, AllocateSpan)
        .WillByDefault([this](int size_class, size_t objects_per_span,
                              Length pages_per_span) {
          return static_cast<FakeStaticForwarder*>(this)->AllocateSpan(
              size_class, objects_per_span, pages_per_span);
        });
    ON_CALL(*this, DeallocateSpans)
        .WillByDefault([this](int size_class, size_t objects_per_span,
                              absl::Span<Span*> free_spans) {
          static_cast<FakeStaticForwarder*>(this)->DeallocateSpans(
              size_class, objects_per_span, free_spans);
        });
  }

  MOCK_METHOD(Span*, MapObjectToSpan, (const void* object));
  MOCK_METHOD(Span*, AllocateSpan,
              (int size_class, size_t objects_per_span, Length pages_per_span));
  MOCK_METHOD(void, DeallocateSpans,
              (int size_class, size_t objects_per_span,
               absl::Span<Span*> free_spans));
};

using MockStaticForwarder = testing::NiceMock<RawMockStaticForwarder>;

// Wires up a largely functional CentralFreeList + MockStaticForwarder.
//
// By default, it fills allocations and responds sensibly.  Because it backs
// onto malloc/free, it will detect leaks and memory misuse when run under
// sanitizers.
//
// Exposes the underlying mocks to allow for more whitebox tests.
template <typename CentralFreeListT>
class FakeCentralFreeListEnvironment {
 public:
  using CentralFreeList = CentralFreeListT;
  using Forwarder = typename CentralFreeListT::Forwarder;

  static constexpr int kSizeClass = 1;
  static constexpr size_t kBatchSize = kMaxObjectsToMove;
  static constexpr size_t kObjectsPerSpan =
      Forwarder::class_to_pages(kSizeClass).in_bytes() /
      Forwarder::class_to_size(kSizeClass);

  FakeCentralFreeListEnvironment() { cache_.Init(kSizeClass); }

  ~FakeCentralFreeListEnvironment() { EXPECT_EQ(cache_.length(), 0); }

  CentralFreeList& central_freelist() { return cache_; }

  Forwarder& forwarder() { return cache_.forwarder(); }

 private:
  CentralFreeList cache_;
};

}  // namespace tcmalloc_internal
}  // namespace tcmalloc

#endif  // TCMALLOC_MOCK_STATIC_FORWARDER_H_
