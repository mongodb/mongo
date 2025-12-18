/*
 * Copyright The OpenTelemetry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * DO NOT EDIT, this is an Auto-generated file from:
 * buildscripts/semantic-convention/templates/registry/semantic_attributes-h.j2
 */

#pragma once

#include "opentelemetry/common/macros.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace semconv
{
namespace process
{

/**
  Length of the process.command_args array
  <p>
  This field can be useful for querying or performing bucket analysis on how many arguments were
  provided to start a process. More arguments may be an indication of suspicious activity.
 */
static constexpr const char *kProcessArgsCount = "process.args_count";

/**
  The command used to launch the process (i.e. the command name). On Linux based systems, can be set
  to the zeroth string in @code proc/[pid]/cmdline @endcode. On Windows, can be set to the first
  parameter extracted from @code GetCommandLineW @endcode.
 */
static constexpr const char *kProcessCommand = "process.command";

/**
  All the command arguments (including the command/executable itself) as received by the process. On
  Linux-based systems (and some other Unixoid systems supporting procfs), can be set according to
  the list of null-delimited strings extracted from @code proc/[pid]/cmdline @endcode. For
  libc-based executables, this would be the full argv vector passed to @code main @endcode. SHOULD
  NOT be collected by default unless there is sanitization that excludes sensitive data.
 */
static constexpr const char *kProcessCommandArgs = "process.command_args";

/**
  The full command used to launch the process as a single string representing the full command. On
  Windows, can be set to the result of @code GetCommandLineW @endcode. Do not set this if you have
  to assemble it just for monitoring; use @code process.command_args @endcode instead. SHOULD NOT be
  collected by default unless there is sanitization that excludes sensitive data.
 */
static constexpr const char *kProcessCommandLine = "process.command_line";

/**
  Specifies whether the context switches for this data point were voluntary or involuntary.
 */
static constexpr const char *kProcessContextSwitchType = "process.context_switch.type";

/**
  Deprecated, use @code cpu.mode @endcode instead.

  @deprecated
  {"note": "Replaced by @code cpu.mode @endcode.", "reason": "renamed", "renamed_to": "cpu.mode"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kProcessCpuState = "process.cpu.state";

/**
  The date and time the process was created, in ISO 8601 format.
 */
static constexpr const char *kProcessCreationTime = "process.creation.time";

/**
  Process environment variables, @code <key> @endcode being the environment variable name, the value
  being the environment variable value. <p> Examples: <ul> <li>an environment variable @code USER
  @endcode with value @code "ubuntu" @endcode SHOULD be recorded as the @code
  process.environment_variable.USER @endcode attribute with value @code "ubuntu" @endcode.</li>
    <li>an environment variable @code PATH @endcode with value @code "/usr/local/bin:/usr/bin"
  @endcode SHOULD be recorded as the @code process.environment_variable.PATH @endcode attribute with
  value @code "/usr/local/bin:/usr/bin" @endcode.</li>
  </ul>
 */
static constexpr const char *kProcessEnvironmentVariable = "process.environment_variable";

/**
  The GNU build ID as found in the @code .note.gnu.build-id @endcode ELF section (hex string).
 */
static constexpr const char *kProcessExecutableBuildIdGnu = "process.executable.build_id.gnu";

/**
  The Go build ID as retrieved by @code go tool buildid <go executable> @endcode.
 */
static constexpr const char *kProcessExecutableBuildIdGo = "process.executable.build_id.go";

/**
  Profiling specific build ID for executables. See the OTel specification for Profiles for more
  information.
 */
static constexpr const char *kProcessExecutableBuildIdHtlhash =
    "process.executable.build_id.htlhash";

/**
  "Deprecated, use @code process.executable.build_id.htlhash @endcode instead."

  @deprecated
  {"note": "Replaced by @code process.executable.build_id.htlhash @endcode.", "reason": "renamed",
  "renamed_to": "process.executable.build_id.htlhash"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kProcessExecutableBuildIdProfiling =
    "process.executable.build_id.profiling";

/**
  The name of the process executable. On Linux based systems, this SHOULD be set to the base name of
  the target of @code /proc/[pid]/exe @endcode. On Windows, this SHOULD be set to the base name of
  @code GetProcessImageFileNameW @endcode.
 */
static constexpr const char *kProcessExecutableName = "process.executable.name";

/**
  The full path to the process executable. On Linux based systems, can be set to the target of @code
  proc/[pid]/exe @endcode. On Windows, can be set to the result of @code GetProcessImageFileNameW
  @endcode.
 */
static constexpr const char *kProcessExecutablePath = "process.executable.path";

/**
  The exit code of the process.
 */
static constexpr const char *kProcessExitCode = "process.exit.code";

/**
  The date and time the process exited, in ISO 8601 format.
 */
static constexpr const char *kProcessExitTime = "process.exit.time";

/**
  The PID of the process's group leader. This is also the process group ID (PGID) of the process.
 */
static constexpr const char *kProcessGroupLeaderPid = "process.group_leader.pid";

/**
  Whether the process is connected to an interactive shell.
 */
static constexpr const char *kProcessInteractive = "process.interactive";

/**
  The control group associated with the process.
  <p>
  Control groups (cgroups) are a kernel feature used to organize and manage process resources. This
  attribute provides the path(s) to the cgroup(s) associated with the process, which should match
  the contents of the <a
  href="https://man7.org/linux/man-pages/man7/cgroups.7.html">/proc/[PID]/cgroup</a> file.
 */
static constexpr const char *kProcessLinuxCgroup = "process.linux.cgroup";

/**
  The username of the user that owns the process.
 */
static constexpr const char *kProcessOwner = "process.owner";

/**
  Deprecated, use @code system.paging.fault.type @endcode instead.

  @deprecated
  {"note": "Replaced by @code system.paging.fault.type @endcode.", "reason": "renamed",
  "renamed_to": "system.paging.fault.type"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kProcessPagingFaultType =
    "process.paging.fault_type";

/**
  Parent Process identifier (PPID).
 */
static constexpr const char *kProcessParentPid = "process.parent_pid";

/**
  Process identifier (PID).
 */
static constexpr const char *kProcessPid = "process.pid";

/**
  The real user ID (RUID) of the process.
 */
static constexpr const char *kProcessRealUserId = "process.real_user.id";

/**
  The username of the real user of the process.
 */
static constexpr const char *kProcessRealUserName = "process.real_user.name";

/**
  An additional description about the runtime of the process, for example a specific vendor
  customization of the runtime environment.
 */
static constexpr const char *kProcessRuntimeDescription = "process.runtime.description";

/**
  The name of the runtime of this process.
 */
static constexpr const char *kProcessRuntimeName = "process.runtime.name";

/**
  The version of the runtime of this process, as returned by the runtime without modification.
 */
static constexpr const char *kProcessRuntimeVersion = "process.runtime.version";

/**
  The saved user ID (SUID) of the process.
 */
static constexpr const char *kProcessSavedUserId = "process.saved_user.id";

/**
  The username of the saved user.
 */
static constexpr const char *kProcessSavedUserName = "process.saved_user.name";

/**
  The PID of the process's session leader. This is also the session ID (SID) of the process.
 */
static constexpr const char *kProcessSessionLeaderPid = "process.session_leader.pid";

/**
  The process state, e.g., <a
  href="https://man7.org/linux/man-pages/man1/ps.1.html#PROCESS_STATE_CODES">Linux Process State
  Codes</a>
 */
static constexpr const char *kProcessState = "process.state";

/**
  Process title (proctitle)
  <p>
  In many Unix-like systems, process title (proctitle), is the string that represents the name or
  command line of a running process, displayed by system monitoring tools like ps, top, and htop.
 */
static constexpr const char *kProcessTitle = "process.title";

/**
  The effective user ID (EUID) of the process.
 */
static constexpr const char *kProcessUserId = "process.user.id";

/**
  The username of the effective user of the process.
 */
static constexpr const char *kProcessUserName = "process.user.name";

/**
  Virtual process identifier.
  <p>
  The process ID within a PID namespace. This is not necessarily unique across all processes on the
  host but it is unique within the process namespace that the process exists within.
 */
static constexpr const char *kProcessVpid = "process.vpid";

/**
  The working directory of the process.
 */
static constexpr const char *kProcessWorkingDirectory = "process.working_directory";

namespace ProcessContextSwitchTypeValues
{

static constexpr const char *kVoluntary = "voluntary";

static constexpr const char *kInvoluntary = "involuntary";

}  // namespace ProcessContextSwitchTypeValues

namespace ProcessCpuStateValues
{

static constexpr const char *kSystem = "system";

static constexpr const char *kUser = "user";

static constexpr const char *kWait = "wait";

}  // namespace ProcessCpuStateValues

namespace ProcessPagingFaultTypeValues
{

static constexpr const char *kMajor = "major";

static constexpr const char *kMinor = "minor";

}  // namespace ProcessPagingFaultTypeValues

namespace ProcessStateValues
{

static constexpr const char *kRunning = "running";

static constexpr const char *kSleeping = "sleeping";

static constexpr const char *kStopped = "stopped";

static constexpr const char *kDefunct = "defunct";

}  // namespace ProcessStateValues

}  // namespace process
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
