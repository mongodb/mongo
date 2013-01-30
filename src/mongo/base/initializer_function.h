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

#include <boost/function.hpp>

#include "mongo/base/status.h"

namespace mongo {

    class InitializerContext;

    /**
     * An InitializerFunction implements the behavior of an initializer operation.
     *
     * On successful execution, an InitializerFunction returns Status::OK().  It may
     * inspect and mutate the supplied InitializerContext.
     */
    typedef boost::function<Status (InitializerContext*)> InitializerFunction;

}  // namespace mongo
