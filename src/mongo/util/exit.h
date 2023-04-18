/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <boost/optional.hpp>

#include "mongo/platform/compiler.h"
#include "mongo/util/duration.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/functional.h"

namespace mongo {

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

/**
 * Registers a new shutdown task to be called when shutdown or
 * shutdownNoTerminate is called. If this function is invoked after
 * shutdown or shutdownNoTerminate has been called, std::terminate is
 * called.
 */
void registerShutdownTask(unique_function<void(const ShutdownTaskArgs& shutdownArgs)>);

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
