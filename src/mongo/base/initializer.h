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
#include <string>
#include <vector>

#include "mongo/base/status.h"

namespace mongo {

/** Context of an initialization process. Passed as a parameter to initialization functions. */
class InitializerContext {
public:
    explicit InitializerContext(std::vector<std::string> args) : _args(std::move(args)) {}

    const std::vector<std::string>& args() const {
        return _args;
    }

private:
    std::vector<std::string> _args;
};

/** Context of a deinitialization process. Passed as a parameter to deinitialization functions. */
class DeinitializerContext {
public:
    DeinitializerContext(const DeinitializerContext&) = delete;
    DeinitializerContext& operator=(const DeinitializerContext&) = delete;
};

/**
 * An InitializerFunction implements the behavior of an initializer operation.
 * It may inspect and mutate the supplied InitializerContext.
 * Throws on failure.
 */
using InitializerFunction = std::function<void(InitializerContext*)>;

/**
 * A DeinitializerFunction implements the behavior of a deinitializer operation.
 * It may inspect and mutate the supplied DeinitializerContext.
 * Throws on failure.
 */
using DeinitializerFunction = std::function<void(DeinitializerContext*)>;


/**
 * Class representing an initialization process.
 *
 * Such a process is described by a directed acyclic graph of initialization operations, the
 * InitializerDependencyGraph. One constructs an initialization process by adding nodes and
 * edges to the graph.  Then, one executes the process, causing each initialization operation to
 * execute in an order that respects the programmer-established prerequistes.
 *
 * The initialize and delinitialize process can repeat, a features which
 * supports embedded contexts.  However, the graph cannot be modified with
 * `addInitializer` after the first initialization. Latecomers are rejected.
 */
class Initializer {
public:
    Initializer();
    ~Initializer();

    /**
     * Add a new initializer node, with the specified `name`, to the dependency graph, with the
     * given behavior, `initFn`, `deinitFn`, and with the given `prerequisites` and `dependents`,
     * which are the names of other initializers which will be in the graph when `topSort`
     * is called. `initFn` must be non-null, but null-valued `deinitFn` are allowed.
     *
     * - Throws `ErrorCodes::BadValue` if `initFn` is null-valued.
     *
     * - Throws with `ErrorCodes::CannotMutateObject` if the graph has been frozen
     *   by a previous call to `executeInitializers`.
     */
    void addInitializer(std::string name,
                        InitializerFunction initFn,
                        DeinitializerFunction deinitFn,
                        std::vector<std::string> prerequisites,
                        std::vector<std::string> dependents);

    /**
     * Execute the initializer process, using the given args as input.
     * This call freezes the graph, so that addInitializer will reject any latecomers.
     *
     * Throws on initialization failures, or on invalid call sequences
     * (double-init, double-deinit, etc) and the thing being initialized should
     * be considered dead in the water.
     */
    void executeInitializers(const std::vector<std::string>& args);

    /**
     * Executes all deinit functions in reverse order from init order.
     * Note that this does not unfreeze the graph. Freezing is permanent.
     */
    void executeDeinitializers();

    /**
     * Returns the function mapped to `name`, for testing only.
     *
     * Throws with `ErrorCodes::BadValue` if name is not mapped to a node.
     */
    InitializerFunction getInitializerFunctionForTesting(const std::string& name);

private:
    class Graph;

    /**
     *  kNeverInitialized
     *  |
     *  +-> kUninitialized <----------+
     *      |                         |
     *      +-> kInitializing         |
     *          |                     |
     *          +-> kInitialized      |
     *              |                 |
     *              +-> kDeinitializing
     */
    enum class State {
        kNeverInitialized,  ///< still accepting addInitializer calls
        kUninitialized,
        kInitializing,
        kInitialized,
        kDeinitializing,
    };

    void _transition(State expected, State next);

    std::unique_ptr<Graph> _graph;  // pimpl
    std::vector<std::string> _sortedNodes;
    State _lifecycleState = State::kNeverInitialized;
};

/**
 * Get the process-global initializer object.
 */
Initializer& getGlobalInitializer();

/**
 * Run the global initializers.
 *
 * It's a programming error for this to fail, but if it does it will return a status other
 * than Status::OK.
 *
 * This means that the few initializers that might want to terminate the program by failing
 * should probably arrange to terminate the process themselves.
 */
Status runGlobalInitializers(const std::vector<std::string>& argv);

/**
 * Same as runGlobalInitializers(), except prints a brief message to std::cerr
 * and terminates the process on failure.
 */
void runGlobalInitializersOrDie(const std::vector<std::string>& argv);

/**
 * Run the global deinitializers. They will execute in reverse order from initialization.
 *
 * It's a programming error for this to fail, but if it does it will return a status other
 * than Status::OK.
 *
 * This means that the few initializers that might want to terminate the program by failing
 * should probably arrange to terminate the process themselves.
 */
Status runGlobalDeinitializers();

}  // namespace mongo
