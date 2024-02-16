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

#ifndef TCMALLOC_TESTING_TEST_ALLOCATOR_HARNESS_H_
#define TCMALLOC_TESTING_TEST_ALLOCATOR_HARNESS_H_

#include <random>

#include "gtest/gtest.h"
#include "absl/base/optimization.h"
#include "absl/random/random.h"
#include "absl/synchronization/mutex.h"
#include "tcmalloc/testing/testutil.h"

namespace tcmalloc {

struct Object {
  Object() : ptr(nullptr), size(0), generation(0) {}

  Object(const Object& o) = delete;
  Object(Object&& o)
      : ptr(absl::exchange(o.ptr, nullptr)),
        size(absl::exchange(o.size, 0)),
        generation(absl::exchange(o.generation, 0)) {}

  Object& operator=(const Object& o) = delete;
  Object& operator=(Object&& o) {
    using std::swap;
    swap(ptr, o.ptr);
    swap(size, o.size);
    swap(generation, o.generation);

    return *this;
  }

  void* ptr;       // Allocated pointer
  int size;        // Allocated size
  int generation;  // Generation counter of object contents
};

struct ABSL_CACHELINE_ALIGNED State {
  Object RemoveRandomObject() {
    size_t index = absl::Uniform<size_t>(rng, 0, owned.size());

    using std::swap;
    swap(owned[index], owned.back());
    Object to_remove = std::move(owned.back());
    owned.pop_back();

    return to_remove;
  }

  // These objects are accessed exclusively by a single thread and do not
  // require locking.
  absl::BitGen rng;
  std::vector<Object> owned;

  // Other threads can pass us objects.
  absl::Mutex ABSL_CACHELINE_ALIGNED lock;
  std::vector<Object> inbound ABSL_GUARDED_BY(lock);
};

class AllocatorHarness {
 public:
  explicit AllocatorHarness(int nthreads)
      : nthreads_(nthreads),
        state_(nthreads),
        bytes_available_(nthreads * kSizePerThread) {}

  ~AllocatorHarness() {
    for (auto& state : state_) {
      for (auto& o : state.owned) {
        sized_delete(o.ptr, o.size);
      }

      absl::MutexLock m(&state.lock);
      for (auto& o : state.inbound) {
        sized_delete(o.ptr, o.size);
      }
    }
  }

  void Run(int thread_id) {
    // Take ownership of inbound objects.
    auto& state = state_[thread_id];

    std::vector<Object> tmp;
    {
      absl::MutexLock m(&state.lock);
      tmp.swap(state.inbound);
    }

    state.owned.reserve(state.owned.size() + tmp.size());
    for (auto& o : tmp) {
      state.owned.push_back(std::move(o));
    }
    tmp.clear();

    const double coin = absl::Uniform(state.rng, 0., 1.);
    if (coin < 0.45) {
      // Allocate
      size_t size = absl::LogUniform<size_t>(state.rng, 1, kMaxTestSize);

      bool success = false;
      {
        absl::MutexLock m(&lock_);
        if (bytes_available_ >= size) {
          bytes_available_ -= size;
          success = true;
        }
      }

      if (success) {
        Object o;
        o.ptr = ::operator new(size);
        o.size = size;

        FillContents(o);

        state.owned.push_back(std::move(o));
        return;
      }

      // Fall through to try deallocating.
    }

    if (state.owned.empty()) {
      return;
    }

    if (coin < 0.9) {
      // Deallocate
      Object to_delete = state.RemoveRandomObject();

      CheckContents(to_delete);

      sized_delete(to_delete.ptr, to_delete.size);

      absl::MutexLock m(&lock_);
      bytes_available_ += to_delete.size;
      return;
    } else if (coin < 0.92) {
      // Update an object.
      size_t index = absl::Uniform(state.rng, 0u, state.owned.size());

      auto& obj = state.owned[index];
      CheckContents(obj);
      obj.generation++;
      FillContents(obj);
    } else {
      // Hand an object to another thread.
      int thread = absl::Uniform(state.rng, 0, nthreads_);

      Object to_transfer = state.RemoveRandomObject();

      auto& remote_state = state_[thread];
      absl::MutexLock m(&remote_state.lock);
      remote_state.inbound.push_back(std::move(to_transfer));
    }
  }

 private:
  // Fill object contents according to ptr/generation
  void FillContents(Object& object) {
    std::mt19937 r(reinterpret_cast<intptr_t>(object.ptr) & 0x7fffffff);
    for (int i = 0; i < object.generation; ++i) {
      absl::Uniform<uint32_t>(r);
    }
    const char c = absl::Uniform<char>(r, CHAR_MIN, CHAR_MAX);
    memset(object.ptr, c, std::min(ABSL_CACHELINE_SIZE, object.size));
    if (object.size > ABSL_CACHELINE_SIZE) {
      memset(static_cast<char*>(object.ptr) + object.size - ABSL_CACHELINE_SIZE,
             c, ABSL_CACHELINE_SIZE);
    }
  }

  // Check object contents
  void CheckContents(const Object& object) {
    // We use a fixed, seeded RNG to ensure determinism when different threads
    // compute the expected contents.
    std::mt19937 r(reinterpret_cast<intptr_t>(object.ptr) & 0x7fffffff);
    for (int i = 0; i < object.generation; ++i) {
      absl::Uniform<uint32_t>(r);
    }

    // For large objects, we just check a prefix/suffix
    const char expected = absl::Uniform<char>(r, CHAR_MIN, CHAR_MAX);
    const int limit1 = std::min(object.size, ABSL_CACHELINE_SIZE);
    const int start2 = std::max(limit1, object.size - ABSL_CACHELINE_SIZE);
    for (int i = 0; i < limit1; ++i) {
      ASSERT_EQ(static_cast<const char*>(object.ptr)[i], expected);
    }
    for (int i = start2; i < object.size; ++i) {
      ASSERT_EQ(static_cast<const char*>(object.ptr)[i], expected);
    }
  }

  static constexpr size_t kMaxTestSize = 1 << 16;
  static constexpr size_t kSizePerThread = 4 << 20;

  int nthreads_;
  std::vector<State> state_;

  ABSL_CACHELINE_ALIGNED absl::Mutex lock_;
  size_t bytes_available_ ABSL_GUARDED_BY(lock_);
};

}  // namespace tcmalloc

#endif  // TCMALLOC_TESTING_TEST_ALLOCATOR_HARNESS_H_
