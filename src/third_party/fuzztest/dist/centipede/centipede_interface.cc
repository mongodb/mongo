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

#include "./centipede/centipede_interface.h"

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>  // NOLINT
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/base/optimization.h"
#include "absl/cleanup/cleanup.h"
#include "absl/container/flat_hash_set.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/escaping.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_replace.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "./centipede/analyze_corpora.h"
#include "./centipede/binary_info.h"
#include "./centipede/centipede.h"
#include "./centipede/centipede_callbacks.h"
#include "./centipede/command.h"
#include "./centipede/coverage.h"
#include "./centipede/crash_summary.h"
#include "./centipede/distill.h"
#include "./centipede/environment.h"
#include "./centipede/minimize_crash.h"
#include "./centipede/pc_info.h"
#include "./centipede/periodic_action.h"
#include "./centipede/runner_result.h"
#include "./centipede/seed_corpus_maker_lib.h"
#include "./centipede/stats.h"
#include "./centipede/stop.h"
#include "./centipede/thread_pool.h"
#include "./centipede/util.h"
#include "./centipede/workdir.h"
#include "./common/bazel.h"
#include "./common/blob_file.h"
#include "./common/defs.h"
#include "./common/hash.h"
#include "./common/logging.h"  // IWYU pragma: keep
#include "./common/remote_file.h"
#include "./common/status_macros.h"
#include "./fuzztest/internal/configuration.h"

