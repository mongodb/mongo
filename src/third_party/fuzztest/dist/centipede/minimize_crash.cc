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

#include "./centipede/minimize_crash.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <filesystem>  // NOLINT
#include <string>
#include <string_view>
#include <vector>

#include "absl/base/thread_annotations.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/synchronization/mutex.h"
#include "./centipede/centipede_callbacks.h"
#include "./centipede/environment.h"
#include "./centipede/mutation_input.h"
#include "./centipede/runner_result.h"
#include "./centipede/stop.h"
#include "./centipede/thread_pool.h"
#include "./centipede/util.h"
#include "./centipede/workdir.h"
#include "./common/defs.h"
#include "./common/hash.h"
#include "./common/logging.h"  // IWYU pragma: keep

namespace fuzztest::internal {

// Work queue for the minimizer.
// Thread-safe.
struct MinimizerWorkQueue {
 public:
  // Creates the queue.
  // `crash_dir_path` is the directory path where new crashers are written.
  // `crasher` is the initial crashy input.
  MinimizerWorkQueue(const std::string_view crash_dir_path,
                     const ByteArray crasher)
      : crash_dir_path_(crash_dir_path), crashers_{ByteArray(crasher)} {
    std::filesystem::create_directory(crash_dir_path_);
  }

  // Returns up to `max_num_crashers` most recently added crashers.
  std::vector<ByteArray> GetRecentCrashers(size_t max_num_crashers) {
    absl::MutexLock lock(&mutex_);
    size_t num_crashers_to_return =
        std::min(crashers_.size(), max_num_crashers);
    return {crashers_.end() - num_crashers_to_return, crashers_.end()};
  }

  // Adds `crasher` to the queue, writes it to `crash_dir_path_/Hash(crasher)`.
  // The crasher must be smaller than the original one.
  void AddCrasher(ByteArray crasher) {
    absl::MutexLock lock(&mutex_);
    CHECK_LT(crasher.size(), crashers_.front().size());
    crashers_.emplace_back(crasher);
    // Write the crasher to disk.
    auto hash = Hash(crasher);
    auto dir = crash_dir_path_;
    std::string file_path = dir.append(hash);
    WriteToLocalFile(file_path, crasher);
  }

  // Returns true if new smaller crashes were found.
  bool SmallerCrashesFound() const {
    absl::MutexLock lock(&mutex_);
    return crashers_.size() > 1;
  }

 private:
  mutable absl::Mutex mutex_;
  const std::filesystem::path crash_dir_path_;
  std::vector<ByteArray> crashers_ ABSL_GUARDED_BY(mutex_);
};

// Performs a minimization loop in one thread.
static void MinimizeCrash(const Environment &env,
                          CentipedeCallbacksFactory &callbacks_factory,
                          MinimizerWorkQueue &queue) {
  ScopedCentipedeCallbacks scoped_callback(callbacks_factory, env);
  auto callbacks = scoped_callback.callbacks();
  BatchResult batch_result;

  size_t num_batches = env.num_runs / env.batch_size;
  for (size_t i = 0; i < num_batches; ++i) {
    LOG_EVERY_POW_2(INFO) << "[" << i << "] Minimizing... Interrupt to stop";
    if (ShouldStop()) break;
    // Get up to kMaxNumCrashersToGet most recent crashers. We don't want just
    // the most recent crasher to avoid being stuck in local minimum.
    constexpr size_t kMaxNumCrashersToGet = 20;
    const auto recent_crashers = queue.GetRecentCrashers(kMaxNumCrashersToGet);
    CHECK(!recent_crashers.empty());
    // Compute the minimal known crasher size.
    size_t min_known_size = recent_crashers.front().size();
    for (const auto &crasher : recent_crashers) {
      min_known_size = std::min(min_known_size, crasher.size());
    }

    // Create several mutants that are smaller than the current smallest one.
    //
    // Currently, we do this by calling the vanilla mutator and
    // discarding all inputs that are too large.
    // TODO(kcc): modify the Mutate() interface such that max_len can be passed.
    //
    const std::vector<ByteArray> mutants = callbacks->Mutate(
        GetMutationInputRefsFromDataInputs(recent_crashers), env.batch_size);
    std::vector<ByteArray> smaller_mutants;
    for (const auto &m : mutants) {
      if (m.size() < min_known_size) smaller_mutants.push_back(m);
    }

    // Execute all mutants. If a new crasher is found, add it to `queue`.
    if (!callbacks->Execute(env.binary, smaller_mutants, batch_result)) {
      size_t crash_inputs_idx = batch_result.num_outputs_read();
      CHECK_LT(crash_inputs_idx, smaller_mutants.size());
      const auto &new_crasher = smaller_mutants[crash_inputs_idx];
      LOG(INFO) << "Crasher: size: " << new_crasher.size() << ": "
                << AsPrintableString(new_crasher, /*max_len=*/40);
      queue.AddCrasher(new_crasher);
    }
  }
}

int MinimizeCrash(ByteSpan crashy_input, const Environment &env,
                  CentipedeCallbacksFactory &callbacks_factory) {
  ScopedCentipedeCallbacks scoped_callback(callbacks_factory, env);
  auto callbacks = scoped_callback.callbacks();

  LOG(INFO) << "MinimizeCrash: trying the original crashy input";

  BatchResult batch_result;
  ByteArray original_crashy_input(crashy_input.begin(), crashy_input.end());
  if (callbacks->Execute(env.binary, {original_crashy_input}, batch_result)) {
    LOG(INFO) << "The original crashy input did not crash; exiting";
    return EXIT_FAILURE;
  }

  LOG(INFO) << "Starting the crash minimization loop in " << env.num_threads
            << "threads";

  MinimizerWorkQueue queue(WorkDir{env}.CrashReproducerDirPaths().MyShard(),
                           original_crashy_input);

  {
    ThreadPool threads{static_cast<int>(env.num_threads)};
    for (size_t i = 0; i < env.num_threads; ++i) {
      threads.Schedule([&env, &callbacks_factory, &queue]() {
        MinimizeCrash(env, callbacks_factory, queue);
      });
    }
  }  // The threads join here.

  return queue.SmallerCrashesFound() ? EXIT_SUCCESS : EXIT_FAILURE;
}

}  // namespace fuzztest::internal
