// record_store_v1_test_help.cpp

/**
*    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/db/storage/mmap_v1/record_store_v1_test_help.h"

#include <algorithm>
#include <boost/next_prior.hpp>
#include <map>
#include <set>
#include <vector>

#include "mongo/db/storage/mmap_v1/extent.h"
#include "mongo/db/storage/mmap_v1/record.h"
#include "mongo/db/storage/record_fetcher.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/allocator.h"
#include "mongo/util/log.h"

namespace mongo {

using std::numeric_limits;

DummyRecordStoreV1MetaData::DummyRecordStoreV1MetaData(bool capped, int userFlags) {
    _dataSize = 0;
    _numRecords = 0;
    _capped = capped;
    _userFlags = userFlags;
    _lastExtentSize = 0;
    _paddingFactor = 1;
    _maxCappedDocs = numeric_limits<long long>::max();
    _capFirstNewRecord.setInvalid();
    if (_capped) {
        // copied from NamespaceDetails::NamespaceDetails()
        setDeletedListEntry(NULL, 1, DiskLoc().setInvalid());
    }
}

const DiskLoc& DummyRecordStoreV1MetaData::capExtent() const {
    return _capExtent;
}

void DummyRecordStoreV1MetaData::setCapExtent(OperationContext* txn, const DiskLoc& loc) {
    _capExtent = loc;
}

const DiskLoc& DummyRecordStoreV1MetaData::capFirstNewRecord() const {
    return _capFirstNewRecord;
}

void DummyRecordStoreV1MetaData::setCapFirstNewRecord(OperationContext* txn, const DiskLoc& loc) {
    _capFirstNewRecord = loc;
}

long long DummyRecordStoreV1MetaData::dataSize() const {
    return _dataSize;
}

long long DummyRecordStoreV1MetaData::numRecords() const {
    return _numRecords;
}

void DummyRecordStoreV1MetaData::incrementStats(OperationContext* txn,
                                                long long dataSizeIncrement,
                                                long long numRecordsIncrement) {
    _dataSize += dataSizeIncrement;
    _numRecords += numRecordsIncrement;
}

void DummyRecordStoreV1MetaData::setStats(OperationContext* txn,
                                          long long dataSize,
                                          long long numRecords) {
    _dataSize = dataSize;
    _numRecords = numRecords;
}

namespace {
DiskLoc myNull;
}

DiskLoc DummyRecordStoreV1MetaData::deletedListEntry(int bucket) const {
    invariant(bucket >= 0);
    if (static_cast<size_t>(bucket) >= _deletedLists.size())
        return myNull;
    return _deletedLists[bucket];
}

void DummyRecordStoreV1MetaData::setDeletedListEntry(OperationContext* txn,
                                                     int bucket,
                                                     const DiskLoc& loc) {
    invariant(bucket >= 0);
    invariant(bucket < 1000);
    while (static_cast<size_t>(bucket) >= _deletedLists.size())
        _deletedLists.push_back(DiskLoc());
    _deletedLists[bucket] = loc;
}

DiskLoc DummyRecordStoreV1MetaData::deletedListLegacyGrabBag() const {
    return _deletedListLegacyGrabBag;
}

void DummyRecordStoreV1MetaData::setDeletedListLegacyGrabBag(OperationContext* txn,
                                                             const DiskLoc& loc) {
    _deletedListLegacyGrabBag = loc;
}

void DummyRecordStoreV1MetaData::orphanDeletedList(OperationContext* txn) {
    // They will be recreated on demand.
    _deletedLists.clear();
}

const DiskLoc& DummyRecordStoreV1MetaData::firstExtent(OperationContext* txn) const {
    return _firstExtent;
}

void DummyRecordStoreV1MetaData::setFirstExtent(OperationContext* txn, const DiskLoc& loc) {
    _firstExtent = loc;
}

const DiskLoc& DummyRecordStoreV1MetaData::lastExtent(OperationContext* txn) const {
    return _lastExtent;
}

void DummyRecordStoreV1MetaData::setLastExtent(OperationContext* txn, const DiskLoc& loc) {
    _lastExtent = loc;
}

bool DummyRecordStoreV1MetaData::isCapped() const {
    return _capped;
}

bool DummyRecordStoreV1MetaData::isUserFlagSet(int flag) const {
    return _userFlags & flag;
}

bool DummyRecordStoreV1MetaData::setUserFlag(OperationContext* txn, int flag) {
    if ((_userFlags & flag) == flag)
        return false;

    _userFlags |= flag;
    return true;
}
bool DummyRecordStoreV1MetaData::clearUserFlag(OperationContext* txn, int flag) {
    if ((_userFlags & flag) == 0)
        return false;

    _userFlags &= ~flag;
    return true;
}
bool DummyRecordStoreV1MetaData::replaceUserFlags(OperationContext* txn, int flags) {
    if (_userFlags == flags)
        return false;
    _userFlags = flags;
    return true;
}


int DummyRecordStoreV1MetaData::lastExtentSize(OperationContext* txn) const {
    return _lastExtentSize;
}

void DummyRecordStoreV1MetaData::setLastExtentSize(OperationContext* txn, int newMax) {
    _lastExtentSize = newMax;
}

long long DummyRecordStoreV1MetaData::maxCappedDocs() const {
    return _maxCappedDocs;
}

// -----------------------------------------

DummyExtentManager::~DummyExtentManager() {
    for (size_t i = 0; i < _extents.size(); i++) {
        if (_extents[i].data)
            free(_extents[i].data);
    }
}

Status DummyExtentManager::init(OperationContext* txn) {
    return Status::OK();
}

int DummyExtentManager::numFiles() const {
    return static_cast<int>(_extents.size());
}

long long DummyExtentManager::fileSize() const {
    invariant(false);
    return -1;
}

DiskLoc DummyExtentManager::allocateExtent(OperationContext* txn,
                                           bool capped,
                                           int size,
                                           bool enforceQuota) {
    size = quantizeExtentSize(size);

    ExtentInfo info;
    info.data = static_cast<char*>(mongoMalloc(size));
    info.length = size;

    DiskLoc loc(_extents.size(), 0);
    _extents.push_back(info);

    Extent* e = getExtent(loc, false);
    e->magic = Extent::extentSignature;
    e->myLoc = loc;
    e->xnext.Null();
    e->xprev.Null();
    e->length = size;
    e->firstRecord.Null();
    e->lastRecord.Null();

    return loc;
}

void DummyExtentManager::freeExtents(OperationContext* txn, DiskLoc firstExt, DiskLoc lastExt) {
    // XXX
}

void DummyExtentManager::freeExtent(OperationContext* txn, DiskLoc extent) {
    // XXX
}
void DummyExtentManager::freeListStats(OperationContext* txn,
                                       int* numExtents,
                                       int64_t* totalFreeSizeBytes) const {
    invariant(false);
}

std::unique_ptr<RecordFetcher> DummyExtentManager::recordNeedsFetch(const DiskLoc& loc) const {
    return {};
}

MmapV1RecordHeader* DummyExtentManager::recordForV1(const DiskLoc& loc) const {
    if (static_cast<size_t>(loc.a()) >= _extents.size())
        return NULL;
    if (static_cast<size_t>(loc.getOfs()) >= _extents[loc.a()].length)
        return NULL;
    char* root = _extents[loc.a()].data;
    return reinterpret_cast<MmapV1RecordHeader*>(root + loc.getOfs());
}

Extent* DummyExtentManager::extentForV1(const DiskLoc& loc) const {
    invariant(false);
}

DiskLoc DummyExtentManager::extentLocForV1(const DiskLoc& loc) const {
    return DiskLoc(loc.a(), 0);
}

Extent* DummyExtentManager::getExtent(const DiskLoc& loc, bool doSanityCheck) const {
    invariant(!loc.isNull());
    invariant(static_cast<size_t>(loc.a()) < _extents.size());
    invariant(loc.getOfs() == 0);
    Extent* ext = reinterpret_cast<Extent*>(_extents[loc.a()].data);
    if (doSanityCheck)
        ext->assertOk();
    return ext;
}

int DummyExtentManager::maxSize() const {
    return 1024 * 1024 * 64;
}

DummyExtentManager::CacheHint* DummyExtentManager::cacheHint(const DiskLoc& extentLoc,
                                                             const HintType& hint) {
    return new CacheHint();
}

DataFileVersion DummyExtentManager::getFileFormat(OperationContext* txn) const {
    return DataFileVersion::defaultForNewFiles();
}

void DummyExtentManager::setFileFormat(OperationContext* txn, DataFileVersion newVersion) {}

const DataFile* DummyExtentManager::getOpenFile(int n) const {
    return nullptr;
}

namespace {
void accumulateExtentSizeRequirements(const LocAndSize* las, std::map<int, size_t>* sizes) {
    if (!las)
        return;

    while (!las->loc.isNull()) {
        // We require passed in offsets to be > 1000 to leave room for Extent headers.
        invariant(Extent::HeaderSize() < 1000);
        invariant(las->loc.getOfs() >= 1000);

        const size_t end = las->loc.getOfs() + las->size;
        size_t& sizeNeeded = (*sizes)[las->loc.a()];
        sizeNeeded = std::max(sizeNeeded, end);
        las++;
    }
}

void printRecList(OperationContext* txn, const ExtentManager* em, const RecordStoreV1MetaData* md) {
    log() << " *** BEGIN ACTUAL RECORD LIST *** ";
    DiskLoc extLoc = md->firstExtent(txn);
    std::set<DiskLoc> seenLocs;
    while (!extLoc.isNull()) {
        Extent* ext = em->getExtent(extLoc, true);
        DiskLoc actualLoc = ext->firstRecord;
        while (!actualLoc.isNull()) {
            const MmapV1RecordHeader* actualRec = em->recordForV1(actualLoc);
            const int actualSize = actualRec->lengthWithHeaders();

            log() << "loc: " << actualLoc  // <--hex
                  << " (" << actualLoc.getOfs() << ")"
                  << " size: " << actualSize << " prev: " << actualRec->prevOfs()
                  << " next: " << actualRec->nextOfs()
                  << (actualLoc == md->capFirstNewRecord() ? " (CAP_FIRST_NEW)" : "");

            const bool foundCycle = !seenLocs.insert(actualLoc).second;
            invariant(!foundCycle);

            const int nextOfs = actualRec->nextOfs();
            actualLoc = (nextOfs == DiskLoc::NullOfs ? DiskLoc() : DiskLoc(actualLoc.a(), nextOfs));
        }
        extLoc = ext->xnext;
    }
    log() << " *** END ACTUAL RECORD LIST *** ";
}

void printDRecList(const ExtentManager* em, const RecordStoreV1MetaData* md) {
    log() << " *** BEGIN ACTUAL DELETED RECORD LIST *** ";
    std::set<DiskLoc> seenLocs;
    for (int bucketIdx = 0; bucketIdx < RecordStoreV1Base::Buckets; bucketIdx++) {
        DiskLoc actualLoc = md->deletedListEntry(bucketIdx);
        while (!actualLoc.isNull()) {
            const DeletedRecord* actualDrec = &em->recordForV1(actualLoc)->asDeleted();
            const int actualSize = actualDrec->lengthWithHeaders();

            log() << "loc: " << actualLoc  // <--hex
                  << " (" << actualLoc.getOfs() << ")"
                  << " size: " << actualSize << " bucket: " << bucketIdx
                  << " next: " << actualDrec->nextDeleted();

            const bool foundCycle = !seenLocs.insert(actualLoc).second;
            invariant(!foundCycle);

            actualLoc = actualDrec->nextDeleted();
        }

        // Only print bucket 0 in capped collections since it contains all deleted records
        if (md->isCapped())
            break;
    }
    log() << " *** END ACTUAL DELETED RECORD LIST *** ";
}
}

void initializeV1RS(OperationContext* txn,
                    const LocAndSize* records,
                    const LocAndSize* drecs,
                    const LocAndSize* legacyGrabBag,
                    DummyExtentManager* em,
                    DummyRecordStoreV1MetaData* md) {
    invariant(records || drecs);  // if both are NULL nothing is being created...

    // Need to start with a blank slate
    invariant(em->numFiles() == 0);
    invariant(md->firstExtent(txn).isNull());

    // pre-allocate extents (even extents that aren't part of this RS)
    {
        typedef std::map<int, size_t> ExtentSizes;
        ExtentSizes extentSizes;
        accumulateExtentSizeRequirements(records, &extentSizes);
        accumulateExtentSizeRequirements(drecs, &extentSizes);
        accumulateExtentSizeRequirements(legacyGrabBag, &extentSizes);
        invariant(!extentSizes.empty());

        const int maxExtent = extentSizes.rbegin()->first;
        for (int i = 0; i <= maxExtent; i++) {
            const size_t size = extentSizes.count(i) ? extentSizes[i] : 0;
            const DiskLoc loc = em->allocateExtent(txn, md->isCapped(), size, 0);

            // This function and assertState depend on these details of DummyExtentManager
            invariant(loc.a() == i);
            invariant(loc.getOfs() == 0);
        }

        // link together extents that should be part of this RS
        md->setFirstExtent(txn, DiskLoc(extentSizes.begin()->first, 0));
        md->setLastExtent(txn, DiskLoc(extentSizes.rbegin()->first, 0));
        for (ExtentSizes::iterator it = extentSizes.begin(); boost::next(it) != extentSizes.end();
             /* ++it */) {
            const int a = it->first;
            ++it;
            const int b = it->first;
            em->getExtent(DiskLoc(a, 0))->xnext = DiskLoc(b, 0);
            em->getExtent(DiskLoc(b, 0))->xprev = DiskLoc(a, 0);
        }

        // This signals "done allocating new extents".
        if (md->isCapped())
            md->setDeletedListEntry(txn, 1, DiskLoc());
    }

    if (records && !records[0].loc.isNull()) {
        int recIdx = 0;
        DiskLoc extLoc = md->firstExtent(txn);
        while (!extLoc.isNull()) {
            Extent* ext = em->getExtent(extLoc);
            int prevOfs = DiskLoc::NullOfs;
            while (extLoc.a() == records[recIdx].loc.a()) {  // for all records in this extent
                const DiskLoc loc = records[recIdx].loc;
                const int size = records[recIdx].size;
                ;
                invariant(size >= MmapV1RecordHeader::HeaderSize);

                md->incrementStats(txn, size - MmapV1RecordHeader::HeaderSize, 1);

                if (ext->firstRecord.isNull())
                    ext->firstRecord = loc;

                MmapV1RecordHeader* rec = em->recordForV1(loc);
                rec->lengthWithHeaders() = size;
                rec->extentOfs() = 0;

                rec->prevOfs() = prevOfs;
                prevOfs = loc.getOfs();

                const DiskLoc nextLoc = records[recIdx + 1].loc;
                if (nextLoc.a() == loc.a()) {  // if next is in same extent
                    rec->nextOfs() = nextLoc.getOfs();
                } else {
                    rec->nextOfs() = DiskLoc::NullOfs;
                    ext->lastRecord = loc;
                }

                recIdx++;
            }
            extLoc = ext->xnext;
        }
        invariant(records[recIdx].loc.isNull());
    }

    if (drecs && !drecs[0].loc.isNull()) {
        int drecIdx = 0;
        DiskLoc* prevNextPtr = NULL;
        int lastBucket = -1;
        while (!drecs[drecIdx].loc.isNull()) {
            const DiskLoc loc = drecs[drecIdx].loc;
            const int size = drecs[drecIdx].size;
            invariant(size >= MmapV1RecordHeader::HeaderSize);
            const int bucket = RecordStoreV1Base::bucket(size);

            if (md->isCapped()) {
                // All drecs form a single list in bucket 0
                if (prevNextPtr == NULL) {
                    md->setDeletedListEntry(txn, 0, loc);
                } else {
                    *prevNextPtr = loc;
                }

                if (loc.a() < md->capExtent().a() &&
                    drecs[drecIdx + 1].loc.a() == md->capExtent().a()) {
                    // Bucket 1 is known as cappedLastDelRecLastExtent
                    md->setDeletedListEntry(txn, 1, loc);
                }
            } else if (bucket != lastBucket) {
                invariant(bucket > lastBucket);  // if this fails, drecs weren't sorted by bucket
                md->setDeletedListEntry(txn, bucket, loc);
                lastBucket = bucket;
            } else {
                *prevNextPtr = loc;
            }

            DeletedRecord* drec = &em->recordForV1(loc)->asDeleted();
            drec->lengthWithHeaders() = size;
            drec->extentOfs() = 0;
            drec->nextDeleted() = DiskLoc();
            prevNextPtr = &drec->nextDeleted();

            drecIdx++;
        }
    }

    if (legacyGrabBag && !legacyGrabBag[0].loc.isNull()) {
        invariant(!md->isCapped());  // capped should have an empty legacy grab bag.

        int grabBagIdx = 0;
        DiskLoc* prevNextPtr = NULL;
        while (!legacyGrabBag[grabBagIdx].loc.isNull()) {
            const DiskLoc loc = legacyGrabBag[grabBagIdx].loc;
            const int size = legacyGrabBag[grabBagIdx].size;
            invariant(size >= MmapV1RecordHeader::HeaderSize);

            if (grabBagIdx == 0) {
                md->setDeletedListLegacyGrabBag(txn, loc);
            } else {
                *prevNextPtr = loc;
            }

            DeletedRecord* drec = &em->recordForV1(loc)->asDeleted();
            drec->lengthWithHeaders() = size;
            drec->extentOfs() = 0;
            drec->nextDeleted() = DiskLoc();
            prevNextPtr = &drec->nextDeleted();

            grabBagIdx++;
        }
    }

    // Make sure we set everything up as requested.
    assertStateV1RS(txn, records, drecs, legacyGrabBag, em, md);
}

