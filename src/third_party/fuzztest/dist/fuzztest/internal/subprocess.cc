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

#include "./fuzztest/internal/subprocess.h"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <variant>

#if !defined(_MSC_VER)
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#endif  // !defined(_MSC_VER)

#include <future>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#include "absl/container/flat_hash_map.h"
#include "absl/functional/function_ref.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "./fuzztest/internal/logging.h"

#if !defined(_MSC_VER)
// Needed to pass the current environment to posix_spawn, which needs an
// explicit envp without an option to inherit implicitly.
extern char** environ;
#endif

namespace fuzztest::internal {

#if !defined(_MSC_VER) &&                     \
    !(defined(__ANDROID_MIN_SDK_VERSION__) && \
      __ANDROID_MIN_SDK_VERSION__ < 28) &&    \
    !(defined(TARGET_OS_TV) && TARGET_OS_TV)

TerminationStatus::TerminationStatus(int status) : status_(status) {}

bool TerminationStatus::Exited() const { return WIFEXITED(status_); }

bool TerminationStatus::Signaled() const { return WIFSIGNALED(status_); }

std::variant<ExitCodeT, SignalT> TerminationStatus::Status() const {
  if (Exited()) return static_cast<ExitCodeT>(WEXITSTATUS(status_));
  FUZZTEST_INTERNAL_CHECK(Signaled(), "!Exited && !Signaled");
  return static_cast<SignalT>(WTERMSIG(status_));
}

// Helper class for running commands in a subprocess.
class SubProcess {
 public:
  TerminationStatus Run(
      absl::Span<const std::string> command_line,
      absl::FunctionRef<void(absl::string_view)> on_stdout_output,
      absl::FunctionRef<void(absl::string_view)> on_stderr_output,
      absl::FunctionRef<bool()> should_stop,
      const std::optional<absl::flat_hash_map<std::string, std::string>>&
          environment);

 private:
  void CreatePipes();
  void CloseChildPipes();
  void CloseParentPipes();
  posix_spawn_file_actions_t CreateChildFileActions();
  void StartWatchdog(absl::Duration timeout);
  pid_t StartChild(
      absl::Span<const std::string> command_line,
      const std::optional<absl::flat_hash_map<std::string, std::string>>&
          environment);
  void ReadChildOutput(
      absl::FunctionRef<void(absl::string_view)> on_stdout_output,
      absl::FunctionRef<void(absl::string_view)> on_stderr_output);

