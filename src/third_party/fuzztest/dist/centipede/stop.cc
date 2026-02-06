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

#include "./centipede/stop.h"

#include <atomic>
#include <cstdlib>

#include "absl/time/clock.h"
#include "absl/time/time.h"

namespace fuzztest::internal {
namespace {

struct EarlyStop {
  int exit_code = EXIT_SUCCESS;
  bool is_requested = false;
};
std::atomic<EarlyStop> early_stop;

absl::Time stop_time = absl::InfiniteFuture();

}  // namespace

bool EarlyStopRequested() {
  return early_stop.load(std::memory_order_acquire).is_requested;
}

void ClearEarlyStopRequestAndSetStopTime(absl::Time stop_time) {
  early_stop.store({}, std::memory_order_release);
  ::fuzztest::internal::stop_time = stop_time;
}

void RequestEarlyStop(int exit_code) {
  early_stop.store({exit_code, true}, std::memory_order_release);
}

bool ShouldStop() { return EarlyStopRequested() || stop_time < absl::Now(); }

int ExitCode() { return early_stop.load(std::memory_order_acquire).exit_code; }

}  // namespace fuzztest::internal
