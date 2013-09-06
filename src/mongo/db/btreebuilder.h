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
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#pragma once

#include "mongo/db/btree.h"

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
