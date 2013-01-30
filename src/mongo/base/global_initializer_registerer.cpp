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

#include "mongo/base/global_initializer_registerer.h"

#include <cstdlib>
#include <iostream>

#include "mongo/base/global_initializer.h"
#include "mongo/base/initializer.h"

namespace mongo {

    GlobalInitializerRegisterer::GlobalInitializerRegisterer(
            const std::string& name,
            const InitializerFunction& fn,
            const std::vector<std::string>& prerequisites,
            const std::vector<std::string>& dependents) {

        Status status = getGlobalInitializer().getInitializerDependencyGraph().addInitializer(
                name, fn, prerequisites, dependents);


        if (Status::OK() != status) {
            std::cerr << "Attempt to add global initializer failed, status: "
                      << status << std::endl;
            ::abort();
        }
    }

}  // namespace mongo
