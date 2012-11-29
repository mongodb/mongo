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

    inline size_t StringMapDefaultHash::operator()( const char* origKey ) const {
        const char* key = origKey;
        size_t hash = 7;
        while ( *key ) {
            hash += ( 517 * static_cast<int>(*key) );
            hash *= 13;
            key++;
        }
        if ( hash == 0 )
            hash = -1;
        return hash;
    }

    template< typename K_L, typename K_S, typename V, typename H, typename E, typename C, typename C_LS >
    inline typename UnorderedFastKeyTable<K_L, K_S, V, H, E, C, C_LS>::const_iterator
    UnorderedFastKeyTable<K_L, K_S, V, H, E, C, C_LS>::find( const K_L& key ) const {
        if ( _size == 0 )
            return const_iterator();
        int pos = _area.find( key, _hash(key), 0, *this );
        if ( pos < 0 )
            return const_iterator();
        return const_iterator( &_area._entries[pos] );
    }

    template< typename K_L, typename K_S, typename V, typename H, typename E, typename C, typename C_LS >
    inline typename UnorderedFastKeyTable<K_L, K_S, V, H, E, C, C_LS>::const_iterator
    UnorderedFastKeyTable<K_L, K_S, V, H, E, C, C_LS>::end() const {
        return const_iterator();
    }

}
