// string_map_internal.h


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

namespace mongo {

    inline size_t StringMapDefaultHash::operator()( const StringData& key ) const {
        size_t mx = key.size();
        size_t hash = 7;
        for ( size_t i = 0; i < mx; i++ ) {
            hash += ( 517 * static_cast<int>(key[i]) );
            hash *= 13;
        }
        if ( hash == 0 )
            hash = -1;
        return hash;
    }


}
