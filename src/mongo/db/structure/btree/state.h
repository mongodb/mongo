// state.h

/**
*    Copyright (C) 2008 10gen Inc.
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

#include "mongo/base/disallow_copying.h"
#include "mongo/db/diskloc.h"
#include "mongo/db/jsobj.h"
#include "mongo/bson/ordering.h"

namespace mongo {

    class Collection;
    class IndexDetails;
    class IndexDescriptor;
    class NamesapceDetails;
    class RecordStore;

    /**
     * This class is maybe temporary, and terribly named
     * its goal is to keep the state and data structure associated with a btree in one place
     * previously, the btree code called back out of its world through globals, which is bad
     * the goal here is to make the btree at least a sane visitorish pattern
     * so this is passed to each method, and it can be operated on
     * long term, the containmenent should be flipped
     */
    class BtreeInMemoryState {
        MONGO_DISALLOW_COPYING( BtreeInMemoryState );
    public:
        BtreeInMemoryState( Collection* collection,
                            const IndexDescriptor* descriptor,
                            RecordStore* recordStore,
                            IndexDetails* details );

        const Collection* collection() const { return _collection; }

        const IndexDescriptor* descriptor() const { return _descriptor; }
        const Ordering& ordering() const { return _ordering; }

        RecordStore* recordStore() { return _recordStore.get(); }
        const RecordStore* recordStore() const { return _recordStore.get(); }

        // ----

        const DiskLoc& head() const;

        void setHead( DiskLoc newHead );

        void setMultikey();

        // ------------


        template< class V >
        const BtreeBucket<V>* getHeadBucket() const { return getBucket<V>( head() ); }

        template< class V >
        const BtreeBucket<V>* getBucket( const DiskLoc& loc ) const;

    private:
        // the collection this index is over, not storage for the index
        Collection* _collection; // not-owned here

        const IndexDescriptor* _descriptor; // not-owned here

        // the record store for this index (where its buckets go)
        scoped_ptr<RecordStore> _recordStore; // OWNED HERE

        IndexDetails* _indexDetails; // TODO: remove

        Ordering _ordering;

    };

}
