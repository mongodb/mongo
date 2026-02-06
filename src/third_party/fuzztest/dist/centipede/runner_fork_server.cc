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

// Fork server, a.k.a. a process Zygote, for the Centipede runner.
//
// Startup:
// * Centipede creates two named FIFO pipes: pipe0 and pipe1.
// * Centipede runs the target in background, and passes the FIFO names to it
//   using two environment variables: CENTIPEDE_FORK_SERVER_FIFO[01].
// * Centipede opens the pipe0 for writing, pipe1 for reading.
//   These would block until the same pipes are open in the runner.
// * Runner, early at startup, checks if it is given the pipe names.
//    If so, it opens pipe0 for reading, pipe1 for writing,
//    and enters the infinite fork-server loop.
// Loop:
// * Centipede writes a byte to pipe0.
// * Runner blocks until it reads a byte from pipe0, then forks and waits.
//   This is where the child process executes and does the work.
//   This works because every execution of the target has the same arguments.
// * Runner receives the child exit status and writes it to pipe1.
// * Centipede blocks until it reads the status from pipe1.
// Exit:
// * Centipede closes the pipes (and then deletes them).
// * Runner (the fork server) fails on the next read from pipe0 and exits.
//
// The fork server code kicks in super-early in the process startup,
// via injecting itself into the `.preinit_array`.
// Ensure that this code is not dropped from linking (alwayslink=1).
//
// The main benefts of the fork server over plain fork/exec or system() are:
//  * Dynamic linking happens once at the fork-server startup.
//  * fork is cheaper than fork/exec, especially when running multiple threads.
//
// Other than performance, using fork server should be the same as not using it.
//
// Similar ideas:
// * lcamtuf.blogspot.com/2014/10/fuzzing-binaries-without-execve.html
// * Android Zygote.
//
// We try to avoid any high-level code here, even most of libc because this code
// works too early in the process. E.g. getenv() will not work yet.

#include <fcntl.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#else                      // __APPLE__
#include <linux/limits.h>  // ARG_MAX
#endif                     // __APPLE__
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "absl/base/nullability.h"

