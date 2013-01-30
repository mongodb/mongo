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
#include "mongo/base/initializer_function.h"
#include "mongo/base/status.h"

namespace mongo {

    /**
     * Type representing the act of registering a process-global intialization function.
     *
     * Create a module-global instance of this type to register a new initializer, to be run by a
     * call to a variant of mongo::runGlobalInitializers().  See mongo/base/initializer.h,
     * mongo/base/init.h and mongo/base/initializer_dependency_graph.h for details.
     */
    class GlobalInitializerRegisterer {
        MONGO_DISALLOW_COPYING(GlobalInitializerRegisterer);

    public:
        GlobalInitializerRegisterer(const std::string& name,
                                    const InitializerFunction& fn,
                                    const std::vector<std::string>& prerequisites,
                                    const std::vector<std::string>& dependents);
    };

}  // namespace mongo
