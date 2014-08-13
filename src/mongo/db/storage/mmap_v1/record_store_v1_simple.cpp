// record_store_v1_simple.cpp

/**
 *    Copyright (C) 2013-2014 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/storage/mmap_v1/record_store_v1_simple.h"

#include "mongo/base/counter.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/curop.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/storage/mmap_v1/extent.h"
#include "mongo/db/storage/mmap_v1/extent_manager.h"
#include "mongo/db/storage/mmap_v1/record.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/mmap_v1/record_store_v1_simple_iterator.h"
#include "mongo/util/log.h"
#include "mongo/util/progress_meter.h"
#include "mongo/util/timer.h"
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

    SimpleRecordStoreV1::SimpleRecordStoreV1( OperationContext* txn,
                                              const StringData& ns,
                                              RecordStoreV1MetaData* details,
                                              ExtentManager* em,
                                              bool isSystemIndexes )
        : RecordStoreV1Base( ns, details, em, isSystemIndexes ) {

        invariant( !details->isCapped() );
        _normalCollection = NamespaceString::normal( ns );
        if ( _details->paddingFactor() == 0 ) {
            warning() << "implicit updgrade of paddingFactor of very old collection" << endl;
            WriteUnitOfWork wunit(txn);
            _details->setPaddingFactor(txn, 1.0);
            wunit.commit();
        }

    }

    SimpleRecordStoreV1::~SimpleRecordStoreV1() {
    }

    DiskLoc SimpleRecordStoreV1::_allocFromExistingExtents( OperationContext* txn,
                                                            int lenToAlloc ) {
        // align size up to a multiple of 4
        lenToAlloc = (lenToAlloc + (4-1)) & ~(4-1);

        freelistAllocs.increment();
        DiskLoc loc;
        {
            DiskLoc *prev = 0;
            DiskLoc *bestprev = 0;
            DiskLoc bestmatch;
            int bestmatchlen = INT_MAX; // sentinel meaning we haven't found a record big enough
            int b = bucket(lenToAlloc);
            DiskLoc cur = _details->deletedListEntry(b);
            
            int extra = 5; // look for a better fit, a little.
            int chain = 0;
            while ( 1 ) {
                { // defensive check
                    int fileNumber = cur.a();
                    int fileOffset = cur.getOfs();
                    if (fileNumber < -1 || fileNumber >= 100000 || fileOffset < 0) {
                        StringBuilder sb;
                        sb << "Deleted record list corrupted in collection " << _ns
                           << ", bucket " << b
                           << ", link number " << chain
                           << ", invalid link is " << cur.toString()
                           << ", throwing Fatal Assertion";
                        log() << sb.str() << endl;
                        fassertFailed(16469);
                    }
                }
                if ( cur.isNull() ) {
                    // move to next bucket.  if we were doing "extra", just break
                    if ( bestmatchlen < INT_MAX )
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
                if ( bestmatchlen < INT_MAX && --extra <= 0 )
                    break;
                if ( ++chain > 30 && b <= MaxBucket ) {
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
                *txn->recoveryUnit()->writing(bestprev) = bmr->nextDeleted();
            }
            else {
                // should be the front of a free-list
                int myBucket = bucket(bmr->lengthWithHeaders());
                invariant( _details->deletedListEntry(myBucket) == bestmatch );
                _details->setDeletedListEntry(txn, myBucket, bmr->nextDeleted());
            }
            *txn->recoveryUnit()->writing(&bmr->nextDeleted()) = DiskLoc().setInvalid(); // defensive.
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
        if ( left < 24 || left < (lenToAlloc / 8) ) {
            // you get the whole thing.
            return loc;
        }

        // don't quantize:
        //   - $ collections (indexes) as we already have those aligned the way we want SERVER-8425
        if ( _normalCollection ) {
            // we quantize here so that it only impacts newly sized records
            // this prevents oddities with older records and space re-use SERVER-8435
            lenToAlloc = std::min( r->lengthWithHeaders(),
                                   quantizeAllocationSpace( lenToAlloc ) );
            left = regionlen - lenToAlloc;

            if ( left < 24 ) {
                // you get the whole thing.
                return loc;
            }
        }

        /* split off some for further use. */
        txn->recoveryUnit()->writingInt(r->lengthWithHeaders()) = lenToAlloc;
        DiskLoc newDelLoc = loc;
        newDelLoc.inc(lenToAlloc);
        DeletedRecord* newDel = drec(newDelLoc);
        DeletedRecord* newDelW = txn->recoveryUnit()->writing(newDel);
        newDelW->extentOfs() = r->extentOfs();
        newDelW->lengthWithHeaders() = left;
        newDelW->nextDeleted().Null();

        addDeletedRec( txn, newDelLoc );
        return loc;
    }

    StatusWith<DiskLoc> SimpleRecordStoreV1::allocRecord( OperationContext* txn,
                                                          int lengthWithHeaders,
                                                          bool enforceQuota ) {
        DiskLoc loc = _allocFromExistingExtents( txn, lengthWithHeaders );
        if ( !loc.isNull() )
            return StatusWith<DiskLoc>( loc );

        LOG(1) << "allocating new extent";

        increaseStorageSize( txn,
                             _extentManager->followupSize( lengthWithHeaders,
                                                           _details->lastExtentSize(txn)),
                             enforceQuota );

        loc = _allocFromExistingExtents( txn, lengthWithHeaders );
        if ( !loc.isNull() ) {
            // got on first try
            return StatusWith<DiskLoc>( loc );
        }

        log() << "warning: alloc() failed after allocating new extent. "
              << "lengthWithHeaders: " << lengthWithHeaders << " last extent size:"
              << _details->lastExtentSize(txn) << "; trying again";

        for ( int z = 0; z < 10 && lengthWithHeaders > _details->lastExtentSize(txn); z++ ) {
            log() << "try #" << z << endl;

            increaseStorageSize( txn,
                                 _extentManager->followupSize( lengthWithHeaders,
                                                               _details->lastExtentSize(txn)),
                                 enforceQuota );

            loc = _allocFromExistingExtents( txn, lengthWithHeaders );
            if ( ! loc.isNull() )
                return StatusWith<DiskLoc>( loc );
        }

        return StatusWith<DiskLoc>( ErrorCodes::InternalError, "cannot allocate space" );
    }

    Status SimpleRecordStoreV1::truncate(OperationContext* txn) {
        return Status( ErrorCodes::InternalError,
                       "SimpleRecordStoreV1::truncate not implemented" );
    }

    void SimpleRecordStoreV1::addDeletedRec( OperationContext* txn, const DiskLoc& dloc ) {
        DeletedRecord* d = drec( dloc );

        DEBUGGING log() << "TEMP: add deleted rec " << dloc.toString() << ' ' << hex << d->extentOfs() << endl;

        int b = bucket(d->lengthWithHeaders());
        *txn->recoveryUnit()->writing(&d->nextDeleted()) = _details->deletedListEntry(b);
        _details->setDeletedListEntry(txn, b, dloc);
    }

    RecordIterator* SimpleRecordStoreV1::getIterator( OperationContext* txn,
                                                      const DiskLoc& start,
                                                      bool tailable,
                                                      const CollectionScanParams::Direction& dir) const {
        return new SimpleRecordStoreV1Iterator( txn, this, start, dir );
    }

    vector<RecordIterator*> SimpleRecordStoreV1::getManyIterators( OperationContext* txn ) const {
        OwnedPointerVector<RecordIterator> iterators;
        const Extent* ext;
        for (DiskLoc extLoc = details()->firstExtent(txn); !extLoc.isNull(); extLoc = ext->xnext) {
            ext = _getExtent(txn, extLoc);
            if (ext->firstRecord.isNull())
                continue;
            iterators.push_back(
                new RecordStoreV1Base::IntraExtentIterator(txn, ext->firstRecord, this));
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

    void SimpleRecordStoreV1::_compactExtent(OperationContext* txn,
                                             const DiskLoc extentLoc,
                                             int extentNumber,
                                             RecordStoreCompactAdaptor* adaptor,
                                             const CompactOptions* compactOptions,
                                             CompactStats* stats ) {

        log() << "compact begin extent #" << extentNumber
              << " for namespace " << _ns << " " << extentLoc;

        unsigned oldObjSize = 0; // we'll report what the old padding was
        unsigned oldObjSizeWithPadding = 0;

        Extent* const sourceExtent = _extentManager->getExtent( extentLoc );
        sourceExtent->assertOk();
        fassert( 17437, sourceExtent->validates(extentLoc) );

        {
            // The next/prev Record pointers within the Extent might not be in order so we first
            // page in the whole Extent sequentially.
            // TODO benchmark on slow storage to verify this is measurably faster.
            log() << "compact paging in len=" << sourceExtent->length/1000000.0 << "MB" << endl;
            Timer t;
            size_t length = sourceExtent->length;

            touch_pages( reinterpret_cast<const char*>(sourceExtent), length );
            int ms = t.millis();
            if( ms > 1000 )
                log() << "compact end paging in " << ms << "ms "
                      << sourceExtent->length/1000000.0/t.seconds() << "MB/sec" << endl;
        }

        {
            // Move each Record out of this extent and insert it in to the "new" extents.
            log() << "compact copying records" << endl;
            long long totalNetSize = 0;
            long long nrecords = 0;
            DiskLoc nextSourceLoc = sourceExtent->firstRecord;
            while (!nextSourceLoc.isNull()) {
                txn->checkForInterrupt();

                WriteUnitOfWork wunit(txn);
                Record* recOld = recordFor(nextSourceLoc);
                RecordData oldData = recOld->toRecordData();
                nextSourceLoc = getNextRecordInExtent(txn, nextSourceLoc);

                if ( compactOptions->validateDocuments && !adaptor->isDataValid( oldData ) ) {
                    // object is corrupt!
                    log() << "compact removing corrupt document!";
                    stats->corruptDocuments++;
                }
                else {
                    // How much data is in the record. Excludes padding and Record headers.
                    const unsigned rawDataSize = adaptor->dataSize( oldData );

                    nrecords++;
                    oldObjSize += rawDataSize;
                    oldObjSizeWithPadding += recOld->netLength();

                    // Allocation sizes include the headers and possibly some padding.
                    const unsigned minAllocationSize = rawDataSize + Record::HeaderSize;
                    unsigned allocationSize = minAllocationSize;
                    switch( compactOptions->paddingMode ) {
                    case CompactOptions::NONE: // no padding, unless using powerOf2Sizes
                        if ( _details->isUserFlagSet(Flag_UsePowerOf2Sizes) )
                            allocationSize = quantizePowerOf2AllocationSpace(minAllocationSize);
                        else
                            allocationSize = minAllocationSize;
                        break;

                    case CompactOptions::PRESERVE: // keep original padding
                        allocationSize = recOld->lengthWithHeaders();
                        break;

                    case CompactOptions::MANUAL: // user specified how much padding to use
                        allocationSize = compactOptions->computeRecordSize(minAllocationSize);
                        if (allocationSize < minAllocationSize
                                || allocationSize > BSONObjMaxUserSize / 2 ) {
                            allocationSize = minAllocationSize;
                        }
                        break;
                    }
                    invariant(allocationSize >= minAllocationSize);

                    // Copy the data to a new record. Because we orphaned the record freelist at the
                    // start of the compact, this insert will allocate a record in a new extent.
                    // See the comment in compact() for more details.
                    CompactDocWriter writer( recOld, rawDataSize, allocationSize );
                    StatusWith<DiskLoc> status = insertRecord( txn, &writer, false );
                    uassertStatusOK( status.getStatus() );
                    const Record* newRec = recordFor(status.getValue());
                    invariant(unsigned(newRec->netLength()) >= rawDataSize);
                    totalNetSize += newRec->netLength();

                    // Tells the caller that the record has been moved, so it can do things such as
                    // add it to indexes.
                    adaptor->inserted(newRec->toRecordData(), status.getValue());
                }

                // Remove the old record from the linked list of records withing the sourceExtent.
                // The old record is not added to the freelist as we will be freeing the whole
                // extent at the end.
                *txn->recoveryUnit()->writing(&sourceExtent->firstRecord) = nextSourceLoc;
                if (nextSourceLoc.isNull()) {
                    // Just moved the last record out of the extent. Mark extent as empty.
                    *txn->recoveryUnit()->writing(&sourceExtent->lastRecord) = DiskLoc();
                }
                else {
                    Record* newFirstRecord = recordFor(nextSourceLoc);
                    txn->recoveryUnit()->writingInt(newFirstRecord->prevOfs()) = DiskLoc::NullOfs;
                }

                // Adjust the stats to reflect the removal of the old record. The insert above
                // handled adjusting the stats for the new record.
                _details->incrementStats(txn, -(recOld->netLength()), -1);

                wunit.commit();
            }

            // The extent must now be empty.
            invariant(sourceExtent->firstRecord.isNull());
            invariant(sourceExtent->lastRecord.isNull());

            // We are still the first extent, but we must not be the only extent.
            invariant( _details->firstExtent(txn) == extentLoc );
            invariant( _details->lastExtent(txn) != extentLoc );

            // Remove the newly emptied sourceExtent from the extent linked list and return it to
            // the extent manager.
            WriteUnitOfWork wunit(txn);
            const DiskLoc newFirst = sourceExtent->xnext;
            _details->setFirstExtent( txn, newFirst );
            *txn->recoveryUnit()->writing(&_extentManager->getExtent( newFirst )->xprev) = DiskLoc();
            _extentManager->freeExtent( txn, extentLoc );
            wunit.commit();

            {
                const double oldPadding = oldObjSize ? double(oldObjSizeWithPadding) / oldObjSize
                                                     : 1.0; // defining 0/0 as 1 for this.

                log() << "compact finished extent #" << extentNumber << " containing " << nrecords
                      << " documents (" << totalNetSize / (1024*1024.0) << "MB)"
                      << " oldPadding: " << oldPadding;
            }
        }

    }

    Status SimpleRecordStoreV1::compact( OperationContext* txn,
                                         RecordStoreCompactAdaptor* adaptor,
                                         const CompactOptions* options,
                                         CompactStats* stats ) {

        std::vector<DiskLoc> extents;
        for( DiskLoc extLocation = _details->firstExtent(txn);
             !extLocation.isNull();
             extLocation = _extentManager->getExtent( extLocation )->xnext ) {
            extents.push_back( extLocation );
        }
        log() << "compact " << extents.size() << " extents";

        {
            WriteUnitOfWork wunit(txn);
            // Orphaning the deleted lists ensures that all inserts go to new extents rather than
            // the ones that existed before starting the compact. If we abort the operation before
            // completion, any free space in the old extents will be leaked and never reused unless
            // the collection is compacted again or dropped. This is considered an acceptable
            // failure mode as no data will be lost.
            log() << "compact orphan deleted lists" << endl;
            _details->orphanDeletedList(txn);

            // Start over from scratch with our extent sizing and growth
            _details->setLastExtentSize( txn, 0 );

            // create a new extent so new records go there
            increaseStorageSize( txn, _details->lastExtentSize(txn), true );
            wunit.commit();
        }

        ProgressMeterHolder pm(*txn->setMessage("compact extent",
                                                "Extent Compacting Progress",
                                                extents.size()));

        // Go through all old extents and move each record to a new set of extents.
        int extentNumber = 0;
        for( std::vector<DiskLoc>::iterator it = extents.begin(); it != extents.end(); it++ ) {
            txn->checkForInterrupt();
            invariant(_details->firstExtent(txn) == *it);
            // empties and removes the first extent
            _compactExtent(txn, *it, extentNumber++, adaptor, options, stats );
            invariant(_details->firstExtent(txn) != *it);
            pm.hit();
        }

        invariant( _extentManager->getExtent( _details->firstExtent(txn) )->xprev.isNull() );
        invariant( _extentManager->getExtent( _details->lastExtent(txn) )->xnext.isNull() );

        // indexes will do their own progress meter
        pm.finished();

        return Status::OK();
    }

}
