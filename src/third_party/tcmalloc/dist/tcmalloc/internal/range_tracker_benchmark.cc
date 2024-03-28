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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/random/distributions.h"
#include "absl/random/random.h"
#include "benchmark/benchmark.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/range_tracker.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

struct RangeInfo {
  size_t index;
  size_t len;
};

template <size_t N>
static void BM_MarkUnmark(benchmark::State& state) {
  RangeTracker<N> range;
  absl::BitGen rng;
  std::vector<RangeInfo> things;
  while (range.used() < N / 2) {
    size_t len =
        absl::LogUniform<int32_t>(rng, 0, range.longest_free() - 1) + 1;
    size_t i = range.FindAndMark(len);
    things.push_back({i, len});
  }

  // only count successes :/
  for (auto s : state) {
    size_t index = absl::Uniform<int32_t>(rng, 0, things.size());
    auto p = things[index];
    range.Unmark(p.index, p.len);
    size_t len =
        absl::LogUniform<int32_t>(rng, 0, range.longest_free() - 1) + 1;
    things[index] = {range.FindAndMark(len), len};
  }

  state.SetItemsProcessed(state.iterations());
}

BENCHMARK_TEMPLATE(BM_MarkUnmark, 256);
BENCHMARK_TEMPLATE(BM_MarkUnmark, 256 * 32);

template <size_t N, size_t K>
static void BM_MarkUnmarkEmpty(benchmark::State& state) {
  RangeTracker<N> range;
  absl::BitGen rng;
  for (auto s : state) {
    size_t index = range.FindAndMark(K);
    benchmark::DoNotOptimize(index);
    range.Unmark(index, K);
  }

  state.SetItemsProcessed(state.iterations());
}

BENCHMARK_TEMPLATE(BM_MarkUnmarkEmpty, 256, 1);
BENCHMARK_TEMPLATE(BM_MarkUnmarkEmpty, 256 * 32, 1);
BENCHMARK_TEMPLATE(BM_MarkUnmarkEmpty, 256, 128);
BENCHMARK_TEMPLATE(BM_MarkUnmarkEmpty, 256 * 32, 256 * 16);
BENCHMARK_TEMPLATE(BM_MarkUnmarkEmpty, 256, 256);
BENCHMARK_TEMPLATE(BM_MarkUnmarkEmpty, 256 * 32, 256 * 32);

template <size_t N>
static void BM_MarkUnmarkChunks(benchmark::State& state) {
  RangeTracker<N> range;
  range.FindAndMark(N);
  size_t index = 0;
  absl::BitGen rng;
  while (index < N) {
    size_t len = absl::Uniform<int32_t>(rng, 0, 32) + 1;
    len = std::min(len, N - index);
    size_t drop = absl::Uniform<int32_t>(rng, 0, len);
    if (drop > 0) {
      range.Unmark(index, drop);
    }
    index += len;
  }
  size_t m = range.longest_free();
  for (auto s : state) {
    size_t index = range.FindAndMark(m);
    benchmark::DoNotOptimize(index);
    range.Unmark(index, m);
  }

  state.SetItemsProcessed(state.iterations());
}

BENCHMARK_TEMPLATE(BM_MarkUnmarkChunks, 64);
BENCHMARK_TEMPLATE(BM_MarkUnmarkChunks, 256);
BENCHMARK_TEMPLATE(BM_MarkUnmarkChunks, 256 * 32);

template <size_t N>
static void BM_FillOnes(benchmark::State& state) {
  RangeTracker<N> range;
  while (state.KeepRunningBatch(N)) {
    state.PauseTiming();
    range.Clear();
    state.ResumeTiming();
    for (size_t j = 0; j < N; ++j) {
      benchmark::DoNotOptimize(range.FindAndMark(1));
    }
  }

  state.SetItemsProcessed(N * state.iterations());
}

