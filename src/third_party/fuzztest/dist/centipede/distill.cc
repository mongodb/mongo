// Copyright 2023 The Centipede Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "./centipede/distill.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <numeric>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/base/thread_annotations.h"
#include "absl/container/flat_hash_set.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"
#include "./centipede/corpus_io.h"
#include "./centipede/environment.h"
#include "./centipede/feature.h"
#include "./centipede/feature_set.h"
#include "./centipede/periodic_action.h"
#include "./centipede/resource_pool.h"
#include "./centipede/rusage_profiler.h"
#include "./centipede/rusage_stats.h"
#include "./centipede/thread_pool.h"
#include "./centipede/util.h"
#include "./centipede/workdir.h"
#include "./common/blob_file.h"
#include "./common/defs.h"
#include "./common/hash.h"
#include "./common/logging.h"
#include "./common/remote_file.h"
#include "./common/status_macros.h"

namespace fuzztest::internal {

namespace {

// A corpus element. Consists of a fuzz test input and its matching features.
struct CorpusElt {
  ByteArray input;
  FeatureVec features;

  CorpusElt(const ByteArray &input, FeatureVec features)
      : input(input), features(std::move(features)) {}

  // Movable, but not copyable for efficiency.
  CorpusElt(const CorpusElt &) = delete;
  CorpusElt &operator=(const CorpusElt &) = delete;
  CorpusElt(CorpusElt &&) = default;
  CorpusElt &operator=(CorpusElt &&) = default;

  ByteArray PackedFeatures() const {
    return PackFeaturesAndHash(input, features);
  }
};

using CorpusEltVec = std::vector<CorpusElt>;

// The maximum number of threads reading input shards concurrently. This is
// mainly to prevent I/O congestion.
inline constexpr size_t kMaxReadingThreads = 50;
// The maximum number of threads writing shards concurrently. These in turn
// launch up to `kMaxReadingThreads` reading threads.
inline constexpr size_t kMaxWritingThreads = 100;
// A global cap on the total number of threads, both writing and reading. Unlike
// the other two limits, this one is purely to prevent too many threads in the
// process.
inline constexpr size_t kMaxTotalThreads = 5000;
static_assert(kMaxReadingThreads * kMaxWritingThreads <= kMaxTotalThreads);

inline constexpr MemSize kGB = 1024L * 1024L * 1024L;
// The total approximate amount of RAM to be shared by the concurrent threads.
// TODO(ussuri): Replace by a function of free RSS on the system.
inline constexpr RUsageMemory kRamQuota{/*mem_vsize=*/0, /*mem_vpeak=*/0,
                                        /*mem_rss=*/25 * kGB};
// The amount of time that each thread will wait for enough RAM to be freed up
// by its concurrent siblings.
inline constexpr absl::Duration kRamLeaseTimeout = absl::Hours(5);

std::string LogPrefix(const Environment &env) {
  return absl::StrCat("DISTILL[S.", env.my_shard_index, "]: ");
}

std::string LogPrefix() { return absl::StrCat("DISTILL[ALL]: "); }

// TODO(ussuri): Move the reader/writer classes to shard_reader.cc, rename it
//  to corpus_io.cc, and reuse the new APIs where useful in the code base.

// A helper class for reading input corpus shards. Thread-safe.
class InputCorpusShardReader {
 public:
  InputCorpusShardReader(const Environment &env)
      : workdir_{env}, log_prefix_{LogPrefix(env)} {}

  MemSize EstimateRamFootprint(size_t shard_idx) const {
    const auto corpus_path = workdir_.CorpusFilePaths().Shard(shard_idx);
    const auto features_path = workdir_.FeaturesFilePaths().Shard(shard_idx);
    const MemSize corpus_file_size = ValueOrDie(RemoteFileGetSize(corpus_path));
    const MemSize features_file_size =
        ValueOrDie(RemoteFileGetSize(features_path));
    // Conservative compression factors for the two file types. These have been
    // observed empirically for the Riegeli blob format. The legacy format is
    // approximately 1:1, but use the stricter Riegeli numbers, as the legacy
    // should be considered obsolete.
    // TODO(b/322880269): Use the actual in-memory footprint once available.
    constexpr double kMaxCorpusCompressionRatio = 5.0;
    constexpr double kMaxFeaturesCompressionRatio = 10.0;
    return corpus_file_size * kMaxCorpusCompressionRatio +
           features_file_size * kMaxFeaturesCompressionRatio;
  }

