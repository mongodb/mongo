// unordered_fast_key_table.h

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

#include <boost/smart_ptr/scoped_array.hpp>

#include "mongo/base/disallow_copying.h"

namespace mongo {

    template<typename K_L, typename K_S>
    struct UnorderedFastKeyTable_LS_C {
        K_S operator()( const K_L& a ) const {
            return K_S(a);
        }
    };

    template< typename K_L, // key lookup
              typename K_S, // key storage
              typename V, // value
              typename H , // hash of K_L
              typename E, // equal of K_L
              typename C, // convertor from K_S -> K_L
              typename C_LS=UnorderedFastKeyTable_LS_C<K_L,K_S> // convertor from K_L -> K_S
              >
    class UnorderedFastKeyTable {
    public:
        typedef std::pair<K_S, V> value_type;
        typedef K_L key_type;
        typedef V mapped_type;

    private:
        struct Entry {
            Entry()
                : used( false ), everUsed( false ) {
            }

            bool used;
            bool everUsed;
            size_t curHash;
            value_type data;
        };

        struct Area {
            Area( unsigned capacity, double maxProbeRatio );
            Area( const Area& other );

            int find( const K_L& key, size_t hash, int* firstEmpty, const UnorderedFastKeyTable& sm ) const;

            bool transfer( Area* newArea, const UnorderedFastKeyTable& sm ) const;

            void swap( Area* other ) {
                using std::swap;
                swap( _capacity, other->_capacity );
                swap( _maxProbe, other->_maxProbe );
                swap( _entries, other->_entries );
            }

            unsigned _capacity;
            unsigned _maxProbe;
            boost::scoped_array<Entry> _entries;
        };

    public:
        static const unsigned DEFAULT_STARTING_CAPACITY = 20;

        /**
         * @param startingCapacity how many buckets should exist on initial creation
         *                         DEFAULT_STARTING_CAPACITY
         * @param maxProbeRatio the percentage of buckets we're willing to probe
         *                      no defined default as you can't have a static const double on windows
         */
        UnorderedFastKeyTable( unsigned startingCapacity = DEFAULT_STARTING_CAPACITY,
                               double maxProbeRatio = 0.05 );

        UnorderedFastKeyTable( const UnorderedFastKeyTable& other );

        UnorderedFastKeyTable& operator=( const UnorderedFastKeyTable& other ) {
            other.copyTo( this );
            return *this;
        }

        void copyTo( UnorderedFastKeyTable* out ) const;

        /**
         * @return number of elements in map
         */
        size_t size() const { return _size; }

        bool empty() const { return _size == 0; }

        /*
         * @return storage space
         */
        size_t capacity() const { return _area._capacity; }

        V& operator[]( const K_L& key ) { return get( key ); }

        V& get( const K_L& key );

        /**
         * @return number of elements removed
         */
        size_t erase( const K_L& key );

        class const_iterator {
        public:
            const_iterator() { _position = -1; }
            const_iterator( const Area* area ) {
                _area = area;
                _position = 0;
                _max = _area->_capacity - 1;
                _skip();
            }
            const_iterator( const Area* area, int pos ) {
                _area = area;
                _position = pos;
                _max = pos;
            }

            const value_type* operator->() const { return &_area->_entries[_position].data; }

            const_iterator operator++() {
                if ( _position < 0 )
                    return *this;
                _position++;
                if ( _position > _max )
                    _position = -1;
                else
                    _skip();
                return *this;
            }

            bool operator==( const const_iterator& other ) const {
                return _position == other._position;
            }
            bool operator!=( const const_iterator& other ) const {
                return _position != other._position;
            }

        private:

            void _skip() {
                while ( true ) {
                    if ( _area->_entries[_position].used )
                        break;
                    if ( _position >= _max ) {
                        _position = -1;
                        break;
                    }
                    ++_position;
                }
            }

            const Area* _area;
            int _position;
            int _max; // inclusive
        };

        /**
         * @return either a one-shot iterator with the key, or end()
         */
        const_iterator find( const K_L& key ) const;

        const_iterator begin() const;

        const_iterator end() const;

    private:
        /*
         * @param firstEmpty, if we return -1, and firstEmpty != NULL,
         *                    this will be set to the first empty bucket we found
         * @retrun offset into _entries or -1 if not there
         */
        int _find( const K_L& key, int hash, int* firstEmpty ) const;

        void _grow();

        // ----

        size_t _size;
        double _maxProbeRatio;
        Area _area;

        H _hash;
        E _equals;
        C _convertor;
        C_LS _convertorOther;
    };

}

#include "mongo/util/unordered_fast_key_table_internal.h"

