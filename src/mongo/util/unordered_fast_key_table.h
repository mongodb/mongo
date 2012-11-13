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

    template< typename K_L, // key lookup
              typename K_S, // key storage
              typename V, // value
              typename H , // hash of K_L
              typename E, // equal of K_L
              typename C // convertor from K_S -> K_L
              >
    class UnorderedFastKeyTable {
        MONGO_DISALLOW_COPYING(UnorderedFastKeyTable);
    public:
        typedef std::pair<K_S, V> value_type;
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

            int find( const K_L& key, size_t hash, int* firstEmpty, const UnorderedFastKeyTable& sm ) const;

            void transfer( Area* newArea, const UnorderedFastKeyTable& sm ) const;

            void swap( Area* other ) {
                std::swap( _capacity, other->_capacity );
                std::swap( _maxProbe, other->_maxProbe );
                _entries.swap( other->_entries );
            }

            unsigned _capacity;
            unsigned _maxProbe;
            boost::scoped_array<Entry> _entries;
        };

    public:
        static const unsigned DEFAULT_STARTING_CAPACITY = 20;
        static const double DEFAULT_MAX_PROBE_RATIO = 0.05;

        UnorderedFastKeyTable( unsigned startingCapacity = DEFAULT_STARTING_CAPACITY,
                               double maxProbeRatio = DEFAULT_MAX_PROBE_RATIO );

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

        class const_iterator {
        public:
            const_iterator() { _theEntry = NULL; }
            const_iterator( const Entry* entry ) { _theEntry = entry; }

            const value_type* operator->() const { return &_theEntry->data; }

            const_iterator operator++( int n ) { _theEntry = NULL; return *this; }

            bool operator==( const const_iterator& other ) const {
                return _theEntry == other._theEntry;
            }
            bool operator!=( const const_iterator& other ) const {
                return _theEntry != other._theEntry;
            }

        private:
            const Entry* _theEntry;
        };

        const_iterator find( const K_L& key ) const;

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
        const double _maxProbeRatio;
        Area _area;

        H _hash;
        E _equals;
        C _convertor;
    };

}

#include "mongo/util/unordered_fast_key_table_internal.h"

