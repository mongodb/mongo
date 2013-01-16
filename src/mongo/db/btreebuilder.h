/**
*    Copyright (C) 2012 10gen Inc.
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
*/

#pragma once

#include "btree.h"

namespace mongo {

    /**
     * build btree from the bottom up
     */
    template< class V >
    class BtreeBuilder {
        typedef typename V::KeyOwned KeyOwned;
        typedef typename V::Key Key;
        
        bool dupsAllowed;
        IndexDetails& idx;
        /** Number of keys added to btree. */
        unsigned long long n;
        /** Last key passed to addKey(). */
        auto_ptr< typename V::KeyOwned > keyLast;
        BSONObj order;
        Ordering ordering;
        /** true iff commit() completed successfully. */
        bool committed;

        DiskLoc cur, first;
        BtreeBucket<V> *b;

        void newBucket();
        void buildNextLevel(DiskLoc loc, bool mayInterrupt);
        void mayCommitProgressDurably();

    public:
        BtreeBuilder(bool _dupsAllowed, IndexDetails& _idx);

        /**
         * Preconditions: 'key' is > or >= last key passed to this function (depends on _dupsAllowed)
         * Postconditions: 'key' is added to intermediate storage.
         */
        void addKey(BSONObj& key, DiskLoc loc);

        /**
         * commit work.  if not called, destructor will clean up partially completed work
         *  (in case exception has happened).
         */
        void commit(bool mayInterrupt);

        unsigned long long getn() { return n; }
    };

}
