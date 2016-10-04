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
#include "mongo/db/client.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/curop.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/mmap_v1/extent.h"
#include "mongo/db/storage/mmap_v1/extent_manager.h"
#include "mongo/db/storage/mmap_v1/record.h"
#include "mongo/db/storage/mmap_v1/record_store_v1_simple_iterator.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/progress_meter.h"
#include "mongo/util/timer.h"
#include "mongo/util/touch_pages.h"

namespace mongo {

using std::endl;
using std::vector;

static Counter64 freelistAllocs;
static Counter64 freelistBucketExhausted;
static Counter64 freelistIterations;

// TODO figure out what to do about these.
static ServerStatusMetricField<Counter64> dFreelist1("storage.freelist.search.requests",
                                                     &freelistAllocs);

static ServerStatusMetricField<Counter64> dFreelist2("storage.freelist.search.bucketExhausted",
                                                     &freelistBucketExhausted);

static ServerStatusMetricField<Counter64> dFreelist3("storage.freelist.search.scanned",
                                                     &freelistIterations);

SimpleRecordStoreV1::SimpleRecordStoreV1(OperationContext* txn,
                                         StringData ns,
                                         RecordStoreV1MetaData* details,
                                         ExtentManager* em,
                                         bool isSystemIndexes)
    : RecordStoreV1Base(ns, details, em, isSystemIndexes) {
    invariant(!details->isCapped());
    _normalCollection = NamespaceString::normal(ns);
}

SimpleRecordStoreV1::~SimpleRecordStoreV1() {}

DiskLoc SimpleRecordStoreV1::_allocFromExistingExtents(OperationContext* txn, int lenToAllocRaw) {
    // Slowly drain the deletedListLegacyGrabBag by popping one record off and putting it in the
    // correct deleted list each time we try to allocate a new record. This ensures we won't
    // orphan any data when upgrading from old versions, without needing a long upgrade phase.
    // This is done before we try to allocate the new record so we can take advantage of the new
    // space immediately.
    {
        const DiskLoc head = _details->deletedListLegacyGrabBag();
        if (!head.isNull()) {
            _details->setDeletedListLegacyGrabBag(txn, drec(head)->nextDeleted());
            addDeletedRec(txn, head);
        }
    }

    // align size up to a multiple of 4
    const int lenToAlloc = (lenToAllocRaw + (4 - 1)) & ~(4 - 1);

    freelistAllocs.increment();
    DiskLoc loc;
    DeletedRecord* dr = NULL;
    {
        int myBucket;
        for (myBucket = bucket(lenToAlloc); myBucket < Buckets; myBucket++) {
            // Only look at the first entry in each bucket. This works because we are either
            // quantizing or allocating fixed-size blocks.
            const DiskLoc head = _details->deletedListEntry(myBucket);
            if (head.isNull())
                continue;
            DeletedRecord* const candidate = drec(head);
            if (candidate->lengthWithHeaders() >= lenToAlloc) {
                loc = head;
                dr = candidate;
                break;
            }
        }

        if (!dr)
            return DiskLoc();  // no space

        // Unlink ourself from the deleted list
        _details->setDeletedListEntry(txn, myBucket, dr->nextDeleted());
        *txn->recoveryUnit()->writing(&dr->nextDeleted()) = DiskLoc().setInvalid();  // defensive
    }

    invariant(dr->extentOfs() < loc.getOfs());

    // Split the deleted record if it has at least as much left over space as our smallest
    // allocation size. Otherwise, just take the whole DeletedRecord.
    const int remainingLength = dr->lengthWithHeaders() - lenToAlloc;
    if (remainingLength >= bucketSizes[0]) {
        txn->recoveryUnit()->writingInt(dr->lengthWithHeaders()) = lenToAlloc;
        const DiskLoc newDelLoc = DiskLoc(loc.a(), loc.getOfs() + lenToAlloc);
        DeletedRecord* newDel = txn->recoveryUnit()->writing(drec(newDelLoc));
        newDel->extentOfs() = dr->extentOfs();
        newDel->lengthWithHeaders() = remainingLength;
        newDel->nextDeleted().Null();

        addDeletedRec(txn, newDelLoc);
    }

    return loc;
}

StatusWith<DiskLoc> SimpleRecordStoreV1::allocRecord(OperationContext* txn,
                                                     int lengthWithHeaders,
                                                     bool enforceQuota) {
    if (lengthWithHeaders > MaxAllowedAllocation) {
        return StatusWith<DiskLoc>(
            ErrorCodes::InvalidLength,
            str::stream() << "Attempting to allocate a record larger than maximum size: "
                          << lengthWithHeaders
                          << " > 16.5MB");
    }

    DiskLoc loc = _allocFromExistingExtents(txn, lengthWithHeaders);
    if (!loc.isNull())
        return StatusWith<DiskLoc>(loc);

    LOG(1) << "allocating new extent";

    increaseStorageSize(
        txn,
        _extentManager->followupSize(lengthWithHeaders, _details->lastExtentSize(txn)),
        enforceQuota);

    loc = _allocFromExistingExtents(txn, lengthWithHeaders);
    if (!loc.isNull()) {
        // got on first try
        return StatusWith<DiskLoc>(loc);
    }

    log() << "warning: alloc() failed after allocating new extent. "
          << "lengthWithHeaders: " << lengthWithHeaders
          << " last extent size:" << _details->lastExtentSize(txn) << "; trying again";

    for (int z = 0; z < 10 && lengthWithHeaders > _details->lastExtentSize(txn); z++) {
        log() << "try #" << z << endl;

        increaseStorageSize(
            txn,
            _extentManager->followupSize(lengthWithHeaders, _details->lastExtentSize(txn)),
            enforceQuota);

        loc = _allocFromExistingExtents(txn, lengthWithHeaders);
        if (!loc.isNull())
            return StatusWith<DiskLoc>(loc);
    }

    return StatusWith<DiskLoc>(ErrorCodes::InternalError, "cannot allocate space");
}

Status SimpleRecordStoreV1::truncate(OperationContext* txn) {
    const DiskLoc firstExtLoc = _details->firstExtent(txn);
    if (firstExtLoc.isNull() || !firstExtLoc.isValid()) {
        // Already empty
        return Status::OK();
    }

    // Free all extents except the first.
    Extent* firstExt = _extentManager->getExtent(firstExtLoc);
    if (!firstExt->xnext.isNull()) {
        const DiskLoc extNextLoc = firstExt->xnext;
        const DiskLoc oldLastExtLoc = _details->lastExtent(txn);
        Extent* const nextExt = _extentManager->getExtent(extNextLoc);

        // Unlink other extents;
        *txn->recoveryUnit()->writing(&nextExt->xprev) = DiskLoc();
        *txn->recoveryUnit()->writing(&firstExt->xnext) = DiskLoc();
        _details->setLastExtent(txn, firstExtLoc);
        _details->setLastExtentSize(txn, firstExt->length);

        _extentManager->freeExtents(txn, extNextLoc, oldLastExtLoc);
    }

    // Make the first (now only) extent a single large deleted record.
    *txn->recoveryUnit()->writing(&firstExt->firstRecord) = DiskLoc();
    *txn->recoveryUnit()->writing(&firstExt->lastRecord) = DiskLoc();
    _details->orphanDeletedList(txn);
    addDeletedRec(txn, _findFirstSpot(txn, firstExtLoc, firstExt));

    // Make stats reflect that there are now no documents in this record store.
    _details->setStats(txn, 0, 0);

    return Status::OK();
}

void SimpleRecordStoreV1::addDeletedRec(OperationContext* txn, const DiskLoc& dloc) {
    DeletedRecord* d = drec(dloc);

    int b = bucket(d->lengthWithHeaders());
    *txn->recoveryUnit()->writing(&d->nextDeleted()) = _details->deletedListEntry(b);
    _details->setDeletedListEntry(txn, b, dloc);
}

std::unique_ptr<SeekableRecordCursor> SimpleRecordStoreV1::getCursor(OperationContext* txn,
                                                                     bool forward) const {
    return stdx::make_unique<SimpleRecordStoreV1Iterator>(txn, this, forward);
}

vector<std::unique_ptr<RecordCursor>> SimpleRecordStoreV1::getManyCursors(
    OperationContext* txn) const {
    vector<std::unique_ptr<RecordCursor>> cursors;
    const Extent* ext;
    for (DiskLoc extLoc = details()->firstExtent(txn); !extLoc.isNull(); extLoc = ext->xnext) {
        ext = _getExtent(txn, extLoc);
        if (ext->firstRecord.isNull())
            continue;
        cursors.push_back(
            stdx::make_unique<RecordStoreV1Base::IntraExtentIterator>(txn, ext->firstRecord, this));
    }

    return cursors;
}

class CompactDocWriter final : public DocWriter {
public:
    /**
     * param allocationSize - allocation size WITH header
     */
    CompactDocWriter(const MmapV1RecordHeader* rec, unsigned dataSize, size_t allocationSize)
        : _rec(rec), _dataSize(dataSize), _allocationSize(allocationSize) {}