  // Pipe file descriptors pairs. Index 0 is for stdout, index 1 is for stderr.
  static constexpr int kStdOutIdx = 0;
  static constexpr int kStdErrIdx = 1;
  int parent_pipe_[2];
  int child_pipe_[2];
};

// Creates parent/child pipes for piping stdout/stderr from child to parent.
void SubProcess::CreatePipes() {
  for (int channel : {kStdOutIdx, kStdErrIdx}) {
    int pipe_fds[2];
    FUZZTEST_INTERNAL_CHECK(pipe(pipe_fds) == 0,
                            "Cannot create pipe: ", strerror(errno));

    parent_pipe_[channel] = pipe_fds[0];
    child_pipe_[channel] = pipe_fds[1];

    FUZZTEST_INTERNAL_CHECK(
        fcntl(parent_pipe_[channel], F_SETFL, O_NONBLOCK) != -1,
        "Cannot make pipe non-blocking: ", strerror(errno));
  }
}

void SubProcess::CloseChildPipes() {
  for (int channel : {kStdOutIdx, kStdErrIdx}) {
    FUZZTEST_INTERNAL_CHECK(close(child_pipe_[channel]) != -1,
                            "Cannot close pipe: ", strerror(errno));
  }
}

void SubProcess::CloseParentPipes() {
  for (int channel : {kStdOutIdx, kStdErrIdx}) {
    FUZZTEST_INTERNAL_CHECK(close(parent_pipe_[channel]) != -1,
                            "Cannot close pipe: ", strerror(errno));
  }
}

// Create file actions, which specify file-related actions to be performed in
// the child between the fork() and exec() steps.
posix_spawn_file_actions_t SubProcess::CreateChildFileActions() {
  posix_spawn_file_actions_t actions;

  int err;
  err = posix_spawn_file_actions_init(&actions);
  FUZZTEST_INTERNAL_CHECK(err == 0,
                          "Cannot initialize file actions: ", strerror(err));

  // Close stdin.
  err = posix_spawn_file_actions_addclose(&actions, STDIN_FILENO);
  FUZZTEST_INTERNAL_CHECK(err == 0,
                          "Cannot add close() action: ", strerror(err));

  for (int channel : {kStdOutIdx, kStdErrIdx}) {
    // Close parent-side pipes.
    err = posix_spawn_file_actions_addclose(&actions, parent_pipe_[channel]);
    FUZZTEST_INTERNAL_CHECK(err == 0,
                            "Cannot add close() action: ", strerror(err));

    // Replace stdout/stderr file descriptors with the pipes.
    int fd = channel == kStdOutIdx ? STDOUT_FILENO : STDERR_FILENO;
    err = posix_spawn_file_actions_adddup2(&actions, child_pipe_[channel], fd);
    FUZZTEST_INTERNAL_CHECK(err == 0,
                            "Cannot add dup2() action: ", strerror(err));
    err = posix_spawn_file_actions_addclose(&actions, child_pipe_[channel]);
    FUZZTEST_INTERNAL_CHECK(err == 0,
                            "Cannot add close() action: ", strerror(err));
  }

  return actions;
}

// Do fork() and exec() in one step, using posix_spawnp().
pid_t SubProcess::StartChild(
    absl::Span<const std::string> command_line,
    const std::optional<absl::flat_hash_map<std::string, std::string>>&
        environment) {
  posix_spawn_file_actions_t actions = CreateChildFileActions();

  // Create `argv` and `envp` parameters for exec().
  size_t argc = command_line.size();
  std::vector<char*> argv(argc + 1);
  for (int i = 0; i < argc; i++) {
    argv[i] = strndup(command_line[i].data(), command_line[i].size());
  }
  argv[argc] = nullptr;

  std::vector<char*> envp;
  if (environment.has_value()) {
    size_t envc = environment->size();
    envp.resize(envc + 1);
    int i = 0;
    for (const auto& [key, value] : *environment) {
      envp[i++] = strdup(absl::StrCat(key, "=", value).c_str());
    }
    envp[envc] = nullptr;
  }

  pid_t child_pid;
  int err;
  err = posix_spawnp(&child_pid, argv[0], &actions, nullptr, argv.data(),
                     environment.has_value() ? envp.data() : environ);
  FUZZTEST_INTERNAL_CHECK(err == 0,
                          "Cannot spawn child process: ", strerror(err));

  // Free up the used parameters.
  for (char* p : argv) free(p);
  for (char* p : envp) free(p);

  err = posix_spawn_file_actions_destroy(&actions);
  FUZZTEST_INTERNAL_CHECK(err == 0,
                          "Cannot destroy file actions: ", strerror(err));

  return child_pid;
}

static bool ShouldRetry(int e) {
  return ((e == EINTR) || (e == EAGAIN) || (e == EWOULDBLOCK));
}

void SubProcess::ReadChildOutput(
    absl::FunctionRef<void(absl::string_view)> on_stdout_output,
    absl::FunctionRef<void(absl::string_view)> on_stderr_output) {
  // Set up poll()-ing the pipes.
  constexpr int fd_count = 2;
  struct pollfd pfd[fd_count];
  for (int channel : {kStdOutIdx, kStdErrIdx}) {
    pfd[channel].fd = parent_pipe_[channel];
    pfd[channel].events = POLLIN;
    pfd[channel].revents = 0;
  }

  // Loop reading stdout and stderr from the child process.
  int fd_remain = fd_count;

  char buf[4096];
  while (fd_remain > 0) {
    int ret = poll(pfd, fd_count, -1);
    if ((ret == -1) && !ShouldRetry(errno)) {
      FUZZTEST_INTERNAL_CHECK(false, "Cannot poll(): ", strerror(errno));
    } else if (ret == 0) {
      FUZZTEST_INTERNAL_CHECK(false, "Impossible timeout: ", strerror(errno));
    } else if (ret > 0) {
      for (int channel : {kStdOutIdx, kStdErrIdx}) {
        // According to the poll() spec, use -1 for ignored entries.
        if (pfd[channel].fd == -1) {
          continue;
        }
        if ((pfd[channel].revents & (POLLIN | POLLHUP)) != 0) {
          ssize_t n = read(pfd[channel].fd, buf, sizeof(buf));
          if (n > 0) {
            auto on_output =
                channel == kStdOutIdx ? on_stdout_output : on_stderr_output;
            on_output({buf, static_cast<size_t>(n)});
          } else if ((n == 0) || !ShouldRetry(errno)) {
            pfd[channel].fd = -1;
            fd_remain--;
          }
        } else if ((pfd[channel].revents & (POLLERR | POLLNVAL)) != 0) {
          pfd[channel].fd = -1;
          fd_remain--;
        }
      }
    }
  }
}

namespace {

int Wait(pid_t pid) {
  int status;
  while (true) {
    pid_t ret = waitpid(pid, &status, 0);
    if (ret == -1 && ShouldRetry(errno)) {
      continue;
    } else if (ret == pid && (WIFEXITED(status) || WIFSIGNALED(status))) {
      return status;
    } else {
      FUZZTEST_INTERNAL_CHECK(false, "wait() error: ", strerror(errno));
    }
  }
}

// TODO(lszekeres): Consider optimizing this further by eliminating polling.
// Could potentially be done using pselect() to wait for SIGCHLD with a timeout.
// I.e., by setting all args to null, except timeout with a sigmask for SIGCHLD.
int WaitWithStopChecker(pid_t pid, absl::FunctionRef<bool()> should_stop) {
  int status;
  constexpr absl::Duration sleep_duration = absl::Milliseconds(100);
  while (true) {
    pid_t ret = waitpid(pid, &status, WNOHANG);
    if (ret == -1 && ShouldRetry(errno)) {
      continue;
    } else if (ret == 0) {  // Still running.
      if (should_stop()) {
        FUZZTEST_INTERNAL_CHECK(kill(pid, SIGTERM) == 0,
                                "Cannot kill(): ", strerror(errno));
        return Wait(pid);
      } else {
        absl::SleepFor(sleep_duration);
        continue;
      }
    } else if (ret == pid && (WIFEXITED(status) || WIFSIGNALED(status))) {
      return status;
    } else {
      FUZZTEST_INTERNAL_CHECK(false, "wait() error: ", strerror(errno));
    }
  }
}

}  // anonymous namespace

TerminationStatus SubProcess::Run(
    absl::Span<const std::string> command_line,
    absl::FunctionRef<void(absl::string_view)> on_stdout_output,
    absl::FunctionRef<void(absl::string_view)> on_stderr_output,
    absl::FunctionRef<bool()> should_stop,
    const std::optional<absl::flat_hash_map<std::string, std::string>>&
        environment) {
  CreatePipes();
  pid_t child_pid = StartChild(command_line, environment);
  CloseChildPipes();
  std::future<int> status = std::async(std::launch::async, &WaitWithStopChecker,
                                       child_pid, should_stop);
  ReadChildOutput(on_stdout_output, on_stderr_output);
  CloseParentPipes();
  return TerminationStatus(status.get());
}

#endif  // !defined(_MSC_VER) && !(defined(__ANDROID_MIN_SDK_VERSION__) &&
        // __ANDROID_MIN_SDK_VERSION__ < 28)

TerminationStatus RunCommandWithCallbacks(
    absl::Span<const std::string> command_line,
    absl::FunctionRef<void(absl::string_view)> on_stdout_output,
    absl::FunctionRef<void(absl::string_view)> on_stderr_output,
    absl::FunctionRef<bool()> should_stop,
    const std::optional<absl::flat_hash_map<std::string, std::string>>&
        environment) {
#if defined(_MSC_VER)
  FUZZTEST_INTERNAL_CHECK(false,
                          "Subprocess library not implemented on Windows yet.");
#elif defined(__ANDROID_MIN_SDK_VERSION__) && __ANDROID_MIN_SDK_VERSION__ < 28
  FUZZTEST_INTERNAL_CHECK(
      false,
      "Subprocess library not implemented on older Android NDK versions yet");
#elif defined(TARGET_OS_TV) && TARGET_OS_TV
  FUZZTEST_INTERNAL_CHECK(
      false, "Subprocess library not implemented on Apple tvOS yet");
#else
  SubProcess proc;
  return proc.Run(command_line, on_stdout_output, on_stderr_output, should_stop,
                  environment);
#endif
}

RunResults RunCommand(
    absl::Span<const std::string> command_line,
    const std::optional<absl::flat_hash_map<std::string, std::string>>&
        environment,
    absl::Duration timeout) {
  const absl::Time wait_until = absl::Now() + timeout;
  std::string stdout_str;
  std::string stderr_str;
  auto status = RunCommandWithCallbacks(
      command_line,
      [&stdout_str](absl::string_view output) { stdout_str.append(output); },
      [&stderr_str](absl::string_view output) { stderr_str.append(output); },
      [wait_until]() { return absl::Now() > wait_until; }, environment);
  return {std::move(status), std::move(stdout_str), std::move(stderr_str)};
}

}  // namespace fuzztest::internal
