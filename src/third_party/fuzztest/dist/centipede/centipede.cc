// Copyright 2022 The Centipede Authors.
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

// Centipede: an experimental distributed fuzzing engine.
// Very simple / naive so far.
// Main use case: large out-of-process fuzz targets with relatively slow
// execution (< 100 exec/s).
//
// Basic approach (subject to change):
// * All state is stored in a local or remote directory `workdir`.
// * State consists of a corpus (inputs) and feature sets (see feature_t).
// * Feature sets are associated with a binary, so that two binaries
//   have independent feature sets stored in different subdirs in `workdir`,
//   like binaryA-sha1-of-A and binaryB-sha1-of-B.
//   If the binary is recompiled at different revision or with different
//   compiler options, it is a different binary and feature sets will need to be
//   recomputed for the new binary in its separate dir.
// * The corpus is not tied to the binary. It is stored in `workdir`/.
// * The fuzzer runs in `total_shards` independent processes.
// * Each shard appends data to its own files in `workdir`: corpus and features;
//   no other process writes to those files.
// * Each shard may periodically read some other shard's corpus and features.
//   Since all files are append-only (no renames, no deletions) we may only
//   have partial reads, and the algorithm is expected to tolerate those.
// * Fuzzing can be run locally in multiple processes, with a local `workdir`
//   or on a cluster, which supports `workdir` on a remote file system.
// * The intent is to scale to an arbitrary number of shards,
//   currently tested with total_shards = 10000.
//
//  Differential fuzzing is not yet properly implemented.
//  Currently, one can run target A in a given workdir, then target B, and so
//  on, and the corpus will grow over time benefiting from all targets.
#include "./centipede/centipede.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>  // NOLINT
#include <functional>
#include <iostream>
#include <memory>
#include <numeric>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/base/const_init.h"  // NOLINT
#include "absl/base/nullability.h"
#include "absl/container/flat_hash_set.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/strings/ascii.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_split.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "./centipede/binary_info.h"
#include "./centipede/centipede_callbacks.h"
#include "./centipede/command.h"
#include "./centipede/control_flow.h"
#include "./centipede/corpus_io.h"
#include "./centipede/coverage.h"
#include "./centipede/environment.h"
#include "./centipede/feature.h"
#include "./centipede/feature_set.h"
#include "./centipede/mutation_input.h"
#include "./centipede/runner_result.h"
#include "./centipede/rusage_profiler.h"
#include "./centipede/rusage_stats.h"
#include "./centipede/stats.h"
#include "./centipede/stop.h"
#include "./centipede/util.h"
#include "./centipede/workdir.h"
#include "./common/blob_file.h"
#include "./common/defs.h"
#include "./common/hash.h"
#include "./common/logging.h"
#include "./common/remote_file.h"
#include "./common/status_macros.h"

