/*    Copyright 2013 10gen Inc.
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

#include <iosfwd>

namespace mongo {
namespace logger {

    /**
     * Interface for objects that encode Events to std::ostreams.
     *
     * Most appender implementations write to streams, and Encoders represent the process of
     * encoding events into streams.
     */
    template <typename Event>
    class Encoder {
    public:
        virtual ~Encoder() {}
        virtual std::ostream& encode(const Event& event, std::ostream& os) = 0;
    };

}  // namespace logger
}  // nnamspace mongo
