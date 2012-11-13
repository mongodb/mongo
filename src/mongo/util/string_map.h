// string_map.h

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
#include "mongo/util/unordered_fast_key_table.h"

namespace mongo {

    struct StringMapDefaultHash {
        size_t operator()( const char* k ) const;
    };

    struct StringMapDefaultEqual {
        bool operator()( const char* a, const char* b ) const {
            return strcmp( a,b ) == 0;
        }
    };

    struct StringMapDefaultConvertor {
        const char* operator()( const std::string& s ) const {
            return s.c_str();
        }
    };

    template< typename V >
    class StringMap : public UnorderedFastKeyTable< const char*, // K_L
                                      std::string, // K_S
                                      V,           // V
                                      StringMapDefaultHash,
                                      StringMapDefaultEqual,
                                      StringMapDefaultConvertor > {
    };
}

#include "mongo/util/string_map_internal.h"