namespace fuzztest::internal {

namespace {

constexpr bool kForkServerDebug = false;
[[maybe_unused]] constexpr bool kForkServerDumpEnvAtStart = false;

}  // namespace

// Writes a C string to stderr when debugging, no-op otherwise.
void Log(const char *absl_nonnull str) {
  if constexpr (kForkServerDebug) {
    (void)write(STDERR_FILENO, str, strlen(str));
    fsync(STDERR_FILENO);
  }
}

// Maybe writes the `reason` to stderr; then calls _exit. We use this instead of
// CHECK/RunnerCheck since the fork server runs at the very early stage of the
// process, where the logging functions used there may not work.
void Exit(const char *absl_nonnull reason) {
  Log(reason);
  _exit(0);  // The exit code does not matter, it won't be checked anyway.
}

// Contents of /proc/self/environ. We avoid malloc, so it's a fixed-size global.
// The fork server will fail to initialize if /proc/self/environ is too large.
static char env[ARG_MAX];
static ssize_t env_size;

void GetAllEnv() {
#ifdef __APPLE__
  // Reference:
  // https://chromium.googlesource.com/crashpad/crashpad/+/360e441c53ab4191a6fd2472cc57c3343a2f6944/util/posix/process_util_mac.cc
  char args[ARG_MAX];
  size_t args_size = sizeof(args);
  int mib[] = {CTL_KERN, KERN_PROCARGS2, getpid()};
  int rv =
      sysctl(mib, sizeof(mib) / sizeof(mib[0]), args, &args_size, nullptr, 0);
  if (rv != 0) {
    Exit("GetEnv: sysctl({CTK_KERN, KERN_PROCARGS2, ...}) failed");
  }
  if (args_size < sizeof(int)) {
    Exit("GetEnv: args_size too small");
  }
  int argc = 0;
  memcpy(&argc, &args[0], sizeof(argc));
  size_t start_pos = sizeof(argc);
  // Find the end of the executable path.
  while (start_pos < args_size && args[start_pos] != 0) ++start_pos;
  if (start_pos == args_size) {
    Exit("GetEnv: envp not found");
  }
  // Find the beginning of the string area.
  while (start_pos < args_size && args[start_pos] == 0) ++start_pos;
  if (start_pos == args_size) {
    Exit("GetEnv: envp not found");
  }
  // Ignore the first argc strings, after which is the envp.
  for (int i = 0; i < argc; ++i) {
    while (start_pos < args_size && args[start_pos] != 0) ++start_pos;
    if (start_pos == args_size) {
      Exit("GetEnv: envp not found");
    }
    ++start_pos;
  }
  const size_t end_pos = args_size;
  memcpy(env, &args[start_pos], end_pos - start_pos);
  env_size = end_pos - start_pos;
  if constexpr (kForkServerDumpEnvAtStart) {
    size_t pos = start_pos;
    while (pos < args_size) {
      const size_t len = strnlen(&args[pos], args_size - pos);
      (void)write(STDERR_FILENO, &args[pos], len);
      (void)write(STDERR_FILENO, "\n", 1);
      pos += len + 1;
    }
    fsync(STDERR_FILENO);
  }
#else                        // __APPLE__
  // Reads /proc/self/environ into env.
  int fd = open("/proc/self/environ", O_RDONLY);
  if (fd < 0) Exit("GetEnv: can't open /proc/self/environ\n");
  env_size = read(fd, env, sizeof(env));
  if (env_size < 0) Exit("GetEnv: can't read to env\n");
  if (close(fd) != 0) Exit("GetEnv: can't close /proc/self/environ\n");
#endif                       // __APPLE__
  env[sizeof(env) - 1] = 0;  // Just in case.
}

// Gets a zero-terminated string matching the environment `key` (ends with '=').
const char *absl_nullable GetOneEnv(const char *absl_nonnull key) {
  size_t key_len = strlen(key);
  if (env_size < key_len) return nullptr;
  bool in_the_beginning_of_key = true;
  // env is not a C string.
  // It is an array of bytes, with '\0' between individual key=val pairs.
  for (size_t idx = 0; idx < env_size - key_len; ++idx) {
    if (env[idx] == 0) {
      in_the_beginning_of_key = true;
      continue;
    }
    if (in_the_beginning_of_key && 0 == memcmp(env + idx, key, key_len))
      return &env[idx + key_len];  // zero-terminated.
    in_the_beginning_of_key = false;
  }
  return nullptr;
}

// Starts the fork server if the pipes are given.
// This function is called from `.preinit_array` when linked statically,
// or from the DSO constructor when injected via LD_PRELOAD.
// Note: it must run before the GlobalRunnerState constructor because
// GlobalRunnerState may terminate the process early due to an error,
// then we never open the fifos and the corresponding opens in centipede
// hang forever.
// The priority 150 is chosen on the lower end (higher priority)
// of the user-available range (101-999) to allow ordering with other
// constructors and C++ constructors (init_priority). Note: constructors
// without explicitly specified priority run after all constructors with
// explicitly specified priority, thus we still run before most
// "normal" constructors.
__attribute__((constructor(150))) void ForkServerCallMeVeryEarly() {
  // Guard against calling twice.
  static bool called_already = false;
  if (called_already) return;
  called_already = true;
  // Startup.
  GetAllEnv();
  const char *pipe0_name = GetOneEnv("CENTIPEDE_FORK_SERVER_FIFO0=");
  const char *pipe1_name = GetOneEnv("CENTIPEDE_FORK_SERVER_FIFO1=");
  if (!pipe0_name || !pipe1_name) return;
  Log("###Centipede fork server requested\n");
  int pipe0 = open(pipe0_name, O_RDONLY);
  if (pipe0 < 0) Exit("###open pipe0 failed\n");
  int pipe1 = open(pipe1_name, O_WRONLY);
  if (pipe1 < 0) Exit("###open pipe1 failed\n");
  Log("###Centipede fork server ready\n");

  struct sigaction old_sigterm_act{};
  struct sigaction sigterm_act{};
  sigterm_act.sa_handler = [](int) {};
  if (sigaction(SIGTERM, &sigterm_act, &old_sigterm_act) != 0) {
    Exit("###sigaction failed on SIGTERM for the fork server");
  }

  struct sigaction old_sigchld_act{};
  struct sigaction sigchld_act{};
  sigchld_act.sa_handler = [](int) {};
  if (sigaction(SIGCHLD, &sigchld_act, &old_sigchld_act) != 0) {
    Exit("###sigaction failed on SIGCHLD for the fork server");
  }

  sigset_t old_sigset;
  sigset_t server_sigset;
  if (sigprocmask(SIG_SETMASK, nullptr, &server_sigset) != 0) {
    Exit("###sigprocmask() failed to get the existing sigset\n");
  }
  if (sigaddset(&server_sigset, SIGTERM) != 0) {
    Exit("###sigaddset() failed to add SIGTERM\n");
  }
  if (sigaddset(&server_sigset, SIGCHLD) != 0) {
    Exit("###sigaddset() failed to add SIGCHLD\n");
  }
  if (sigprocmask(SIG_SETMASK, &server_sigset, &old_sigset) != 0) {
    Exit("###sigprocmask() failed to set the fork server sigset\n");
  }

  sigset_t wait_sigset;
  if (sigemptyset(&wait_sigset) != 0) {
    Exit("###sigemptyset() failed\n");
  }
  if (sigaddset(&wait_sigset, SIGTERM) != 0) {
    Exit("###sigaddset() failed to add SIGTERM to the wait sigset\n");
  }
  if (sigaddset(&wait_sigset, SIGCHLD) != 0) {
    Exit("###sigaddset() failed to add SIGCHLD to the wait sigset\n");
  }

  // Loop.
  while (true) {
    Log("###Centipede fork server blocking on pipe0\n");
    // This read will fail when Centipede shuts down the pipes.
    char ch = 0;
    if (read(pipe0, &ch, 1) != 1) Exit("###read from pipe0 failed\n");
    Log("###Centipede starting fork\n");
    auto pid = fork();
    if (pid < 0) {
      Exit("###fork failed\n");
    } else if (pid == 0) {
      if (sigaction(SIGTERM, &old_sigterm_act, nullptr) != 0) {
        Exit("###sigaction failed on SIGTERM for the child");
      }
      if (sigaction(SIGCHLD, &old_sigchld_act, nullptr) != 0) {
        Exit("###sigaction failed on SIGCHLD for the child");
      }
      if (sigprocmask(SIG_SETMASK, &old_sigset, nullptr) != 0) {
        Exit("###sigprocmask() failed to restore the previous sigset\n");
      }
      // Child process. Reset stdout/stderr and let it run normally.
      for (int fd = 1; fd <= 2; fd++) {
        lseek(fd, 0, SEEK_SET);
        // NOTE: Allow ftruncate() to fail by ignoring its return; that okay to
        // happen when the stdout/stderr are not redirected to a file.
        (void)ftruncate(fd, 0);
      }
      return;
    } else {
      // Parent process.
      int status = -1;
      while (true) {
        int sig = -1;
        if (sigwait(&wait_sigset, &sig) != 0) {
          Exit("###sigwait() failed\n");
        }
        if (sig == SIGCHLD) {
          Log("###Got SIGCHLD\n");
          const pid_t ret = waitpid(pid, &status, WNOHANG);
          if (ret < 0) {
            Exit("###waitpid failed\n");
          }
          if (ret == pid && (WIFEXITED(status) || WIFSIGNALED(status))) {
            Log("###Got exit status\n");
            break;
          }
        } else if (sig == SIGTERM) {
          Log("###Got SIGTERM\n");
          kill(pid, SIGTERM);
        } else {
          Exit("###Unknown signal from sigwait\n");
        }
      }
      if (WIFEXITED(status)) {
        if (WEXITSTATUS(status) == EXIT_SUCCESS)
          Log("###Centipede fork returned EXIT_SUCCESS\n");
        else if (WEXITSTATUS(status) == EXIT_FAILURE)
          Log("###Centipede fork returned EXIT_FAILURE\n");
        else
          Log("###Centipede fork returned unknown failure status\n");
      } else {
        Log("###Centipede fork crashed\n");
      }
      Log("###Centipede fork writing status to pipe1\n");
      if (write(pipe1, &status, sizeof(status)) == -1) {
        Exit("###write to pipe1 failed\n");
      }
      // Deplete any remaining signals before the next execution. Controller
      // won't send more signals after write succeeded.
      {
        sigset_t pending;
        while (true) {
          if (sigpending(&pending) != 0) {
            Exit("###sigpending() failed\n");
          }
          if (sigismember(&pending, SIGTERM) ||
              sigismember(&pending, SIGCHLD)) {
            int unused_sig;
            if (sigwait(&wait_sigset, &unused_sig) != 0) {
              Exit("###sigwait() failed\n");
            }
          } else {
            break;
          }
        }
      }
    }
  }
  // The only way out of the loop is via Exit() or return.
  __builtin_unreachable();
}

// If supported, use .preinit_array to call `ForkServerCallMeVeryEarly` even
// earlier than the `constructor` attribute of the declaration. This helps to
// avoid potential conflicts with higher-priority constructors.
#ifdef __APPLE__
// .preinit_array is not supported in MacOS.
#else   // __APPLE__
__attribute__((section(".preinit_array"))) auto call_very_early =
    ForkServerCallMeVeryEarly;
#endif  // __APPLE__

}  // namespace fuzztest::internal
