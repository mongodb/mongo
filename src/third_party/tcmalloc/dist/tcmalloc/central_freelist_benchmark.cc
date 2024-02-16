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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/random/random.h"
#include "benchmark/benchmark.h"
#include "tcmalloc/central_freelist.h"
#include "tcmalloc/common.h"
#include "tcmalloc/static_vars.h"
#include "tcmalloc/tcmalloc_policy.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

// This benchmark measures how long it takes to populate multiple
// spans. The spans are freed in the same order as they were populated
// to minimize the time it takes to free them.
void BM_Populate(benchmark::State& state) {
  size_t object_size = state.range(0);
  size_t size_class = tc_globals.sizemap().SizeClass(CppPolicy(), object_size);
  int batch_size = tc_globals.sizemap().num_objects_to_move(size_class);
  int num_objects = 64 * 1024 * 1024 / object_size;
  const int num_batches = num_objects / batch_size;
  CentralFreeList cfl;
  // Initialize the span to contain the appropriate size of object.
  cfl.Init(size_class);

  // Allocate an array large enough to hold 64 MiB of objects.
  std::vector<void*> buffer(num_objects);
  int64_t items_processed = 0;
  absl::BitGen rnd;

  while (state.KeepRunningBatch(num_batches)) {
    int index = 0;
    // The cost of fetching objects will include the cost of fetching and
    // populating the span.
    while (index < num_objects) {
      int count = std::min(batch_size, num_objects - index);
      int got = cfl.RemoveRange(&buffer[index], count);
      index += got;
    }

    // Don't include the cost of returning the objects to the span, and the
    // span to the pageheap.
    state.PauseTiming();
    index = 0;
    while (index < num_objects) {
      uint64_t count = std::min(batch_size, num_objects - index);
      cfl.InsertRange({&buffer[index], count});
      index += count;
    }
    items_processed += index;
    state.ResumeTiming();
  }
  state.SetItemsProcessed(items_processed);
}
BENCHMARK(BM_Populate)
    ->DenseRange(8, 64, 16)
    ->DenseRange(64, 1024, 64)
    ->DenseRange(4096, 28 * 1024, 4096)
    ->DenseRange(32 * 1024, 256 * 1024, 32 * 1024);

// This benchmark fills a large array with objects, shuffles the objects
// and then returns them.
// This should be relatively representative of what happens at runtime.
// Fetching objects from the CFL is usually done in batches, but returning
// them is usually done spread over many active spans.
void BM_MixAndReturn(benchmark::State& state) {
  size_t object_size = state.range(0);
  size_t size_class = tc_globals.sizemap().SizeClass(CppPolicy(), object_size);
  int batch_size = tc_globals.sizemap().num_objects_to_move(size_class);
  int num_objects = 64 * 1024 * 1024 / object_size;
  const int num_batches = num_objects / batch_size;
  CentralFreeList cfl;
  // Initialize the span to contain the appropriate size of object.
  cfl.Init(size_class);

  // Allocate an array large enough to hold 64 MiB of objects.
  std::vector<void*> buffer(num_objects);
  int64_t items_processed = 0;
  absl::BitGen rnd;

  while (state.KeepRunningBatch(num_batches)) {
    int index = 0;
    while (index < num_objects) {
      int count = std::min(batch_size, num_objects - index);
      int got = cfl.RemoveRange(&buffer[index], count);
      index += got;
    }

    state.PauseTiming();
    // Shuffle the vector so that we don't return the objects in the same
    // order as they were allocated.
    absl::c_shuffle(buffer, rnd);
    state.ResumeTiming();

    index = 0;
    while (index < num_objects) {
      unsigned int count = std::min(batch_size, num_objects - index);
      cfl.InsertRange({&buffer[index], count});
      index += count;
    }
    items_processed += index;
  }
  state.SetItemsProcessed(items_processed);
}
BENCHMARK(BM_MixAndReturn)
    ->DenseRange(8, 64, 16)
    ->DenseRange(64, 1024, 64)
    ->DenseRange(4096, 28 * 1024, 4096)
    ->DenseRange(32 * 1024, 256 * 1024, 32 * 1024);

// This benchmark holds onto half the allocated objects so that (except for
// single object spans) spans are never allocated or freed during the
// benchmark run. This evaluates the performance of just the span handling
// code, and avoids timing the pageheap code.
void BM_SpanReuse(benchmark::State& state) {
  size_t object_size = state.range(0);
  size_t size_class = tc_globals.sizemap().SizeClass(CppPolicy(), object_size);
  int batch_size = tc_globals.sizemap().num_objects_to_move(size_class);
  int num_objects = 64 * 1024 * 1024 / object_size;
  const int num_batches = num_objects / batch_size;
  CentralFreeList cfl;
  // Initialize the span to contain the appropriate size of object.
  cfl.Init(size_class);

  // Array used to hold onto half of the objects
  std::vector<void*> held_objects(2 * num_objects);
  // Request twice the objects we need
  for (int index = 0; index < 2 * num_objects;) {
    int count = std::min(batch_size, 2 * num_objects - index);
    int got = cfl.RemoveRange(&held_objects[index], count);
    index += got;
  }

  // Return half of the objects. This will stop the spans from being
  // returned to the pageheap. So future operations will not touch the
  // pageheap.
  for (int index = 0; index < 2 * num_objects; index += 2) {
    cfl.InsertRange({&held_objects[index], 1});
  }
  // Allocate an array large enough to hold 64 MiB of objects.
  std::vector<void*> buffer(num_objects);
  int64_t items_processed = 0;
  absl::BitGen rnd;

  while (state.KeepRunningBatch(num_batches)) {
    int index = 0;
    while (index < num_objects) {
      int count = std::min(batch_size, num_objects - index);
      int got = cfl.RemoveRange(&buffer[index], count);
      index += got;
    }

    state.PauseTiming();
    // Shuffle the vector so that we don't return the objects in the same
    // order as they were allocated.
    absl::c_shuffle(buffer, rnd);
    state.ResumeTiming();

    index = 0;
    while (index < num_objects) {
      uint64_t count = std::min(batch_size, num_objects - index);
      cfl.InsertRange({&buffer[index], count});
      index += count;
    }
    items_processed += index;
  }
  state.SetItemsProcessed(items_processed);

  // Return the other half of the objects.
  for (int index = 1; index < 2 * num_objects; index += 2) {
    cfl.InsertRange({&held_objects[index], 1});
  }
}
// Want to avoid benchmarking spans where there is a single object per span.
BENCHMARK(BM_SpanReuse)
    ->DenseRange(8, 64, 16)
    ->DenseRange(64, 1024, 64)
    ->DenseRange(1024, 4096, 512);

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
