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

/**
 * Utility macros for declaring global initializers
 *
 * Should NOT be included by other header files.  Include only in source files.
 *
 * Initializers are arranged in an acyclic directed dependency graph.  Declaring
 * a cycle will lead to a runtime error.
 *
 * Initializer functions take a parameter of type ::mongo::InitializerContext*.
 * They throw to indicate failure, stopping further intializer processing.
 */

#pragma once

#include "mongo/base/initializer.h"
#include "mongo/base/status.h"

#include <string>
#include <vector>

/**
 * Macro to define an initializer function named "NAME" with the default
 * prerequisite set, that is `("default")`, and no explicit dependents.
 *
 * The default set of prerequisites for initializer functions has one member,
 * called "default". It refers to a do-nothing initializer called "default"
 * defined by this init system, which serves as a partition point. It splits
 * the graph into two broad phases. In practice, most initializers can omit
 * their prerequisites and dependents, and use the MONGO_INITIALIZER macro, and
 * such rules will be constrained to occur _after_ the "default" sequence
 * point. Some special rules perform early preparation like options parsing,
 * and several initializers might depend on these, so they will typically
 * specify that they happen _before_ the "default" sequence point.
 *
 * The pre-"default" phase is further broken down into an orderly series of
 * internal stages. (See util/options_parser/startup_option_init.cpp).
 *
 * See MONGO_INITIALIZER_GENERAL.
 *
 * Usage:
 *     MONGO_INITIALIZER(myModule)(::mongo::InitializerContext* context) {
 *         ...
 *     }
 */
#define MONGO_INITIALIZER(NAME) MONGO_INITIALIZER_GENERAL(NAME, ("default"), ())

/**
 * Macro to define an initializer function named "NAME" that depends on the initializers
 * specified in PREREQUISITES to have been completed, but names no explicit dependents.
 *
 * See MONGO_INITIALIZER_GENERAL.
 *
 * Usage:
 *     MONGO_INITIALIZER_WITH_PREREQUISITES(myGlobalStateChecker,
 *                                         ("globalStateInitialized", "stacktraces"))(
 *            ::mongo::InitializerContext* context) {
 *    }
 */
#define MONGO_INITIALIZER_WITH_PREREQUISITES(NAME, PREREQUISITES) \
    MONGO_INITIALIZER_GENERAL(NAME, PREREQUISITES, ())

#define MONGO_INITIALIZER_STRIP_PARENS_(...) __VA_ARGS__

/**
 * Macro to define an initializer that depends on PREREQUISITES and has DEPENDENTS as explicit
 * dependents.
 *
 * NAME is any legitimate name for a C++ symbol.
 * PREREQUISITES is a tuple of strings surrounded by parens, e.g., ("a", "b", "c"), or ().
 * DEPENDENTS is a tuple of strings surrounded by parens.
 *
 * At run time, the full set of prerequisites for NAME will be computed as the union of the
 * explicit PREREQUISITES and the set of all other mongo initializers that name NAME in their
 * list of DEPENDENTS.
 *
 * Usage:
 *    MONGO_INITIALIZER_GENERAL(myInitializer,
 *                             ("myPrereq1", "myPrereq2", ...),
 *                             ("myDependent1", "myDependent2", ...))(
 *            ::mongo::InitializerContext* context) {
 *    }
 *
 * TODO: May want to be able to name the initializer separately from the function name.
 * A form that takes an existing function or that lets the programmer supply the name
 * of the function to declare would be options.
 */
#define MONGO_INITIALIZER_GENERAL(NAME, PREREQUISITES, DEPENDENTS)               \
    void MONGO_INITIALIZER_FUNCTION_NAME_(NAME)(::mongo::InitializerContext*);   \
    namespace {                                                                  \
    ::mongo::GlobalInitializerRegisterer _mongoInitializerRegisterer_##NAME(     \
        std::string(#NAME),                                                      \
        mongo::InitializerFunction(MONGO_INITIALIZER_FUNCTION_NAME_(NAME)),      \
        mongo::DeinitializerFunction(nullptr),                                   \
        std::vector<std::string>{MONGO_INITIALIZER_STRIP_PARENS_ PREREQUISITES}, \
        std::vector<std::string>{MONGO_INITIALIZER_STRIP_PARENS_ DEPENDENTS});   \
    }                                                                            \
    void MONGO_INITIALIZER_FUNCTION_NAME_(NAME)

/**
 * Macro to define an initializer group.
 *
 * An initializer group is an initializer that performs no actions.  It is useful for organizing
 * initialization steps into phases, such as "all global parameter declarations completed", "all
 * global parameters initialized".
 */
#define MONGO_INITIALIZER_GROUP(NAME, PREREQUISITES, DEPENDENTS) \
    MONGO_INITIALIZER_GENERAL(NAME, PREREQUISITES, DEPENDENTS)(::mongo::InitializerContext*) {}

/**
 * Macro to produce a name for a mongo initializer function for an initializer operation
 * named "NAME".
 */
#define MONGO_INITIALIZER_FUNCTION_NAME_(NAME) _mongoInitializerFunction_##NAME

namespace mongo {

/**
 * Type representing the registration of a global initialization function.
 *
 * Create a nonlocal static storage duration instance of this type to register a new initializer, to
 * be run by a call to a variant of mongo::runGlobalInitializers().
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
     *            }
     *
     *     - DeinitializerFunction deinitFn
     *         A deinitialization that will execute in reverse order from initialization and
     *         support re-initialization. If not specified, defaults to `nullptr`.
     *         Example expression:
     *
     *            [](DeinitializerContext* context) {
     *                // deinitialization code
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
                                std::vector<std::string> prerequisites = {"default"},
                                std::vector<std::string> dependents = {});

    GlobalInitializerRegisterer(const GlobalInitializerRegisterer&) = delete;
    GlobalInitializerRegisterer& operator=(const GlobalInitializerRegisterer&) = delete;
};

}  // namespace mongo
