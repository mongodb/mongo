// record_store_v1_capped.cpp

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

#include "mongo/db/structure/record_store_v1_capped.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/dur.h"
#include "mongo/db/storage/extent.h"
#include "mongo/db/storage/extent_manager.h"
#include "mongo/db/storage/record.h"
#include "mongo/db/structure/catalog/namespace_details.h"

namespace mongo {

    CappedRecordStoreV1::CappedRecordStoreV1( Collection* collection,
                                              const StringData& ns,
                                              NamespaceDetails* details,
                                              ExtentManager* em,
                                              bool isSystemIndexes )
        : RecordStoreV1Base( ns, details, em, isSystemIndexes ),
          _collection( collection ) {

        DiskLoc extentLoc = details->firstExtent();
        while ( !extentLoc.isNull() ) {
            Extent* extent = em->getExtent( extentLoc );
            extentLoc = extent->xnext;
            MAdvise* m( new MAdvise( reinterpret_cast<void*>( extent ),
                                     extent->length,
                                     MAdvise::Sequential ) );
            _extentAdvice.mutableVector().push_back( m );
        }
    }

    CappedRecordStoreV1::~CappedRecordStoreV1() {
    }

    StatusWith<DiskLoc> CappedRecordStoreV1::allocRecord( int lenToAlloc, int quotaMax ) {
        {
            // align very slightly.
            lenToAlloc = (lenToAlloc + 3) & 0xfffffffc;
        }

        if ( lenToAlloc > _details->theCapExtent()->length ) {
            // the extent check is a way to try and improve performance
            // since we have to iterate all the extents (for now) to get
            // storage size
            if ( lenToAlloc > _collection->storageSize() ) {
                return StatusWith<DiskLoc>( ErrorCodes::BadValue,
                                            str::stream() << "document is larger than capped size "
                                            << lenToAlloc << " > " << _collection->storageSize(),
                                            16328 );
            }

        }
        DiskLoc loc;
        { // do allocation

            // signal done allocating new extents.
            if ( !_details->cappedLastDelRecLastExtent().isValid() )
                getDur().writingDiskLoc( _details->cappedLastDelRecLastExtent() ) = DiskLoc();

            verify( lenToAlloc < 400000000 );
            int passes = 0;
            int maxPasses = ( lenToAlloc / 30 ) + 2; // 30 is about the smallest entry that could go in the oplog
            if ( maxPasses < 5000 ) {
                // this is for bacwards safety since 5000 was the old value
                maxPasses = 5000;
            }

            // delete records until we have room and the max # objects limit achieved.

            /* this fails on a rename -- that is ok but must keep commented out */
            //verify( theCapExtent()->ns == ns );

            _details->theCapExtent()->assertOk();
            DiskLoc firstEmptyExtent;
            while ( 1 ) {
                if ( _details->_stats.nrecords < _details->maxCappedDocs() ) {
                    loc = _details->__capAlloc( lenToAlloc );
                    if ( !loc.isNull() )
                        break;
                }

                // If on first iteration through extents, don't delete anything.
                if ( !_details->_capFirstNewRecord.isValid() ) {
                    _details->advanceCapExtent( _ns );

                    if ( _details->_capExtent != _details->_firstExtent )
                        _details->_capFirstNewRecord.writing().setInvalid();
                    // else signal done with first iteration through extents.
                    continue;
                }

                if ( !_details->_capFirstNewRecord.isNull() &&
                     _details->theCapExtent()->firstRecord == _details->_capFirstNewRecord ) {
                    // We've deleted all records that were allocated on the previous
                    // iteration through this extent.
                    _details->advanceCapExtent( _ns );
                    continue;
                }

                if ( _details->theCapExtent()->firstRecord.isNull() ) {
                    if ( firstEmptyExtent.isNull() )
                        firstEmptyExtent = _details->_capExtent;
                    _details->advanceCapExtent( _ns );
                    if ( firstEmptyExtent == _details->_capExtent ) {
                        _details->maybeComplain( _ns, lenToAlloc );
                        return StatusWith<DiskLoc>( ErrorCodes::InternalError,
                                                    "no space in capped collection" );
                    }
                    continue;
                }

                DiskLoc fr = _details->theCapExtent()->firstRecord;
                _collection->deleteDocument( fr, true );
                _details->compact();
                if( ++passes > maxPasses ) {
                    StringBuilder sb;
                    sb << "passes >= maxPasses in NamespaceDetails::cappedAlloc: ns: " << _ns
                       << ", lenToAlloc: " << lenToAlloc
                       << ", maxPasses: " << maxPasses
                       << ", _maxDocsInCapped: " << _details->_maxDocsInCapped
                       << ", nrecords: " << _details->_stats.nrecords
                       << ", datasize: " << _details->_stats.datasize;

                    return StatusWith<DiskLoc>( ErrorCodes::InternalError, sb.str() );
                }
            }

            // Remember first record allocated on this iteration through capExtent.
            if ( _details->_capFirstNewRecord.isValid() && _details->_capFirstNewRecord.isNull() )
                getDur().writingDiskLoc(_details->_capFirstNewRecord) = loc;
        }

        invariant( !loc.isNull() );

        // possibly slice up if we've allocated too much space

        DeletedRecord *r = loc.drec();

        /* note we want to grab from the front so our next pointers on disk tend
        to go in a forward direction which is important for performance. */
        int regionlen = r->lengthWithHeaders();
        invariant( r->extentOfs() < loc.getOfs() );

        int left = regionlen - lenToAlloc;

        /* split off some for further use. */
        getDur().writingInt(r->lengthWithHeaders()) = lenToAlloc;
        DiskLoc newDelLoc = loc;
        newDelLoc.inc(lenToAlloc);
        DeletedRecord* newDel = newDelLoc.drec();
        DeletedRecord* newDelW = getDur().writing(newDel);
        newDelW->extentOfs() = r->extentOfs();
        newDelW->lengthWithHeaders() = left;
        newDelW->nextDeleted().Null();

        _details->addDeletedRec(newDel, newDelLoc);

        return StatusWith<DiskLoc>( loc );
    }

