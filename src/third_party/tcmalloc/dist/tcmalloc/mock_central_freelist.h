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

#ifndef TCMALLOC_MOCK_CENTRAL_FREELIST_H_
#define TCMALLOC_MOCK_CENTRAL_FREELIST_H_

#include <stddef.h>

#include "gmock/gmock.h"
#include "absl/base/internal/spinlock.h"
#include "absl/types/span.h"
#include "tcmalloc/central_freelist.h"

namespace tcmalloc {
namespace tcmalloc_internal {

// CentralFreeList implementation that uses a real central freelist to allocate
// objects. It implements additional methods used by benchmarks and tests.
//
// This is useful for benchmarking in cases where, for instance, we can test the
// efficiency of TCMalloc's frontend as it has to access real central freelist
// upon a miss.
class RealCentralFreeListForTesting : public CentralFreeList {
 public:
  void AllocateBatch(void** batch, int n);
  void FreeBatch(absl::Span<void*> batch);
};

class FakeCentralFreeListBase {
 public:
  FakeCentralFreeListBase() {}
  FakeCentralFreeListBase(const FakeCentralFreeListBase&) = delete;
  FakeCentralFreeListBase& operator=(const FakeCentralFreeListBase&) = delete;

  static constexpr void Init(size_t) {}
};

// CentralFreeList implementation that backs onto the system's malloc.
//
// Useful for unit tests and fuzz tests where identifying leaks and correctness
// is important.
class FakeCentralFreeList : public FakeCentralFreeListBase {
 public:
  void InsertRange(absl::Span<void*> batch);
  int RemoveRange(void** batch, int N);

  void AllocateBatch(void** batch, int n);
  void FreeBatch(absl::Span<void*> batch);
};

// CentralFreeList implementation that does minimal work but no correctness
// checking.
//
// Useful for benchmarks where you want to avoid unrelated expensive operations.
class MinimalFakeCentralFreeList : public FakeCentralFreeListBase {
 public:
  void InsertRange(absl::Span<void*> batch);
  int RemoveRange(void** batch, int N);

  void AllocateBatch(void** batch, int n);
  void FreeBatch(absl::Span<void*> batch);

 private:
  absl::base_internal::SpinLock lock_;
};

// CentralFreeList implementation that allows intercepting specific calls.  By
// default backs onto the system's malloc.
//
// Useful for intrusive unit tests that want to verify internal behavior.
class RawMockCentralFreeList : public FakeCentralFreeList {
 public:
  RawMockCentralFreeList() : FakeCentralFreeList() {
    ON_CALL(*this, InsertRange).WillByDefault([this](absl::Span<void*> batch) {
      return static_cast<FakeCentralFreeList*>(this)->InsertRange(batch);
    });
    ON_CALL(*this, RemoveRange).WillByDefault([this](void** batch, int n) {
      return static_cast<FakeCentralFreeList*>(this)->RemoveRange(batch, n);
    });
  }

  MOCK_METHOD(void, InsertRange, (absl::Span<void*> batch));
  MOCK_METHOD(int, RemoveRange, (void** batch, int N));
};

using MockCentralFreeList = testing::NiceMock<RawMockCentralFreeList>;

}  // namespace tcmalloc_internal
}  // namespace tcmalloc

#endif  // TCMALLOC_MOCK_CENTRAL_FREELIST_H_
