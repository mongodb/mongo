// record_store_v1_simple.cpp

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

#include "mongo/db/structure/record_store_v1_simple.h"

#include "mongo/base/counter.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/dur.h"
#include "mongo/db/storage/extent.h"
#include "mongo/db/storage/extent_manager.h"
#include "mongo/db/storage/record.h"
#include "mongo/db/structure/catalog/namespace_details.h"
#include "mongo/db/structure/record_store_v1_simple_iterator.h"
#include "mongo/util/mmap.h"

namespace mongo {

    static Counter64 freelistAllocs;
    static Counter64 freelistBucketExhausted;
    static Counter64 freelistIterations;

    static ServerStatusMetricField<Counter64> dFreelist1( "storage.freelist.search.requests",
                                                          &freelistAllocs );

    static ServerStatusMetricField<Counter64> dFreelist2( "storage.freelist.search.bucketExhausted",
                                                          &freelistBucketExhausted );

    static ServerStatusMetricField<Counter64> dFreelist3( "storage.freelist.search.scanned",
                                                          &freelistIterations );

    SimpleRecordStoreV1::SimpleRecordStoreV1( const StringData& ns,
                                              NamespaceDetails* details,
                                              ExtentManager* em,
                                              bool isSystemIndexes )
        : RecordStoreV1Base( ns, details, em, isSystemIndexes ) {
        invariant( !details->isCapped() );
        _normalCollection = NamespaceString::normal( ns );
    }

    SimpleRecordStoreV1::~SimpleRecordStoreV1() {
    }

    DiskLoc SimpleRecordStoreV1::_allocFromExistingExtents( int lenToAlloc ) {
        // align very slightly.
        lenToAlloc = (lenToAlloc + 3) & 0xfffffffc;

        freelistAllocs.increment();
        DiskLoc loc;
        {
            DiskLoc *prev;
            DiskLoc *bestprev = 0;
            DiskLoc bestmatch;
            int bestmatchlen = 0x7fffffff;
            int b = _details->bucket(lenToAlloc);
            DiskLoc cur = _details->_deletedList[b];
            prev = &_details->_deletedList[b];
            int extra = 5; // look for a better fit, a little.
            int chain = 0;
            while ( 1 ) {
                { // defensive check
                    int fileNumber = cur.a();
                    int fileOffset = cur.getOfs();
                    if (fileNumber < -1 || fileNumber >= 100000 || fileOffset < 0) {
                        StringBuilder sb;
                        sb << "Deleted record list corrupted in bucket " << b
                           << ", link number " << chain
                           << ", invalid link is " << cur.toString()
                           << ", throwing Fatal Assertion";
                        problem() << sb.str() << endl;
                        fassertFailed(16469);
                    }
                }
                if ( cur.isNull() ) {
                    // move to next bucket.  if we were doing "extra", just break
                    if ( bestmatchlen < 0x7fffffff )
                        break;

                    if ( chain > 0 ) {
                        // if we looked at things in the right bucket, but they were not suitable
                        freelistBucketExhausted.increment();
                    }

                    b++;
                    if ( b > MaxBucket ) {
                        // out of space. alloc a new extent.
                        freelistIterations.increment( 1 + chain );
                        return DiskLoc();
                    }
                    cur = _details->_deletedList[b];
                    prev = &_details->_deletedList[b];
                    continue;
                }
                DeletedRecord *r = drec(cur);
                if ( r->lengthWithHeaders() >= lenToAlloc &&
                     r->lengthWithHeaders() < bestmatchlen ) {
                    bestmatchlen = r->lengthWithHeaders();
                    bestmatch = cur;
                    bestprev = prev;
                    if (r->lengthWithHeaders() == lenToAlloc)
                        // exact match, stop searching
                        break;
                }
                if ( bestmatchlen < 0x7fffffff && --extra <= 0 )
                    break;
                if ( ++chain > 30 && b < MaxBucket ) {
                    // too slow, force move to next bucket to grab a big chunk
                    //b++;
                    freelistIterations.increment( chain );
                    chain = 0;
                    cur.Null();
                }
                else {
                    cur = r->nextDeleted();
                    prev = &r->nextDeleted();
                }
            }

            // unlink ourself from the deleted list
            DeletedRecord *bmr = drec(bestmatch);
            *getDur().writing(bestprev) = bmr->nextDeleted();
            bmr->nextDeleted().writing().setInvalid(); // defensive.
            invariant(bmr->extentOfs() < bestmatch.getOfs());

            freelistIterations.increment( 1 + chain );
            loc = bestmatch;
        }

        if ( loc.isNull() )
            return loc;

        // determine if we should chop up

        DeletedRecord *r = drec(loc);

        /* note we want to grab from the front so our next pointers on disk tend
        to go in a forward direction which is important for performance. */
        int regionlen = r->lengthWithHeaders();
        invariant( r->extentOfs() < loc.getOfs() );

        int left = regionlen - lenToAlloc;
        if ( left < 24 || left < (lenToAlloc >> 3) ) {
            // you get the whole thing.
            return loc;
        }

        // don't quantize:
        //   - $ collections (indexes) as we already have those aligned the way we want SERVER-8425
        if ( _normalCollection ) {
            // we quantize here so that it only impacts newly sized records
            // this prevents oddities with older records and space re-use SERVER-8435
            lenToAlloc = std::min( r->lengthWithHeaders(),
                                   NamespaceDetails::quantizeAllocationSpace( lenToAlloc ) );
            left = regionlen - lenToAlloc;

            if ( left < 24 ) {
                // you get the whole thing.
                return loc;
            }
        }

        /* split off some for further use. */
        getDur().writingInt(r->lengthWithHeaders()) = lenToAlloc;
        DiskLoc newDelLoc = loc;
        newDelLoc.inc(lenToAlloc);
        DeletedRecord* newDel = drec(newDelLoc);
        DeletedRecord* newDelW = getDur().writing(newDel);
        newDelW->extentOfs() = r->extentOfs();
        newDelW->lengthWithHeaders() = left;
        newDelW->nextDeleted().Null();

        addDeletedRec( newDelLoc );

        return loc;

    }

