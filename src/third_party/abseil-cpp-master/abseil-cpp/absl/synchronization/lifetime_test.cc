// Copyright 2017 The Abseil Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <cstdlib>
#include <thread>  // NOLINT(build/c++11), Abseil test
#include <type_traits>

#include "absl/base/attributes.h"
#include "absl/base/internal/raw_logging.h"
#include "absl/base/thread_annotations.h"
#include "absl/synchronization/mutex.h"
#include "absl/synchronization/notification.h"

namespace {

// A two-threaded test which checks that Mutex, CondVar, and Notification have
// correct basic functionality.  The intent is to establish that they
// function correctly in various phases of construction and destruction.
//
// Thread one acquires a lock on 'mutex', wakes thread two via 'notification',
// then waits for 'state' to be set, as signalled by 'condvar'.
//
// Thread two waits on 'notification', then sets 'state' inside the 'mutex',
// signalling the change via 'condvar'.
//
// These tests use ABSL_RAW_CHECK to validate invariants, rather than EXPECT or
// ASSERT from gUnit, because we need to invoke them during global destructors,
// when gUnit teardown would have already begun.
void ThreadOne(absl::Mutex* mutex, absl::CondVar* condvar,
               absl::Notification* notification, bool* state) {
  // Test that the notification is in a valid initial state.
  ABSL_RAW_CHECK(!notification->HasBeenNotified(), "invalid Notification");
  ABSL_RAW_CHECK(*state == false, "*state not initialized");

  {
    absl::MutexLock lock(mutex);

    notification->Notify();
    ABSL_RAW_CHECK(notification->HasBeenNotified(), "invalid Notification");

    while (*state == false) {
      condvar->Wait(mutex);
    }
  }
}

void ThreadTwo(absl::Mutex* mutex, absl::CondVar* condvar,
               absl::Notification* notification, bool* state) {
  ABSL_RAW_CHECK(*state == false, "*state not initialized");

  // Wake thread one
  notification->WaitForNotification();
  ABSL_RAW_CHECK(notification->HasBeenNotified(), "invalid Notification");
  {
    absl::MutexLock lock(mutex);
    *state = true;
    condvar->Signal();
  }
}

// Launch thread 1 and thread 2, and block on their completion.
// If any of 'mutex', 'condvar', or 'notification' is nullptr, use a locally
// constructed instance instead.
void RunTests(absl::Mutex* mutex, absl::CondVar* condvar,
              absl::Notification* notification) {
  absl::Mutex default_mutex;
  absl::CondVar default_condvar;
  absl::Notification default_notification;
  if (!mutex) {
    mutex = &default_mutex;
  }
  if (!condvar) {
    condvar = &default_condvar;
  }
  if (!notification) {
    notification = &default_notification;
  }
  bool state = false;
  std::thread thread_one(ThreadOne, mutex, condvar, notification, &state);
  std::thread thread_two(ThreadTwo, mutex, condvar, notification, &state);
  thread_one.join();
  thread_two.join();
}

void TestLocals() {
  absl::Mutex mutex;
  absl::CondVar condvar;
  absl::Notification notification;
  RunTests(&mutex, &condvar, &notification);
}

// Global variables during start and termination
//
// In a translation unit, static storage duration variables are initialized in
// the order of their definitions, and destroyed in the reverse order of their
// definitions.  We can use this to arrange for tests to be run on these objects
// before they are created, and after they are destroyed.

using Function = void (*)();

class OnConstruction {
 public:
  explicit OnConstruction(Function fn) { fn(); }
};

class OnDestruction {
 public:
  explicit OnDestruction(Function fn) : fn_(fn) {}
  ~OnDestruction() { fn_(); }
 private:
  Function fn_;
};

}  // namespace

int main() {
  TestLocals();
  // Explicitly call exit(0) here, to make it clear that we intend for the
  // above global object destructors to run.
  std::exit(0);
}
