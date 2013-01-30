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

#include "mongo/base/string_data.h"
#include "mongo/util/unordered_fast_key_table.h"

namespace mongo {

    struct StringMapDefaultHash {
        size_t operator()( const StringData& k ) const;
    };

    struct StringMapDefaultEqual {
        bool operator()( const StringData& a, const StringData& b ) const {
            return a == b;
        }
    };

    struct StringMapDefaultConvertor {
        StringData operator()( const std::string& s ) const {
            return StringData( s );
        }
    };

    struct StringMapDefaultConvertorOther {
        string operator()( const StringData& s ) const {
            return s.toString();
        }
    };

    template< typename V >
    class StringMap : public UnorderedFastKeyTable< StringData, // K_L
                                                    std::string, // K_S
                                                    V,           // V
                                                    StringMapDefaultHash,
                                                    StringMapDefaultEqual,
                                                    StringMapDefaultConvertor,
                                                    StringMapDefaultConvertorOther > {
    };
}

#include "mongo/util/string_map_internal.h"

