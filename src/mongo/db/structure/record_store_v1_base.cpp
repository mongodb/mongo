// record_store_v1_base.cpp

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

#include "mongo/db/structure/record_store_v1_base.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/dur.h"
#include "mongo/db/storage/extent.h"
#include "mongo/db/storage/extent_manager.h"
#include "mongo/db/storage/record.h"
#include "mongo/db/structure/catalog/namespace_details.h"
#include "mongo/util/mmap.h"

namespace mongo {

    RecordStoreV1Base::RecordStoreV1Base( const StringData& ns,
                                          NamespaceDetails* details,
                                          ExtentManager* em,
                                          bool isSystemIndexes )
        : RecordStore( ns ),
          _details( details ),
          _extentManager( em ),
          _isSystemIndexes( isSystemIndexes ) {
    }

    RecordStoreV1Base::~RecordStoreV1Base() {
    }

    Record* RecordStoreV1Base::recordFor( const DiskLoc& loc ) const {
        return _extentManager->recordFor( loc );
    }

    const DeletedRecord* RecordStoreV1Base::deletedRecordFor( const DiskLoc& loc ) const {
        invariant( loc.a() != -1 );
        return reinterpret_cast<const DeletedRecord*>( recordFor( loc ) );
    }

    DeletedRecord* RecordStoreV1Base::drec( const DiskLoc& loc ) const {
        invariant( loc.a() != -1 );
        return reinterpret_cast<DeletedRecord*>( recordFor( loc ) );
    }

    StatusWith<DiskLoc> RecordStoreV1Base::insertRecord( const DocWriter* doc, int quotaMax ) {
        int lenWHdr = doc->documentSize() + Record::HeaderSize;
        if ( doc->addPadding() )
            lenWHdr = _details->getRecordAllocationSize( lenWHdr );

        StatusWith<DiskLoc> loc = allocRecord( lenWHdr, quotaMax );
        if ( !loc.isOK() )
            return loc;

        Record *r = recordFor( loc.getValue() );
        fassert( 17319, r->lengthWithHeaders() >= lenWHdr );

        r = reinterpret_cast<Record*>( getDur().writingPtr(r, lenWHdr) );
        doc->writeDocument( r->data() );

        _addRecordToRecListInExtent(r, loc.getValue());

        _details->incrementStats( r->netLength(), 1 );

        return loc;
    }


    StatusWith<DiskLoc> RecordStoreV1Base::insertRecord( const char* data, int len, int quotaMax ) {
        int lenWHdr = _details->getRecordAllocationSize( len + Record::HeaderSize );
        fassert( 17208, lenWHdr >= ( len + Record::HeaderSize ) );

        StatusWith<DiskLoc> loc = allocRecord( lenWHdr, quotaMax );
        if ( !loc.isOK() )
            return loc;

        Record *r = recordFor( loc.getValue() );
        fassert( 17210, r->lengthWithHeaders() >= lenWHdr );

        // copy the data
        r = reinterpret_cast<Record*>( getDur().writingPtr(r, lenWHdr) );
        memcpy( r->data(), data, len );

        _addRecordToRecListInExtent(r, loc.getValue());

        _details->incrementStats( r->netLength(), 1 );

        return loc;
    }

    void RecordStoreV1Base::deleteRecord( const DiskLoc& dl ) {

        Record* todelete = recordFor( dl );

        /* remove ourself from the record next/prev chain */
        {
            if ( todelete->prevOfs() != DiskLoc::NullOfs ) {
                DiskLoc prev = _extentManager->getPrevRecordInExtent( dl );
                Record* prevRecord = recordFor( prev );
                getDur().writingInt( prevRecord->nextOfs() ) = todelete->nextOfs();
            }

            if ( todelete->nextOfs() != DiskLoc::NullOfs ) {
                DiskLoc next = _extentManager->getNextRecord( dl );
                Record* nextRecord = recordFor( next );
                getDur().writingInt( nextRecord->prevOfs() ) = todelete->prevOfs();
            }
        }

        /* remove ourself from extent pointers */
        {
            Extent *e = getDur().writing( _extentManager->extentFor( dl ) );
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
                addDeletedRec(dl);
            }
        }

    }

    void RecordStoreV1Base::_addRecordToRecListInExtent(Record *r, DiskLoc loc) {
        dassert( loc.rec() == r );
        Extent *e = _extentManager->extentFor( loc );
        if ( e->lastRecord.isNull() ) {
            Extent::FL *fl = getDur().writing(e->fl());
            fl->firstRecord = fl->lastRecord = loc;
            r->prevOfs() = r->nextOfs() = DiskLoc::NullOfs;
        }
        else {
            Record *oldlast = e->lastRecord.rec();
            r->prevOfs() = e->lastRecord.getOfs();
            r->nextOfs() = DiskLoc::NullOfs;
            getDur().writingInt(oldlast->nextOfs()) = loc.getOfs();
            getDur().writingDiskLoc(e->lastRecord) = loc;
        }
    }

    void RecordStoreV1Base::increaseStorageSize( int size, int quotaMax ) {
        DiskLoc eloc = _extentManager->allocateExtent( _ns,
                                                       _details->isCapped(),
                                                       size,
                                                       quotaMax );

        Extent *e = _extentManager->getExtent( eloc, false );

        invariant( e );

        DiskLoc emptyLoc = getDur().writing(e)->reuse( _ns, _details->isCapped() );

        if ( _details->lastExtent().isNull() ) {
            verify( _details->firstExtent().isNull() );
            _details->setFirstExtent( eloc );
            _details->setLastExtent( eloc );
            getDur().writingDiskLoc( _details->capExtent() ) = eloc;
            verify( e->xprev.isNull() );
            verify( e->xnext.isNull() );
        }
        else {
            verify( !_details->firstExtent().isNull() );
            getDur().writingDiskLoc(e->xprev) = _details->lastExtent();
            getDur().writingDiskLoc(_extentManager->getExtent(_details->lastExtent())->xnext) = eloc;
            _details->setLastExtent( eloc );
        }

        _details->setLastExtentSize( e->length );

        addDeletedRec(emptyLoc);
    }

}
