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

/**
 * Utility macro to construct a std::vector<std::string> from a sequence of C-style
 * strings.
 *
 * Usage:  MONGO_MAKE_STRING_VECTOR("a", "b", "c") returns a vector containing
 * std::strings "a", "b", "c", in that order.
 */
#define MONGO_MAKE_STRING_VECTOR(...) ::mongo::_makeStringVector(0, __VA_ARGS__, NULL)

namespace mongo {

    /**
     * Create a vector of strings from varargs of C-style strings.
     *
     * WARNING: Only intended for use by MONGO_MAKE_STRING_VECTOR macro, defined above.  Aborts
     * ungracefully if you misuse it, so stick to the macro.
     *
     * The first parameter is ignored in all circumstances. The subsequent parameters must be
     * const char* C-style strings, or NULL. Of these parameters, at least one must be
     * NULL. Parameters at and beyond the NULL are not inserted. Typically, the NULL will be
     * the last parameter. The MONGO_MAKE_STRING_VECTOR macro enforces this.
     *
     * Returns a vector of std::strings.
     */
    std::vector<std::string> _makeStringVector(int ignored, ...);

}  // namespace mongo
