// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/platform/compiler.h"
#include "mongo/util/duration.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/functional.h"
#include "mongo/util/modules.h"

#include <utility>

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * ShutdownTaskArgs holds any arguments we might like to pass from a manual invocation of the
 * shutdown command.  It is meant to give a default shutdown when default constructed.
 */
struct ShutdownTaskArgs {
    // This should be set to true if we called shutdown from the shutdown command
    bool isUserInitiated = false;

    // The time allowed for quiesce mode.
    boost::optional<Milliseconds> quiesceTime;
};

/**
 * Determines if the shutdown flag is set.
 *
 * Calling this function is deprecated because modules that consult it
 * cannot engage in an orderly, coordinated shutdown. Instead, such
 * modules tend to just stop working at some point after mongo::shutdown() is
 * invoked, without regard to whether modules that depend on them have
 * already shut down.
 */
bool globalInShutdownDeprecated();

/**
 * Does not return until all shutdown tasks have run.
 */
ExitCode waitForShutdown();

using ShutdownTask = unique_function<void(const ShutdownTaskArgs& shutdownArgs)>;

/**
 * Registers a new shutdown task to be called when shutdown or
 * shutdownNoTerminate is called. If this function is invoked after
 * shutdown or shutdownNoTerminate has been called, std::terminate is
 * called.
 */
void registerShutdownTask(ShutdownTask);

/**
 * Helper for registering shutdown tasks, converts void lambda to shutdown lambda form.
 */
inline void registerShutdownTask(unique_function<void()> task) {
    registerShutdownTask([task = std::move(task)](const ShutdownTaskArgs&) { task(); });
}

/**
 * Toggles the shutdown flag to 'true', runs registered shutdown
 * tasks, and then exits with the given code. It is safe to call this
 * function from multiple threads, only the first caller executes
 * shutdown tasks. It is illegal to reenter this function from a
 * registered shutdown task. The function does not return.
 */
MONGO_COMPILER_NORETURN void shutdown(ExitCode code, const ShutdownTaskArgs& shutdownArgs = {});

/**
 * Toggles the shutdown flag to 'true' and runs the registered
 * shutdown tasks. It is safe to call this function from multiple
 * threads, only the first caller executes shutdown tasks, subsequent
 * callers return immediately. It is legal to call shutdownNoTerminate
 * from a shutdown task.
 */
void shutdownNoTerminate(const ShutdownTaskArgs& shutdownArgs = {});

/** An alias for 'shutdown'. */
MONGO_COMPILER_NORETURN inline void exitCleanly(ExitCode code) {
    shutdown(code);
}

}  // namespace mongo
