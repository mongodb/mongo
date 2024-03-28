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

#include <stdlib.h>

#include <algorithm>
#include <vector>

#include "absl/random/random.h"
#include "benchmark/benchmark.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/linked_list.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/mock_span.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

void BM_PushPop(benchmark::State& state) {
  const int pointers = state.range(0);
  const int sequential_calls = state.range(1);

  LinkedList list;
  const size_t size = pointers * sizeof(void*);

  std::vector<void*> v(sequential_calls);
  for (int i = 0; i < sequential_calls; i++) {
    v[i] = malloc(size);
  }
  std::shuffle(v.begin(), v.end(), absl::BitGen());

  for (auto s : state) {
    // Push sequential_calls times.
    for (int j = 0; j < sequential_calls; j++) {
      list.Push(v[j]);
    }

    // Pop sequential_calls times.
    for (int j = 0; j < sequential_calls; j++) {
      void* ret;
      list.TryPop(&ret);
    }
  }

  state.SetItemsProcessed(sequential_calls * state.iterations());

  for (int i = 0; i < sequential_calls; i++) {
    free(v[i]);
  }
}
BENCHMARK(BM_PushPop)->RangePair(1, 64, 1, 32);

void BM_PushPopBatch(benchmark::State& state) {
  const int pointers = state.range(0);
  const int batch_size = state.range(1);

  LinkedList list;
  const size_t size = pointers * sizeof(void*);

  const int kNumberOfObjects = 64 << 10;
  std::vector<void*> v(kNumberOfObjects);
  for (int i = 0; i < kNumberOfObjects; i++) {
    v[i] = malloc(size);
  }
  std::shuffle(v.begin(), v.end(), absl::BitGen());

  const int kMaxObjectsToMove = 32;
  void* batch[kMaxObjectsToMove];

  for (auto s : state) {
    // PushBatch
    for (int j = 0; j < kNumberOfObjects / batch_size; j++) {
      list.PushBatch(batch_size, v.data() + j * batch_size);
    }

    // PopBatch.
    for (int j = 0; j < kNumberOfObjects / batch_size; j++) {
      list.PopBatch(batch_size, batch);
    }
  }

  state.SetItemsProcessed((kNumberOfObjects / batch_size) * batch_size *
                          state.iterations());

  for (int i = 0; i < kNumberOfObjects; i++) {
    free(v[i]);
  }
}
BENCHMARK(BM_PushPopBatch)->RangePair(1, 64, 1, 32);

static void BM_AppendRemove(benchmark::State& state) {
  MockSpanList list;

  int sequential_calls = state.range(0);

  std::vector<MockSpan*> vappend(sequential_calls);

  // Create MockSpans in append order
  for (int i = 0; i < sequential_calls; i++) {
    MockSpan* s = MockSpan::New(i);
    TC_CHECK_NE(s, nullptr);
    vappend[i] = s;
  }

  // Remove all sequential_calls elements from the list in a random order
  std::vector<MockSpan*> vremove(sequential_calls);
  vremove = vappend;
  std::shuffle(vremove.begin(), vremove.end(), absl::BitGen());

  for (auto _ : state) {
    // Append sequential_calls elements to the list.
    for (MockSpan* s : vappend) {
      list.append(s);
    }

    // Remove in a random order
    for (MockSpan* s : vremove) {
      list.remove(s);
    }
  }

  for (MockSpan* s : vappend) {
    delete s;
  }
}

BENCHMARK(BM_AppendRemove)->Range(32, 32 * 1024);

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