    StatusWith<DiskLoc> SimpleRecordStoreV1::allocRecord( int lengthWithHeaders, int quotaMax ) {
        DiskLoc loc = _allocFromExistingExtents( lengthWithHeaders );
        if ( !loc.isNull() )
            return StatusWith<DiskLoc>( loc );

        LOG(1) << "allocating new extent";

        increaseStorageSize( Extent::followupSize( lengthWithHeaders,
                                                   _details->lastExtentSize()),
                             quotaMax );

        loc = _allocFromExistingExtents( lengthWithHeaders );
        if ( !loc.isNull() ) {
            // got on first try
            return StatusWith<DiskLoc>( loc );
        }

        log() << "warning: alloc() failed after allocating new extent. "
              << "lengthWithHeaders: " << lengthWithHeaders << " last extent size:"
              << _details->lastExtentSize() << "; trying again";

        for ( int z = 0; z < 10 && lengthWithHeaders > _details->lastExtentSize(); z++ ) {
            log() << "try #" << z << endl;

            increaseStorageSize( Extent::followupSize( lengthWithHeaders,
                                                       _details->lastExtentSize()),
                                 quotaMax );

            loc = _allocFromExistingExtents( lengthWithHeaders );
            if ( ! loc.isNull() )
                return StatusWith<DiskLoc>( loc );
        }

        return StatusWith<DiskLoc>( ErrorCodes::InternalError, "cannot allocate space" );
    }

    Status SimpleRecordStoreV1::truncate() {
        return Status( ErrorCodes::InternalError,
                       "SimpleRecordStoreV1::truncate not implemented" );
    }

    void SimpleRecordStoreV1::addDeletedRec( const DiskLoc& dloc ) {
        DeletedRecord* d = drec( dloc );
        BOOST_STATIC_ASSERT( sizeof(NamespaceDetails::Extra) <= sizeof(NamespaceDetails) );

        {
            Record *r = (Record *) getDur().writingPtr(d, sizeof(Record));
            d = &r->asDeleted();
            // defensive code: try to make us notice if we reference a deleted record
            reinterpret_cast<unsigned*>( r->data() )[0] = 0xeeeeeeee;
        }
        DEBUGGING log() << "TEMP: add deleted rec " << dloc.toString() << ' ' << hex << d->extentOfs() << endl;

        int b = _details->bucket(d->lengthWithHeaders());
        DiskLoc& list = _details->_deletedList[b];
        DiskLoc oldHead = list;
        getDur().writingDiskLoc(list) = dloc;
        d->nextDeleted() = oldHead;
    }

    RecordIterator* SimpleRecordStoreV1::getIterator( const DiskLoc& start, bool tailable,
                                                      const CollectionScanParams::Direction& dir) const {
        return new SimpleRecordStoreV1Iterator( this, start, dir );
    }

}
