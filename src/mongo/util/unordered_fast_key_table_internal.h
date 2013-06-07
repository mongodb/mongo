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

#include "mongo/util/assert_util.h"

namespace mongo {
    template< typename K_L, typename K_S, typename V, typename H, typename E, typename C, typename C_LS >
    inline UnorderedFastKeyTable<K_L, K_S, V, H, E, C, C_LS>::Area::Area(unsigned capacity,
                                                                   double maxProbeRatio)
        : _capacity( capacity ),
          _maxProbe( static_cast<unsigned>( capacity * maxProbeRatio ) ),
          _entries( new Entry[_capacity] ) {
    }

    template< typename K_L, typename K_S, typename V, typename H, typename E, typename C, typename C_LS >
    inline UnorderedFastKeyTable<K_L, K_S, V, H, E, C, C_LS>::Area::Area(const Area& other )
        : _capacity( other._capacity ),
          _maxProbe( other._maxProbe ),
          _entries( new Entry[_capacity] ) {
        for ( unsigned i = 0; i < _capacity; i++ ) {
            _entries[i] = other._entries[i];
        }
    }

    template< typename K_L, typename K_S, typename V, typename H, typename E, typename C, typename C_LS >
    inline int UnorderedFastKeyTable<K_L, K_S, V, H, E, C, C_LS>::Area::find(
            const K_L& key,
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

    template< typename K_L, typename K_S, typename V, typename H, typename E, typename C, typename C_LS >
    inline bool UnorderedFastKeyTable<K_L, K_S, V, H, E, C, C_LS>::Area::transfer(
            Area* newArea,
            const UnorderedFastKeyTable& sm) const {
        for ( unsigned i = 0; i < _capacity; i++ ) {
            if ( ! _entries[i].used )
                continue;

            int firstEmpty = -1;
            int loc = newArea->find( sm._convertor( _entries[i].data.first ),
                                     _entries[i].curHash,
                                     &firstEmpty,
                                     sm );

            verify( loc == -1 );
            if ( firstEmpty < 0 ) {
                return false;
            }

            newArea->_entries[firstEmpty] = _entries[i];
        }
        return true;
    }

    template< typename K_L, typename K_S, typename V, typename H, typename E, typename C, typename C_LS >
    inline UnorderedFastKeyTable<K_L, K_S, V, H, E, C, C_LS>::UnorderedFastKeyTable(
            unsigned startingCapacity,
            double maxProbeRatio)
        : _maxProbeRatio( maxProbeRatio ), _area( startingCapacity, maxProbeRatio ) {
        _size = 0;
    }

    template< typename K_L, typename K_S, typename V, typename H, typename E, typename C, typename C_LS >
    inline UnorderedFastKeyTable<K_L, K_S, V, H, E, C, C_LS>::UnorderedFastKeyTable(
            const UnorderedFastKeyTable& other )
        : _size( other._size ),
          _maxProbeRatio( other._maxProbeRatio ),
          _area( other._area ),
          _hash( other._hash ),
          _equals( other._equals ),
          _convertor( other._convertor ),
          _convertorOther( other._convertorOther ) {
    }

    template< typename K_L, typename K_S, typename V, typename H, typename E, typename C, typename C_LS >
    inline void UnorderedFastKeyTable<K_L, K_S, V, H, E, C, C_LS>::copyTo( UnorderedFastKeyTable* out ) const {
        out->_size = _size;
        out->_maxProbeRatio = _maxProbeRatio;
        Area x( _area );
        out->_area.swap( &x );
    }

    template< typename K_L, typename K_S, typename V, typename H, typename E, typename C, typename C_LS >
    inline V& UnorderedFastKeyTable<K_L, K_S, V, H, E, C, C_LS>::get( const K_L& key ) {

        const size_t hash = _hash( key );

        for ( int numGrowTries = 0; numGrowTries < 5; numGrowTries++ ) {
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
                _area._entries[firstEmpty].data.first = _convertorOther(key);
                return _area._entries[firstEmpty].data.second;
            }

            // no space left in map
            _grow();
        }
        msgasserted( 16471, "UnorderedFastKeyTable couldn't add entry after growing many times" );
    }

    template< typename K_L, typename K_S, typename V, typename H, typename E, typename C, typename C_LS >
    inline size_t UnorderedFastKeyTable<K_L, K_S, V, H, E, C, C_LS>::erase( const K_L& key ) {

        const size_t hash = _hash( key );
        int pos = _area.find( key, hash, NULL, *this );

        if ( pos < 0 )
            return 0;

        _area._entries[pos].used = false;
        _area._entries[pos].data.second = V();
        return 1;
    }

    template< typename K_L, typename K_S, typename V, typename H, typename E, typename C, typename C_LS >
    inline void UnorderedFastKeyTable<K_L, K_S, V, H, E, C, C_LS>::_grow() {
        unsigned capacity = _area._capacity;
        for ( int numGrowTries = 0; numGrowTries < 5; numGrowTries++ ) {
            capacity *= 2;
            Area newArea( capacity, _maxProbeRatio );
            bool success = _area.transfer( &newArea, *this );
            if ( !success ) {
                continue;
            }
            _area.swap( &newArea );
            return;
        }
        msgasserted( 16845,
                     "UnorderedFastKeyTable::_grow couldn't add entry after growing many times" );
    }

    template< typename K_L, typename K_S, typename V, typename H, typename E, typename C, typename C_LS >
    inline typename UnorderedFastKeyTable<K_L, K_S, V, H, E, C, C_LS>::const_iterator
    UnorderedFastKeyTable<K_L, K_S, V, H, E, C, C_LS>::find( const K_L& key ) const {
        if ( _size == 0 )
            return const_iterator();
        int pos = _area.find( key, _hash(key), 0, *this );
        if ( pos < 0 )
            return const_iterator();
        return const_iterator( &_area, pos );
    }

    template< typename K_L, typename K_S, typename V, typename H, typename E, typename C, typename C_LS >
    inline typename UnorderedFastKeyTable<K_L, K_S, V, H, E, C, C_LS>::const_iterator
    UnorderedFastKeyTable<K_L, K_S, V, H, E, C, C_LS>::end() const {
        return const_iterator();
    }

    template< typename K_L, typename K_S, typename V, typename H, typename E, typename C, typename C_LS >
    inline typename UnorderedFastKeyTable<K_L, K_S, V, H, E, C, C_LS>::const_iterator
    UnorderedFastKeyTable<K_L, K_S, V, H, E, C, C_LS>::begin() const {
        return const_iterator( &_area );
    }
}
