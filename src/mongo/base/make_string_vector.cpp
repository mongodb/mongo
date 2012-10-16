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

#include "mongo/base/make_string_vector.h"

#include <cstdlib>
#include <iostream>
#include <stdarg.h>

namespace mongo {

    std::vector<std::string> _makeStringVector(int ignored, ...) {
        va_list ap;
        va_start(ap, ignored);
        const char* arg = va_arg(ap, const char *);
        if (arg) {
            std::cerr << "Internal error!\n";
            std::abort();
        }
        std::vector<std::string> result;
        while ((arg = va_arg(ap, const char *)))
            result.push_back(arg);
        va_end(ap);
        return result;
    }

}  // namespace mongo
