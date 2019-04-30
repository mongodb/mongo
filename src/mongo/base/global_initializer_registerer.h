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

#include <string>
#include <vector>

#include "mongo/base/initializer_function.h"
#include "mongo/base/status.h"

namespace mongo {

// The name of the "default" global initializer.
// Global initializers with no explicit prerequisites depends on it by default.
extern const std::string& defaultInitializerName();

/**
 * Type representing the registration of a global intialization function.
 *
 * Create a nonlocal static storage duration instance of this type to register a new initializer, to
 * be run by a call to a variant of mongo::runGlobalInitializers().  See "mongo/base/initializer.h",
 * "mongo/base/init.h', and "mongo/base/initializer_dependency_graph.h" for details.
 */
class GlobalInitializerRegisterer {
public:
    /**
    * Constructor parameters:
    *
    *     - std::string name
    *
    *     - InitializerFunction initFn
    *         Must be nonnull.
    *         Example expression:
    *
    *            [](InitializerContext* context) {
    *                // initialization code
    *                return Status::OK();
    *            }
    *
    *     - DeinitializerFunction deinitFn
    *         A deinitialization that will execute in reverse order from initialization and
    *         support re-initialization. If not specified, defaults to the `nullptr` function.
    *         Example expression:
    *
    *            [](DeinitializerContext* context) {
    *                // deinitialization code
    *                return Status::OK();
    *            }
    *
    *     - std::vector<std::string> prerequisites
    *         If not specified, defaults to {"default"}.
    *
    *     - std::vector<std::string> dependents
    *         If not specified, defaults to {} (no dependents).
    *
    *
    * At run time, the full set of prerequisites for `name` will be computed as the union of the
    * `prerequisites` (which can be defaulted) and all other mongo initializers that list `name` in
    * their `dependents`.
    *
    * A non-null `deinitFn` will tag the initializer as supporting re-initialization.
    */
    GlobalInitializerRegisterer(std::string name,
                                InitializerFunction initFn,
                                DeinitializerFunction deinitFn = nullptr,
                                std::vector<std::string> prerequisites = {defaultInitializerName()},
                                std::vector<std::string> dependents = {});

    GlobalInitializerRegisterer(const GlobalInitializerRegisterer&) = delete;
    GlobalInitializerRegisterer& operator=(const GlobalInitializerRegisterer&) = delete;
};

}  // namespace mongo
