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

#include "mongo/bson/optime.h"

#include <iostream>
#include <limits>
#include <ctime>

#include "mongo/bson/inline_decls.h"

namespace mongo {

    OpTime OpTime::max() {
        unsigned int t = static_cast<unsigned int>(std::numeric_limits<int32_t>::max());
        unsigned int i = std::numeric_limits<uint32_t>::max();
        return OpTime(t, i);
    }

}
