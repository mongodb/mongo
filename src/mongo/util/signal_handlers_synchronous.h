// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/platform/compiler.h"
#include "mongo/util/dynamic_catch.h"
#include "mongo/util/modules.h"

#include <functional>
#include <iosfwd>
#include <typeinfo>
#include <vector>

#include <boost/optional.hpp>

[[MONGO_MOD_PUBLIC]];

namespace mongo {
/**
 * Sets the appropriate state to enable/disable diagnostic logging based on `newVal`.
 */
void setDiagnosticLoggingInSignalHandlers(bool newVal);

/**
 * Restores the default signal handlers and ends the process.
 */
void endProcessWithSignal(int signalNum);

/**
 * Sets up handlers for synchronous events, like segv, abort, terminate and malloc-failure.
 *
 * Call this very early in main(), before runGlobalInitializers().
 *
 * Called by setupSignalHandlers() in signal_handlers.h.  Prefer that method to this one,
 * in server code and tools that use the storage engine.
 */
void setupSynchronousSignalHandlers();

/**
 * Registers a user-defined callback to be invoked on any signal handler. Clobbers the
 * previously-registered callback.
 */
[[MONGO_MOD_PUBLIC]] void setSynchronousSignalHandlerCallback_forTest(std::function<void()> cb);

/**
 * Report out of memory error with a stack trace and exit.
 *
 * Called when any of the following functions fails to allocate memory:
 *     operator new
 *     mongoMalloc
 *     mongoRealloc
 */
MONGO_COMPILER_NORETURN void reportOutOfMemoryErrorAndExit();

/**
 * Clears the signal mask for the process. This is called from forkServer and to setup
 * the unit tests. On Windows, this is a noop.
 */
void clearSignalMask();

#if defined(__linux__) || defined(__APPLE__)
#define MONGO_STACKTRACE_HAS_SIGNAL
#endif

#if defined(MONGO_STACKTRACE_HAS_SIGNAL)
/**
 * Returns the signal used to initiate all-thread stack traces.
 */
int stackTraceSignal();
#endif

/**
 * Returns the signal used for stress-testing of EINTR resilience.
 */
int interruptResilienceTestingSignal();

/**
 * Returns a `shared_ptr` that will have sole ownership of a `MallocFreeOStreamGuard`. The passed in
 * signal number is used to end the process if the deadlock mitigation is triggered.
 */
std::shared_ptr<void> makeMallocFreeOStreamGuard_forTest(int sig);
void setMallocFreeOStreamGuardDeadlockCallback_forTest(std::function<void(int)> cb);

}  // namespace mongo
