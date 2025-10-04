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

#include <atomic>
#include <optional>

#include "absl/random/distributions.h"
#include "absl/random/random.h"
#include "absl/types/optional.h"
#include "benchmark/benchmark.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/mock_central_freelist.h"
#include "tcmalloc/mock_transfer_cache.h"
#include "tcmalloc/transfer_cache_internals.h"
#include "tcmalloc/transfer_cache_stats.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

using TransferCacheWithRealCFLEnv =
    FakeTransferCacheEnvironment<internal_transfer_cache::TransferCache<
        RealCentralFreeListForTesting, FakeTransferCacheManager>>;
using TransferCacheEnv =
    FakeTransferCacheEnvironment<internal_transfer_cache::TransferCache<
        MinimalFakeCentralFreeList, FakeTransferCacheManager>>;
static constexpr int kSizeClass = 0;

template <typename Env>
void BM_CrossThread(benchmark::State& state) {
  using Cache = typename Env::TransferCache;
  const int kBatchSize = Env::kBatchSize;
  const int kMaxObjectsToMove = Env::kMaxObjectsToMove;
  void* batch[kMaxObjectsToMove];

  struct CrossThreadState {
    CrossThreadState()
        : m{},
          c{Cache(&m, 1, /*use_all_buckets_for_few_object_spans=*/false),
            Cache(&m, 1, /*use_all_buckets_for_few_object_spans=*/false)} {}
    FakeTransferCacheManager m;
    Cache c[2];
  };

  static CrossThreadState* s = nullptr;
  if (state.thread_index() == 0) {
    s = new CrossThreadState();
    for (int i = 0; i < ::tcmalloc::tcmalloc_internal::internal_transfer_cache::
                                kInitialCapacityInBatches /
                            2;
         ++i) {
      for (Cache& c : s->c) {
        c.freelist().AllocateBatch(batch, kBatchSize);
        c.InsertRange(kSizeClass, {batch, kBatchSize});
      }
    }
  }

  int src = state.thread_index() % 2;
  int dst = (src + 1) % 2;
  for (auto iter : state) {
    benchmark::DoNotOptimize(batch);
    (void)s->c[src].RemoveRange(kSizeClass, batch, kBatchSize);
    benchmark::DoNotOptimize(batch);
    s->c[dst].InsertRange(kSizeClass, {batch, kBatchSize});
    benchmark::DoNotOptimize(batch);
  }
  if (state.thread_index() == 0) {
    TransferCacheStats stats{};
    for (Cache& c : s->c) {
      TransferCacheStats other = c.GetStats();
      stats.insert_hits += other.insert_hits;
      stats.insert_misses += other.insert_misses;
      stats.remove_hits += other.remove_hits;
      stats.remove_misses += other.remove_misses;
    }

    state.counters["insert_hit_ratio"] =
        static_cast<double>(stats.insert_hits) /
        (stats.insert_hits + stats.insert_misses);
    state.counters["remove_hit_ratio"] =
        static_cast<double>(stats.remove_hits) /
        (stats.remove_hits + stats.remove_misses);
    delete s;
    s = nullptr;
  }
}

template <typename Env>
void BM_InsertRange(benchmark::State& state) {
  const int kBatchSize = Env::kBatchSize;
  const int kMaxObjectsToMove = Env::kMaxObjectsToMove;

  // optional to have more precise control of when the destruction occurs, as
  // we want to avoid polluting the timing with the dtor.
  std::optional<Env> e;
  void* batch[kMaxObjectsToMove];
  for (auto iter : state) {
    state.PauseTiming();
    e.emplace();
    e->central_freelist().AllocateBatch(batch, kBatchSize);
    benchmark::DoNotOptimize(e);
    benchmark::DoNotOptimize(batch);
    state.ResumeTiming();

    e->transfer_cache().InsertRange(kSizeClass, {batch, kBatchSize});
  }
}

