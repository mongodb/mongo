// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/dynamic_catch.h"
#include "mongo/util/modules.h"

#include <functional>
#include <iosfwd>
#include <string>
#include <typeinfo>
#include <vector>

#include <boost/optional.hpp>

[[MONGO_MOD_PUBLIC]];

namespace mongo {

/**
 * Analyzes the active exception, describing it to an ostream.
 * Consults a dynamic registry of exception handlers.
 * See `util/dynamic_catch.h`.
 */
class ActiveExceptionWitness {
public:
    struct ExceptionInfo {
        std::string description;
        const std::type_info* type;
    };

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
    void addHandler(std::function<ExceptionInfo(const Ex&)> handler) {
        _configurators.push_back([handler](CatchAndDescribe& dc) {
            dc.addCatch<Ex>([handler](const Ex& ex, ExceptionInfo& info) { info = handler(ex); });
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

    /**
     * Returns the info of the active exception if any of the built-in
     * exception probes or any of the configured exception probes given by calls to `addHandler`
     * are able to catch the exception. Returns boost::none otherwise.
     *
     * Requires there is an active exception.
     */
    boost::optional<ExceptionInfo> info();

private:
    /**
     * A DynamicCatch that provides handlers with an std::ostream& into which
     * to describe the exception they've caught.
     */
    using CatchAndDescribe = DynamicCatch<ExceptionInfo&>;

    static void _exceptionTypeBlurb(const std::type_info& ex, std::ostream& os);

    std::vector<std::function<void(CatchAndDescribe&)>> _configurators;
};

ActiveExceptionWitness& globalActiveExceptionWitness();

/**
 * Returns the result of `ActiveExceptionWitness::describe` as a string.
 */
std::string describeActiveException();

/**
 * Returns information about the active exception. If there are no handlers for the active
 * exception, returns a null optional.
 *
 * Requires there is an active exception. Equivalent of calling `ActiveExceptionWitness::info`.
 */
boost::optional<ActiveExceptionWitness::ExceptionInfo> activeExceptionInfo();

}  // namespace mongo
