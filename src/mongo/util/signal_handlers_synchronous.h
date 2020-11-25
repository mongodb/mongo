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

#include <functional>
#include <iosfwd>
#include <vector>

#include "mongo/util/dynamic_catch.h"

namespace mongo {

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
 * Report out of memory error with a stack trace and exit.
 *
 * Called when any of the following functions fails to allocate memory:
 *     operator new
 *     mongoMalloc
 *     mongoRealloc
 */
void reportOutOfMemoryErrorAndExit();

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
 * Analyzes the active exception, describing it to an ostream.
 * Consults a dynamic registry of exception handlers.
 * See `util/dynamic_catch.h`.
 */
class ActiveExceptionWitness {
public:
    /** Default constructor creates handlers for some basic exception types. */
    ActiveExceptionWitness();

    /**
     * Called at startup to teach our std::terminate handler how to print a
     * diagnostic for decoupled types of exceptions (e.g. in third_party, in
     * layers above base, or outside of the server codebase).
     *
     * This is not thread-safe, call at startup before multithreading. The
     * probes are evaluated in order so that later entries here will supersede
     * earlier entries and match more tightly in the catch hierarchy.
     */
    template <typename Ex>
    void addHandler(std::function<void(const Ex&, std::ostream&)> handler) {
        _configurators.push_back([=](CatchAndDescribe& dc) {
            dc.addCatch<Ex>([=](const Ex& ex, std::ostream& os) {
                handler(ex, os);
                _exceptionTypeBlurb(typeid(ex), os);
            });
        });
    }

    /**
     * Writes a description of the active exception to `os`, using built-in
     * exception probes, augmented by any configured exception probes given by calls to
     * `addHandler`.
     *
     * Called by our std::terminate handler when it detects an active
     * exception. The active exception is probably related to why the process
     * is terminating, but not necessarily. Consult a dynamic registry of
     * exception types to diagnose the active exception.
     */
    void describe(std::ostream& os);

private:
    /**
     * A DynamicCatch that provides handlers with an std::ostream& into which
     * to describe the exception they've caught.
     */
    using CatchAndDescribe = DynamicCatch<std::ostream&>;

    static void _exceptionTypeBlurb(const std::type_info& ex, std::ostream& os);

    std::vector<std::function<void(CatchAndDescribe&)>> _configurators;
};

/** The singleton ActiveExceptionWitness. */
ActiveExceptionWitness& globalActiveExceptionWitness();

}  // namespace mongo