namespace fuzztest::internal {

Centipede::Centipede(const Environment &env, CentipedeCallbacks &user_callbacks,
                     const BinaryInfo &binary_info,
                     CoverageLogger &coverage_logger, std::atomic<Stats> &stats)
    : env_(env),
      user_callbacks_(user_callbacks),
      rng_(env_.seed),
      // TODO(kcc): [impl] find a better way to compute frequency_threshold.
      fs_(env_.feature_frequency_threshold, env_.MakeDomainDiscardMask()),
      coverage_frontier_(binary_info),
      binary_info_(binary_info),
      pc_table_(binary_info_.pc_table),
      symbols_(binary_info_.symbols),
      function_filter_(env_.function_filter, symbols_),
      coverage_logger_(coverage_logger),
      stats_(stats),
      input_filter_path_(std::filesystem::path(TemporaryLocalDirPath())
                             .append("filter-input")),
      input_filter_cmd_{[&] {
        Command::Options cmd_options;
        cmd_options.args = {input_filter_path_};
        cmd_options.stdout_file = "/dev/null";
        cmd_options.stderr_file = "/dev/null";
        return Command{env_.input_filter, std::move(cmd_options)};
      }()},
      rusage_profiler_(
          /*scope=*/RUsageScope::ThisProcess(),
          /*metrics=*/env.DumpRUsageTelemetryInThisShard()
              ? RUsageProfiler::kAllMetrics
              : RUsageProfiler::kMetricsOff,
          /*raii_actions=*/RUsageProfiler::kRaiiOff,
          /*location=*/{__FILE__, __LINE__},
          /*description=*/"Engine") {
  CHECK(env_.seed) << "env_.seed must not be zero";
  if (!env_.input_filter.empty() && env_.fork_server)
    input_filter_cmd_.StartForkServer(TemporaryLocalDirPath(), "input_filter");
}

void Centipede::CorpusToFiles(const Environment &env, std::string_view dir) {
  std::vector<std::string> sharded_corpus_files;
  CHECK_OK(RemoteGlobMatch(WorkDir{env}.CorpusFilePaths().AllShardsGlob(),
                           sharded_corpus_files));
  ExportCorpus(sharded_corpus_files, dir);
}

void Centipede::CorpusFromFiles(const Environment &env, std::string_view dir) {
  // Shard the file paths in the source `dir` based on hashes of filenames.
  // Such partition is stable: a given file always goes to a specific shard.
  std::vector<std::vector<std::string>> sharded_paths(env.total_shards);
  std::vector<std::string> paths;
  size_t total_paths = 0;
  const std::vector<std::string> listed_paths =
      ValueOrDie(RemoteListFiles(dir, /*recursively=*/true));
  for (const std::string &path : listed_paths) {
    size_t filename_hash = std::hash<std::string>{}(path);
    sharded_paths[filename_hash % env.total_shards].push_back(path);
    ++total_paths;
  }

  // If the destination `workdir` is specified (note that empty means "use the
  // current directory"), we might need to create it.
  if (!env.workdir.empty()) {
    CHECK_OK(RemoteMkdir(env.workdir));
  }

  // Iterate over all shards, adding inputs to the current shard.
  size_t inputs_added = 0;
  size_t inputs_ignored = 0;
  const auto corpus_file_paths = WorkDir{env}.CorpusFilePaths();
  for (size_t shard = 0; shard < env.total_shards; shard++) {
    const std::string corpus_file_path = corpus_file_paths.Shard(shard);
    size_t num_shard_bytes = 0;
    // Read the shard (if it exists), collect input hashes from it.
    absl::flat_hash_set<std::string> existing_hashes;
    if (RemotePathExists(corpus_file_path)) {
      auto reader = DefaultBlobFileReaderFactory();
      // May fail to open if file doesn't exist.
      reader->Open(corpus_file_path).IgnoreError();
      ByteSpan blob;
      while (reader->Read(blob).ok()) {
        existing_hashes.insert(Hash(blob));
      }
    }
    // Add inputs to the current shard, if the shard doesn't have them already.
    auto appender = DefaultBlobFileWriterFactory(env.riegeli);
    CHECK_OK(appender->Open(corpus_file_path, "a"))
        << "Failed to open corpus file: " << corpus_file_path;
    ByteArray shard_data;
    for (const auto &path : sharded_paths[shard]) {
      std::string input;
      CHECK_OK(RemoteFileGetContents(path, input));
      if (input.empty() || existing_hashes.contains(Hash(input))) {
        ++inputs_ignored;
        continue;
      }
      CHECK_OK(appender->Write(ByteArray{input.begin(), input.end()}));
      ++inputs_added;
    }
    LOG(INFO) << VV(shard) << VV(inputs_added) << VV(inputs_ignored)
              << VV(num_shard_bytes) << VV(shard_data.size());
  }
  CHECK_EQ(total_paths, inputs_added + inputs_ignored);
}

absl::Status Centipede::CrashesToFiles(const Environment &env,
                                       std::string_view dir) {
  std::vector<std::string> reproducer_dirs;
  const auto wd = WorkDir{env};
  auto reproducer_match_status = RemoteGlobMatch(
      wd.CrashReproducerDirPaths().AllShardsGlob(), reproducer_dirs);
  if (!reproducer_match_status.ok() &&
      !absl::IsNotFound(reproducer_match_status)) {
    return reproducer_match_status;
  }
  absl::flat_hash_set<std::string> crash_ids;
  for (const auto &reproducer_dir : reproducer_dirs) {
    ASSIGN_OR_RETURN_IF_NOT_OK(
        std::vector<std::string> reproducer_paths,
        RemoteListFiles(reproducer_dir, /*recursively=*/false));
    for (const auto &reproducer_path : reproducer_paths) {
      std::string id = std::filesystem::path{reproducer_path}.filename();
      if (auto [_it, inserted] = crash_ids.insert(id); !inserted) {
        continue;
      }
      RETURN_IF_NOT_OK(RemoteFileCopy(
          reproducer_path,
          (std::filesystem::path{dir} / absl::StrCat(id, ".data")).string()));
      const auto shard_index = wd.CrashReproducerDirPaths().GetShardIndex(
          std::filesystem::path{reproducer_path}.parent_path().string());
      CHECK(shard_index.has_value());
      const auto metadata_dir = wd.CrashMetadataDirPaths().Shard(*shard_index);
      const auto description_filename = absl::StrCat(id, ".desc");
      const auto signature_filename = absl::StrCat(id, ".sig");
      RETURN_IF_NOT_OK(RemoteFileCopy(
          (std::filesystem::path{metadata_dir} / description_filename).string(),
          (std::filesystem::path{dir} / description_filename).string()));
      RETURN_IF_NOT_OK(RemoteFileCopy(
          (std::filesystem::path{metadata_dir} / signature_filename).string(),
          (std::filesystem::path{dir} / signature_filename).string()));
    }
  }
  return absl::OkStatus();
}

void Centipede::UpdateAndMaybeLogStats(std::string_view log_type,
                                       size_t min_log_level) {
  // `fuzz_start_time_ == ` means that fuzzing hasn't started yet. If so, grab
  // the baseline numbers.
  const double fuzz_time_secs =
      fuzz_start_time_ == absl::InfiniteFuture()
          ? 0
          : absl::ToDoubleSeconds(absl::Now() - fuzz_start_time_);
  const double execs_per_sec =
      fuzz_time_secs == 0 ? 0 : (1.0 * num_runs_ / fuzz_time_secs);
  const auto [max_corpus_size, avg_corpus_size] = corpus_.MaxAndAvgSize();

  // NOTE: For now, this will double-count rusage in every shard on the same
  // machine. The stats reporter knows and deals with that.
  static const auto rusage_scope = RUsageScope::ThisProcess();
  const auto rusage_timing = RUsageTiming::Snapshot(rusage_scope);
  const auto rusage_memory = RUsageMemory::Snapshot(rusage_scope);

  namespace fd = feature_domains;

  stats_.store(Stats{
      StatsMeta{
          /*timestamp_unix_micros=*/
          static_cast<uint64_t>(absl::ToUnixMicros(absl::Now())),
      },
      ExecStats{
          /*fuzz_time_sec=*/static_cast<uint64_t>(std::ceil(fuzz_time_secs)),
          /*num_executions*/ num_runs_,
          /*num_target_crashes*/ static_cast<uint64_t>(num_crashes_),
      },
      CovStats{
          /*num_covered_pcs=*/fs_.CountFeatures(fd::kPCs),
          /*num_8bit_counter_features=*/fs_.CountFeatures(fd::k8bitCounters),
          /*num_data_flow_features=*/fs_.CountFeatures(fd::kDataFlow),
          /*num_cmp_features=*/fs_.CountFeatures(fd::kCMPDomains),
          /*num_call_stack_features=*/fs_.CountFeatures(fd::kCallStack),
          /*num_bounded_path_features=*/fs_.CountFeatures(fd::kBoundedPath),
          /*num_pc_pair_features=*/fs_.CountFeatures(fd::kPCPair),
          /*num_user_features=*/fs_.CountFeatures(fd::kUserDomains),
          /*num_user0_features=*/fs_.CountFeatures(fd::kUserDomains[0]),
          /*num_user1_features=*/fs_.CountFeatures(fd::kUserDomains[1]),
          /*num_user2_features=*/fs_.CountFeatures(fd::kUserDomains[2]),
          /*num_user3_features=*/fs_.CountFeatures(fd::kUserDomains[3]),
          /*num_user4_features=*/fs_.CountFeatures(fd::kUserDomains[4]),
          /*num_user5_features=*/fs_.CountFeatures(fd::kUserDomains[5]),
          /*num_user6_features=*/fs_.CountFeatures(fd::kUserDomains[6]),
          /*num_user7_features=*/fs_.CountFeatures(fd::kUserDomains[7]),
          /*num_user8_features=*/fs_.CountFeatures(fd::kUserDomains[8]),
          /*num_user9_features=*/fs_.CountFeatures(fd::kUserDomains[9]),
          /*num_user10_features=*/fs_.CountFeatures(fd::kUserDomains[10]),
          /*num_user11_features=*/fs_.CountFeatures(fd::kUserDomains[11]),
          /*num_user12_features=*/fs_.CountFeatures(fd::kUserDomains[12]),
          /*num_user13_features=*/fs_.CountFeatures(fd::kUserDomains[13]),
          /*num_user14_features=*/fs_.CountFeatures(fd::kUserDomains[14]),
          /*num_user15_features=*/fs_.CountFeatures(fd::kUserDomains[15]),
          /*num_unknown_features=*/fs_.CountFeatures(fd::kUnknown),
          /*num_funcs_in_frontier=*/coverage_frontier_.NumFunctionsInFrontier(),
      },
      CorpusStats{
          /*active_corpus_size=*/corpus_.NumActive(),
          /*total_corpus_size=*/corpus_.NumTotal(),
          /*max_corpus_element_size=*/max_corpus_size,
          /*avg_corpus_element_size=*/avg_corpus_size,
      },
      RusageStats{
          /*engine_rusage_avg_millicores=*/static_cast<uint64_t>(
              std::lround(rusage_timing.cpu_hyper_cores * 1000)),
          /*engine_rusage_cpu_percent=*/
          static_cast<uint64_t>(
              std::lround(rusage_timing.cpu_utilization * 100)),
          /*engine_rusage_rss_mb=*/
          static_cast<uint64_t>(rusage_memory.mem_rss >> 20),
          /*engine_rusage_vsize_mb=*/
          static_cast<uint64_t>(rusage_memory.mem_vsize >> 20),
      },
  });

  if (env_.log_level < min_log_level) return;

  std::ostringstream os;
  auto LogIfNotZero = [&os](size_t value, std::string_view name) {
    if (!value) return;
    os << " " << name << ": " << value;
  };
  if (!env_.experiment_name.empty()) os << env_.experiment_name << " ";
  os << "[S" << env_.my_shard_index << "." << num_runs_ << "] " << log_type
     << ": ";
  os << fs_;
  os << " corp: " << corpus_.NumActive() << "/" << corpus_.NumTotal();
  LogIfNotZero(coverage_frontier_.NumFunctionsInFrontier(), "fr");
  LogIfNotZero(num_crashes_, "crash");
  os << " max/avg: " << max_corpus_size << "/" << avg_corpus_size << " "
     << corpus_.MemoryUsageString();
  os << " exec/s: "
     << (execs_per_sec < 1.0 ? execs_per_sec : std::round(execs_per_sec));
  os << " mb: " << (rusage_memory.mem_rss >> 20);
  LOG(INFO) << os.str();
}

void Centipede::LogFeaturesAsSymbols(const FeatureVec &fv) {
  if (!env_.LogFeaturesInThisShard()) return;
  for (auto feature : fv) {
    if (!feature_domains::kPCs.Contains(feature)) continue;
    PCIndex pc_index = ConvertPCFeatureToPcIndex(feature);
    auto description = coverage_logger_.ObserveAndDescribeIfNew(pc_index);
    if (description.empty()) continue;
    LOG(INFO) << description;
  }
}

bool Centipede::InputPassesFilter(const ByteArray &input) {
  if (env_.input_filter.empty()) return true;
  WriteToLocalFile(input_filter_path_, input);
  bool result = input_filter_cmd_.Execute() == EXIT_SUCCESS;
  std::filesystem::remove(input_filter_path_);
  return result;
}

bool Centipede::ExecuteAndReportCrash(std::string_view binary,
                                      const std::vector<ByteArray> &input_vec,
                                      BatchResult &batch_result) {
  bool success = user_callbacks_.Execute(binary, input_vec, batch_result);
  if (!success) ReportCrash(binary, input_vec, batch_result);
  return success || batch_result.IsIgnoredFailure();
}

// *** Highly experimental and risky. May not scale well for large targets. ***
//
// The idea: an unordered pair of two features {a, b} is by itself a feature.
// In the worst case, the number of such synthetic features is a square of
// the number of regular features, which may not scale.
// For now, we only treat pairs of PCs as features, which is still quadratic
// by the number of PCs. But in moderate-sized programs this may be tolerable.
//
// Rationale: if two different parts of the target are exercised simultaneously,
// this may create interesting behaviour that is hard to capture with regular
// control flow (or other) features.
size_t Centipede::AddPcPairFeatures(FeatureVec &fv) {
  // Using a scratch vector to avoid allocations.
  auto &pcs = add_pc_pair_scratch_;
  pcs.clear();

  size_t num_pcs = pc_table_.size();
  size_t num_added_pairs = 0;

  // Collect PCs from fv.
  for (auto feature : fv) {
    if (feature_domains::kPCs.Contains(feature))
      pcs.push_back(ConvertPCFeatureToPcIndex(feature));
  }

  // The quadratic loop: iterate all PC pairs (!!).
  for (size_t i = 0, n = pcs.size(); i < n; ++i) {
    size_t pc1 = pcs[i];
    for (size_t j = i + 1; j < n; ++j) {
      size_t pc2 = pcs[j];
      feature_t f = feature_domains::kPCPair.ConvertToMe(
          ConvertPcPairToNumber(pc1, pc2, num_pcs));
      // If we have seen this pair at least once, ignore it.
      if (fs_.Frequency(f) != 0) continue;
      fv.push_back(f);
      ++num_added_pairs;
    }
  }
  return num_added_pairs;
}

bool Centipede::RunBatch(
    const std::vector<ByteArray> &input_vec,
    BlobFileWriter *absl_nullable corpus_file,
    BlobFileWriter *absl_nullable features_file,
    BlobFileWriter *absl_nullable unconditional_features_file) {
  BatchResult batch_result;
  bool success = ExecuteAndReportCrash(env_.binary, input_vec, batch_result);
  CHECK_EQ(input_vec.size(), batch_result.results().size());

  for (const auto &extra_binary : env_.extra_binaries) {
    if (ShouldStop()) break;
    BatchResult extra_batch_result;
    success =
        ExecuteAndReportCrash(extra_binary, input_vec, extra_batch_result) &&
        success;
  }
  if (EarlyStopRequested()) return false;
  if (!success && env_.exit_on_crash) {
    LOG(INFO) << "--exit_on_crash is enabled; exiting soon";
    RequestEarlyStop(EXIT_FAILURE);
    return false;
  }
  CHECK_EQ(batch_result.results().size(), input_vec.size());
  num_runs_ += input_vec.size();
  bool batch_gained_new_coverage = false;
  for (size_t i = 0; i < input_vec.size(); i++) {
    if (ShouldStop()) break;
    FeatureVec &fv = batch_result.results()[i].mutable_features();
    bool function_filter_passed = function_filter_.filter(fv);
    bool input_gained_new_coverage = fs_.PruneFeaturesAndCountUnseen(fv) != 0;
    if (env_.use_pcpair_features && AddPcPairFeatures(fv) != 0)
      input_gained_new_coverage = true;
    if (unconditional_features_file != nullptr) {
      CHECK_OK(unconditional_features_file->Write(
          PackFeaturesAndHash(input_vec[i], fv)));
    }
    if (input_gained_new_coverage) {
      // TODO(kcc): [impl] add stats for filtered-out inputs.
      if (!InputPassesFilter(input_vec[i])) continue;
      fs_.IncrementFrequencies(fv);
      LogFeaturesAsSymbols(fv);
      batch_gained_new_coverage = true;
      CHECK_GT(fv.size(), 0UL);
      if (function_filter_passed) {
        corpus_.Add(input_vec[i], fv, batch_result.results()[i].metadata(), fs_,
                    coverage_frontier_);
      }
      if (corpus_file != nullptr) {
        CHECK_OK(corpus_file->Write(input_vec[i]));
      }
      if (!env_.corpus_dir.empty() && !env_.corpus_dir[0].empty()) {
        WriteToLocalHashedFileInDir(env_.corpus_dir[0], input_vec[i]);
      }
      if (features_file != nullptr) {
        CHECK_OK(features_file->Write(PackFeaturesAndHash(input_vec[i], fv)));
      }
    }
  }
  return batch_gained_new_coverage;
}

// TODO(kcc): [impl] don't reread the same corpus twice.
void Centipede::LoadShard(const Environment &load_env, size_t shard_index,
                          bool rerun) {
  VLOG(1) << "Loading shard " << shard_index
          << (rerun ? " with rerunning" : " without rerunning");
  size_t num_added_inputs = 0;
  size_t num_skipped_inputs = 0;
  std::vector<ByteArray> inputs_to_rerun;
  auto input_features_callback = [&](ByteArray input,
                                     FeatureVec input_features) {
    if (ShouldStop()) return;
    if (input_features.empty()) {
      if (rerun) {
        inputs_to_rerun.emplace_back(std::move(input));
      }
    } else {
      LogFeaturesAsSymbols(input_features);
      const auto num_new_features =
          fs_.PruneFeaturesAndCountUnseen(input_features);
      if (num_new_features != 0) {
        VLOG(10) << "Adding input " << Hash(input)
                 << "; new features: " << num_new_features;
        fs_.IncrementFrequencies(input_features);
        // TODO(kcc): cmp_args are currently not saved to disk and not reloaded.
        corpus_.Add(input, input_features, {}, fs_, coverage_frontier_);
        ++num_added_inputs;
      } else {
        VLOG(10) << "Skipping input: " << Hash(input);
        ++num_skipped_inputs;
      }
    }
  };

  // See serialize_shard_loads on why we may want to serialize shard loads.
  // TODO(kcc): remove serialize_shard_loads when LoadShards() uses less RAM.
  const WorkDir wd{load_env};
  const std::string corpus_path = wd.CorpusFilePaths().Shard(shard_index);
  const std::string features_path = wd.FeaturesFilePaths().Shard(shard_index);
  if (env_.serialize_shard_loads) {
    ABSL_CONST_INIT static absl::Mutex load_shard_mu{absl::kConstInit};
    absl::MutexLock lock(&load_shard_mu);
    ReadShard(corpus_path, features_path, input_features_callback);
  } else {
    ReadShard(corpus_path, features_path, input_features_callback);
  }

  VLOG(1) << "Loaded shard " << shard_index << ": added " << num_added_inputs
          << " / skipped " << num_skipped_inputs << " inputs";

  if (num_added_inputs > 0) UpdateAndMaybeLogStats("load-shard", 1);
  if (!inputs_to_rerun.empty()) Rerun(inputs_to_rerun);
}

void Centipede::LoadAllShardsInRandomOrder(const Environment &load_env,
                                           bool rerun_my_shard) {
  // TODO(ussuri): It seems logical to reset `corpus_` before this, but
  //  that broke `ShardsAndDistillTest` in testing/centipede_test.cc.
  //  Investigate.
  std::vector<size_t> shard_idxs(env_.total_shards);
  std::iota(shard_idxs.begin(), shard_idxs.end(), 0);
  std::shuffle(shard_idxs.begin(), shard_idxs.end(), rng_);
  size_t num_shards_loaded = 0;
  for (size_t shard_idx : shard_idxs) {
    const bool rerun = rerun_my_shard && shard_idx == env_.my_shard_index;
    LoadShard(load_env, shard_idx, rerun);
    LOG_IF(INFO, (++num_shards_loaded % 100) == 0) << VV(num_shards_loaded);
  }
}

void Centipede::Rerun(std::vector<ByteArray> &to_rerun) {
  if (to_rerun.empty()) return;
  auto features_file_path = wd_.FeaturesFilePaths().Shard(env_.my_shard_index);
  auto features_file = DefaultBlobFileWriterFactory(env_.riegeli);
  CHECK_OK(features_file->Open(features_file_path, "a"));

  LOG(INFO) << to_rerun.size() << " inputs to rerun";
  // Re-run all inputs for which we don't know their features.
  // Run in batches of at most env_.batch_size inputs each.
  while (!to_rerun.empty()) {
    if (ShouldStop()) break;
    size_t batch_size = std::min(to_rerun.size(), env_.batch_size);
    std::vector<ByteArray> batch(to_rerun.end() - batch_size, to_rerun.end());
    to_rerun.resize(to_rerun.size() - batch_size);
    if (RunBatch(batch, nullptr, nullptr, features_file.get())) {
      UpdateAndMaybeLogStats("rerun-old", 1);
    }
  }
}

void Centipede::GenerateCoverageReport(std::string_view filename_annotation,
                                       std::string_view description) {
  if (pc_table_.empty()) return;

  auto coverage_path = wd_.CoverageReportPath(filename_annotation);
  LOG(INFO) << "Generate coverage report [" << description << "]; "
            << VV(coverage_path);
  auto pci_vec = fs_.ToCoveragePCs();
  Coverage coverage(pc_table_, pci_vec);
  coverage.DumpReportToFile(symbols_, coverage_path, description);
}

void Centipede::GenerateCorpusStats(std::string_view filename_annotation,
                                    std::string_view description) {
  auto stats_path = wd_.CorpusStatsPath(filename_annotation);
  LOG(INFO) << "Generate corpus stats [" << description << "]; "
            << VV(stats_path);
  corpus_.DumpStatsToFile(fs_, stats_path, description);
}

// TODO(nedwill): add integration test once tests are refactored per b/255660879
void Centipede::GenerateSourceBasedCoverageReport(
    std::string_view filename_annotation, std::string_view description) {
  if (env_.clang_coverage_binary.empty()) return;

  auto report_path = wd_.SourceBasedCoverageReportPath(filename_annotation);
  LOG(INFO) << "Generate source based coverage report [" << description << "]; "
            << VV(report_path);
  CHECK_OK(RemoteMkdir(report_path));

  std::vector<std::string> raw_profiles = wd_.EnumerateRawCoverageProfiles();

  if (raw_profiles.empty()) {
    LOG(ERROR) << "No raw profiles found for coverage report";
    return;
  }

  std::string indexed_profile_path =
      wd_.SourceBasedCoverageIndexedProfilePath();

  std::vector<std::string> merge_arguments = {"merge", "-o",
                                              indexed_profile_path, "-sparse"};
  for (const std::string &raw_profile : raw_profiles) {
    merge_arguments.push_back(raw_profile);
  }

  Command::Options merge_cmd_options;
  merge_cmd_options.args = std::move(merge_arguments);
  Command merge_command{"llvm-profdata", std::move(merge_cmd_options)};
  if (merge_command.Execute() != EXIT_SUCCESS) {
    LOG(ERROR) << "Failed to run command " << merge_command.ToString();
    return;
  }

  Command::Options generate_report_cmd_options;
  generate_report_cmd_options.args = {
      "show", "-format=html", absl::StrCat("-output-dir=", report_path),
      absl::StrCat("-instr-profile=", indexed_profile_path),
      env_.clang_coverage_binary};
  Command generate_report_command{"llvm-cov",
                                  std::move(generate_report_cmd_options)};
  if (generate_report_command.Execute() != EXIT_SUCCESS) {
    LOG(ERROR) << "Failed to run command "
               << generate_report_command.ToString();
    return;
  }
}

void Centipede::GenerateRUsageReport(std::string_view filename_annotation,
                                     std::string_view description) {
  class ReportDumper : public RUsageProfiler::ReportSink {
   public:
    explicit ReportDumper(std::string_view path)
        : file_{*RemoteFileOpen(path, "w")} {
      CHECK(file_ != nullptr) << VV(path);
      CHECK_OK(RemoteFileSetWriteBufferSize(file_, 10UL * 1024 * 1024));
    }

    ~ReportDumper() override { CHECK_OK(RemoteFileClose(file_)); }

    ReportDumper &operator<<(std::string_view fragment) override {
      CHECK_OK(RemoteFileAppend(file_,
                                ByteArray{fragment.cbegin(), fragment.cend()}));
      return *this;
    }

   private:
    RemoteFile *file_;
  };

  const auto &snapshot = rusage_profiler_.TakeSnapshot(
      {__FILE__, __LINE__}, std::string{description});
  VLOG(1) << "Rusage @ " << description << ": " << snapshot.ShortMetricsStr();
  auto path = wd_.RUsageReportPath(filename_annotation);
  LOG(INFO) << "Generate rusage report [" << description << "]; "
            << VV(env_.my_shard_index) << VV(path);
  ReportDumper dumper{path};
  rusage_profiler_.GenerateReport(&dumper);
}

void Centipede::MaybeGenerateTelemetry(std::string_view filename_annotation,
                                       std::string_view description) {
  if (env_.DumpCorpusTelemetryInThisShard()) {
    GenerateCoverageReport(filename_annotation, description);
    GenerateCorpusStats(filename_annotation, description);
    GenerateSourceBasedCoverageReport(filename_annotation, description);
  }
  if (env_.DumpRUsageTelemetryInThisShard()) {
    GenerateRUsageReport(filename_annotation, description);
  }
}

void Centipede::MaybeGenerateTelemetryAfterBatch(
    std::string_view filename_annotation, size_t batch_index) {
  if (env_.DumpTelemetryForThisBatch(batch_index)) {
    MaybeGenerateTelemetry(  //
        filename_annotation, absl::StrCat("After batch ", batch_index));
  }
}

void Centipede::MergeFromOtherCorpus(std::string_view merge_from_dir,
                                     size_t shard_index_to_merge) {
  LOG(INFO) << __func__ << ": " << merge_from_dir;
  Environment merge_from_env = env_;
  merge_from_env.workdir = merge_from_dir;
  size_t initial_corpus_size = corpus_.NumActive();
  LoadShard(merge_from_env, shard_index_to_merge, /*rerun=*/true);
  size_t new_corpus_size = corpus_.NumActive();
  CHECK_GE(new_corpus_size, initial_corpus_size);  // Corpus can't shrink here.
  if (new_corpus_size > initial_corpus_size) {
    auto appender = DefaultBlobFileWriterFactory(env_.riegeli);
    CHECK_OK(
        appender->Open(wd_.CorpusFilePaths().Shard(env_.my_shard_index), "a"));
    for (size_t idx = initial_corpus_size; idx < new_corpus_size; ++idx) {
      CHECK_OK(appender->Write(corpus_.Get(idx)));
    }
    LOG(INFO) << "Merge: " << (new_corpus_size - initial_corpus_size)
              << " new inputs added";
  }
}

void Centipede::ReloadAllShardsAndWriteDistilledCorpus() {
  // Reload the shards. This automatically distills the corpus by discarding
  // inputs with duplicate feature sets as they are being added. Reloading
  // randomly leaves random winners from such sets of duplicates in the
  // distilled output: so multiple distilling shards will produce different
  // outputs from the same inputs (the property that we want).
  LoadAllShardsInRandomOrder(env_, /*rerun_my_shard=*/false);

  // Save the distilled corpus to a file in workdir and possibly to a hashed
  // file in the first corpus dir passed in `--corpus_dir`.
  const auto distill_to_path = wd_.DistilledCorpusFilePaths().MyShard();
  LOG(INFO) << "Distilling: shard: " << env_.my_shard_index
            << " output: " << distill_to_path << " "
            << " distilled size: " << corpus_.NumActive();
  const auto appender = DefaultBlobFileWriterFactory(env_.riegeli);
  // NOTE: Always overwrite distilled corpus files -- never append, unlike
  // "regular", per-shard corpus files.
  CHECK_OK(appender->Open(distill_to_path, "w"));
  for (size_t i = 0; i < corpus_.NumActive(); ++i) {
    const ByteArray &input = corpus_.Get(i);
    CHECK_OK(appender->Write(input));
    if (!env_.corpus_dir.empty() && !env_.corpus_dir[0].empty()) {
      WriteToLocalHashedFileInDir(env_.corpus_dir[0], input);
    }
  }
}

void Centipede::LoadSeedInputs(BlobFileWriter *absl_nonnull corpus_file,
                               BlobFileWriter *absl_nonnull features_file) {
  std::vector<ByteArray> seed_inputs;
  const size_t num_seeds_available =
      user_callbacks_.GetSeeds(env_.batch_size, seed_inputs);
  if (num_seeds_available > env_.batch_size) {
    LOG(WARNING) << "More seeds available than requested: "
                 << num_seeds_available << " > " << env_.batch_size;
  }
  if (seed_inputs.empty()) {
    QCHECK(!env_.require_seeds)
        << "No seeds returned and --require_seeds=true, exiting early.";
    LOG(WARNING)
        << "No seeds returned - will use the default seed of single byte {0}";
    seed_inputs.push_back({0});
  }

  RunBatch(seed_inputs, corpus_file, features_file,
           /*unconditional_features_file=*/nullptr);
  LOG(INFO) << "Number of input seeds available: " << num_seeds_available
            << ", number included in corpus: " << corpus_.NumTotal();

  // Forcely add all seed inputs to avoid empty corpus if none of them increased
  // coverage and passed the filters.
  if (corpus_.NumTotal() == 0) {
    for (const auto &seed_input : seed_inputs)
      corpus_.Add(seed_input, {}, {}, fs_, coverage_frontier_);
  }
}

void Centipede::FuzzingLoop() {
  LOG(INFO) << "Shard: " << env_.my_shard_index << "/" << env_.total_shards
            << " " << TemporaryLocalDirPath() << " "
            << "seed: " << env_.seed << "\n\n\n";

  UpdateAndMaybeLogStats("begin-fuzz", 0);

  if (env_.full_sync) {
    LoadAllShardsInRandomOrder(env_, /*rerun_my_shard=*/true);
  } else {
    LoadShard(env_, env_.my_shard_index, /*rerun=*/true);
  }

  if (!env_.merge_from.empty()) {
    // Merge a shard with the same index from another corpus.
    MergeFromOtherCorpus(env_.merge_from, env_.my_shard_index);
  }

  if (env_.load_shards_only) return;

  auto corpus_path = wd_.CorpusFilePaths().Shard(env_.my_shard_index);
  auto corpus_file = DefaultBlobFileWriterFactory(env_.riegeli);
  CHECK_OK(corpus_file->Open(corpus_path, "a"));
  auto features_path = wd_.FeaturesFilePaths().Shard(env_.my_shard_index);
  auto features_file = DefaultBlobFileWriterFactory(env_.riegeli);
  CHECK_OK(features_file->Open(features_path, "a"));

  LoadSeedInputs(corpus_file.get(), features_file.get());

  UpdateAndMaybeLogStats("init-done", 0);

  // If we're going to fuzz, dump the initial telemetry files. For a brand-new
  // run, these will be functionally empty, e.g. the coverage report will list
  // all target functions as not covered (NONE). For a bootstrapped run (the
  // workdir already has data), these may or may not coincide with the final
  // "latest" report of the previous run, depending on how the runs are
  // configured (the same number of shards, for example).
  if (env_.num_runs != 0) MaybeGenerateTelemetry("initial", "Before fuzzing");

  // Reset fuzz_start_time_ and num_runs_, so that the pre-init work doesn't
  // affect them.
  fuzz_start_time_ = absl::Now();
  num_runs_ = 0;

  // num_runs / batch_size, rounded up.
  size_t number_of_batches = env_.num_runs / env_.batch_size;
  if (env_.num_runs % env_.batch_size != 0) ++number_of_batches;
  size_t new_runs = 0;
  size_t corpus_size_at_last_prune = corpus_.NumActive();
  for (size_t batch_index = 0; batch_index < number_of_batches; batch_index++) {
    if (ShouldStop()) break;
    CHECK_LT(new_runs, env_.num_runs);
    auto remaining_runs = env_.num_runs - new_runs;
    auto batch_size = std::min(env_.batch_size, remaining_runs);
    std::vector<MutationInputRef> mutation_inputs;
    mutation_inputs.reserve(env_.mutate_batch_size);
    for (size_t i = 0; i < env_.mutate_batch_size; i++) {
      const auto &corpus_record = env_.use_corpus_weights
                                      ? corpus_.WeightedRandom(rng_())
                                      : corpus_.UniformRandom(rng_());
      mutation_inputs.push_back(
          MutationInputRef{corpus_record.data, &corpus_record.metadata});
    }

    const std::vector<ByteArray> mutants =
        user_callbacks_.Mutate(mutation_inputs, batch_size);
    if (ShouldStop()) break;

    bool gained_new_coverage =
        RunBatch(mutants, corpus_file.get(), features_file.get(), nullptr);
    new_runs += mutants.size();

    if (gained_new_coverage) {
      UpdateAndMaybeLogStats("new-feature", 1);
    } else if (((batch_index - 1) & batch_index) == 0) {
      // Log if batch_index is a power of two.
      UpdateAndMaybeLogStats("pulse", 1);
    }

    // Dump the intermediate telemetry files.
    MaybeGenerateTelemetryAfterBatch("latest", batch_index);

    if (env_.load_other_shard_frequency != 0 && batch_index != 0 &&
        (batch_index % env_.load_other_shard_frequency) == 0 &&
        env_.total_shards > 1) {
      size_t rand = rng_() % (env_.total_shards - 1);
      size_t other_shard_index =
          (env_.my_shard_index + 1 + rand) % env_.total_shards;
      CHECK_NE(other_shard_index, env_.my_shard_index);
      LoadShard(env_, other_shard_index, /*rerun=*/false);
    }

    // Prune if we added enough new elements since last prune.
    if (env_.prune_frequency != 0 &&
        corpus_.NumActive() >
            corpus_size_at_last_prune + env_.prune_frequency) {
      if (env_.use_coverage_frontier) coverage_frontier_.Compute(corpus_);
      corpus_.Prune(fs_, coverage_frontier_, env_.max_corpus_size, rng_);
      corpus_size_at_last_prune = corpus_.NumActive();
    }
  }

  // The tests rely on this stat being logged last.
  UpdateAndMaybeLogStats("end-fuzz", 0);

  // If we've fuzzed anything, dump the final telemetry files.
  if (env_.num_runs != 0) MaybeGenerateTelemetry("final", "After fuzzing");
}

void Centipede::ReportCrash(std::string_view binary,
                            const std::vector<ByteArray> &input_vec,
                            const BatchResult &batch_result) {
  CHECK_EQ(input_vec.size(), batch_result.results().size());

  const size_t suspect_input_idx = std::clamp<size_t>(
      batch_result.num_outputs_read(), 0, input_vec.size() - 1);
  auto log_execution_failure = [&](std::string_view log_prefix) {
    LOG(INFO) << log_prefix << "Batch execution failed:"
              << "\nBinary               : " << binary
              << "\nExit code            : " << batch_result.exit_code()
              << "\nFailure              : "
              << batch_result.failure_description()
              << "\nSignature            : "
              << AsPrintableString(AsByteSpan(batch_result.failure_signature()),
                                   /*max_len=*/32)
              << "\nNumber of inputs     : " << input_vec.size()
              << "\nNumber of inputs read: " << batch_result.num_outputs_read()
              << (batch_result.IsSetupFailure()
                      ? ""
                      : absl::StrCat("\nSuspect input index  : ",
                                     suspect_input_idx))
              << "\nCrash log            :\n\n";
    for (const auto &log_line :
         absl::StrSplit(absl::StripAsciiWhitespace(batch_result.log()), '\n')) {
      LOG(INFO).NoPrefix() << "CRASH LOG: " << log_line;
    }
    LOG(INFO).NoPrefix() << "\n";
  };

  if (batch_result.IsIgnoredFailure()) {
    LOG(INFO) << "Skip further processing of "
              << batch_result.failure_description();
    return;
  }

  if (batch_result.IsSkippedTest()) {
    log_execution_failure("Skipped Test: ");
    LOG(INFO) << "Requesting early stop due to skipped test.";
    RequestEarlyStop(EXIT_SUCCESS);
    return;
  }

  if (batch_result.IsSetupFailure()) {
    log_execution_failure("Test Setup Failure: ");
    LOG(INFO) << "Requesting early stop due to setup failure in the test.";
    RequestEarlyStop(EXIT_FAILURE);
    return;
  }

  // Skip reporting only if RequestEarlyStop is called - still reporting if time
  // runs out.
  if (EarlyStopRequested()) return;

  if (++num_crashes_ > env_.max_num_crash_reports) return;

  const std::string log_prefix =
      absl::StrCat("ReportCrash[", num_crashes_, "]: ");
  log_execution_failure(log_prefix);

  LOG_IF(INFO, num_crashes_ == env_.max_num_crash_reports)
      << log_prefix
      << "Reached --max_num_crash_reports: further reports will be suppressed";

  if (batch_result.failure_description() == kExecutionFailurePerBatchTimeout) {
    LOG(INFO) << log_prefix
              << "Failure applies to entire batch: not executing inputs "
                 "one-by-one, trying to find the reproducer";
    return;
  }

  // Determine the optimal order of the inputs to try to maximize the chances of
  // finding the reproducer fast.
  std::vector<size_t> input_idxs_to_try;
  // Prioritize the presumed crasher by inserting it in front of everything
  // else.
  input_idxs_to_try.push_back(suspect_input_idx);
  if (!env_.batch_triage_suspect_only) {
    // TODO(b/274705740): When the bug is fixed, set `input_idxs_to_try`'s size
    // to `suspect_input_idx + 1`.
    input_idxs_to_try.resize(input_vec.size() + 1);
    // Keep the suspect at the old location, too, in case the target was
    // primed for a crash by the sequence of inputs that preceded the crasher.
    std::iota(input_idxs_to_try.begin() + 1, input_idxs_to_try.end(), 0);
  } else {
    LOG(INFO)
        << log_prefix
        << "Skip finding the reproducer from the inputs other than the suspect";
  }

  // Try inputs one-by-one in the determined order.
  LOG(INFO) << log_prefix
            << "Executing inputs one-by-one, trying to find the reproducer";
  for (auto input_idx : input_idxs_to_try) {
    if (ShouldStop()) return;
    const auto &one_input = input_vec[input_idx];
    BatchResult one_input_batch_result;
    if (!user_callbacks_.Execute(binary, {one_input}, one_input_batch_result)) {
      auto hash = Hash(one_input);
      auto crash_dir = wd_.CrashReproducerDirPaths().MyShard();
      CHECK_OK(RemoteMkdir(crash_dir));
      std::string input_file_path = std::filesystem::path(crash_dir) / hash;
      auto crash_metadata_dir = wd_.CrashMetadataDirPaths().MyShard();
      CHECK_OK(RemoteMkdir(crash_metadata_dir));
      std::string crash_metadata_path_prefix =
          std::filesystem::path(crash_metadata_dir) / hash;
      LOG(INFO) << log_prefix << "Detected crash-reproducing input:"
                << "\nInput index    : " << input_idx << "\nInput bytes    : "
                << AsPrintableString(one_input, /*max_len=*/32)
                << "\nExit code      : " << one_input_batch_result.exit_code()
                << "\nFailure        : "
                << one_input_batch_result.failure_description()
                << "\nSignature      : "
                << AsPrintableString(
                       AsByteSpan(one_input_batch_result.failure_signature()),
                       /*max_len=*/32)
                << "\nSaving input to: " << input_file_path
                << "\nSaving crash"  //
                << "\nmetadata to    : " << crash_metadata_path_prefix << ".*";
      CHECK_OK(RemoteFileSetContents(input_file_path, one_input));
      CHECK_OK(RemoteFileSetContents(
          absl::StrCat(crash_metadata_path_prefix, ".desc"),
          one_input_batch_result.failure_description()));
      CHECK_OK(RemoteFileSetContents(
          absl::StrCat(crash_metadata_path_prefix, ".sig"),
          one_input_batch_result.failure_signature()));
      return;
    }
  }

  LOG(INFO) << log_prefix
            << "Crash was not observed when running inputs one-by-one";

  // There will be cases when several inputs collectively cause a crash, but no
  // single input does. Handle this by writing out the inputs from the batch
  // between 0 and `suspect_input_idx` (inclusive) as individual files under the
  // <--workdir>/crash/crashing_batch-<HASH_OF_SUSPECT_INPUT> directory.
  // TODO(bookholt): Check for repro by re-running the whole batch.
  // TODO(ussuri): Consolidate the crash reproduction logic here and above.
  // TODO(ussuri): This can create a lot of tiny files. Write to a single
  //  shard-like corpus file instead.
  const auto &suspect_input = input_vec[suspect_input_idx];
  auto suspect_hash = Hash(suspect_input);
  auto crash_dir = wd_.CrashReproducerDirPaths().MyShard();
  CHECK_OK(RemoteMkdir(crash_dir));
  std::string crashing_batch_name =
      absl::StrCat("crashing_batch-", suspect_hash);
  std::string save_dir = std::filesystem::path(crash_dir) / crashing_batch_name;
  CHECK_OK(RemoteMkdir(save_dir));
  LOG(INFO) << log_prefix << "Saving used inputs from batch to: " << save_dir;
  for (int i = 0; i <= suspect_input_idx; ++i) {
    const auto &one_input = input_vec[i];
    auto hash = Hash(one_input);
    std::string file_path = std::filesystem::path(save_dir).append(
        absl::StrFormat("input-%010d-%s", i, hash));
    CHECK_OK(RemoteFileSetContents(file_path, one_input));
  }
  auto crash_metadata_dir = wd_.CrashMetadataDirPaths().MyShard();
  CHECK_OK(RemoteMkdir(crash_metadata_dir));
  std::string crash_metadata_file_path =
      std::filesystem::path(crash_metadata_dir) / crashing_batch_name;
  LOG(INFO) << log_prefix
            << "Saving crash metadata to: " << crash_metadata_file_path;
  CHECK_OK(RemoteFileSetContents(crash_metadata_file_path,
                                 batch_result.failure_description()));
}

}  // namespace fuzztest::internal