BENCHMARK_TEMPLATE(BM_FillOnes, 256);
BENCHMARK_TEMPLATE(BM_FillOnes, 256 * 32);

template <size_t N>
static void BM_EmptyOnes(benchmark::State& state) {
  RangeTracker<N> range;
  while (state.KeepRunningBatch(N)) {
    state.PauseTiming();
    range.Clear();
    range.FindAndMark(N);
    state.ResumeTiming();
    for (size_t j = 0; j < N; ++j) {
      range.Unmark(j, 1);
    }
  }

  state.SetItemsProcessed(N * state.iterations());
}

BENCHMARK_TEMPLATE(BM_EmptyOnes, 256);
BENCHMARK_TEMPLATE(BM_EmptyOnes, 256 * 32);

enum SearchDirection {
  Forward,
  Backward,
};

template <size_t N, bool Goal, SearchDirection Dir>
ABSL_ATTRIBUTE_NOINLINE size_t ExamineDoFind(Bitmap<N>* map, size_t index) {
  if (Dir == Forward) {
    if (Goal) {
      return map->FindSet(index);
    } else {
      return map->FindClear(index);
    }
  } else {
    if (Goal) {
      return map->FindSetBackwards(index);
    } else {
      return map->FindClearBackwards(index);
    }
  }
}

template <size_t N, bool Goal, SearchDirection Dir>
ABSL_ATTRIBUTE_NOINLINE void DoSearchBenchmark(Bitmap<N>* map,
                                               benchmark::State& state) {
  if (Dir == Forward) {
    size_t index = 0;
    for (auto s : state) {
      index = ExamineDoFind<N, Goal, Dir>(map, index);
      benchmark::DoNotOptimize(index);
      index++;
      if (index >= N) index = 0;
    }
  } else {
    ssize_t index = N - 1;
    for (auto s : state) {
      index = ExamineDoFind<N, Goal, Dir>(map, index);
      benchmark::DoNotOptimize(index);
      index--;
      if (index < 0) index = N - 1;
    }
  }
}

template <size_t N, bool Goal, SearchDirection Dir>
static void BM_FindEmpty(benchmark::State& state) {
  Bitmap<N> set;
  // Volatile set/clears prevent the compiler from const-propagating the whole
  // search.
  volatile size_t to_set = 0;
  volatile size_t to_clear = 0;
  set.SetBit(to_set);
  set.ClearBit(to_clear);
  DoSearchBenchmark<N, Goal, Dir>(&set, state);
}

BENCHMARK_TEMPLATE(BM_FindEmpty, 64, false, Forward);
BENCHMARK_TEMPLATE(BM_FindEmpty, 64, false, Backward);
BENCHMARK_TEMPLATE(BM_FindEmpty, 64, true, Forward);
BENCHMARK_TEMPLATE(BM_FindEmpty, 64, true, Backward);
BENCHMARK_TEMPLATE(BM_FindEmpty, 256, false, Forward);
BENCHMARK_TEMPLATE(BM_FindEmpty, 256, false, Backward);
BENCHMARK_TEMPLATE(BM_FindEmpty, 256, true, Forward);
BENCHMARK_TEMPLATE(BM_FindEmpty, 256, true, Backward);
BENCHMARK_TEMPLATE(BM_FindEmpty, 256 * 32, false, Forward);
BENCHMARK_TEMPLATE(BM_FindEmpty, 256 * 32, false, Backward);
BENCHMARK_TEMPLATE(BM_FindEmpty, 256 * 32, true, Forward);
BENCHMARK_TEMPLATE(BM_FindEmpty, 256 * 32, true, Backward);

template <size_t N, bool Goal, SearchDirection Dir>
static void BM_FindLast(benchmark::State& state) {
  Bitmap<N> set;
  volatile size_t to_set = 0;
  volatile size_t to_clear = 0;
  set.SetBit(to_set);
  set.ClearBit(to_clear);
  set.SetBit(N - 1);
  DoSearchBenchmark<N, Goal, Dir>(&set, state);
}

