/**
* Copyright (C) 2014 MongoDB Inc.
*
* This program is free software: you can redistribute it and/or  modify
* it under the terms of the GNU Affero General Public License, version 3,
* as published by the Free Software Foundation.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU Affero General Public License for more details.
*
* You should have received a copy of the GNU Affero General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
* As a special exception, the copyright holders give permission to link the
* code of portions of this program with the OpenSSL library under certain
* conditions as described in each individual source file and distribute
* linked combinations including the program with the OpenSSL library. You
* must comply with the GNU Affero General Public License in all respects
* for all of the code used other than as permitted herein. If you modify
* file(s) with this exception, you may extend this exception to your
* version of the file(s), but you are not obligated to do so. If you do not
* wish to do so, delete this exception statement from your version. If you
* delete this exception statement from all source files in the program,
* then also delete it in the license file.
*/

#pragma once

#include "mongo/platform/compiler.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/exit_code.h"

namespace mongo {

/**
 * Quickly determines if the shutdown flag is set.  May not be definitive.
 */
bool inShutdown();

/**
 * Definitively determines if the shutdown flag is set.  Calling this is more expensive
 * than inShutdown().
 */
bool inShutdownStrict();

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
void registerShutdownTask(stdx::function<void()>);

/**
 * Toggles the shutdown flag to 'true', runs registered shutdown
 * tasks, and then exits with the given code. It is safe to call this
 * function from multiple threads, only the first caller executes
 * shutdown tasks. It is illegal to reenter this function from a
 * registered shutdown task. The function does not return.
 */
MONGO_COMPILER_NORETURN void shutdown(ExitCode code);

/**
 * Toggles the shutdown flag to 'true' and runs the registered
 * shutdown tasks. It is safe to call this function from multiple
 * threads, only the first caller executes shutdown tasks, subsequent
 * callers return immediately. It is legal to call shutdownNoTerminate
 * from a shutdown task.
 */
void shutdownNoTerminate();

/** An alias for 'shutdown'. */
MONGO_COMPILER_NORETURN inline void exitCleanly(ExitCode code) {
    shutdown(code);
}

}  // namespace mongo