  // Reads and returns a single shard's elements. Thread-safe.
  CorpusEltVec ReadShard(size_t shard_idx) {
    const auto corpus_path = workdir_.CorpusFilePaths().Shard(shard_idx);
    const auto features_path = workdir_.FeaturesFilePaths().Shard(shard_idx);
    VLOG(1) << log_prefix_ << "reading input shard " << shard_idx << ":\n"
            << VV(corpus_path) << "\n"
            << VV(features_path);
    CorpusEltVec elts;
    // Read elements from the current shard.
    fuzztest::internal::ReadShard(  //
        corpus_path, features_path,
        [&elts](ByteArray input, FeatureVec features) {
          elts.emplace_back(std::move(input), std::move(features));
        });
    return elts;
  }

 private:
  const WorkDir workdir_;
  const std::string log_prefix_;
};

// A helper class for writing corpus shards. Thread-safe.
class CorpusShardWriter {
 public:
  // The writing stats so far.
  struct Stats {
    size_t num_total_elts = 0;
    size_t num_written_elts = 0;
    size_t num_written_batches = 0;
  };

  CorpusShardWriter(const Environment &env, bool append)
      : workdir_{env},
        log_prefix_{LogPrefix(env)},
        corpus_path_{workdir_.DistilledCorpusFilePaths().MyShard()},
        features_path_{workdir_.DistilledFeaturesFilePaths().MyShard()},
        corpus_writer_{DefaultBlobFileWriterFactory()},
        feature_writer_{DefaultBlobFileWriterFactory()} {
    CHECK_OK(corpus_writer_->Open(corpus_path_, append ? "a" : "w"));
    CHECK_OK(feature_writer_->Open(features_path_, append ? "a" : "w"));
  }

  virtual ~CorpusShardWriter() = default;

  void WriteElt(CorpusElt elt) {
    absl::MutexLock lock(&mu_);
    WriteEltImpl(std::move(elt));
  }

  void WriteBatch(CorpusEltVec elts) {
    absl::MutexLock lock(&mu_);
    VLOG(1) << log_prefix_ << "writing " << elts.size()
            << " elements to output shard:\n"
            << VV(corpus_path_) << "\n"
            << VV(features_path_);
    for (auto &elt : elts) {
      WriteEltImpl(std::move(elt));
    }
    ++stats_.num_written_batches;
  }

  Stats GetStats() const {
    absl::MutexLock lock(&mu_);
    return stats_;
  }

 protected:
  // A behavior customization point: a derived class gets an opportunity to
  // analyze and/or preprocess `elt` before it is written. For example, a
  // derived class can trim the element's feature set before it is written, or
  // choose to skip writing it entirely by returning `std::nullopt`.
  virtual std::optional<CorpusElt> PreprocessElt(CorpusElt elt) {
    return std::move(elt);
  }

 private:
  void WriteEltImpl(CorpusElt elt) ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_) {
    ++stats_.num_total_elts;
    const auto preprocessed_elt = PreprocessElt(std::move(elt));
    if (preprocessed_elt.has_value()) {
      // Append to the distilled corpus and features files.
      CHECK_OK(corpus_writer_->Write(preprocessed_elt->input));
      CHECK_OK(feature_writer_->Write(preprocessed_elt->PackedFeatures()));
      ++stats_.num_written_elts;
    }
  }

  // Const state.
  const WorkDir workdir_;
  const std::string log_prefix_;
  const std::string corpus_path_;
  const std::string features_path_;

  // Mutable state.
  mutable absl::Mutex mu_;
  std::unique_ptr<BlobFileWriter> corpus_writer_ ABSL_GUARDED_BY(mu_);
  std::unique_ptr<BlobFileWriter> feature_writer_ ABSL_GUARDED_BY(mu_);
  Stats stats_ ABSL_GUARDED_BY(mu_);
};

// A distilling input filter:
// - Deduplicates byte-identical inputs: only the first one is allowed to pass.
// - Deduplicates feature-equivalent inputs: up to N from each equivalency set
//   are allowed to pass.
// - Discards the specified set of "uninteresting" feature domains from the
//   feature sets of filtered inputs.
class DistillingInputFilter {
 public:
  // An extension to the parent class's `Stats`.
  struct Stats {
    size_t num_total_elts = 0;
    size_t num_byte_unique_elts = 0;
    size_t num_feature_unique_elts = 0;
    // The accumulated features of the distilled corpus so far, represents in
    // the same compact textual form that Centipede uses in its fuzzing progress
    // log messages, e.g.: "ft: 96331 cov: 81793 usr1: 5045 ...".
    std::string coverage_str;
  };