BENCHMARK_TEMPLATE(BM_FindLast, 64, false, Forward);
BENCHMARK_TEMPLATE(BM_FindLast, 64, false, Backward);
BENCHMARK_TEMPLATE(BM_FindLast, 64, true, Forward);
BENCHMARK_TEMPLATE(BM_FindLast, 64, true, Backward);
BENCHMARK_TEMPLATE(BM_FindLast, 256, false, Forward);
BENCHMARK_TEMPLATE(BM_FindLast, 256, false, Backward);
BENCHMARK_TEMPLATE(BM_FindLast, 256, true, Forward);
BENCHMARK_TEMPLATE(BM_FindLast, 256, true, Backward);
BENCHMARK_TEMPLATE(BM_FindLast, 256 * 32, false, Forward);
BENCHMARK_TEMPLATE(BM_FindLast, 256 * 32, false, Backward);
BENCHMARK_TEMPLATE(BM_FindLast, 256 * 32, true, Forward);
BENCHMARK_TEMPLATE(BM_FindLast, 256 * 32, true, Backward);

template <size_t N, bool Goal, SearchDirection Dir>
static void BM_FindFull(benchmark::State& state) {
  Bitmap<N> set;
  set.SetRange(0, N);
  volatile size_t to_set = 0;
  volatile size_t to_clear = 0;
  set.SetBit(to_set);
  set.ClearBit(to_clear);
  DoSearchBenchmark<N, Goal, Dir>(&set, state);
}

BENCHMARK_TEMPLATE(BM_FindFull, 64, false, Forward);
BENCHMARK_TEMPLATE(BM_FindFull, 64, false, Backward);
BENCHMARK_TEMPLATE(BM_FindFull, 64, true, Forward);
BENCHMARK_TEMPLATE(BM_FindFull, 64, true, Backward);
BENCHMARK_TEMPLATE(BM_FindFull, 256, false, Forward);
BENCHMARK_TEMPLATE(BM_FindFull, 256, false, Backward);
BENCHMARK_TEMPLATE(BM_FindFull, 256, true, Forward);
BENCHMARK_TEMPLATE(BM_FindFull, 256, true, Backward);
BENCHMARK_TEMPLATE(BM_FindFull, 256 * 32, false, Forward);
BENCHMARK_TEMPLATE(BM_FindFull, 256 * 32, false, Backward);
BENCHMARK_TEMPLATE(BM_FindFull, 256 * 32, true, Forward);
BENCHMARK_TEMPLATE(BM_FindFull, 256 * 32, true, Backward);

template <size_t N, bool Goal, SearchDirection Dir>
static void BM_FindRandom(benchmark::State& state) {
  Bitmap<N> set;
  volatile size_t to_set = 0;
  volatile size_t to_clear = 0;
  set.SetBit(to_set);
  set.ClearBit(to_clear);
  absl::BitGen rng;
  for (int i = 0; i < N; ++i) {
    if (absl::Bernoulli(rng, 1.0 / 2)) set.SetBit(i);
  }
  DoSearchBenchmark<N, Goal, Dir>(&set, state);
}

BENCHMARK_TEMPLATE(BM_FindRandom, 64, false, Forward);
BENCHMARK_TEMPLATE(BM_FindRandom, 64, false, Backward);
BENCHMARK_TEMPLATE(BM_FindRandom, 64, true, Forward);
BENCHMARK_TEMPLATE(BM_FindRandom, 64, true, Backward);
BENCHMARK_TEMPLATE(BM_FindRandom, 256, false, Forward);
BENCHMARK_TEMPLATE(BM_FindRandom, 256, false, Backward);
BENCHMARK_TEMPLATE(BM_FindRandom, 256, true, Forward);
BENCHMARK_TEMPLATE(BM_FindRandom, 256, true, Backward);
BENCHMARK_TEMPLATE(BM_FindRandom, 256 * 32, false, Forward);
BENCHMARK_TEMPLATE(BM_FindRandom, 256 * 32, false, Backward);
BENCHMARK_TEMPLATE(BM_FindRandom, 256 * 32, true, Forward);
BENCHMARK_TEMPLATE(BM_FindRandom, 256 * 32, true, Backward);

