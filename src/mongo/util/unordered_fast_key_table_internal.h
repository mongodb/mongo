// unordered_fast_key_table_internal.h


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
    template< typename K_L, typename K_S, typename V, typename H, typename E, typename C >
    UnorderedFastKeyTable<K_L, K_S, V, H, E, C>::Area::Area( unsigned capacity, double maxProbeRatio )
        : _capacity( capacity ),
          _maxProbe( static_cast<unsigned>( capacity * maxProbeRatio ) ),
          _entries( new Entry[_capacity] ) {
    }

    template< typename K_L, typename K_S, typename V, typename H, typename E, typename C >
    int UnorderedFastKeyTable<K_L, K_S, V, H, E, C>::Area::find( const K_L& key,
                                                                 size_t hash,
                                                                 int* firstEmpty,
                                                                 const UnorderedFastKeyTable& sm ) const {
        if ( firstEmpty )
            *firstEmpty = -1;

        for ( unsigned probe = 0; probe < _maxProbe; probe++ ) {
            unsigned pos = (hash + probe) % _capacity;

            if ( ! _entries[pos].used ) {
                // space is empty
                if ( firstEmpty && *firstEmpty == -1 )
                    *firstEmpty = pos;
                if ( ! _entries[pos].everUsed )
                    return -1;
                continue;
            }

            if ( _entries[pos].curHash != hash ) {
                // space has something else
                continue;
            }

            if ( ! sm._equals(key, sm._convertor( _entries[pos].data.first ) ) ) {
                // hashes match
                // strings are not equals
                continue;
            }

            // hashes and strings are equal
            // yay!
            return pos;
        }
        return -1;
    }

    template< typename K_L, typename K_S, typename V, typename H, typename E, typename C >
    void UnorderedFastKeyTable<K_L, K_S, V, H, E, C>::Area::transfer( Area* newArea,
                                                                      const UnorderedFastKeyTable& sm ) const {
        for ( unsigned i = 0; i < _capacity; i++ ) {
            if ( ! _entries[i].used )
                continue;

            int firstEmpty = -1;
            int loc = newArea->find( sm._convertor( _entries[i].data.first ),
                                     _entries[i].curHash,
                                     &firstEmpty,
                                     sm );

            verify( loc == -1 );
            verify( firstEmpty >= 0 );

            newArea->_entries[firstEmpty] = _entries[i];
        }
    }

    template< typename K_L, typename K_S, typename V, typename H, typename E, typename C >
    UnorderedFastKeyTable<K_L, K_S, V, H, E, C>::UnorderedFastKeyTable( unsigned startingCapacity,
                                                                        double maxProbeRatio )
        : _maxProbeRatio( maxProbeRatio ), _area( startingCapacity, maxProbeRatio ) {
        _size = 0;
    }

    template< typename K_L, typename K_S, typename V, typename H, typename E, typename C >
    V& UnorderedFastKeyTable<K_L, K_S, V, H, E, C>::get( const K_L& key ) {

        const size_t hash = _hash( key );

        for ( int numGrowTries = 0; numGrowTries < 10; numGrowTries++ ) {
            int firstEmpty = -1;
            int pos = _area.find( key, hash, &firstEmpty, *this );
            if ( pos >= 0 )
                return _area._entries[pos].data.second;

            // key not in map
            // need to add
            if ( firstEmpty >= 0 ) {
                _size++;
                _area._entries[firstEmpty].used = true;
                _area._entries[firstEmpty].everUsed = true;
                _area._entries[firstEmpty].curHash = hash;
                _area._entries[firstEmpty].data.first = key;
                return _area._entries[firstEmpty].data.second;
            }

            // no space left in map
            _grow();
        }
        msgasserted( 16471, "UnorderedFastKeyTable couldn't add entry after growing many times" );
    }

    template< typename K_L, typename K_S, typename V, typename H, typename E, typename C >
    void UnorderedFastKeyTable<K_L, K_S, V, H, E, C>::_grow() {
        Area newArea( _area._capacity * 2, _maxProbeRatio );
        _area.transfer( &newArea, *this );
        _area.swap( &newArea );
    }

}