  // `feature_equiv_redundancy` specifies how many inputs with equivalent
  // feature sets are allowed to pass the filter. Any subsequent inputs with the
  // equivalent set will be rejected.
  // `should_discard_domains` specifies the domains that should be discarded
  // from the feature set of a filtered input.
  DistillingInputFilter(  //
      uint8_t feature_frequency_threshold,
      const FeatureSet::FeatureDomainSet &domains_to_discard)
      : seen_inputs_{},
        seen_features_{
            /*frequency_threshold=*/feature_frequency_threshold,
            /*should_discard_domain=*/domains_to_discard,
        } {}

  std::optional<CorpusElt> FilterElt(CorpusElt elt) {
    absl::MutexLock lock{&mu_};

    ++stats_.num_total_elts;

    // Filter out approximately byte-identical inputs ("approximately" because
    // we use hashes).
    std::string hash = Hash(elt.input);
    const auto [iter, inserted] = seen_inputs_.insert(std::move(hash));
    if (!inserted) return std::nullopt;
    ++stats_.num_byte_unique_elts;

    // Filter out feature-equivalent inputs.
    seen_features_.PruneDiscardedDomains(elt.features);
    if (!seen_features_.HasUnseenFeatures(elt.features)) return std::nullopt;
    seen_features_.IncrementFrequencies(elt.features);
    ++stats_.num_feature_unique_elts;

    return std::move(elt);
  }

  Stats GetStats() {
    absl::MutexLock lock{&mu_};
    std::stringstream ss;
    ss << seen_features_;
    stats_.coverage_str = std::move(ss).str();
    return stats_;
  }

 private:
  absl::Mutex mu_;
  absl::flat_hash_set<std::string /*hash*/> seen_inputs_ ABSL_GUARDED_BY(mu_);
  FeatureSet seen_features_ ABSL_GUARDED_BY(mu_);
  Stats stats_ ABSL_GUARDED_BY(mu_);
};

// A helper class for writing distilled corpus shards. NOT thread-safe because
// all writes go to a single file.
class DistilledCorpusShardWriter : public CorpusShardWriter {
 public:
  DistilledCorpusShardWriter(  //
      const Environment &env, bool append, DistillingInputFilter &filter)
      : CorpusShardWriter{env, append}, input_filter_{filter} {}

  ~DistilledCorpusShardWriter() override = default;

 protected:
  std::optional<CorpusElt> PreprocessElt(CorpusElt elt) override {
    return input_filter_.FilterElt(std::move(elt));
  }

 private:
  DistillingInputFilter &input_filter_;
};

}  // namespace

// Runs one independent distillation task. Reads shards in the order specified
// by `shard_indices`, distills inputs from them using `input_filter`, and
// writes the result to `WorkDir{env}.DistilledPath()`. Every task gets its own
// `env.my_shard_index`, and so every task creates its own independent distilled
// corpus file. `parallelism` is the maximum number of concurrent
// reading/writing threads. Values > 1 can cause non-determinism in which of the
// same-coverage inputs gets selected to be written to the output shard; set to
// 1 for tests.
void DistillToOneOutputShard(                  //
    const Environment &env,                    //
    const std::vector<size_t> &shard_indices,  //
    DistillingInputFilter &input_filter,       //
    ResourcePool<RUsageMemory> &ram_pool,      //
    int parallelism) {
  LOG(INFO) << LogPrefix(env) << "Distilling to output shard "
            << env.my_shard_index << "; input shard indices:\n"
            << absl::StrJoin(shard_indices, ", ");

  // Read and write the shards in parallel, but gate reading of each on the
  // availability of free RAM to keep the peak RAM usage under control.
  const size_t num_shards = shard_indices.size();
  InputCorpusShardReader reader{env};
  // NOTE: Always overwrite corpus and features files, never append.
  DistilledCorpusShardWriter writer{env, /*append=*/false, input_filter};

  {
    ThreadPool threads{parallelism};
    for (size_t shard_idx : shard_indices) {
      threads.Schedule([shard_idx, &reader, &writer, &env, num_shards,
                        &ram_pool] {
        const auto ram_lease = ram_pool.AcquireLeaseBlocking({
            /*id=*/absl::StrCat("out_", env.my_shard_index, "/in_", shard_idx),
            /*amount=*/
            {/*mem_vsize=*/0, /*mem_vpeak=*/0,
             /*mem_rss=*/reader.EstimateRamFootprint(shard_idx)},
            /*timeout=*/kRamLeaseTimeout,
        });
        CHECK_OK(ram_lease.status());

        CorpusEltVec shard_elts = reader.ReadShard(shard_idx);
        // Reverse the order of elements. The intuition is as follows:
        // * If the shard is the result of fuzzing with Centipede, the inputs
        //   that are closer to the end are more interesting, so we start there.
        // * If the shard resulted from somethening else, the reverse order is
        //   not any better or worse than any other order.
        std::reverse(shard_elts.begin(), shard_elts.end());
        writer.WriteBatch(std::move(shard_elts));
        const CorpusShardWriter::Stats shard_stats = writer.GetStats();
        LOG(INFO) << LogPrefix(env)
                  << "batches: " << shard_stats.num_written_batches << "/"
                  << num_shards << " inputs: " << shard_stats.num_total_elts
                  << " written: " << shard_stats.num_written_elts;
      });
    }
  }  // The threads join here.

  LOG(INFO) << LogPrefix(env) << "Done distilling to output shard "
            << env.my_shard_index;
}