    Status CappedRecordStoreV1::truncate() {
        // Get a writeable reference to 'this' and reset all pertinent
        // attributes.
        NamespaceDetails* t = _details->writingWithoutExtra();

        t->cappedLastDelRecLastExtent() = DiskLoc();
        t->cappedListOfAllDeletedRecords() = DiskLoc();

        // preserve firstExtent/lastExtent
        t->_capExtent = t->_firstExtent;
        t->_stats.datasize = t->_stats.nrecords = 0;
        // lastExtentSize preserve
        // nIndexes preserve 0
        // capped preserve true
        // max preserve
        t->_paddingFactor = 1.0;
        t->_systemFlags = 0;
        t->_capFirstNewRecord = DiskLoc();
        t->_capFirstNewRecord.setInvalid();
        t->cappedLastDelRecLastExtent().setInvalid();
        // dataFileVersion preserve
        // indexFileVersion preserve
        t->_multiKeyIndexBits = 0;
        t->_reservedA = 0;
        t->_extraOffset = 0;
        // indexBuildInProgress preserve 0
        memset(t->_reserved, 0, sizeof(t->_reserved));

        // Reset all existing extents and recreate the deleted list.
        for( DiskLoc ext = t->_firstExtent; !ext.isNull(); ext = ext.ext()->xnext ) {
            DiskLoc prev = ext.ext()->xprev;
            DiskLoc next = ext.ext()->xnext;
            DiskLoc empty = ext.ext()->reuse( _ns, true );
            ext.ext()->xprev.writing() = prev;
            ext.ext()->xnext.writing() = next;
            _details->addDeletedRec( empty.drec(), empty );
        }

        return Status::OK();
    }

    void CappedRecordStoreV1::temp_cappedTruncateAfter( DiskLoc end, bool inclusive ) {
        _details->cappedTruncateAfter( _ns.c_str(), end, inclusive );
    }
}
