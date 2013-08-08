/*    Copyright 2012 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#pragma once

#include <string>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/initializer_context.h"
#include "mongo/base/initializer_dependency_graph.h"
#include "mongo/base/status.h"

namespace mongo {

    /**
     * Class representing an initialization process.
     *
     * Such a process is described by a directed acyclic graph of initialization operations, the
     * InitializerDependencyGraph.  One constructs an initialization process by adding nodes and
     * edges to the graph.  Then, one executes the process, causing each initialization operation to
     * execute in an order that respects the programmer-established prerequistes.
     */
    class Initializer {
        MONGO_DISALLOW_COPYING(Initializer);
    public:
        Initializer();
        ~Initializer();

        /**
         * Get the initializer dependency graph, presumably for the purpose of adding more nodes.
         */
        InitializerDependencyGraph& getInitializerDependencyGraph() { return _graph; }

        /**
         * Execute the initializer process, using the given argv and environment data as input.
         *
         * Returns Status::OK on success.  All other returns constitute initialization failures,
         * and the thing being initialized should be considered dead in the water.
         */
        Status execute(const InitializerContext::ArgumentVector& args,
                       const InitializerContext::EnvironmentMap& env) const;

    private:

        InitializerDependencyGraph _graph;
    };

    /**
     * Run the global initializers.
     *
     * It's a programming error for this to fail, but if it does it will return a status other
     * than Status::OK.
     *
     * This means that the few initializers that might want to terminate the program by failing
     * should probably arrange to terminate the process themselves.
     */
    Status runGlobalInitializers(const InitializerContext::ArgumentVector& args,
                                 const InitializerContext::EnvironmentMap& env);

    Status runGlobalInitializers(int argc, const char* const* argv, const char* const* envp);

    /**
     * Same as runGlobalInitializers(), except prints a brief message to std::cerr
     * and terminates the process on failure.
     */
    void runGlobalInitializersOrDie(int argc, const char* const* argv, const char* const* envp);

}  // namespace mongo