int Distill(const Environment &env, const DistillOptions &opts) {
  RPROF_THIS_FUNCTION_WITH_TIMELAPSE(                                      //
      /*enable=*/ABSL_VLOG_IS_ON(1),                                       //
      /*timelapse_interval=*/absl::Seconds(ABSL_VLOG_IS_ON(2) ? 10 : 60),  //
      /*also_log_timelapses=*/ABSL_VLOG_IS_ON(10));

  // Prepare the per-thread envs.
  std::vector<Environment> envs_per_thread(env.num_threads, env);
  for (size_t thread_idx = 0; thread_idx < env.num_threads; ++thread_idx) {
    envs_per_thread[thread_idx].my_shard_index += thread_idx;
  }

  // Prepare the per-thread input shard indices. This assigns a randomized and
  // shuffled subset of the input shards to each output shard writer. The subset
  // sizes are roughly equal between the writers.
  std::vector<std::vector<size_t>> shard_indices_per_thread(env.num_threads);
  std::vector<size_t> all_shard_indices(env.total_shards);
  std::iota(all_shard_indices.begin(), all_shard_indices.end(), 0);
  Rng rng{GetRandomSeed(env.seed)};
  std::shuffle(all_shard_indices.begin(), all_shard_indices.end(), rng);
  size_t thread_idx = 0;
  for (size_t shard_idx : all_shard_indices) {
    shard_indices_per_thread[thread_idx].push_back(shard_idx);
    thread_idx = (thread_idx + 1) % env.num_threads;
  }

  // Run the distillation threads in parallel.
  {
    // A global input filter shared by all output shard writers. The output
    // shards will collectively contain a deduplicated set of byte- and
    // feature-unique inputs.
    DistillingInputFilter input_filter{
        opts.feature_frequency_threshold,
        env.MakeDomainDiscardMask(),
    };
    // A periodic logger of the global distillation progress. Runs on a separate
    // thread.
    PeriodicAction progress_logger{
        [&input_filter]() {
          const auto stats = input_filter.GetStats();
          LOG(INFO) << LogPrefix() << stats.coverage_str
                    << " inputs: " << stats.num_total_elts
                    << " unique: " << stats.num_byte_unique_elts
                    << " distilled: " << stats.num_feature_unique_elts;
        },
        // Seeing 0's at the beginning is not interesting, unless debugging.
        // Likewise, increase the frequency --v >= 1 to aid debugging.
        PeriodicAction::ConstDelayConstInterval(
            absl::Seconds(ABSL_VLOG_IS_ON(1) ? 0 : 60),
            absl::Seconds(ABSL_VLOG_IS_ON(1) ? 10 : 60)),
    };
    // The RAM pool shared between all the `DistillToOneOutputShard()` threads.
    ResourcePool ram_pool{kRamQuota};
    const size_t num_threads = std::min(env.num_threads, kMaxWritingThreads);
    ThreadPool threads{static_cast<int>(num_threads)};
    for (size_t thread_idx = 0; thread_idx < env.num_threads; ++thread_idx) {
      threads.Schedule(
          [&thread_env = envs_per_thread[thread_idx],
           &thread_shard_indices = shard_indices_per_thread[thread_idx],
           &input_filter, &progress_logger, &ram_pool]() {
            DistillToOneOutputShard(  //
                thread_env, thread_shard_indices, input_filter, ram_pool,
                kMaxReadingThreads);
            // In addition to periodic progress reports, also log the progress
            // after writing each output shard.
            progress_logger.Nudge();
          });
    }
  }  // The threads join here.

  return EXIT_SUCCESS;
}

void DistillForTests(const Environment &env,
                     const std::vector<size_t> &shard_indices) {
  DistillingInputFilter input_filter{
      /*feature_frequency_threshold=*/1,
      env.MakeDomainDiscardMask(),
  };
  // Do not limit the max RAM.
  ResourcePool ram_pool{RUsageMemory::Max()};
  // Read the input shards sequentially and in order to ensure deterministic
  // outputs.
  DistillToOneOutputShard(  //
      env, shard_indices, input_filter, ram_pool, /*parallelism=*/1);
}

}  // namespace fuzztest::internal
