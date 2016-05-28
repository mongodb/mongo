// record_store_v1_base.cpp

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

#include "mongo/db/storage/mmap_v1/record_store_v1_base.h"


#include "mongo/db/catalog/collection.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/mmap_v1/extent.h"
#include "mongo/db/storage/mmap_v1/extent_manager.h"
#include "mongo/db/storage/mmap_v1/record.h"
#include "mongo/db/storage/mmap_v1/record_store_v1_repair_iterator.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"
#include "mongo/util/progress_meter.h"
#include "mongo/util/timer.h"
#include "mongo/util/touch_pages.h"

namespace mongo {

using std::unique_ptr;
using std::set;
using std::string;

/* Deleted list buckets are used to quickly locate free space based on size.  Each bucket
   contains records up to that size (meaning a record with a size exactly equal to
   bucketSizes[n] would go into bucket n+1).
*/
const int RecordStoreV1Base::bucketSizes[] = {
    0x20,
    0x40,
    0x80,
    0x100,  // 32,   64,   128,  256
    0x200,
    0x400,
    0x800,
    0x1000,  // 512,  1K,   2K,   4K
    0x2000,
    0x4000,
    0x8000,
    0x10000,  // 8K,   16K,  32K,  64K
    0x20000,
    0x40000,
    0x80000,
    0x100000,  // 128K, 256K, 512K, 1M
    0x200000,
    0x400000,
    0x600000,
    0x800000,  // 2M,   4M,   6M,   8M
    0xA00000,
    0xC00000,
    0xE00000,                  // 10M,  12M,  14M,
    MaxAllowedAllocation,      // 16.5M
    MaxAllowedAllocation + 1,  // Only MaxAllowedAllocation sized records go here.
    INT_MAX,                   // "oversized" bucket for unused parts of extents.
};

// If this fails, it means that bucketSizes doesn't have the correct number of entries.
static_assert(sizeof(RecordStoreV1Base::bucketSizes) / sizeof(RecordStoreV1Base::bucketSizes[0]) ==
                  RecordStoreV1Base::Buckets,
              "sizeof(RecordStoreV1Base::bucketSizes) / sizeof(RecordStoreV1Base::bucketSizes[0]) "
              "== RecordStoreV1Base::Buckets");

SavedCursorRegistry::~SavedCursorRegistry() {
    for (SavedCursorSet::iterator it = _cursors.begin(); it != _cursors.end(); it++) {
        (*it)->_registry = NULL;  // prevent SavedCursor destructor from accessing this
    }
}

void SavedCursorRegistry::registerCursor(SavedCursor* cursor) {
    invariant(!cursor->_registry);
    cursor->_registry = this;
    scoped_spinlock lock(_mutex);
    _cursors.insert(cursor);
}

bool SavedCursorRegistry::unregisterCursor(SavedCursor* cursor) {
    if (!cursor->_registry) {
        return false;
    }
    invariant(cursor->_registry == this);
    cursor->_registry = NULL;
    scoped_spinlock lock(_mutex);
    invariant(_cursors.erase(cursor));
    return true;
}

void SavedCursorRegistry::invalidateCursorsForBucket(DiskLoc bucket) {
    // While this is not strictly necessary as an exclusive collection lock will be held,
    // it's cleaner to just make the SavedCursorRegistry thread-safe. Spinlock is OK here.
    scoped_spinlock lock(_mutex);
    for (SavedCursorSet::iterator it = _cursors.begin(); it != _cursors.end();) {
        if ((*it)->bucket == bucket) {
            (*it)->_registry = NULL;  // prevent ~SavedCursor from trying to unregister
            _cursors.erase(it++);
        } else {
            it++;
        }
    }
}

RecordStoreV1Base::RecordStoreV1Base(StringData ns,
                                     RecordStoreV1MetaData* details,
                                     ExtentManager* em,
                                     bool isSystemIndexes)
    : RecordStore(ns), _details(details), _extentManager(em), _isSystemIndexes(isSystemIndexes) {}

RecordStoreV1Base::~RecordStoreV1Base() {}


int64_t RecordStoreV1Base::storageSize(OperationContext* txn,
                                       BSONObjBuilder* extraInfo,
                                       int level) const {
    BSONArrayBuilder extentInfo;

    int64_t total = 0;
    int n = 0;

    DiskLoc cur = _details->firstExtent(txn);

    while (!cur.isNull()) {
        Extent* e = _extentManager->getExtent(cur);

        total += e->length;
        n++;

        if (extraInfo && level > 0) {
            extentInfo.append(BSON("len" << e->length << "loc: " << e->myLoc.toBSONObj()));
        }
        cur = e->xnext;
    }

    if (extraInfo) {
        extraInfo->append("numExtents", n);
        if (level > 0)
            extraInfo->append("extents", extentInfo.arr());
    }

    return total;
}

RecordData RecordStoreV1Base::dataFor(OperationContext* txn, const RecordId& loc) const {
    return recordFor(DiskLoc::fromRecordId(loc))->toRecordData();
}

bool RecordStoreV1Base::findRecord(OperationContext* txn,
                                   const RecordId& loc,
                                   RecordData* rd) const {
    // this is a bit odd, as the semantics of using the storage engine imply it _has_ to be.
    // And in fact we can't actually check.
    // So we assume the best.
    MmapV1RecordHeader* rec = recordFor(DiskLoc::fromRecordId(loc));
    if (!rec) {
        return false;
    }
    *rd = rec->toRecordData();
    return true;
}

MmapV1RecordHeader* RecordStoreV1Base::recordFor(const DiskLoc& loc) const {
    return _extentManager->recordForV1(loc);
}

const DeletedRecord* RecordStoreV1Base::deletedRecordFor(const DiskLoc& loc) const {
    invariant(loc.a() != -1);
    return reinterpret_cast<const DeletedRecord*>(recordFor(loc));
}

DeletedRecord* RecordStoreV1Base::drec(const DiskLoc& loc) const {
    invariant(loc.a() != -1);
    return reinterpret_cast<DeletedRecord*>(recordFor(loc));
}

Extent* RecordStoreV1Base::_getExtent(OperationContext* txn, const DiskLoc& loc) const {
    return _extentManager->getExtent(loc);
}

DiskLoc RecordStoreV1Base::_getExtentLocForRecord(OperationContext* txn, const DiskLoc& loc) const {
    return _extentManager->extentLocForV1(loc);
}


DiskLoc RecordStoreV1Base::getNextRecord(OperationContext* txn, const DiskLoc& loc) const {
    DiskLoc next = getNextRecordInExtent(txn, loc);
    if (!next.isNull()) {
        return next;
    }

    // now traverse extents

    Extent* e = _getExtent(txn, _getExtentLocForRecord(txn, loc));
    while (1) {
        if (e->xnext.isNull())
            return DiskLoc();  // end of collection
        e = _getExtent(txn, e->xnext);
        if (!e->firstRecord.isNull())
            break;
        // entire extent could be empty, keep looking
    }
    return e->firstRecord;
}

DiskLoc RecordStoreV1Base::getPrevRecord(OperationContext* txn, const DiskLoc& loc) const {
    DiskLoc prev = getPrevRecordInExtent(txn, loc);
    if (!prev.isNull()) {
        return prev;
    }

    // now traverse extents

    Extent* e = _getExtent(txn, _getExtentLocForRecord(txn, loc));
    while (1) {
        if (e->xprev.isNull())
            return DiskLoc();  // end of collection
        e = _getExtent(txn, e->xprev);
        if (!e->firstRecord.isNull())
            break;
        // entire extent could be empty, keep looking
    }
    return e->lastRecord;
}

DiskLoc RecordStoreV1Base::_findFirstSpot(OperationContext* txn,
                                          const DiskLoc& extDiskLoc,
                                          Extent* e) {
    DiskLoc emptyLoc = extDiskLoc;
    emptyLoc.inc(Extent::HeaderSize());
    int delRecLength = e->length - Extent::HeaderSize();
    if (delRecLength >= 32 * 1024 && NamespaceString::virtualized(_ns) && !isCapped()) {
        // probably an index. so skip forward to keep its records page aligned
        int& ofs = emptyLoc.GETOFS();
        int newOfs = (ofs + 0xfff) & ~0xfff;
        delRecLength -= (newOfs - ofs);
        dassert(delRecLength > 0);
        ofs = newOfs;
    }

    DeletedRecord* empty = txn->recoveryUnit()->writing(drec(emptyLoc));
    empty->lengthWithHeaders() = delRecLength;
    empty->extentOfs() = e->myLoc.getOfs();
    empty->nextDeleted().Null();
    return emptyLoc;
}

DiskLoc RecordStoreV1Base::getNextRecordInExtent(OperationContext* txn, const DiskLoc& loc) const {
    int nextOffset = recordFor(loc)->nextOfs();

    if (nextOffset == DiskLoc::NullOfs)
        return DiskLoc();

    fassert(17441, abs(nextOffset) >= 8);  // defensive
    DiskLoc result(loc.a(), nextOffset);
    return result;
}

DiskLoc RecordStoreV1Base::getPrevRecordInExtent(OperationContext* txn, const DiskLoc& loc) const {
    int prevOffset = recordFor(loc)->prevOfs();

    if (prevOffset == DiskLoc::NullOfs)
        return DiskLoc();

    fassert(17442, abs(prevOffset) >= 8);  // defensive
    DiskLoc result(loc.a(), prevOffset);
    return result;
}

Status RecordStoreV1Base::insertRecordsWithDocWriter(OperationContext* txn,
                                                     const DocWriter* const* docs,
                                                     size_t nDocs,
                                                     RecordId* idsOut) {
    for (size_t i = 0; i < nDocs; i++) {
        int docSize = docs[i]->documentSize();
        if (docSize < 4) {
            return Status(ErrorCodes::InvalidLength, "record has to be >= 4 bytes");
        }
        const int lenWHdr = docSize + MmapV1RecordHeader::HeaderSize;
        if (lenWHdr > MaxAllowedAllocation) {
            return Status(ErrorCodes::InvalidLength, "record has to be <= 16.5MB");
        }
        const int lenToAlloc = (docs[i]->addPadding() && shouldPadInserts())
            ? quantizeAllocationSpace(lenWHdr)
            : lenWHdr;

        StatusWith<DiskLoc> loc = allocRecord(txn, lenToAlloc, /*enforceQuota=*/false);
        if (!loc.isOK())
            return loc.getStatus();

        MmapV1RecordHeader* r = recordFor(loc.getValue());
        fassert(17319, r->lengthWithHeaders() >= lenWHdr);

        r = reinterpret_cast<MmapV1RecordHeader*>(txn->recoveryUnit()->writingPtr(r, lenWHdr));
        docs[i]->writeDocument(r->data());

        _addRecordToRecListInExtent(txn, r, loc.getValue());

        _details->incrementStats(txn, r->netLength(), 1);

        if (idsOut)
            idsOut[i] = loc.getValue().toRecordId();
    }


    return Status::OK();
}


StatusWith<RecordId> RecordStoreV1Base::insertRecord(OperationContext* txn,
                                                     const char* data,
                                                     int len,
                                                     bool enforceQuota) {
    if (len < 4) {
        return StatusWith<RecordId>(ErrorCodes::InvalidLength, "record has to be >= 4 bytes");
    }

    if (len + MmapV1RecordHeader::HeaderSize > MaxAllowedAllocation) {
        return StatusWith<RecordId>(ErrorCodes::InvalidLength, "record has to be <= 16.5MB");
    }

    return _insertRecord(txn, data, len, enforceQuota);
}

StatusWith<RecordId> RecordStoreV1Base::_insertRecord(OperationContext* txn,
                                                      const char* data,
                                                      int len,
                                                      bool enforceQuota) {
    const int lenWHdr = len + MmapV1RecordHeader::HeaderSize;
    const int lenToAlloc = shouldPadInserts() ? quantizeAllocationSpace(lenWHdr) : lenWHdr;
    fassert(17208, lenToAlloc >= lenWHdr);

    StatusWith<DiskLoc> loc = allocRecord(txn, lenToAlloc, enforceQuota);
    if (!loc.isOK())
        return StatusWith<RecordId>(loc.getStatus());

    MmapV1RecordHeader* r = recordFor(loc.getValue());
    fassert(17210, r->lengthWithHeaders() >= lenWHdr);

    // copy the data
    r = reinterpret_cast<MmapV1RecordHeader*>(txn->recoveryUnit()->writingPtr(r, lenWHdr));
    memcpy(r->data(), data, len);

    _addRecordToRecListInExtent(txn, r, loc.getValue());

    _details->incrementStats(txn, r->netLength(), 1);

    return StatusWith<RecordId>(loc.getValue().toRecordId());
}

Status RecordStoreV1Base::updateRecord(OperationContext* txn,
                                       const RecordId& oldLocation,
                                       const char* data,
                                       int dataSize,
                                       bool enforceQuota,
                                       UpdateNotifier* notifier) {
    MmapV1RecordHeader* oldRecord = recordFor(DiskLoc::fromRecordId(oldLocation));
    if (oldRecord->netLength() >= dataSize) {
        // Make sure to notify other queries before we do an in-place update.
        if (notifier) {
            Status callbackStatus = notifier->recordStoreGoingToUpdateInPlace(txn, oldLocation);
            if (!callbackStatus.isOK())
                return callbackStatus;
        }

        // we fit
        memcpy(txn->recoveryUnit()->writingPtr(oldRecord->data(), dataSize), data, dataSize);
        return Status::OK();
    }

    // We enforce the restriction of unchanging capped doc sizes above the storage layer.
    invariant(!isCapped());

    return {ErrorCodes::NeedsDocumentMove, "Update requires document move"};
}

bool RecordStoreV1Base::updateWithDamagesSupported() const {
    return true;
}

StatusWith<RecordData> RecordStoreV1Base::updateWithDamages(
    OperationContext* txn,
    const RecordId& loc,
    const RecordData& oldRec,
    const char* damageSource,
    const mutablebson::DamageVector& damages) {
    MmapV1RecordHeader* rec = recordFor(DiskLoc::fromRecordId(loc));
    char* root = rec->data();

    // All updates were in place. Apply them via durability and writing pointer.
    mutablebson::DamageVector::const_iterator where = damages.begin();
    const mutablebson::DamageVector::const_iterator end = damages.end();
    for (; where != end; ++where) {
        const char* sourcePtr = damageSource + where->sourceOffset;
        void* targetPtr = txn->recoveryUnit()->writingPtr(root + where->targetOffset, where->size);
        std::memcpy(targetPtr, sourcePtr, where->size);
    }

    return rec->toRecordData();
}

void RecordStoreV1Base::deleteRecord(OperationContext* txn, const RecordId& rid) {
    const DiskLoc dl = DiskLoc::fromRecordId(rid);

    MmapV1RecordHeader* todelete = recordFor(dl);
    invariant(todelete->netLength() >= 4);  // this is required for defensive code

    /* remove ourself from the record next/prev chain */
    {
        if (todelete->prevOfs() != DiskLoc::NullOfs) {
            DiskLoc prev = getPrevRecordInExtent(txn, dl);
            MmapV1RecordHeader* prevRecord = recordFor(prev);
            txn->recoveryUnit()->writingInt(prevRecord->nextOfs()) = todelete->nextOfs();
        }

        if (todelete->nextOfs() != DiskLoc::NullOfs) {
            DiskLoc next = getNextRecord(txn, dl);
            MmapV1RecordHeader* nextRecord = recordFor(next);
            txn->recoveryUnit()->writingInt(nextRecord->prevOfs()) = todelete->prevOfs();
        }
    }

    /* remove ourself from extent pointers */
    {
        DiskLoc extentLoc = todelete->myExtentLoc(dl);
        Extent* e = _getExtent(txn, extentLoc);
        if (e->firstRecord == dl) {
            txn->recoveryUnit()->writing(&e->firstRecord);
            if (todelete->nextOfs() == DiskLoc::NullOfs)
                e->firstRecord.Null();
            else
                e->firstRecord.set(dl.a(), todelete->nextOfs());
        }
        if (e->lastRecord == dl) {
            txn->recoveryUnit()->writing(&e->lastRecord);
            if (todelete->prevOfs() == DiskLoc::NullOfs)
                e->lastRecord.Null();
            else
                e->lastRecord.set(dl.a(), todelete->prevOfs());
        }
    }

    /* add to the free list */
    {
        _details->incrementStats(txn, -1 * todelete->netLength(), -1);

        if (_isSystemIndexes) {
            /* temp: if in system.indexes, don't reuse, and zero out: we want to be
               careful until validated more, as IndexDetails has pointers
               to this disk location.  so an incorrectly done remove would cause
               a lot of problems.
            */
            memset(txn->recoveryUnit()->writingPtr(todelete, todelete->lengthWithHeaders()),
                   0,
                   todelete->lengthWithHeaders());
        } else {
            // this is defensive so we can detect if we are still using a location
            // that was deleted
            memset(txn->recoveryUnit()->writingPtr(todelete->data(), 4), 0xee, 4);
            addDeletedRec(txn, dl);
        }
    }
}

std::unique_ptr<RecordCursor> RecordStoreV1Base::getCursorForRepair(OperationContext* txn) const {
    return stdx::make_unique<RecordStoreV1RepairCursor>(txn, this);
}

void RecordStoreV1Base::_addRecordToRecListInExtent(OperationContext* txn,
                                                    MmapV1RecordHeader* r,
                                                    DiskLoc loc) {
    dassert(recordFor(loc) == r);
    DiskLoc extentLoc = _getExtentLocForRecord(txn, loc);
    Extent* e = _getExtent(txn, extentLoc);
    if (e->lastRecord.isNull()) {
        *txn->recoveryUnit()->writing(&e->firstRecord) = loc;
        *txn->recoveryUnit()->writing(&e->lastRecord) = loc;
        r->prevOfs() = r->nextOfs() = DiskLoc::NullOfs;
    } else {
        MmapV1RecordHeader* oldlast = recordFor(e->lastRecord);
        r->prevOfs() = e->lastRecord.getOfs();
        r->nextOfs() = DiskLoc::NullOfs;
        txn->recoveryUnit()->writingInt(oldlast->nextOfs()) = loc.getOfs();
        *txn->recoveryUnit()->writing(&e->lastRecord) = loc;
    }
}

void RecordStoreV1Base::increaseStorageSize(OperationContext* txn, int size, bool enforceQuota) {
    DiskLoc eloc = _extentManager->allocateExtent(txn, isCapped(), size, enforceQuota);
    Extent* e = _extentManager->getExtent(eloc);
    invariant(e);

    *txn->recoveryUnit()->writing(&e->nsDiagnostic) = _ns;

    txn->recoveryUnit()->writing(&e->xnext)->Null();
    txn->recoveryUnit()->writing(&e->xprev)->Null();
    txn->recoveryUnit()->writing(&e->firstRecord)->Null();
    txn->recoveryUnit()->writing(&e->lastRecord)->Null();

    DiskLoc emptyLoc = _findFirstSpot(txn, eloc, e);

    if (_details->lastExtent(txn).isNull()) {
        invariant(_details->firstExtent(txn).isNull());
        _details->setFirstExtent(txn, eloc);
        _details->setLastExtent(txn, eloc);
        _details->setCapExtent(txn, eloc);
        invariant(e->xprev.isNull());
        invariant(e->xnext.isNull());
    } else {
        invariant(!_details->firstExtent(txn).isNull());
        *txn->recoveryUnit()->writing(&e->xprev) = _details->lastExtent(txn);
        *txn->recoveryUnit()->writing(
            &_extentManager->getExtent(_details->lastExtent(txn))->xnext) = eloc;
        _details->setLastExtent(txn, eloc);
    }

    _details->setLastExtentSize(txn, e->length);

    addDeletedRec(txn, emptyLoc);
}

Status RecordStoreV1Base::validate(OperationContext* txn,
                                   ValidateCmdLevel level,
                                   ValidateAdaptor* adaptor,
                                   ValidateResults* results,
                                   BSONObjBuilder* output) {
    // 1) basic status that require no iteration
    // 2) extent level info
    // 3) check extent start and end
    // 4) check each non-deleted record
    // 5) check deleted list

    // -------------

    // 1111111111111111111
    if (isCapped()) {
        output->appendBool("capped", true);
        output->appendNumber("max", _details->maxCappedDocs());
    }

    output->appendNumber("datasize", _details->dataSize());
    output->appendNumber("nrecords", _details->numRecords());
    output->appendNumber("lastExtentSize", _details->lastExtentSize(txn));

    if (_details->firstExtent(txn).isNull())
        output->append("firstExtent", "null");
    else
        output->append(
            "firstExtent",
            str::stream() << _details->firstExtent(txn).toString() << " ns:"
                          << _getExtent(txn, _details->firstExtent(txn))->nsDiagnostic.toString());
    if (_details->lastExtent(txn).isNull())
        output->append("lastExtent", "null");
    else
        output->append(
            "lastExtent",
            str::stream() << _details->lastExtent(txn).toString() << " ns:"
                          << _getExtent(txn, _details->lastExtent(txn))->nsDiagnostic.toString());

    // 22222222222222222222222222
    {  // validate extent basics
        BSONArrayBuilder extentData;
        int extentCount = 0;
        DiskLoc extentDiskLoc;
        try {
            if (!_details->firstExtent(txn).isNull()) {
                _getExtent(txn, _details->firstExtent(txn))->assertOk();
                _getExtent(txn, _details->lastExtent(txn))->assertOk();
            }

            extentDiskLoc = _details->firstExtent(txn);
            while (!extentDiskLoc.isNull()) {
                Extent* thisExtent = _getExtent(txn, extentDiskLoc);
                if (level == kValidateFull) {
                    extentData << thisExtent->dump();
                }
                if (!thisExtent->validates(extentDiskLoc, &results->errors)) {
                    results->valid = false;
                }
                DiskLoc nextDiskLoc = thisExtent->xnext;

                if (extentCount > 0 && !nextDiskLoc.isNull() &&
                    _getExtent(txn, nextDiskLoc)->xprev != extentDiskLoc) {
                    StringBuilder sb;
                    sb << "'xprev' pointer " << _getExtent(txn, nextDiskLoc)->xprev.toString()
                       << " in extent " << nextDiskLoc.toString() << " does not point to extent "
                       << extentDiskLoc.toString();
                    results->errors.push_back(sb.str());
                    results->valid = false;
                }
                if (nextDiskLoc.isNull() && extentDiskLoc != _details->lastExtent(txn)) {
                    StringBuilder sb;
                    sb << "'lastExtent' pointer " << _details->lastExtent(txn).toString()
                       << " does not point to last extent in list " << extentDiskLoc.toString();
                    results->errors.push_back(sb.str());
                    results->valid = false;
                }
                extentDiskLoc = nextDiskLoc;
                extentCount++;
                txn->checkForInterrupt();
            }
        } catch (const DBException& e) {
            StringBuilder sb;
            sb << "exception validating extent " << extentCount << ": " << e.what();
            results->errors.push_back(sb.str());
            results->valid = false;
            return Status::OK();
        }
        output->append("extentCount", extentCount);

        if (level == kValidateFull)
            output->appendArray("extents", extentData.arr());
    }

    try {
        // 333333333333333333333333333
        bool testingLastExtent = false;
        try {
            DiskLoc firstExtentLoc = _details->firstExtent(txn);
            if (firstExtentLoc.isNull()) {
                // this is ok
            } else {
                output->append("firstExtentDetails", _getExtent(txn, firstExtentLoc)->dump());
                if (!_getExtent(txn, firstExtentLoc)->xprev.isNull()) {
                    StringBuilder sb;
                    sb << "'xprev' pointer in 'firstExtent' "
                       << _details->firstExtent(txn).toString() << " is "
                       << _getExtent(txn, firstExtentLoc)->xprev.toString() << ", should be null";
                    results->errors.push_back(sb.str());
                    results->valid = false;
                }
            }
            testingLastExtent = true;
            DiskLoc lastExtentLoc = _details->lastExtent(txn);
            if (lastExtentLoc.isNull()) {
                // this is ok
            } else {
                if (firstExtentLoc != lastExtentLoc) {
                    output->append("lastExtentDetails", _getExtent(txn, lastExtentLoc)->dump());
                    if (!_getExtent(txn, lastExtentLoc)->xnext.isNull()) {
                        StringBuilder sb;
                        sb << "'xnext' pointer in 'lastExtent' " << lastExtentLoc.toString()
                           << " is " << _getExtent(txn, lastExtentLoc)->xnext.toString()
                           << ", should be null";
                        results->errors.push_back(sb.str());
                        results->valid = false;
                    }
                }
            }
        } catch (const DBException& e) {
            StringBuilder sb;
            sb << "exception processing '" << (testingLastExtent ? "lastExtent" : "firstExtent")
               << "': " << e.what();
            results->errors.push_back(sb.str());
            results->valid = false;
        }

        // 4444444444444444444444444

        set<DiskLoc> recs;
        if (level == kValidateRecordStore || level == kValidateFull) {
            int n = 0;
            int nInvalid = 0;
            long long nQuantizedSize = 0;
            long long len = 0;
            long long nlen = 0;
            long long bsonLen = 0;
            int outOfOrder = 0;
            DiskLoc dl_last;

            auto cursor = getCursor(txn);
            while (auto record = cursor->next()) {
                const auto dl = DiskLoc::fromRecordId(record->id);
                n++;

                if (n < 1000000)
                    recs.insert(dl);
                if (isCapped()) {
                    if (dl < dl_last)
                        outOfOrder++;
                    dl_last = dl;
                }

                MmapV1RecordHeader* r = recordFor(dl);
                len += r->lengthWithHeaders();
                nlen += r->netLength();

                if (isQuantized(r->lengthWithHeaders())) {
                    // Count the number of records having a size consistent with
                    // the quantizeAllocationSpace quantization implementation.
                    ++nQuantizedSize;
                }

                if (level == kValidateFull) {
                    size_t dataSize = 0;
                    const Status status =
                        adaptor->validate(record->id, r->toRecordData(), &dataSize);
                    if (!status.isOK()) {
                        results->valid = false;
                        if (nInvalid == 0)  // only log once;
                            results->errors.push_back("invalid object detected (see logs)");

                        nInvalid++;
                        log() << "Invalid object detected in " << _ns << ": " << status.reason();
                    } else {
                        bsonLen += dataSize;
                    }
                }
            }

            if (isCapped() && !_details->capLooped()) {
                output->append("cappedOutOfOrder", outOfOrder);
                if (outOfOrder > 1) {
                    results->valid = false;
                    results->errors.push_back("too many out of order records");
                }
            }
            output->append("objectsFound", n);

            if (level == kValidateFull) {
                output->append("invalidObjects", nInvalid);
            }

            output->appendNumber("nQuantizedSize", nQuantizedSize);
            output->appendNumber("bytesWithHeaders", len);
            output->appendNumber("bytesWithoutHeaders", nlen);

            if (level == kValidateFull) {
                output->appendNumber("bytesBson", bsonLen);
            }
        }  // end scanData

        // 55555555555555555555555555
        BSONArrayBuilder deletedListArray;
        for (int i = 0; i < Buckets; i++) {
            deletedListArray << _details->deletedListEntry(i).isNull();
        }

        int ndel = 0;
        long long delSize = 0;
        BSONArrayBuilder delBucketSizes;
        int incorrect = 0;
        for (int i = 0; i < Buckets; i++) {
            DiskLoc loc = _details->deletedListEntry(i);
            try {
                int k = 0;
                while (!loc.isNull()) {
                    if (recs.count(loc))
                        incorrect++;
                    ndel++;

                    if (loc.questionable()) {
                        if (isCapped() && !loc.isValid() && i == 1) {
                            /* the constructor for NamespaceDetails intentionally sets
                             * deletedList[1] to invalid see comments in namespace.h
                            */
                            break;
                        }

                        string err(str::stream() << "bad pointer in deleted record list: "
                                                 << loc.toString()
                                                 << " bucket: "
                                                 << i
                                                 << " k: "
                                                 << k);
                        results->errors.push_back(err);
                        results->valid = false;
                        break;
                    }

                    const DeletedRecord* d = deletedRecordFor(loc);
                    delSize += d->lengthWithHeaders();
                    loc = d->nextDeleted();
                    k++;
                    txn->checkForInterrupt();
                }
                delBucketSizes << k;
            } catch (...) {
                results->errors.push_back((string) "exception in deleted chain for bucket " +
                                          BSONObjBuilder::numStr(i));
                results->valid = false;
            }
        }
        output->appendNumber("deletedCount", ndel);
        output->appendNumber("deletedSize", delSize);
        if (level == kValidateFull) {
            output->append("delBucketSizes", delBucketSizes.arr());
        }

        if (incorrect) {
            results->errors.push_back(BSONObjBuilder::numStr(incorrect) +
                                      " records from datafile are in deleted list");
            results->valid = false;
        }

    } catch (AssertionException) {
        results->errors.push_back("exception during validate");
        results->valid = false;
    }

    return Status::OK();
}

void RecordStoreV1Base::appendCustomStats(OperationContext* txn,
                                          BSONObjBuilder* result,
                                          double scale) const {
    result->append("lastExtentSize", _details->lastExtentSize(txn) / scale);
    result->append("paddingFactor", 1.0);  // hard coded
    result->append("paddingFactorNote",
                   "paddingFactor is unused and unmaintained in 3.0. It "
                   "remains hard coded to 1.0 for compatibility only.");
    result->append("userFlags", _details->userFlags());
    result->appendBool("capped", isCapped());
    if (isCapped()) {
        result->appendNumber("max", _details->maxCappedDocs());
        result->appendNumber("maxSize", static_cast<long long>(storageSize(txn, NULL, 0) / scale));
    }
}


namespace {
struct touch_location {
    const char* root;
    size_t length;
};
}

Status RecordStoreV1Base::touch(OperationContext* txn, BSONObjBuilder* output) const {
    Timer t;

    std::vector<touch_location> ranges;
    {
        DiskLoc nextLoc = _details->firstExtent(txn);
        Extent* ext = nextLoc.isNull() ? NULL : _getExtent(txn, nextLoc);
        while (ext) {
            touch_location tl;
            tl.root = reinterpret_cast<const char*>(ext);
            tl.length = ext->length;
            ranges.push_back(tl);

            nextLoc = ext->xnext;
            if (nextLoc.isNull())
                ext = NULL;
            else
                ext = _getExtent(txn, nextLoc);
        }
    }

    std::string progress_msg = "touch " + ns() + " extents";
    stdx::unique_lock<Client> lk(*txn->getClient());
    ProgressMeterHolder pm(
        *txn->setMessage_inlock(progress_msg.c_str(), "Touch Progress", ranges.size()));
    lk.unlock();

    for (std::vector<touch_location>::iterator it = ranges.begin(); it != ranges.end(); ++it) {
        touch_pages(it->root, it->length);
        pm.hit();
        txn->checkForInterrupt();
    }
    pm.finished();

    if (output) {
        output->append("numRanges", static_cast<int>(ranges.size()));
        output->append("millis", t.millis());
    }

    return Status::OK();
}

boost::optional<Record> RecordStoreV1Base::IntraExtentIterator::next() {
    if (_curr.isNull())
        return {};
    auto out = _curr.toRecordId();
    advance();
    return {{out, _rs->dataFor(_txn, out)}};
}

void RecordStoreV1Base::IntraExtentIterator::advance() {
    if (_curr.isNull())
        return;

    const MmapV1RecordHeader* rec = recordFor(_curr);
    const int nextOfs = _forward ? rec->nextOfs() : rec->prevOfs();
    _curr = (nextOfs == DiskLoc::NullOfs ? DiskLoc() : DiskLoc(_curr.a(), nextOfs));
}

void RecordStoreV1Base::IntraExtentIterator::invalidate(OperationContext* txn,
                                                        const RecordId& rid) {
    if (rid == _curr.toRecordId()) {
        const DiskLoc origLoc = _curr;

        // Undo the advance on rollback, as the deletion that forced it "never happened".
        txn->recoveryUnit()->onRollback([this, origLoc]() { this->_curr = origLoc; });
        advance();
    }
}

std::unique_ptr<RecordFetcher> RecordStoreV1Base::IntraExtentIterator::fetcherForNext() const {
    return _rs->_extentManager->recordNeedsFetch(_curr);
}

int RecordStoreV1Base::quantizeAllocationSpace(int allocSize) {
    invariant(allocSize <= MaxAllowedAllocation);
    for (int i = 0; i < Buckets - 2; i++) {  // last two bucketSizes are invalid
        if (bucketSizes[i] >= allocSize) {
            // Return the size of the first bucket sized >= the requested size.
            return bucketSizes[i];
        }
    }
    invariant(false);  // prior invariant means we should find something.
}

bool RecordStoreV1Base::isQuantized(int recordSize) {
    if (recordSize > MaxAllowedAllocation)
        return false;

    return recordSize == quantizeAllocationSpace(recordSize);
}

int RecordStoreV1Base::bucket(int size) {
    for (int i = 0; i < Buckets; i++) {
        if (bucketSizes[i] > size) {
            // Return the first bucket sized _larger_ than the requested size. This is important
            // since we want all records in a bucket to be >= the quantized size, therefore the
            // quantized size must be the smallest allowed record per bucket.
            return i;
        }
    }
    // Technically, this is reachable if size == INT_MAX, but it would be an error to pass that
    // in anyway since it would be impossible to have a record that large given the file and
    // extent headers.
    invariant(false);
}
}