void assertStateV1RS(OperationContext* txn,
                     const LocAndSize* records,
                     const LocAndSize* drecs,
                     const LocAndSize* legacyGrabBag,
                     const ExtentManager* em,
                     const DummyRecordStoreV1MetaData* md) {
    invariant(records || drecs);  // if both are NULL nothing is being asserted...

    try {
        if (records) {
            long long dataSize = 0;
            long long numRecs = 0;

            int recIdx = 0;

            DiskLoc extLoc = md->firstExtent(txn);
            while (!extLoc.isNull()) {  // for each Extent
                Extent* ext = em->getExtent(extLoc, true);
                int expectedPrevOfs = DiskLoc::NullOfs;
                DiskLoc actualLoc = ext->firstRecord;
                while (!actualLoc.isNull()) {  // for each MmapV1RecordHeader in this Extent
                    const MmapV1RecordHeader* actualRec = em->recordForV1(actualLoc);
                    const int actualSize = actualRec->lengthWithHeaders();

                    dataSize += actualSize - MmapV1RecordHeader::HeaderSize;
                    numRecs += 1;

                    ASSERT_EQUALS(actualLoc, records[recIdx].loc);
                    ASSERT_EQUALS(actualSize, records[recIdx].size);

                    ASSERT_EQUALS(actualRec->extentOfs(), extLoc.getOfs());
                    ASSERT_EQUALS(actualRec->prevOfs(), expectedPrevOfs);
                    expectedPrevOfs = actualLoc.getOfs();

                    recIdx++;
                    const int nextOfs = actualRec->nextOfs();
                    actualLoc =
                        (nextOfs == DiskLoc::NullOfs ? DiskLoc() : DiskLoc(actualLoc.a(), nextOfs));
                }

                if (ext->xnext.isNull()) {
                    ASSERT_EQUALS(md->lastExtent(txn), extLoc);
                }

                extLoc = ext->xnext;
            }

            // both the expected and actual record lists must be done at this point
            ASSERT_EQUALS(records[recIdx].loc, DiskLoc());

            ASSERT_EQUALS(dataSize, md->dataSize());
            ASSERT_EQUALS(numRecs, md->numRecords());
        }

        if (drecs) {
            int drecIdx = 0;
            for (int bucketIdx = 0; bucketIdx < RecordStoreV1Base::Buckets; bucketIdx++) {
                DiskLoc actualLoc = md->deletedListEntry(bucketIdx);

                if (md->isCapped() && bucketIdx == 1) {
                    // In capped collections, the 2nd bucket (index 1) points to the drec before
                    // the first drec in the capExtent. If the capExtent is the first Extent,
                    // it should be Null.

                    if (md->capExtent() == md->firstExtent(txn)) {
                        ASSERT_EQUALS(actualLoc, DiskLoc());
                    } else {
                        ASSERT_NOT_EQUALS(actualLoc.a(), md->capExtent().a());
                        const DeletedRecord* actualDrec = &em->recordForV1(actualLoc)->asDeleted();
                        ASSERT_EQUALS(actualDrec->nextDeleted().a(), md->capExtent().a());
                    }

                    // Don't do normal checking of bucket 1 in capped collections. Checking
                    // other buckets to verify that they are Null.
                    continue;
                }

                while (!actualLoc.isNull()) {
                    const DeletedRecord* actualDrec = &em->recordForV1(actualLoc)->asDeleted();
                    const int actualSize = actualDrec->lengthWithHeaders();

                    ASSERT_EQUALS(actualLoc, drecs[drecIdx].loc);
                    ASSERT_EQUALS(actualSize, drecs[drecIdx].size);

                    // Make sure the drec is correct
                    ASSERT_EQUALS(actualDrec->extentOfs(), 0);

                    // in capped collections all drecs are linked into a single list in bucket 0
                    ASSERT_EQUALS(bucketIdx,
                                  md->isCapped() ? 0 : RecordStoreV1Base::bucket(actualSize));

                    drecIdx++;
                    actualLoc = actualDrec->nextDeleted();
                }
            }
            // both the expected and actual deleted lists must be done at this point
            ASSERT_EQUALS(drecs[drecIdx].loc, DiskLoc());
        }

        if (legacyGrabBag) {
            int grabBagIdx = 0;
            DiskLoc actualLoc = md->deletedListLegacyGrabBag();
            while (!actualLoc.isNull()) {
                const DeletedRecord* actualDrec = &em->recordForV1(actualLoc)->asDeleted();
                const int actualSize = actualDrec->lengthWithHeaders();

                ASSERT_EQUALS(actualLoc, legacyGrabBag[grabBagIdx].loc);
                ASSERT_EQUALS(actualSize, legacyGrabBag[grabBagIdx].size);

                grabBagIdx++;
                actualLoc = actualDrec->nextDeleted();
            }

            // both the expected and actual deleted lists must be done at this point
            ASSERT_EQUALS(legacyGrabBag[grabBagIdx].loc, DiskLoc());
        } else {
            // Unless a test is actually using the grabBag it should be empty
            ASSERT_EQUALS(md->deletedListLegacyGrabBag(), DiskLoc());
        }
    } catch (...) {
        // If a test fails, provide extra info to make debugging easier
        printRecList(txn, em, md);
        printDRecList(em, md);
        throw;
    }
}
}