    virtual ~CompactDocWriter() {}

    virtual void writeDocument(char* buf) const {
        memcpy(buf, _rec->data(), _dataSize);
    }

    virtual size_t documentSize() const {
        return _allocationSize - MmapV1RecordHeader::HeaderSize;
    }

    virtual bool addPadding() const {
        return false;
    }

private:
    const MmapV1RecordHeader* _rec;
    size_t _dataSize;
    size_t _allocationSize;
};

void SimpleRecordStoreV1::_compactExtent(OperationContext* txn,
                                         const DiskLoc extentLoc,
                                         int extentNumber,
                                         RecordStoreCompactAdaptor* adaptor,
                                         const CompactOptions* compactOptions,
                                         CompactStats* stats) {
    log() << "compact begin extent #" << extentNumber << " for namespace " << _ns << " "
          << extentLoc;

    unsigned oldObjSize = 0;  // we'll report what the old padding was
    unsigned oldObjSizeWithPadding = 0;

    Extent* const sourceExtent = _extentManager->getExtent(extentLoc);
    sourceExtent->assertOk();
    fassert(17437, sourceExtent->validates(extentLoc));

    {
        // The next/prev MmapV1RecordHeader pointers within the Extent might not be in order so we
        // first page in the whole Extent sequentially.
        // TODO benchmark on slow storage to verify this is measurably faster.
        log() << "compact paging in len=" << sourceExtent->length / 1000000.0 << "MB" << endl;
        Timer t;
        size_t length = sourceExtent->length;

        touch_pages(reinterpret_cast<const char*>(sourceExtent), length);
        int ms = t.millis();
        if (ms > 1000)
            log() << "compact end paging in " << ms << "ms "
                  << sourceExtent->length / 1000000.0 / t.seconds() << "MB/sec" << endl;
    }

    {
        // Move each MmapV1RecordHeader out of this extent and insert it in to the "new" extents.
        log() << "compact copying records" << endl;
        long long totalNetSize = 0;
        long long nrecords = 0;
        DiskLoc nextSourceLoc = sourceExtent->firstRecord;
        while (!nextSourceLoc.isNull()) {
            txn->checkForInterrupt();

            WriteUnitOfWork wunit(txn);
            MmapV1RecordHeader* recOld = recordFor(nextSourceLoc);
            RecordData oldData = recOld->toRecordData();
            nextSourceLoc = getNextRecordInExtent(txn, nextSourceLoc);

            if (compactOptions->validateDocuments && !adaptor->isDataValid(oldData)) {
                // object is corrupt!
                log() << "compact removing corrupt document!";
                stats->corruptDocuments++;
            } else {
                // How much data is in the record. Excludes padding and MmapV1RecordHeader headers.
                const unsigned rawDataSize = adaptor->dataSize(oldData);

                nrecords++;
                oldObjSize += rawDataSize;
                oldObjSizeWithPadding += recOld->netLength();

                // Allocation sizes include the headers and possibly some padding.
                const unsigned minAllocationSize = rawDataSize + MmapV1RecordHeader::HeaderSize;
                unsigned allocationSize = minAllocationSize;
                switch (compactOptions->paddingMode) {
                    case CompactOptions::NONE:  // default padding
                        if (shouldPadInserts()) {
                            allocationSize = quantizeAllocationSpace(minAllocationSize);
                        }
                        break;

                    case CompactOptions::PRESERVE:  // keep original padding
                        allocationSize = recOld->lengthWithHeaders();
                        break;

                    case CompactOptions::MANUAL:  // user specified how much padding to use
                        allocationSize = compactOptions->computeRecordSize(minAllocationSize);
                        if (allocationSize < minAllocationSize ||
                            allocationSize > BSONObjMaxUserSize / 2) {
                            allocationSize = minAllocationSize;
                        }
                        break;
                }
                invariant(allocationSize >= minAllocationSize);

                // Copy the data to a new record. Because we orphaned the record freelist at the
                // start of the compact, this insert will allocate a record in a new extent.
                // See the comment in compact() for more details.
                CompactDocWriter writer(recOld, rawDataSize, allocationSize);
                StatusWith<RecordId> status = insertRecordWithDocWriter(txn, &writer);
                uassertStatusOK(status.getStatus());
                const MmapV1RecordHeader* newRec =
                    recordFor(DiskLoc::fromRecordId(status.getValue()));
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
            } else {
                MmapV1RecordHeader* newFirstRecord = recordFor(nextSourceLoc);
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
        invariant(_details->firstExtent(txn) == extentLoc);
        invariant(_details->lastExtent(txn) != extentLoc);

        // Remove the newly emptied sourceExtent from the extent linked list and return it to
        // the extent manager.
        WriteUnitOfWork wunit(txn);
        const DiskLoc newFirst = sourceExtent->xnext;
        _details->setFirstExtent(txn, newFirst);
        *txn->recoveryUnit()->writing(&_extentManager->getExtent(newFirst)->xprev) = DiskLoc();
        _extentManager->freeExtent(txn, extentLoc);
        wunit.commit();

        {
            const double oldPadding = oldObjSize ? double(oldObjSizeWithPadding) / oldObjSize
                                                 : 1.0;  // defining 0/0 as 1 for this.

            log() << "compact finished extent #" << extentNumber << " containing " << nrecords
                  << " documents (" << totalNetSize / (1024 * 1024.0) << "MB)"
                  << " oldPadding: " << oldPadding;
        }
    }
}

Status SimpleRecordStoreV1::compact(OperationContext* txn,
                                    RecordStoreCompactAdaptor* adaptor,
                                    const CompactOptions* options,
                                    CompactStats* stats) {
    std::vector<DiskLoc> extents;
    for (DiskLoc extLocation = _details->firstExtent(txn); !extLocation.isNull();
         extLocation = _extentManager->getExtent(extLocation)->xnext) {
        extents.push_back(extLocation);
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
        _details->setLastExtentSize(txn, 0);

        // create a new extent so new records go there
        increaseStorageSize(txn, _details->lastExtentSize(txn), true);
        wunit.commit();
    }

    stdx::unique_lock<Client> lk(*txn->getClient());
    ProgressMeterHolder pm(
        *txn->setMessage_inlock("compact extent", "Extent Compacting Progress", extents.size()));
    lk.unlock();

    // Go through all old extents and move each record to a new set of extents.
    int extentNumber = 0;
    for (std::vector<DiskLoc>::iterator it = extents.begin(); it != extents.end(); it++) {
        txn->checkForInterrupt();
        invariant(_details->firstExtent(txn) == *it);
        // empties and removes the first extent
        _compactExtent(txn, *it, extentNumber++, adaptor, options, stats);
        invariant(_details->firstExtent(txn) != *it);
        pm.hit();
    }

    invariant(_extentManager->getExtent(_details->firstExtent(txn))->xprev.isNull());
    invariant(_extentManager->getExtent(_details->lastExtent(txn))->xnext.isNull());

    // indexes will do their own progress meter
    pm.finished();

    return Status::OK();
}
}