template <typename Env>
void BM_RemoveRange(benchmark::State& state) {
  const int kBatchSize = Env::kBatchSize;
  const int kMaxObjectsToMove = Env::kMaxObjectsToMove;

  // optional to have more precise control of when the destruction occurs, as
  // we want to avoid polluting the timing with the dtor.
  std::optional<Env> e;
  void* batch[kMaxObjectsToMove];
  for (auto iter : state) {
    state.PauseTiming();
    e.emplace();
    e->Insert(kBatchSize);
    benchmark::DoNotOptimize(e);
    state.ResumeTiming();

    (void)e->transfer_cache().RemoveRange(kSizeClass, batch, kBatchSize);
    benchmark::DoNotOptimize(batch);
  }
}

template <typename Env>
void BM_RealisticBatchNonBatchMutations(benchmark::State& state) {
  const int kBatchSize = Env::kBatchSize;

  Env e;
  absl::BitGen gen;

  for (auto iter : state) {
    state.PauseTiming();
    const double choice = absl::Uniform(gen, 0.0, 1.0);
    state.ResumeTiming();

    // These numbers have been determined by looking at production data.
    if (choice < 0.424) {
      e.Insert(kBatchSize);
    } else if (choice < 0.471) {
      e.Insert(1);
    } else if (choice < 0.959) {
      e.Remove(kBatchSize);
    } else {
      e.Remove(1);
    }
  }

  const TransferCacheStats stats = e.transfer_cache().GetStats();
  state.counters["insert_hit_ratio"] =
      static_cast<double>(stats.insert_hits) /
      (stats.insert_hits + stats.insert_misses);
  state.counters["remove_hit_ratio"] =
      static_cast<double>(stats.remove_hits) /
      (stats.remove_hits + stats.remove_misses);
}

template <typename Env>
void BM_RealisticHitRate(benchmark::State& state) {
  const int kBatchSize = Env::kBatchSize;

  Env e;
  absl::BitGen gen;
  // We switch between insert-heavy and remove-heavy access pattern every 5k
  // iterations. kBias specifies the fraction of insert (or remove) operations
  // during insert-heavy (or remove-heavy) phase of the microbenchmark. These
  // constants have been determined through experimentation so that the
  // resulting insert and remove miss rate matches that of the production.
  constexpr int kInterval = 5000;
  constexpr double kBias = 0.85;
  bool insert_heavy = true;
  unsigned int iterations = 0;
  for (auto iter : state) {
    state.PauseTiming();
    const double partial = absl::Uniform(gen, 0.0, 1.0);
    // We perform insert (or remove) operations with a probablity specified by
    // kBias during the insert-heavy (or remove-heavy) phase of this benchmark.
    const bool insert = absl::Bernoulli(gen, kBias) == insert_heavy;
    state.ResumeTiming();

    if (insert) {
      // These numbers have been determined by looking at production data.
      if (partial < 0.65) {
        e.Insert(kBatchSize);
      } else {
        e.Insert(1);
      }
    } else {
      // These numbers have been determined by looking at production data.
      if (partial < 0.99) {
        e.Remove(kBatchSize);
      } else {
        e.Remove(1);
      }
    }
    ++iterations;
    if (iterations % kInterval == 0) {
      insert_heavy = !insert_heavy;
    }
  }

  const TransferCacheStats stats = e.transfer_cache().GetStats();
  const size_t total_inserts = stats.insert_hits + stats.insert_misses;
  state.counters["insert_aggregate_miss_ratio"] =
      static_cast<double>(stats.insert_misses) / total_inserts;

  const size_t total_removes = stats.remove_hits + stats.remove_misses;
  state.counters["remove_aggregate_miss_ratio"] =
      static_cast<double>(stats.remove_misses) / total_removes;
}

BENCHMARK_TEMPLATE(BM_CrossThread, TransferCacheEnv)->ThreadRange(2, 64);
BENCHMARK_TEMPLATE(BM_InsertRange, TransferCacheEnv);
BENCHMARK_TEMPLATE(BM_RemoveRange, TransferCacheEnv);
BENCHMARK_TEMPLATE(BM_RealisticBatchNonBatchMutations, TransferCacheEnv);
BENCHMARK_TEMPLATE(BM_RealisticHitRate, TransferCacheEnv);
BENCHMARK_TEMPLATE(BM_RealisticHitRate, TransferCacheWithRealCFLEnv);

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
