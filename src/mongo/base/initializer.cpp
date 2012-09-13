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

#include "mongo/base/initializer.h"

namespace mongo {

    Initializer::Initializer() {}
    Initializer::~Initializer() {}

    Status Initializer::execute(const InitializerContext::ArgumentVector& args,
                                const InitializerContext::EnvironmentMap& env) const {

        std::vector<std::string> sortedNodes;
        Status status = _graph.topSort(&sortedNodes);
        if (Status::OK() != status)
            return status;

        InitializerContext context(args, env, &_configVariables);

        for (size_t i = 0; i < sortedNodes.size(); ++i) {
            InitializerFunction fn = _graph.getInitializerFunction(sortedNodes[i]);
            if (!fn) {
                return Status(ErrorCodes::InternalError,
                              "topSort returned a node that has no associated function: \"" +
                              sortedNodes[i] + '"');
            }
            status = fn(&context);
            if (Status::OK() != status)
                return status;
        }
        return Status::OK();
    }

}  // namespace mongo
