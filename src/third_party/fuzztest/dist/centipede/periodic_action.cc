// Copyright 2024 The Centipede Authors.
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

#include "./centipede/periodic_action.h"

#include <cstdint>
#include <memory>
#include <thread>
#include <utility>

#include "absl/base/thread_annotations.h"
#include "absl/functional/any_invocable.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"

namespace fuzztest::internal {

class PeriodicAction::Impl {
 public:
  Impl(absl::AnyInvocable<void()> action, PeriodicAction::Options options)
      : action_{std::move(action)},
        options_{std::move(options)},
        thread_{[this]() { RunLoop(); }} {}

  void Stop() {
    StopAsync();
    // The run-loop should exit the next time it checks `stop_`. Note that if
    // the loop is currently in the middle of an invocation of `action_`, it
    // will wait for the invocation to finish, so we might block here for an
    // `action_`-dependent amount of time.
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  void StopAsync() {
    absl::MutexLock lock{&mu_};
    stop_ = true;
  }

  void Nudge() {
    absl::MutexLock lock{&mu_};
    nudge_ = true;
  }

 private:
  void RunLoop() {
    uint64_t iteration = 0;
    while (true) {
      SleepOrWakeEarly(options_.sleep_before_each(iteration));
      const bool schedule = !nudge_ && !stop_;
      const bool nudge = nudge_;
      const bool stop = stop_;
      mu_.Unlock();
      // NOTE: The caller might call `Stop()` immediately after one final
      // `Nudge()`: in that case we still should run the action, and only then
      // terminate the loop. This is in contrast to waking after sleeping the
      // full duration while the caller calls `Stop()` during that time: in that
      // case, we should NOT run the action and terminate the loop immediately.
      if (schedule || nudge) {
        action_();
      }
      if (stop) {
        return;
      }
      ++iteration;
    }
  }

  void SleepOrWakeEarly(absl::Duration duration)
      ABSL_EXCLUSIVE_LOCK_FUNCTION(mu_) {
    mu_.Lock();
    // NOTE: Reset only `nudge_`, but not `stop_`: nudging is transient and
    // can be activated repeatedly, the latter is persistent and can be
    // activated only once (repeated calls to `Stop()` are no-ops).
    nudge_ = false;
    mu_.Unlock();
    const auto wake_early = [this]() {
      mu_.AssertReaderHeld();
      return nudge_ || stop_;
    };
    mu_.LockWhenWithTimeout(absl::Condition{&wake_early}, duration);
    mu_.AssertHeld();
  }

  absl::AnyInvocable<void()> action_;
  PeriodicAction::Options options_;

  // WARNING!!! The order below is important.
  absl::Mutex mu_;
  bool nudge_ ABSL_GUARDED_BY(mu_) = false;
  bool stop_ ABSL_GUARDED_BY(mu_) = false;
  std::thread thread_;
};

PeriodicAction::PeriodicAction(  //
    absl::AnyInvocable<void()> action, Options options)
    : pimpl_{std::make_unique<Impl>(std::move(action), std::move(options))} {}

PeriodicAction::~PeriodicAction() {
  // NOTE: `pimpl_` will be null if this object has been moved to another one.
  if (pimpl_ != nullptr) pimpl_->Stop();
}

void PeriodicAction::Stop() { pimpl_->Stop(); }

void PeriodicAction::StopAsync() { pimpl_->StopAsync(); }

void PeriodicAction::Nudge() { pimpl_->Nudge(); }

// NOTE: Even though these are defaulted, they still must be defined here in the
// .cc, because `Impl` is an incomplete type in the .h.
PeriodicAction::PeriodicAction(PeriodicAction&&) = default;
PeriodicAction& PeriodicAction::operator=(PeriodicAction&&) = default;

}  // namespace fuzztest::internal
