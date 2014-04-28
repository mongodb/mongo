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
#include "mongo/db/client.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/curop.h"
#include "mongo/db/storage/mmap_v1/dur.h"
#include "mongo/db/kill_current_op.h"
#include "mongo/db/storage/extent.h"
#include "mongo/db/storage/extent_manager.h"
#include "mongo/db/storage/record.h"
#include "mongo/db/structure/catalog/namespace_details.h"
#include "mongo/db/structure/record_store_v1_simple_iterator.h"
#include "mongo/util/progress_meter.h"
#include "mongo/util/touch_pages.h"

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
                                              RecordStoreV1MetaData* details,
                                              ExtentManager* em,
                                              bool isSystemIndexes )
        : RecordStoreV1Base( ns, details, em, isSystemIndexes ) {

        invariant( !details->isCapped() );
        _normalCollection = NamespaceString::normal( ns );
        if ( _details->paddingFactor() == 0 ) {
            warning() << "implicit updgrade of paddingFactor of very old collection" << endl;
            _details->setPaddingFactor(1.0);
        }

    }

    SimpleRecordStoreV1::~SimpleRecordStoreV1() {
    }

    DiskLoc SimpleRecordStoreV1::_allocFromExistingExtents( int lenToAlloc ) {
        // align very slightly.
        lenToAlloc = (lenToAlloc + 3) & 0xfffffffc;

        freelistAllocs.increment();
        DiskLoc loc;
        {
            DiskLoc *prev = 0;
            DiskLoc *bestprev = 0;
            DiskLoc bestmatch;
            int bestmatchlen = 0x7fffffff;
            int b = _details->bucket(lenToAlloc);
            DiskLoc cur = _details->deletedListEntry(b);
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
                    cur = _details->deletedListEntry(b);
                    prev = 0;
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
            if ( bestprev ) {
                *getDur().writing(bestprev) = bmr->nextDeleted();
            }
            else {
                // should be the front of a free-list
                int bucket = _details->bucket(bmr->lengthWithHeaders());
                invariant( _details->deletedListEntry(bucket) == bestmatch );
                _details->setDeletedListEntry(bucket, bmr->nextDeleted());
            }
            *getDur().writing(&bmr->nextDeleted()) = DiskLoc().setInvalid(); // defensive.
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

        {
            Record *r = (Record *) getDur().writingPtr(d, sizeof(Record));
            d = &r->asDeleted();
            // defensive code: try to make us notice if we reference a deleted record
            reinterpret_cast<unsigned*>( r->data() )[0] = 0xeeeeeeee;
        }
        DEBUGGING log() << "TEMP: add deleted rec " << dloc.toString() << ' ' << hex << d->extentOfs() << endl;

        int b = _details->bucket(d->lengthWithHeaders());
        d->nextDeleted() = _details->deletedListEntry(b);
        _details->setDeletedListEntry(b, dloc);
    }

    RecordIterator* SimpleRecordStoreV1::getIterator( const DiskLoc& start, bool tailable,
                                                      const CollectionScanParams::Direction& dir) const {
        return new SimpleRecordStoreV1Iterator( this, start, dir );
    }

    vector<RecordIterator*> SimpleRecordStoreV1::getManyIterators() const {
        OwnedPointerVector<RecordIterator> iterators;
        const Extent* ext;
        for (DiskLoc extLoc = details()->firstExtent(); !extLoc.isNull(); extLoc = ext->xnext) {
            ext = _getExtent(extLoc);
            if (ext->firstRecord.isNull())
                continue;

            iterators.push_back(new RecordStoreV1Base::IntraExtentIterator(ext->firstRecord, this));
        }

        return iterators.release();
    }

    class CompactDocWriter : public DocWriter {
    public:
        /**
         * param allocationSize - allocation size WITH header
         */
        CompactDocWriter( const Record* rec, unsigned dataSize, size_t allocationSize )
            : _rec( rec ),
              _dataSize( dataSize ),
              _allocationSize( allocationSize ) {
        }

        virtual ~CompactDocWriter() {}

        virtual void writeDocument( char* buf ) const {
            memcpy( buf, _rec->data(), _dataSize );
        }

        virtual size_t documentSize() const {
            return _allocationSize - Record::HeaderSize;
        }

        virtual bool addPadding() const {
            return false;
        }

    private:
        const Record* _rec;
        size_t _dataSize;
        size_t _allocationSize;
    };

    void SimpleRecordStoreV1::_compactExtent(const DiskLoc diskloc, int extentNumber,
                                             RecordStoreCompactAdaptor* adaptor,
                                             const CompactOptions* compactOptions,
                                             CompactStats* stats ) {

        log() << "compact begin extent #" << extentNumber
              << " for namespace " << _ns << " " << diskloc;

        unsigned oldObjSize = 0; // we'll report what the old padding was
        unsigned oldObjSizeWithPadding = 0;

        Extent *e = _extentManager->getExtent( diskloc );
        e->assertOk();
        fassert( 17437, e->validates(diskloc) );

        {
            // the next/prev pointers within the extent might not be in order so we first
            // page the whole thing in sequentially
            log() << "compact paging in len=" << e->length/1000000.0 << "MB" << endl;
            Timer t;
            size_t length = e->length;

            touch_pages( reinterpret_cast<const char*>(e), length );
            int ms = t.millis();
            if( ms > 1000 )
                log() << "compact end paging in " << ms << "ms "
                      << e->length/1000000.0/t.seconds() << "MB/sec" << endl;
        }

        {
            log() << "compact copying records" << endl;
            long long datasize = 0;
            long long nrecords = 0;
            DiskLoc L = e->firstRecord;
            if( !L.isNull() ) {
                while( 1 ) {
                    Record *recOld = recordFor(L);
                    L = getNextRecordInExtent(L);

                    if ( compactOptions->validateDocuments && !adaptor->isDataValid(recOld) ) {
                        // object is corrupt!
                        log() << "compact skipping corrupt document!";
                        stats->corruptDocuments++;
                    }
                    else {
                        unsigned dataSize = adaptor->dataSize( recOld );
                        unsigned docSize = dataSize;

                        nrecords++;
                        oldObjSize += docSize;
                        oldObjSizeWithPadding += recOld->netLength();

                        unsigned lenWHdr = docSize + Record::HeaderSize;
                        unsigned lenWPadding = lenWHdr;

                        switch( compactOptions->paddingMode ) {
                        case CompactOptions::NONE:
                            if ( _details->isUserFlagSet(NamespaceDetails::Flag_UsePowerOf2Sizes) )
                                lenWPadding = _details->quantizePowerOf2AllocationSpace(lenWPadding);
                            break;
                        case CompactOptions::PRESERVE:
                            // if we are preserving the padding, the record should not change size
                            lenWPadding = recOld->lengthWithHeaders();
                            break;
                        case CompactOptions::MANUAL:
                            lenWPadding = compactOptions->computeRecordSize(lenWPadding);
                            if (lenWPadding < lenWHdr || lenWPadding > BSONObjMaxUserSize / 2 ) {
                                lenWPadding = lenWHdr;
                            }
                            break;
                        }

                        CompactDocWriter writer( recOld, dataSize, lenWPadding );
                        StatusWith<DiskLoc> status = insertRecord( &writer, 0 );
                        uassertStatusOK( status.getStatus() );
                        datasize += recordFor( status.getValue() )->netLength();

                        adaptor->inserted( recordFor( status.getValue() ), status.getValue() );
                    }

                    if( L.isNull() ) {
                        // we just did the very last record from the old extent.  it's still pointed to
                        // by the old extent ext, but that will be fixed below after this loop
                        break;
                    }

                    // remove the old records (orphan them) periodically so our commit block doesn't get too large
                    bool stopping = false;
                    RARELY stopping = *killCurrentOp.checkForInterruptNoAssert() != 0;
                    if( stopping || getDur().isCommitNeeded() ) {
                        *getDur().writing(&e->firstRecord) = L;
                        Record *r = recordFor(L);
                        getDur().writingInt(r->prevOfs()) = DiskLoc::NullOfs;
                        getDur().commitIfNeeded();
                        killCurrentOp.checkForInterrupt();
                    }
                }
            } // if !L.isNull()

            invariant( _details->firstExtent() == diskloc );
            invariant( _details->lastExtent() != diskloc );
            DiskLoc newFirst = e->xnext;
            _details->setFirstExtent( newFirst );
            *getDur().writing(&_extentManager->getExtent( newFirst )->xprev) = DiskLoc();
            getDur().writing(e)->markEmpty();
            _extentManager->freeExtents( diskloc, diskloc );

            getDur().commitIfNeeded();

            {
                double op = 1.0;
                if( oldObjSize )
                    op = static_cast<double>(oldObjSizeWithPadding)/oldObjSize;
                log() << "compact finished extent #" << extentNumber << " containing " << nrecords
                      << " documents (" << datasize/1000000.0 << "MB)"
                      << " oldPadding: " << op << ' ' << static_cast<unsigned>(op*100.0)/100;
            }
        }

    }

    Status SimpleRecordStoreV1::compact( RecordStoreCompactAdaptor* adaptor,
                                         const CompactOptions* options,
                                         CompactStats* stats ) {

        // this is a big job, so might as well make things tidy before we start just to be nice.
        getDur().commitIfNeeded();

        list<DiskLoc> extents;
        for( DiskLoc extLocation = _details->firstExtent();
             !extLocation.isNull();
             extLocation = _extentManager->getExtent( extLocation )->xnext ) {
            extents.push_back( extLocation );
        }
        log() << "compact " << extents.size() << " extents";

        log() << "compact orphan deleted lists" << endl;
        _details->orphanDeletedList();

        // Start over from scratch with our extent sizing and growth
        _details->setLastExtentSize( 0 );

        // create a new extent so new records go there
        increaseStorageSize( _details->lastExtentSize(), true );

        // reset data size and record counts to 0 for this namespace
        // as we're about to tally them up again for each new extent
        _details->setStats( 0, 0 );

        ProgressMeterHolder pm(cc().curop()->setMessage("compact extent",
                                                        "Extent Compacting Progress",
                                                        extents.size()));

        int extentNumber = 0;
        for( list<DiskLoc>::iterator i = extents.begin(); i != extents.end(); i++ ) {
            _compactExtent(*i, extentNumber++, adaptor, options, stats );
            pm.hit();
        }

        invariant( _extentManager->getExtent( _details->firstExtent() )->xprev.isNull() );
        invariant( _extentManager->getExtent( _details->lastExtent() )->xnext.isNull() );

        // indexes will do their own progress meter
        pm.finished();

        return Status::OK();
    }

}
