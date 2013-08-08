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

#include <map>
#include <string>
#include <vector>

#include "mongo/base/disallow_copying.h"

namespace mongo {

    /**
     * Context of an initialization process.  Passed as a parameter to initialization functions.
     *
     * See mongo/base/initializer.h and mongo/base/initializer_dependency_graph.h for more details.
     */
    class InitializerContext {
        MONGO_DISALLOW_COPYING(InitializerContext);

    public:
        typedef std::vector<std::string> ArgumentVector;
        typedef std::map<std::string, std::string> EnvironmentMap;

        InitializerContext(const ArgumentVector& args,
                           const EnvironmentMap& env);

        const ArgumentVector& args() const { return _args; }
        const EnvironmentMap& env() const { return _env; }

    private:
        ArgumentVector _args;
        EnvironmentMap _env;
    };

}  // namespace mongo
