// Copyright 2022 Google LLC
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

#include "./fuzztest/internal/logging.h"

#include <errno.h>
#include <string.h>

#include "absl/base/attributes.h"
#include "absl/base/const_init.h"
#include "absl/base/thread_annotations.h"
#include "absl/synchronization/mutex.h"

#if defined(__linux__)
#include <unistd.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <string>

namespace fuzztest::internal {

namespace {

ABSL_CONST_INIT absl::Mutex stderr_file_guard_(absl::kConstInit);
FILE* stderr_file_ ABSL_GUARDED_BY(stderr_file_guard_);  // Zero-initialized.

}  // namespace

#if defined(__linux__)

namespace {

FILE* stdout_file_ = stdout;  // Never accessed concurrently.

void Silence(int fd) {
  FILE* tmp = fopen("/dev/null", "w");
  FUZZTEST_INTERNAL_CHECK(tmp, "fopen() error:", strerror(errno));
  FUZZTEST_INTERNAL_CHECK(dup2(fileno(tmp), fd) != -1,
                          "dup2() error:", strerror(errno));
  FUZZTEST_INTERNAL_CHECK(fclose(tmp) == 0, "fclose() error:", strerror(errno));
}

// Only accepts 1 or 2 (stdout or stderr).
// If it's stdout, silence it after duping it as a global temporary, which
// will be used when restoring the stdout.
// If it's a stderr, silence it after duping it as the global stderr, which
// will be used internally to log and be used when restoring the stderr.
void DupAndSilence(int fd) {
  FUZZTEST_INTERNAL_CHECK(fd == STDOUT_FILENO || fd == STDERR_FILENO,
                          "DupAndSilence only accepts stderr or stdout.");
  int new_fd = dup(fd);
  FUZZTEST_INTERNAL_CHECK(new_fd != -1, "dup() error:", strerror(errno));
  FILE* new_output_file = fdopen(new_fd, "w");
  FUZZTEST_INTERNAL_CHECK(new_output_file, "fdopen error:", strerror(errno));
  if (new_output_file) {
    if (fd == STDOUT_FILENO) {
      stdout_file_ = new_output_file;
    } else {
      absl::MutexLock lock(&stderr_file_guard_);
      stderr_file_ = new_output_file;
    }
    Silence(fd);
  }
}
}  // namespace

void SilenceTargetStdoutAndStderr() {
  DupAndSilence(STDOUT_FILENO);
  DupAndSilence(STDERR_FILENO);
}

void RestoreTargetStdoutAndStderr() {
  // The CHECK-s below call GetStderr(), which accesses stderr_file_, which
  // would lead to a deadlock if we kept the guard locked and the CHECK-s
  // failed. To avoid this, we use a local variable.
  stderr_file_guard_.Lock();
  FILE* silenced_stderr = stderr_file_;
  stderr_file_ = stderr;
  stderr_file_guard_.Unlock();
  FUZZTEST_INTERNAL_CHECK(silenced_stderr != stderr,
                          "Error, calling RestoreStderr without calling"
                          "DupandSilenceStderr first.");
  FUZZTEST_INTERNAL_CHECK(dup2(fileno(silenced_stderr), STDERR_FILENO) != -1,
                          "dup2 error:", strerror(errno));
  FUZZTEST_INTERNAL_CHECK(fclose(silenced_stderr) == 0,
                          "close() error:", strerror(errno));

  FUZZTEST_INTERNAL_CHECK(stdout_file_ != stdout,
                          "Error, calling RestoreStdout without calling"
                          "DupandSilenceStdout first.");
  FUZZTEST_INTERNAL_CHECK(dup2(fileno(stdout_file_), STDOUT_FILENO) != -1,
                          "dup2() error:", strerror(errno));
  FUZZTEST_INTERNAL_CHECK(fclose(stdout_file_) == 0,
                          "close() error:", strerror(errno));
  stdout_file_ = stdout;
}

bool IsSilenceTargetEnabled() {
  return absl::NullSafeStringView(getenv("FUZZTEST_SILENCE_TARGET")) == "1";
}

#else

void SilenceTargetStdoutAndStderr() { return; }

void RestoreTargetStdoutAndStderr() { return; }

bool IsSilenceTargetEnabled() { return false; }

#endif  // defined(__linux__)

FILE* GetStderr() {
  absl::MutexLock lock(&stderr_file_guard_);
  if (!stderr_file_) {
    stderr_file_ = stderr;
  }
  return stderr_file_;
}

void Abort(const char* file, int line, const std::string& message) {
  fprintf(GetStderr(), "%s:%d: %s\n", file, line, message.c_str());
  std::abort();
}

const std::string* volatile test_abort_message = nullptr;
void AbortInTest(const std::string& message) {
  // When we are within a test, we set the message here and call abort().
  // The signal handler will pickup the message and print it at the right time.
  test_abort_message = &message;
  std::abort();
}

}  // namespace fuzztest::internal