namespace fuzztest::internal {

namespace {

// Sets signal handler for SIGINT.
// TODO(b/378532202): Replace this with a more generic mechanism that allows
// the called or `CentipedeMain()` to indicate when to stop.
void SetSignalHandlers() {
  struct sigaction sigact = {};
  sigact.sa_flags = SA_ONSTACK;
  sigact.sa_handler = [](int received_signum) {
    if (received_signum == SIGINT) {
      LOG(INFO) << "Ctrl-C pressed: winding down";
      RequestEarlyStop(EXIT_FAILURE);
      return;
    }
    ABSL_UNREACHABLE();
  };
  sigaction(SIGINT, &sigact, nullptr);
}

// Runs env.for_each_blob on every blob extracted from env.args.
// Returns EXIT_SUCCESS on success, EXIT_FAILURE otherwise.
int ForEachBlob(const Environment &env) {
  auto tmpdir = TemporaryLocalDirPath();
  CreateLocalDirRemovedAtExit(tmpdir);
  std::string tmpfile = std::filesystem::path(tmpdir).append("t");

  for (const auto &arg : env.args) {
    LOG(INFO) << "Running '" << env.for_each_blob << "' on " << arg;
    auto blob_reader = DefaultBlobFileReaderFactory();
    absl::Status open_status = blob_reader->Open(arg);
    if (!open_status.ok()) {
      LOG(INFO) << "Failed to open " << arg << ": " << open_status;
      return EXIT_FAILURE;
    }
    ByteSpan blob;
    while (blob_reader->Read(blob) == absl::OkStatus()) {
      ByteArray bytes;
      bytes.insert(bytes.begin(), blob.data(), blob.end());
      // TODO(kcc): [impl] add a variant of WriteToLocalFile that accepts Span.
      WriteToLocalFile(tmpfile, bytes);
      std::string command_line = absl::StrReplaceAll(
          env.for_each_blob, {{"%P", tmpfile}, {"%H", Hash(bytes)}});
      Command cmd(command_line);
      // TODO(kcc): [as-needed] this creates one process per blob.
      // If this flag gets active use, we may want to define special cases,
      // e.g. if for_each_blob=="cp %P /some/where" we can do it in-process.
      cmd.Execute();
      if (ShouldStop()) return ExitCode();
    }
  }
  return EXIT_SUCCESS;
}

// Loads corpora from work dirs provided in `env.args`, if there are two args
// provided, analyzes differences. If there is one arg provided, reports the
// function coverage. Returns EXIT_SUCCESS on success, EXIT_FAILURE otherwise.
int Analyze(const Environment &env) {
  LOG(INFO) << "Analyze " << absl::StrJoin(env.args, ",");
  CHECK(!env.binary.empty()) << "--binary must be used";
  if (env.args.size() == 1) {
    const CoverageResults coverage_results =
        GetCoverage(env.binary_name, env.binary_hash, env.args[0]);
    WorkDir workdir{env};
    const std::string coverage_report_path =
        workdir.CoverageReportPath(/*annotation=*/"");
    DumpCoverageReport(coverage_results, coverage_report_path);
  } else if (env.args.size() == 2) {
    AnalyzeCorporaToLog(env.binary_name, env.binary_hash, env.args[0],
                        env.args[1]);
  } else {
    LOG(FATAL) << "for now, --analyze supports only 1 or 2 work dirs; got "
               << env.args.size();
  }
  return EXIT_SUCCESS;
}

void SavePCTableToFile(const PCTable &pc_table, std::string_view file_path) {
  WriteToLocalFile(file_path, AsByteSpan(pc_table));
}

BinaryInfo PopulateBinaryInfoAndSavePCsIfNecessary(
    const Environment &env, CentipedeCallbacksFactory &callbacks_factory,
    std::string &pcs_file_path) {
  BinaryInfo binary_info;
  // Some fuzz targets have coverage not based on instrumenting binaries.
  // For those target, we should not populate binary info.
  if (env.populate_binary_info) {
    ScopedCentipedeCallbacks scoped_callbacks(callbacks_factory, env);
    scoped_callbacks.callbacks()->PopulateBinaryInfo(binary_info);
  }
  if (env.save_binary_info) {
    const std::string binary_info_dir = WorkDir{env}.BinaryInfoDirPath();
    CHECK_OK(RemoteMkdir(binary_info_dir));
    LOG(INFO) << "Serializing binary info to: " << binary_info_dir;
    binary_info.Write(binary_info_dir);
  }
  if (binary_info.uses_legacy_trace_pc_instrumentation) {
    pcs_file_path = std::filesystem::path(TemporaryLocalDirPath()) / "pcs";
    SavePCTableToFile(binary_info.pc_table, pcs_file_path);
  }
  if (env.use_pcpair_features) {
    CHECK(!binary_info.pc_table.empty())
        << "--use_pcpair_features requires non-empty pc_table";
  }
  return binary_info;
}

std::vector<Environment> CreateEnvironmentsForThreads(
    const Environment &origin_env, std::string_view pcs_file_path) {
  std::vector<Environment> envs(origin_env.num_threads, origin_env);
  size_t thread_idx = 0;
  for (auto &env : envs) {
    env.my_shard_index += thread_idx++;
    env.UpdateForExperiment();
    env.pcs_file_path = pcs_file_path;
  }
  return envs;
}

int Fuzz(const Environment &env, const BinaryInfo &binary_info,
         std::string_view pcs_file_path,
         CentipedeCallbacksFactory &callbacks_factory) {
  CoverageLogger coverage_logger(binary_info.pc_table, binary_info.symbols);

  std::vector<Environment> envs =
      CreateEnvironmentsForThreads(env, pcs_file_path);
  std::vector<std::atomic<Stats>> stats_vec(env.num_threads);

  // Start periodic stats dumping and, optionally, logging.
  std::vector<PeriodicAction> stats_reporters;
  stats_reporters.emplace_back(
      [csv_appender = StatsCsvFileAppender{stats_vec, envs}]() mutable {
        csv_appender.ReportCurrStats();
      },
      PeriodicAction::Options{
          /*sleep_before_each=*/
          [](size_t iteration) {
            return absl::Minutes(std::clamp(iteration, 0UL, 10UL));
          },
      });
  if (!envs.front().experiment.empty() || ABSL_VLOG_IS_ON(1)) {
    stats_reporters.emplace_back(
        [logger = StatsLogger{stats_vec, envs}]() mutable {
          logger.ReportCurrStats();
        },
        PeriodicAction::Options{
            /*sleep_before_each=*/
            [](size_t iteration) {
              return absl::Seconds(std::clamp(iteration, 5UL, 600UL));
            },
        });
  }

  auto fuzzing_worker =
      [&env, &callbacks_factory, &binary_info, &coverage_logger](
          Environment &my_env, std::atomic<Stats> &stats, bool create_tmpdir) {
        if (create_tmpdir) CreateLocalDirRemovedAtExit(TemporaryLocalDirPath());
        // Uses TID, call in this thread.
        my_env.seed = GetRandomSeed(env.seed);

        if (env.dry_run) return;

        ScopedCentipedeCallbacks scoped_callbacks(callbacks_factory, my_env);
        Centipede centipede(my_env, *scoped_callbacks.callbacks(), binary_info,
                            coverage_logger, stats);
        centipede.FuzzingLoop();
      };

  if (env.num_threads == 1) {
    // When fuzzing with one thread, run fuzzing loop in the current
    // thread. This is because FuzzTest/Centipede's single-process
    // fuzzing requires the test body, which is invoked by the fuzzing
    // loop, to run in the main thread.
    //
    // Here, the fuzzing worker should not re-create the tmpdir since the path
    // is thread-local and it has been created in the current function.
    fuzzing_worker(envs[0], stats_vec[0], /*create_tmpdir=*/false);
  } else {
    ThreadPool fuzzing_worker_threads{static_cast<int>(env.num_threads)};
    for (size_t thread_idx = 0; thread_idx < env.num_threads; thread_idx++) {
      Environment &my_env = envs[thread_idx];
      std::atomic<Stats> &my_stats = stats_vec[thread_idx];
      fuzzing_worker_threads.Schedule([&fuzzing_worker, &my_env, &my_stats]() {
        fuzzing_worker(my_env, my_stats, /*create_tmpdir=*/true);
      });
    }  // All `fuzzing_worker_threads` join here.
  }

  for (auto &reporter : stats_reporters) {
    // Nudge one final update and stop the reporting thread.
    reporter.Nudge();
    reporter.Stop();
  }

  if (!env.knobs_file.empty()) PrintRewardValues(stats_vec, std::cerr);

  return ExitCode();
}


TestShard SetUpTestSharding() {
  TestShard test_shard = GetBazelTestShard();
  // Update the shard status file to indicate that we support test sharding.
  // It suffices to update the file's modification time, but we clear the
  // contents for simplicity. This is also what the GoogleTest framework does.
  if (const char *test_shard_status_file =
          std::getenv("TEST_SHARD_STATUS_FILE");
      test_shard_status_file != nullptr) {
    ClearLocalFileContents(test_shard_status_file);
  }
  return test_shard;
}

// Prunes non-reproducible and duplicate crashes and returns the crash
// signatures of the remaining crashes.
absl::flat_hash_set<std::string> PruneOldCrashesAndGetRemainingCrashSignatures(
    const std::filesystem::path &crashing_dir, const Environment &env,
    CentipedeCallbacksFactory &callbacks_factory, CrashSummary &crash_summary) {
  const std::vector<std::string> crashing_input_files =
      // The corpus database layout assumes the crash input files are located
      // directly in the crashing subdirectory, so we don't list recursively.
      ValueOrDie(RemoteListFiles(crashing_dir.c_str(), /*recursively=*/false));
  ScopedCentipedeCallbacks scoped_callbacks(callbacks_factory, env);
  BatchResult batch_result;
  absl::flat_hash_set<std::string> remaining_crash_signatures;

  for (const std::string &crashing_input_file : crashing_input_files) {
    ByteArray crashing_input;
    CHECK_OK(RemoteFileGetContents(crashing_input_file, crashing_input));
    const bool is_reproducible = !scoped_callbacks.callbacks()->Execute(
        env.binary, {crashing_input}, batch_result);
    const bool is_duplicate =
        is_reproducible && !batch_result.IsSetupFailure() &&
        !remaining_crash_signatures.insert(batch_result.failure_signature())
             .second;
    if (!is_reproducible || batch_result.IsSetupFailure() || is_duplicate) {
      CHECK_OK(RemotePathDelete(crashing_input_file, /*recursively=*/false));
    } else {
      crash_summary.AddCrash(
          {std::filesystem::path(crashing_input_file).filename(),
           /*category=*/batch_result.failure_description(),
           batch_result.failure_signature(),
           batch_result.failure_description()});
      CHECK_OK(RemotePathTouchExistingFile(crashing_input_file));
    }
  }
  return remaining_crash_signatures;
}

// TODO(b/405382531): Add unit tests once the function is unit-testable.
void DeduplicateAndStoreNewCrashes(
    const std::filesystem::path &crashing_dir, const WorkDir &workdir,
    size_t total_shards, absl::flat_hash_set<std::string> crash_signatures,
    CrashSummary &crash_summary) {
  for (size_t shard_idx = 0; shard_idx < total_shards; ++shard_idx) {
    const std::vector<std::string> new_crashing_input_files =
        // The crash reproducer directory may contain subdirectories with
        // input files that don't individually cause a crash. We ignore those
        // for now and don't list the files recursively.
        ValueOrDie(
            RemoteListFiles(workdir.CrashReproducerDirPaths().Shard(shard_idx),
                            /*recursively=*/false));
    const std::filesystem::path crash_metadata_dir =
        workdir.CrashMetadataDirPaths().Shard(shard_idx);

    CHECK_OK(RemoteMkdir(crashing_dir.c_str()));
    for (const std::string &crashing_input_file : new_crashing_input_files) {
      const std::string crashing_input_file_name =
          std::filesystem::path(crashing_input_file).filename();
      const std::string crash_signature_path =
          crash_metadata_dir / absl::StrCat(crashing_input_file_name, ".sig");
      std::string new_crash_signature;
      const absl::Status status =
          RemoteFileGetContents(crash_signature_path, new_crash_signature);
      if (!status.ok()) {
        LOG(WARNING) << "Ignoring crashing input " << crashing_input_file_name
                     << " due to failure to read the crash signature: "
                     << status;
        continue;
      }
      const bool is_duplicate =
          !crash_signatures.insert(new_crash_signature).second;
      if (is_duplicate) continue;

      const std::string crash_description_path =
          crash_metadata_dir / absl::StrCat(crashing_input_file_name, ".desc");
      std::string new_crash_description;
      const absl::Status description_status =
          RemoteFileGetContents(crash_description_path, new_crash_description);
      if (!description_status.ok()) {
        LOG(WARNING)
            << "Failed to read crash description for "
            << crashing_input_file_name
            << ". Will use the crash signature as the description. Status: "
            << description_status;
        new_crash_description = new_crash_signature;
      }
      crash_summary.AddCrash({crashing_input_file_name,
                              /*category=*/new_crash_description,
                              std::move(new_crash_signature),
                              new_crash_description});
      CHECK_OK(
          RemoteFileRename(crashing_input_file,
                           (crashing_dir / crashing_input_file_name).c_str()));
    }
  }
}

// Seeds the corpus files in `env.workdir` with the inputs in `regression_dir`
// (always used) and the previously distilled corpus files from `coverage_dir`
// (used if non-empty).
SeedCorpusConfig GetSeedCorpusConfig(const Environment &env,
                                     std::string_view regression_dir,
                                     std::string_view coverage_dir) {
  const WorkDir workdir{env};
  SeedCorpusSource regression;
  regression.dir_glob = std::string(regression_dir);
  regression.num_recent_dirs = 1;
  regression.individual_input_rel_glob = "*";
  regression.sampled_fraction_or_count = 1.0f;
  std::vector<SeedCorpusSource> sources = {std::move(regression)};
  if (!coverage_dir.empty()) {
    SeedCorpusSource coverage;
    coverage.dir_glob = std::string(coverage_dir);
    coverage.num_recent_dirs = 1;
    // We're using the previously distilled corpus files as seeds.
    coverage.shard_rel_glob =
        std::filesystem::path{
            workdir.DistilledCorpusFilePaths().AllShardsGlob()}
            .filename();
    coverage.individual_input_rel_glob = "*";
    coverage.sampled_fraction_or_count = 1.0f;
    sources.push_back(std::move(coverage));
  }
  SeedCorpusDestination destination;
  destination.dir_path = env.workdir;
  // We're seeding the current corpus files.
  destination.shard_rel_glob =
      std::filesystem::path{workdir.CorpusFilePaths().AllShardsGlob()}
          .filename();
  destination.shard_index_digits = WorkDir::kDigitsInShardIndex;
  destination.num_shards = static_cast<uint32_t>(env.num_threads);
  return {
      std::move(sources),
      std::move(destination),
  };
}

absl::Duration ReadFuzzingTime(std::string_view fuzzing_time_file) {
  std::string fuzzing_time_str;
  CHECK_OK(RemoteFileGetContents(fuzzing_time_file, fuzzing_time_str));
  absl::Duration fuzzing_time;
  if (!absl::ParseDuration(absl::StripAsciiWhitespace(fuzzing_time_str),
                           &fuzzing_time)) {
    LOG(WARNING) << "Failed to parse fuzzing time of a resuming fuzz test: '"
                 << fuzzing_time_str << "'. Assuming no fuzzing time so far.";
    return absl::ZeroDuration();
  }
  return fuzzing_time;
}

PeriodicAction RecordFuzzingTime(std::string_view fuzzing_time_file,
                                 absl::Time start_time) {
  return {[=] {
            absl::Status status = RemoteFileSetContents(
                fuzzing_time_file,
                absl::FormatDuration(absl::Now() - start_time));
            LOG_IF(WARNING, !status.ok())
                << "Failed to write fuzzing time: " << status;
          },
          PeriodicAction::ZeroDelayConstInterval(absl::Seconds(15))};
}

// TODO(b/368325638): Add tests for this.
int UpdateCorpusDatabaseForFuzzTests(
    Environment env, const fuzztest::internal::Configuration &fuzztest_config,
    CentipedeCallbacksFactory &callbacks_factory) {
  env.UpdateWithTargetConfig(fuzztest_config);

  absl::Time start_time = absl::Now();
  LOG(INFO) << "Starting the update of the corpus database for fuzz tests:"
            << "\nBinary: " << env.binary
            << "\nCorpus database: " << fuzztest_config.corpus_database;

  // Step 1: Preliminary set up of test sharding, binary info, etc.
  const auto [test_shard_index, total_test_shards] = SetUpTestSharding();
  const auto corpus_database_path =
      std::filesystem::path(fuzztest_config.corpus_database) /
      fuzztest_config.binary_identifier;
  const auto stats_root_path =
      fuzztest_config.stats_root.empty()
          ? std::filesystem::path()
          : std::filesystem::path(fuzztest_config.stats_root) /
                fuzztest_config.binary_identifier;
  const auto workdir_root_path =
      fuzztest_config.workdir_root.empty()
          ? corpus_database_path
          : std::filesystem::path(fuzztest_config.workdir_root) /
                fuzztest_config.binary_identifier;
  const auto execution_stamp = [] {
    std::string stamp =
        absl::FormatTime("%Y-%m-%d-%H-%M-%S", absl::Now(), absl::UTCTimeZone());
    return stamp;
  }();
  std::vector<std::string> fuzz_tests_to_run;
  if (env.fuzztest_single_test_mode) {
    CHECK(fuzztest_config.fuzz_tests_in_current_shard.size() == 1)
        << "Must select exactly one fuzz test when running in the single test "
           "mode";
    fuzz_tests_to_run = fuzztest_config.fuzz_tests_in_current_shard;
  } else {
    for (int i = 0; i < fuzztest_config.fuzz_tests.size(); ++i) {
      if (i % total_test_shards == test_shard_index) {
        fuzz_tests_to_run.push_back(fuzztest_config.fuzz_tests[i]);
      }
    }
  }
  LOG(INFO) << "Fuzz tests to run:" << absl::StrJoin(fuzz_tests_to_run, ", ");

  const bool is_workdir_specified = !env.workdir.empty();
  CHECK(!is_workdir_specified || env.fuzztest_single_test_mode);
  // When env.workdir is empty, the full workdir paths will be formed by
  // appending the fuzz test names to the base workdir path. We use different
  // path when only replaying to avoid replaying an unfinished fuzzing sessions.
  const auto base_workdir_path =
      is_workdir_specified
          ? std::filesystem::path{}  // Will not be used.
          : workdir_root_path /
                absl::StrFormat("workdir%s.%03d",
                                fuzztest_config.only_replay ? "-replay" : "",
                                test_shard_index);
  // There's no point in saving the binary info to the workdir, since the
  // workdir is deleted at the end.
  env.save_binary_info = false;
  std::string pcs_file_path;
  BinaryInfo binary_info = PopulateBinaryInfoAndSavePCsIfNecessary(
      env, callbacks_factory, pcs_file_path);

  LOG(INFO) << "Test shard index: " << test_shard_index
            << " Total test shards: " << total_test_shards;

  // Step 2: Iterate over the fuzz tests and run them.
  const std::string binary = env.binary;
  for (int i = 0; i < fuzz_tests_to_run.size(); ++i) {
    // Clean up previous stop requests. stop_time will be set later.
    ClearEarlyStopRequestAndSetStopTime(/*stop_time=*/absl::InfiniteFuture());
    if (!env.fuzztest_single_test_mode &&
        fuzztest_config.GetTimeLimitPerTest() < absl::InfiniteDuration()) {
      const absl::Duration test_time_limit =
          fuzztest_config.GetTimeLimitPerTest();
      const absl::Status has_enough_time = VerifyBazelHasEnoughTimeToRunTest(
          start_time, test_time_limit,
          /*executed_tests_in_shard=*/i, fuzztest_config.fuzz_tests.size());
      CHECK_OK(has_enough_time)
          << "Not enough time for running the fuzz test "
          << fuzz_tests_to_run[i] << " for " << test_time_limit;
    }
    if (!is_workdir_specified) {
      env.workdir = base_workdir_path / fuzz_tests_to_run[i];
    }
    const auto execution_id_path =
        (base_workdir_path /
         absl::StrCat(fuzz_tests_to_run[i], ".execution_id"))
            .string();

    bool is_resuming = false;
    if (!is_workdir_specified && fuzztest_config.execution_id.has_value()) {
      // Use the execution IDs to resume or skip tests.
      const bool execution_id_matched = [&] {
        if (!RemotePathExists(execution_id_path)) return false;
        CHECK(!RemotePathIsDirectory(execution_id_path));
        std::string prev_execution_id;
        CHECK_OK(RemoteFileGetContents(execution_id_path, prev_execution_id));
        return prev_execution_id == *fuzztest_config.execution_id;
      }();
      if (execution_id_matched) {
        // If execution IDs match but the previous coverage is missing, it means
        // the test was previously finished, and we skip running for the test.
        if (!RemotePathExists(WorkDir{env}.CoverageDirPath())) {
          LOG(INFO) << "Skipping running the fuzz test "
                    << fuzz_tests_to_run[i];
          continue;
        }
        // If execution IDs match and the previous coverage exists, it means
        // the same workflow got interrupted when running the test. So we resume
        // the test.
        is_resuming = true;
        LOG(INFO) << "Resuming running the fuzz test " << fuzz_tests_to_run[i];
      } else {
        // If the execution IDs mismatch, we start a new run.
        is_resuming = false;
        LOG(INFO) << "Starting a new run of the fuzz test "
                  << fuzz_tests_to_run[i];
      }
    }
    if (RemotePathExists(env.workdir) && !is_resuming) {
      // This could be a workdir from a failed run that used a different version
      // of the binary. We delete it so that we don't have to deal with
      // the assumptions under which it is safe to reuse an old workdir.
      CHECK_OK(RemotePathDelete(env.workdir, /*recursively=*/true));
    }
    const WorkDir workdir{env};
    CHECK_OK(RemoteMkdir(
        workdir.CoverageDirPath()));  // Implicitly creates the workdir

    // Updating execution ID must be after creating the coverage dir. Otherwise
    // if it fails to create coverage dir after updating execution ID, next
    // attempt would skip this test.
    if (!is_workdir_specified && fuzztest_config.execution_id.has_value() &&
        !is_resuming) {
      CHECK_OK(RemoteFileSetContents(execution_id_path,
                                     *fuzztest_config.execution_id));
    }

    absl::Cleanup clean_up_workdir = [is_workdir_specified, &env] {
      if (!is_workdir_specified && !EarlyStopRequested()) {
        CHECK_OK(RemotePathDelete(env.workdir, /*recursively=*/true));
      }
    };

    const std::filesystem::path fuzztest_db_path =
        corpus_database_path / fuzz_tests_to_run[i];
    const std::filesystem::path regression_dir =
        fuzztest_db_path / "regression";
    const std::filesystem::path coverage_dir = fuzztest_db_path / "coverage";

    // Seed the fuzzing session with the latest coverage corpus and regression
    // inputs from the previous fuzzing session.
    if (!is_resuming) {
      CHECK_OK(GenerateSeedCorpusFromConfig(
          GetSeedCorpusConfig(env, regression_dir.c_str(),
                              fuzztest_config.replay_coverage_inputs
                                  ? coverage_dir.c_str()
                                  : ""),
          env.binary_name, env.binary_hash))
          << "while generating the seed corpus";
    }

    if (!env.fuzztest_single_test_mode) {
      // TODO: b/338217594 - Call the FuzzTest binary in a flag-agnostic way.
      constexpr std::string_view kFuzzTestFuzzFlag = "--fuzz=";
      constexpr std::string_view kFuzzTestReplayCorpusFlag =
          "--replay_corpus=";
      std::string_view test_selection_flag = fuzztest_config.only_replay
                                                 ? kFuzzTestReplayCorpusFlag
                                                 : kFuzzTestFuzzFlag;
      env.binary =
          absl::StrCat(binary, " ", test_selection_flag, fuzz_tests_to_run[i]);
    }

    absl::Duration time_limit = fuzztest_config.GetTimeLimitPerTest();
    absl::Duration time_spent = absl::ZeroDuration();
    const std::string fuzzing_time_file =
        std::filesystem::path(env.workdir) / "fuzzing_time";
    if (is_resuming && RemotePathExists(fuzzing_time_file)) {
      time_spent = ReadFuzzingTime(fuzzing_time_file);
      time_limit = std::max(time_limit - time_spent, absl::ZeroDuration());
    }
    is_resuming = false;

    if (EarlyStopRequested()) {
      LOG(INFO) << "Skipping test " << fuzz_tests_to_run[i]
                << " because early stop requested.";
      continue;
    }

    LOG(INFO) << (fuzztest_config.only_replay ? "Replaying " : "Fuzzing ")
              << fuzz_tests_to_run[i] << " for " << time_limit
              << "\n\tTest binary: " << env.binary;

    const absl::Time start_time = absl::Now();
    ClearEarlyStopRequestAndSetStopTime(/*stop_time=*/start_time + time_limit);
    PeriodicAction record_fuzzing_time =
        RecordFuzzingTime(fuzzing_time_file, start_time - time_spent);
    Fuzz(env, binary_info, pcs_file_path, callbacks_factory);
    record_fuzzing_time.Nudge();
    record_fuzzing_time.Stop();

    if (!stats_root_path.empty()) {
      const auto stats_dir = stats_root_path / fuzz_tests_to_run[i];
      CHECK_OK(RemoteMkdir(stats_dir.c_str()));
      CHECK_OK(RemoteFileRename(
          workdir.FuzzingStatsPath(),
          (stats_dir / absl::StrCat("fuzzing_stats_", execution_stamp))
              .c_str()));
    }

    if (EarlyStopRequested()) {
      LOG(INFO) << "Skip updating corpus database due to early stop requested.";
      continue;
    }

    // TODO(xinhaoyuan): Have a separate flag to skip corpus updating instead
    // of checking whether workdir is specified or not.
    if (fuzztest_config.only_replay || is_workdir_specified) continue;

    // Distill and store the coverage corpus.
    Distill(env);
    if (RemotePathExists(coverage_dir.c_str())) {
      // In the future, we will store k latest coverage corpora for some k, but
      // for now we only keep the latest one.
      CHECK_OK(RemotePathDelete(coverage_dir.c_str(), /*recursively=*/true));
    }
    CHECK_OK(RemoteMkdir(coverage_dir.c_str()));
    std::vector<std::string> distilled_corpus_files;
    CHECK_OK(RemoteGlobMatch(workdir.DistilledCorpusFilePaths().AllShardsGlob(),
                             distilled_corpus_files));
    for (const std::string &corpus_file : distilled_corpus_files) {
      const std::string file_name =
          std::filesystem::path(corpus_file).filename();
      CHECK_OK(
          RemoteFileRename(corpus_file, (coverage_dir / file_name).c_str()));
    }

    // Deduplicate and update the crashing inputs.
    CrashSummary crash_summary{fuzztest_config.binary_identifier,
                               fuzz_tests_to_run[i]};
    const std::filesystem::path crashing_dir = fuzztest_db_path / "crashing";
    absl::flat_hash_set<std::string> crash_signatures =
        PruneOldCrashesAndGetRemainingCrashSignatures(
            crashing_dir, env, callbacks_factory, crash_summary);
    DeduplicateAndStoreNewCrashes(crashing_dir, workdir, env.total_shards,
                                  std::move(crash_signatures), crash_summary);
    crash_summary.Report(&std::cerr);
  }

  return EXIT_SUCCESS;
}

int ListCrashIds(const Environment &env,
                 const fuzztest::internal::Configuration &target_config) {
  CHECK(!env.list_crash_ids_file.empty())
      << "Need list_crash_ids_file to be set for listing crash IDs";
  CHECK_EQ(target_config.fuzz_tests_in_current_shard.size(), 1);
  std::vector<std::string> crash_paths;
  // TODO: b/406003594 - move the path construction to a library.
  const auto crash_dir = std::filesystem::path(target_config.corpus_database) /
                         target_config.binary_identifier /
                         target_config.fuzz_tests_in_current_shard[0] /
                         "crashing";
  if (RemotePathExists(crash_dir.string())) {
    CHECK(RemotePathIsDirectory(crash_dir.string()))
        << "Crash dir " << crash_dir << " in the corpus database "
        << target_config.corpus_database << " is not a directory";
    crash_paths =
        ValueOrDie(RemoteListFiles(crash_dir.string(), /*recursively=*/false));
  }
  std::vector<std::string> results;
  results.reserve(crash_paths.size());
  for (const auto &crash_path : crash_paths) {
    std::string crash_id = std::filesystem::path{crash_path}.filename();
    results.push_back(std::move(crash_id));
  }
  CHECK_OK(RemoteFileSetContents(env.list_crash_ids_file,
                                 absl::StrJoin(results, "\n")));
  return EXIT_SUCCESS;
}

int ReplayCrash(const Environment &env,
                const fuzztest::internal::Configuration &target_config,
                CentipedeCallbacksFactory &callbacks_factory) {
  CHECK(!env.crash_id.empty()) << "Need crash_id to be set for replay a crash";
  CHECK(target_config.fuzz_tests_in_current_shard.size() == 1)
      << "Expecting exactly one test for replay_crash";
  // TODO: b/406003594 - move the path construction to a library.
  const auto crash_dir = std::filesystem::path(target_config.corpus_database) /
                         target_config.binary_identifier /
                         target_config.fuzz_tests_in_current_shard[0] /
                         "crashing";
  const WorkDir workdir{env};
  SeedCorpusSource crash_corpus_source;
  crash_corpus_source.dir_glob = crash_dir;
  crash_corpus_source.num_recent_dirs = 1;
  crash_corpus_source.individual_input_rel_glob = env.crash_id;
  crash_corpus_source.sampled_fraction_or_count = 1.0f;
  const SeedCorpusConfig crash_corpus_config = {
      /*sources=*/{crash_corpus_source},
      /*destination=*/{
          /*dir_path=*/env.workdir,
          /*shard_rel_glob=*/
          std::filesystem::path{workdir.CorpusFilePaths().AllShardsGlob()}
              .filename(),
          /*shard_index_digits=*/WorkDir::kDigitsInShardIndex,
          /*num_shards=*/1}};
  CHECK_OK(GenerateSeedCorpusFromConfig(crash_corpus_config, env.binary_name,
                                        env.binary_hash));
  Environment run_crash_env = env;
  run_crash_env.load_shards_only = true;
  return Fuzz(run_crash_env, {}, "", callbacks_factory);
}

int ExportCrash(const Environment &env,
                const fuzztest::internal::Configuration &target_config) {
  CHECK(!env.crash_id.empty())
      << "Need crash_id to be set for exporting a crash";
  CHECK(!env.export_crash_file.empty())
      << "Need export_crash_file to be set for exporting a crash";
  CHECK(target_config.fuzz_tests_in_current_shard.size() == 1)
      << "Expecting exactly one test for exporting a crash";
  // TODO: b/406003594 - move the path construction to a library.
  const auto crash_dir = std::filesystem::path(target_config.corpus_database) /
                         target_config.binary_identifier /
                         target_config.fuzz_tests_in_current_shard[0] /
                         "crashing";
  std::string crash_contents;
  const auto read_status =
      RemoteFileGetContents((crash_dir / env.crash_id).c_str(), crash_contents);
  if (!read_status.ok()) {
    LOG(ERROR) << "Failed reading the crash " << env.crash_id << " from "
               << crash_dir.c_str() << ": " << read_status;
    return EXIT_FAILURE;
  }
  const auto write_status =
      RemoteFileSetContents(env.export_crash_file, crash_contents);
  if (!write_status.ok()) {
    LOG(ERROR) << "Failed write the crash " << env.crash_id << " to "
               << env.export_crash_file << ": " << write_status;
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}

}  // namespace

int CentipedeMain(const Environment &env,
                  CentipedeCallbacksFactory &callbacks_factory) {
  ClearEarlyStopRequestAndSetStopTime(env.stop_at);
  SetSignalHandlers();

  if (!env.corpus_to_files.empty()) {
    Centipede::CorpusToFiles(env, env.corpus_to_files);
    return EXIT_SUCCESS;
  }

  if (!env.crashes_to_files.empty()) {
    const auto status = Centipede::CrashesToFiles(env, env.crashes_to_files);
    if (status.ok()) return EXIT_SUCCESS;
    LOG(ERROR) << "Got error when exporting crashes to files: " << status;
    return EXIT_FAILURE;
  }

  if (!env.for_each_blob.empty()) return ForEachBlob(env);

  if (!env.minimize_crash_file_path.empty()) {
    ByteArray crashy_input;
    ReadFromLocalFile(env.minimize_crash_file_path, crashy_input);
    return MinimizeCrash(crashy_input, env, callbacks_factory);
  }

  // Just export the corpus from a local dir and exit.
  if (!env.corpus_from_files.empty()) {
    Centipede::CorpusFromFiles(env, env.corpus_from_files);
    return EXIT_SUCCESS;
  }

  // Export the corpus from a local dir and then fuzz.
  if (!env.corpus_dir.empty()) {
    for (size_t i = 0; i < env.corpus_dir.size(); ++i) {
      const auto &corpus_dir = env.corpus_dir[i];
      if (i > 0 || !env.first_corpus_dir_output_only)
        Centipede::CorpusFromFiles(env, corpus_dir);
    }
  }

  if (env.distill) return Distill(env);

  // Create the local temporary dir once, before creating any threads. The
  // temporary dir must typically exist before `CentipedeCallbacks` can be used.
  const auto tmpdir = TemporaryLocalDirPath();
  CreateLocalDirRemovedAtExit(tmpdir);

  // Enter the update corpus database mode only if we have a binary to invoke
  // and a corpus database to update.
  // We don't update the corpus database for standalone binaries (i.e., when
  // `env.has_input_wildcards` is true).
  if (!env.binary.empty() && !env.has_input_wildcards) {
    const auto serialized_target_config = [&]() -> absl::StatusOr<std::string> {
      // TODO: b/410051414 Use Centipede flags to pass necessary information
      // instead of passing the entirely serialized Configuration once switched
      // to the unified execution model.
      if (!env.fuzztest_configuration.empty()) {
        std::string result;
        CHECK(absl::WebSafeBase64Unescape(env.fuzztest_configuration, &result));
        return result;
      }
      ScopedCentipedeCallbacks scoped_callbacks(callbacks_factory, env);
      return scoped_callbacks.callbacks()->GetSerializedTargetConfig();
    }();
    CHECK_OK(serialized_target_config.status());
    if (!serialized_target_config->empty()) {
      const auto target_config = fuzztest::internal::Configuration::Deserialize(
          *serialized_target_config);
      CHECK_OK(target_config.status())
          << "Failed to deserialize target configuration";
      if (!target_config->corpus_database.empty()) {
        LOG_IF(FATAL,
               env.list_crash_ids + env.replay_crash + env.export_crash > 1)
            << "At most one of list_crash_ids/replay_crash/export_crash can "
               "be set, but seeing list_crash_ids: "
            << env.list_crash_ids << ", replay_crash: " << env.replay_crash
            << ", export_crash: " << env.export_crash;
        if (env.list_crash_ids) {
          return ListCrashIds(env, *target_config);
        }
        if (env.replay_crash) {
          return ReplayCrash(env, *target_config, callbacks_factory);
        }
        if (env.export_crash) {
          return ExportCrash(env, *target_config);
        }

        const auto time_limit_per_test = target_config->GetTimeLimitPerTest();
        CHECK(target_config->only_replay ||
              time_limit_per_test < absl::InfiniteDuration())
            << "Updating corpus database requires specifying time limit per "
               "fuzz test.";
        CHECK(time_limit_per_test >= absl::Seconds(1))
            << "Time limit per fuzz test must be at least 1 second.";
        return UpdateCorpusDatabaseForFuzzTests(env, *target_config,
                                                callbacks_factory);
      }
    }
  }

  // Create the remote coverage dirs once, before creating any threads.
  const auto coverage_dir = WorkDir{env}.CoverageDirPath();
  CHECK_OK(RemoteMkdir(coverage_dir));
  LOG(INFO) << "Coverage dir: " << coverage_dir
            << "; temporary dir: " << tmpdir;

  std::string pcs_file_path;
  BinaryInfo binary_info = PopulateBinaryInfoAndSavePCsIfNecessary(
      env, callbacks_factory, pcs_file_path);

  if (env.analyze) return Analyze(env);

  return Fuzz(env, binary_info, pcs_file_path, callbacks_factory);
}

}  // namespace fuzztest::internal
