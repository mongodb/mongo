// record_store.cpp

/**
*    Copyright (C) 2013 10gen Inc.
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

#include "mongo/db/structure/record_store.h"

#include "mongo/db/storage/extent.h"
#include "mongo/db/structure/collection.h"

#include "mongo/db/pdfile.h" // XXX-ERH

namespace mongo {

    RecordStore::RecordStore() {
        _collection = NULL;
        _details = NULL;
    }

    void RecordStore::init( CollectionTemp* collection) {
        _collection = collection;
        _details = _collection->_details;
        _isSystemIndexes = _collection->ns().coll() == "system.indexes";
    }

    void RecordStore::deallocRecord( const DiskLoc& dl, Record* todelete ) {
        ExtentManager* em = _collection->getExtentManager();

        /* remove ourself from the record next/prev chain */
        {
            if ( todelete->prevOfs() != DiskLoc::NullOfs ) {
                DiskLoc prev = em->getPrevRecordInExtent( dl );
                Record* prevRecord = em->recordFor( prev );
                getDur().writingInt( prevRecord->nextOfs() ) = todelete->nextOfs();
            }

            if ( todelete->nextOfs() != DiskLoc::NullOfs ) {
                DiskLoc next = em->getNextRecord( dl );
                Record* nextRecord = em->recordFor( next );
                getDur().writingInt( nextRecord->prevOfs() ) = todelete->prevOfs();
            }
        }

        /* remove ourself from extent pointers */
        {
            Extent *e = getDur().writing( todelete->myExtent(dl) );
            if ( e->firstRecord == dl ) {
                if ( todelete->nextOfs() == DiskLoc::NullOfs )
                    e->firstRecord.Null();
                else
                    e->firstRecord.set(dl.a(), todelete->nextOfs() );
            }
            if ( e->lastRecord == dl ) {
                if ( todelete->prevOfs() == DiskLoc::NullOfs )
                    e->lastRecord.Null();
                else
                    e->lastRecord.set(dl.a(), todelete->prevOfs() );
            }
        }

        /* add to the free list */
        {
            _details->incrementStats( -1 * todelete->netLength(), -1 );

            if ( _isSystemIndexes ) {
                /* temp: if in system.indexes, don't reuse, and zero out: we want to be
                   careful until validated more, as IndexDetails has pointers
                   to this disk location.  so an incorrectly done remove would cause
                   a lot of problems.
                */
                memset( getDur().writingPtr(todelete, todelete->lengthWithHeaders() ),
                        0, todelete->lengthWithHeaders() );
            }
            else {
                DEV {
                    unsigned long long *p = reinterpret_cast<unsigned long long *>( todelete->data() );
                    *getDur().writing(p) = 0;
                }
                _details->addDeletedRec((DeletedRecord*)todelete, dl);
            }
        }

    }

}
