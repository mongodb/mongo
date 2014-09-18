// unordered_fast_key_table_internal.h


/*    Copyright 2012 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
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

        --_size;
        _area._entries[pos].used = false;
        _area._entries[pos].data.second = V();
        return 1;
    }

    template< typename K_L, typename K_S, typename V, typename H, typename E, typename C, typename C_LS >
    void UnorderedFastKeyTable<K_L, K_S, V, H, E, C, C_LS>::erase( const_iterator it ) {
        dassert(it._position >= 0);
        dassert(it._area == &_area);

        --_size;
        _area._entries[it._position].used = false;
        _area._entries[it._position].data.second = V();
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