template <size_t N>
ABSL_ATTRIBUTE_NOINLINE size_t DoScanBenchmark(Bitmap<N>* set,
                                               benchmark::State& state) {
  size_t total = 0;
  for (auto s : state) {
    size_t index = 0, len;
    while (set->NextFreeRange(index, &index, &len)) {
      benchmark::DoNotOptimize(index);
      benchmark::DoNotOptimize(len);
      index += len;
      total++;
    }
  }

  return total;
}

template <size_t N>
static void BM_ScanEmpty(benchmark::State& state) {
  Bitmap<N> set;
  volatile size_t to_set = 0;
  volatile size_t to_clear = 0;
  set.SetBit(to_set);
  set.ClearBit(to_clear);
  size_t total = DoScanBenchmark<N>(&set, state);
  state.SetItemsProcessed(total);
}

BENCHMARK_TEMPLATE(BM_ScanEmpty, 64);
BENCHMARK_TEMPLATE(BM_ScanEmpty, 256);
BENCHMARK_TEMPLATE(BM_ScanEmpty, 256 * 32);

template <size_t N>
static void BM_ScanFull(benchmark::State& state) {
  Bitmap<N> set;
  volatile size_t to_set = 0;
  volatile size_t to_clear = 0;
  set.SetBit(to_set);
  set.ClearBit(to_clear);
  set.SetRange(0, N);

  size_t total = DoScanBenchmark<N>(&set, state);
  state.SetItemsProcessed(total);
}

BENCHMARK_TEMPLATE(BM_ScanFull, 64);
BENCHMARK_TEMPLATE(BM_ScanFull, 256);
BENCHMARK_TEMPLATE(BM_ScanFull, 256 * 32);

template <size_t N>
static void BM_ScanRandom(benchmark::State& state) {
  Bitmap<N> set;
  volatile size_t to_set = 0;
  volatile size_t to_clear = 0;
  set.SetBit(to_set);
  set.ClearBit(to_clear);
  absl::BitGen rng;
  for (int i = 0; i < N; ++i) {
    if (absl::Bernoulli(rng, 1.0 / 2)) set.SetBit(i);
  }
  size_t total = DoScanBenchmark<N>(&set, state);
  state.SetItemsProcessed(total);
}

BENCHMARK_TEMPLATE(BM_ScanRandom, 64);
BENCHMARK_TEMPLATE(BM_ScanRandom, 256);
BENCHMARK_TEMPLATE(BM_ScanRandom, 256 * 32);

template <size_t N>
static void BM_ScanChunks(benchmark::State& state) {
  Bitmap<N> set;
  volatile size_t to_set = 0;
  volatile size_t to_clear = 0;
  set.SetBit(to_set);
  set.ClearBit(to_clear);
  absl::BitGen rng;
  size_t index = 0;
  while (index < N) {
    // Paint ~half of a chunk of random size.
    size_t len = absl::Uniform<int32_t>(rng, 0, 32) + 1;
    len = std::min(len, N - index);
    size_t mid = absl::Uniform<int32_t>(rng, 0, len) + index;
    size_t ones = mid + 1;
    size_t limit = index + len;
    if (ones < limit) {
      set.SetRange(ones, limit - ones);
    }
    index = limit;
  }
  size_t total = DoScanBenchmark<N>(&set, state);
  state.SetItemsProcessed(total);
}

BENCHMARK_TEMPLATE(BM_ScanChunks, 64);
BENCHMARK_TEMPLATE(BM_ScanChunks, 256);
BENCHMARK_TEMPLATE(BM_ScanChunks, 256 * 32);

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
