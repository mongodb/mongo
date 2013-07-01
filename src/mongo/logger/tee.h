/*    Copyright 2009 10gen Inc.
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

namespace mongo {
namespace logger {

    class Tee {
    public:
        virtual ~Tee() {}

        /**
         * Implementations of Tee::write must handle their own synchronization.  Callers may assume
         * it is safe to call this method at any time from any thread.
         */
        virtual void write(const std::string& str) = 0;
    };

}  // namespace logger
}  // namespace mongo
