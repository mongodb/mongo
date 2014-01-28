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
#include "mongo/db/catalog/collection.h"


#include "mongo/db/pdfile.h" // XXX-ERH

namespace mongo {

    RecordStore::RecordStore( const StringData& ns )
        : _ns( ns.toString() ) {
    }

    RecordStore::~RecordStore() {
    }

    // -------------------------------

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

        addRecordToRecListInExtent(r, loc.getValue()); // XXX move code here from pdfile

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

        addRecordToRecListInExtent(r, loc.getValue()); // XXX move code here from pdfile

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

    // -------------------------------

    SimpleRecordStoreV1::SimpleRecordStoreV1( const StringData& ns,
                                              NamespaceDetails* details,
                                              ExtentManager* em,
                                              bool isSystemIndexes )
        : RecordStoreV1Base( ns, details, em, isSystemIndexes ) {
    }

    SimpleRecordStoreV1::~SimpleRecordStoreV1() {
    }

    StatusWith<DiskLoc> SimpleRecordStoreV1::allocRecord( int lengthWithHeaders, int quotaMax ) {
        DiskLoc loc = _details->alloc( _ns, lengthWithHeaders );
        if ( !loc.isNull() )
            return StatusWith<DiskLoc>( loc );

        LOG(1) << "allocating new extent";

        _extentManager->increaseStorageSize( _ns, _details,
                                             Extent::followupSize( lengthWithHeaders,
                                                                   _details->lastExtentSize()),
                                             quotaMax );

        loc = _details->alloc( _ns, lengthWithHeaders );
        if ( !loc.isNull() ) {
            // got on first try
            return StatusWith<DiskLoc>( loc );
        }

        log() << "warning: alloc() failed after allocating new extent. "
              << "lengthWithHeaders: " << lengthWithHeaders << " last extent size:"
              << _details->lastExtentSize() << "; trying again";

        for ( int z = 0; z < 10 && lengthWithHeaders > _details->lastExtentSize(); z++ ) {
            log() << "try #" << z << endl;

            _extentManager->increaseStorageSize( _ns, _details,
                                                 Extent::followupSize( lengthWithHeaders,
                                                                       _details->lastExtentSize()),
                                                 quotaMax );

            loc = _details->alloc( _ns, lengthWithHeaders);
            if ( ! loc.isNull() )
                return StatusWith<DiskLoc>( loc );
        }

        return StatusWith<DiskLoc>( ErrorCodes::InternalError, "cannot allocate space" );
    }

    // -------------------------------

    CappedRecordStoreV1::CappedRecordStoreV1( const StringData& ns,
                                              NamespaceDetails* details,
                                              ExtentManager* em,
                                              bool isSystemIndexes )
        : RecordStoreV1Base( ns, details, em, isSystemIndexes ) {
    }

    CappedRecordStoreV1::~CappedRecordStoreV1() {
    }

    StatusWith<DiskLoc> CappedRecordStoreV1::allocRecord( int lengthWithHeaders, int quotaMax ) {
        DiskLoc loc = _details->alloc( _ns, lengthWithHeaders );
        if ( !loc.isNull() )
            return StatusWith<DiskLoc>( loc );

        return StatusWith<DiskLoc>( ErrorCodes::InternalError,
                                    "no space in capped collection" );
    }

}
