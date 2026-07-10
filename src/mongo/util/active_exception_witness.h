/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
