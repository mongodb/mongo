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

#include "mongo/db/structure/record_store_v1_test_help.h"

#include <algorithm>
#include <map>
#include <set>
#include <vector>

#include "mongo/db/storage/extent.h"
#include "mongo/db/storage/record.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

    bool DummyRecoveryUnit::commitIfNeeded( bool force ) {
        return false;
    }

    bool DummyRecoveryUnit::isCommitNeeded() const {
        return false;
    }

    void* DummyRecoveryUnit::writingPtr(void* data, size_t len) {
        return data;
    }

    void DummyRecoveryUnit::createdFile(const std::string& filename, unsigned long long len) {
    }

    void DummyRecoveryUnit::syncDataAndTruncateJournal() {
    }

    DummyOperationContext::DummyOperationContext() {
        _recoveryUnit.reset(new DummyRecoveryUnit());
    }

    ProgressMeter* DummyOperationContext::setMessage(const char* msg,
                                                          const std::string& name ,
                                                          unsigned long long progressMeterTotal,
                                                          int secondsBetween) {
        invariant( false );
    }

    void DummyOperationContext::checkForInterrupt(bool heedMutex ) const {
    }

    Status DummyOperationContext::checkForInterruptNoAssert() const {
        return Status::OK();
    }

    // -----------------------------------------

    DummyRecordStoreV1MetaData::DummyRecordStoreV1MetaData( bool capped, int userFlags ) {
        _dataSize = 0;
        _numRecords = 0;
        _capped = capped;
        _userFlags = userFlags;
        _lastExtentSize = 0;
        _paddingFactor = 1;
        _maxCappedDocs = numeric_limits<long long>::max();
        _capFirstNewRecord.setInvalid();
        if ( _capped ) {
            // copied from NamespaceDetails::NamespaceDetails()
            setDeletedListEntry( NULL, 1, DiskLoc().setInvalid() );
        }
    }

    const DiskLoc& DummyRecordStoreV1MetaData::capExtent() const {
        return _capExtent;
    }

    void DummyRecordStoreV1MetaData::setCapExtent( OperationContext* txn,
                                                   const DiskLoc& loc ) {
        _capExtent = loc;
    }

    const DiskLoc& DummyRecordStoreV1MetaData::capFirstNewRecord() const {
        return _capFirstNewRecord;
    }

    void DummyRecordStoreV1MetaData::setCapFirstNewRecord( OperationContext* txn,
                                                           const DiskLoc& loc ) {
        _capFirstNewRecord = loc;
    }

    bool DummyRecordStoreV1MetaData::capLooped() const {
        invariant( false );
    }

    long long DummyRecordStoreV1MetaData::dataSize() const {
        return _dataSize;
    }

    long long DummyRecordStoreV1MetaData::numRecords() const {
        return _numRecords;
    }

    void DummyRecordStoreV1MetaData::incrementStats( OperationContext* txn,
                                                     long long dataSizeIncrement,
                                                     long long numRecordsIncrement ) {
        _dataSize += dataSizeIncrement;
        _numRecords += numRecordsIncrement;
    }

    void DummyRecordStoreV1MetaData::setStats( OperationContext* txn,
                                               long long dataSizeIncrement,
                                               long long numRecordsIncrement ) {
        _dataSize = dataSizeIncrement;
        _numRecords = numRecordsIncrement;
    }

    namespace {
        DiskLoc myNull;
    }

    const DiskLoc& DummyRecordStoreV1MetaData::deletedListEntry( int bucket ) const {
        invariant( bucket >= 0 );
        if ( static_cast<size_t>( bucket ) >= _deletedLists.size() )
            return myNull;
        return _deletedLists[bucket];
    }

    void DummyRecordStoreV1MetaData::setDeletedListEntry( OperationContext* txn,
                                                          int bucket,
                                                          const DiskLoc& loc ) {
        invariant( bucket >= 0 );
        invariant( bucket < 1000 );
        while ( static_cast<size_t>( bucket ) >= _deletedLists.size() )
            _deletedLists.push_back( DiskLoc() );
        _deletedLists[bucket] = loc;
    }

    void DummyRecordStoreV1MetaData::orphanDeletedList(OperationContext* txn) {
        invariant( false );
    }

    const DiskLoc& DummyRecordStoreV1MetaData::firstExtent() const {
        return _firstExtent;
    }

    void DummyRecordStoreV1MetaData::setFirstExtent( OperationContext* txn,
                                                     const DiskLoc& loc ) {
        _firstExtent = loc;
    }

    const DiskLoc& DummyRecordStoreV1MetaData::lastExtent() const {
        return _lastExtent;
    }

    void DummyRecordStoreV1MetaData::setLastExtent( OperationContext* txn,
                                                    const DiskLoc& loc ) {
        _lastExtent = loc;
    }

    bool DummyRecordStoreV1MetaData::isCapped() const {
        return _capped;
    }

    bool DummyRecordStoreV1MetaData::isUserFlagSet( int flag ) const {
        return _userFlags & flag;
    }

    int DummyRecordStoreV1MetaData::lastExtentSize() const {
        return _lastExtentSize;
    }

    void DummyRecordStoreV1MetaData::setLastExtentSize( OperationContext* txn, int newMax ) {
        _lastExtentSize = newMax;
    }

    long long DummyRecordStoreV1MetaData::maxCappedDocs() const {
        return _maxCappedDocs;
    }

    double DummyRecordStoreV1MetaData::paddingFactor() const {
        return _paddingFactor;
    }

    void DummyRecordStoreV1MetaData::setPaddingFactor( OperationContext* txn,
                                                       double paddingFactor ) {
        _paddingFactor = paddingFactor;
    }

    // -----------------------------------------

    DummyExtentManager::~DummyExtentManager() {
        for ( size_t i = 0; i < _extents.size(); i++ ) {
            if ( _extents[i].data )
                free( _extents[i].data );
        }
    }

    Status DummyExtentManager::init(OperationContext* txn) {
        return Status::OK();
    }

    size_t DummyExtentManager::numFiles() const {
        return _extents.size();
    }

    long long DummyExtentManager::fileSize() const {
        invariant( false );
        return -1;
    }

    void DummyExtentManager::flushFiles( bool sync ) {
    }

    DiskLoc DummyExtentManager::allocateExtent( OperationContext* txn,
                                                bool capped,
                                                int size,
                                                int quotaMax ) {
        size = quantizeExtentSize( size );

        ExtentInfo info;
        info.data = static_cast<char*>( malloc( size ) );
        info.length = size;

        DiskLoc loc( _extents.size(), 0 );
        _extents.push_back( info );

        Extent* e = getExtent( loc, false );
        e->magic = Extent::extentSignature;
        e->myLoc = loc;
        e->xnext.Null();
        e->xprev.Null();
        e->length = size;
        e->firstRecord.Null();
        e->lastRecord.Null();

        return loc;

    }

    void DummyExtentManager::freeExtents( OperationContext* txn,
                                          DiskLoc firstExt, DiskLoc lastExt ) {
        // XXX
    }

    void DummyExtentManager::freeExtent( OperationContext* txn, DiskLoc extent ) {
        // XXX
    }
    void DummyExtentManager::freeListStats( int* numExtents, int64_t* totalFreeSize ) const {
        invariant( false );
    }

    Record* DummyExtentManager::recordForV1( const DiskLoc& loc ) const {
        invariant( static_cast<size_t>( loc.a() ) < _extents.size() );
        invariant( static_cast<size_t>( loc.getOfs() ) < _extents[loc.a()].length );
        char* root = _extents[loc.a()].data;
        return reinterpret_cast<Record*>( root + loc.getOfs() );
    }

    Extent* DummyExtentManager::extentForV1( const DiskLoc& loc ) const {
        invariant( false );
    }

    DiskLoc DummyExtentManager::extentLocForV1( const DiskLoc& loc ) const {
        return DiskLoc( loc.a(), 0 );
    }

    Extent* DummyExtentManager::getExtent( const DiskLoc& loc, bool doSanityCheck ) const {
        invariant( !loc.isNull() );
        invariant( static_cast<size_t>( loc.a() ) < _extents.size() );
        invariant( loc.getOfs() == 0 );
        Extent* ext = reinterpret_cast<Extent*>( _extents[loc.a()].data );
        if (doSanityCheck)
            ext->assertOk();
        return ext;
    }

    int DummyExtentManager::maxSize() const {
        return 1024 * 1024 * 64;
    }

    DummyExtentManager::CacheHint* DummyExtentManager::cacheHint( const DiskLoc& extentLoc, const HintType& hint ) {
        return new CacheHint();
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

    void printRecList(const ExtentManager* em, const RecordStoreV1MetaData* md) {
        log() << " *** BEGIN ACTUAL RECORD LIST *** ";
        DiskLoc extLoc = md->firstExtent();
        std::set<DiskLoc> seenLocs;
        while (!extLoc.isNull()) {
            Extent* ext = em->getExtent(extLoc, true);
            DiskLoc actualLoc = ext->firstRecord;
            while (!actualLoc.isNull()) {
                const Record* actualRec = em->recordForV1(actualLoc);
                const int actualSize = actualRec->lengthWithHeaders();

                log() << "loc: " << actualLoc // <--hex
                      << " (" << actualLoc.getOfs() << ")"
                      << " size: " << actualSize
                      << " next: " << actualRec->nextOfs()
                      << " prev: " << actualRec->prevOfs();

                const bool foundCycle = !seenLocs.insert(actualLoc).second;
                invariant(!foundCycle);

                const int nextOfs = actualRec->nextOfs();
                actualLoc = (nextOfs == DiskLoc::NullOfs ? DiskLoc()
                                                         : DiskLoc(actualLoc.a(), nextOfs));
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

                log() << "loc: " << actualLoc // <--hex
                      << " (" << actualLoc.getOfs() << ")"
                      << " size: " << actualSize
                      << " bucket: " << bucketIdx
                      << " next: " << actualDrec->nextDeleted();

                const bool foundCycle = !seenLocs.insert(actualLoc).second;
                invariant(!foundCycle);

                actualLoc = actualDrec->nextDeleted();
            }
        }
        log() << " *** END ACTUAL DELETED RECORD LIST *** ";
    }
}

    void initializeV1RS(OperationContext* txn,
                        const LocAndSize* records,
                        const LocAndSize* drecs,
                        DummyExtentManager* em,
                        DummyRecordStoreV1MetaData* md) {
        invariant(records || drecs); // if both are NULL nothing is being created...
        invariant(em->numFiles() == 0);
        invariant(md->firstExtent().isNull());

        // pre-allocate extents (even extents that aren't part of this RS)
        {
            typedef std::map<int, size_t> ExtentSizes;
            ExtentSizes extentSizes;
            accumulateExtentSizeRequirements(records, &extentSizes);
            accumulateExtentSizeRequirements(drecs, &extentSizes);
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
            for (ExtentSizes::iterator it = extentSizes.begin();
                    boost::next(it) != extentSizes.end(); /* ++it */ ) {
                const int a = it->first;
                ++it;
                const int b = it->first;
                em->getExtent(DiskLoc(a, 0))->xnext = DiskLoc(b, 0);
                em->getExtent(DiskLoc(b, 0))->xprev = DiskLoc(a, 0);
            }
        }

        if (records && !records[0].loc.isNull()) {
            // TODO figure out how to handle capExtent specially in cappedCollections
            int recIdx = 0;
            DiskLoc extLoc = md->firstExtent();
            while (!extLoc.isNull()) {
                Extent* ext = em->getExtent(extLoc);
                int prevOfs = DiskLoc::NullOfs;
                while (extLoc.a() == records[recIdx].loc.a()) { // for all records in this extent
                    const DiskLoc loc = records[recIdx].loc;
                    const int size = records[recIdx].size;;
                    invariant(size >= Record::HeaderSize);

                    md->incrementStats(txn, size - Record::HeaderSize, 1);

                    if (ext->firstRecord.isNull())
                        ext->firstRecord = loc;

                    Record* rec = em->recordForV1(loc);
                    rec->lengthWithHeaders() = size;
                    rec->extentOfs() = 0;

                    rec->prevOfs() = prevOfs;
                    prevOfs = loc.getOfs();

                    const DiskLoc nextLoc = records[++recIdx].loc;
                    if (nextLoc.a() == loc.a()) { // if next is in same extent
                        rec->nextOfs() = nextLoc.getOfs();
                    }
                    else {
                        rec->nextOfs() = DiskLoc::NullOfs;
                        ext->lastRecord = loc;
                    }
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
                invariant(size >= Record::HeaderSize);
                const int bucket = RecordStoreV1Base::bucket(size);

                if (bucket != lastBucket) {
                    invariant(bucket > lastBucket); // if this fails, drecs weren't sorted by bucket
                    md->setDeletedListEntry(txn, bucket, loc);
                    lastBucket = bucket;
                }
                else {
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

        // Make sure we set everything up as requested.
        assertStateV1RS(records, drecs, em, md);
    }

    void assertStateV1RS(const LocAndSize* records,
                         const LocAndSize* drecs,
                         const ExtentManager* em,
                         const DummyRecordStoreV1MetaData* md) {
        invariant(records || drecs); // if both are NULL nothing is being asserted...

        try {
            if (records) {
                long long dataSize = 0;
                long long numRecs = 0;

                int recIdx = 0;

                DiskLoc extLoc = md->firstExtent();
                while (!extLoc.isNull()) {
                    Extent* ext = em->getExtent(extLoc, true);
                    int actualPrevOfs = DiskLoc::NullOfs;
                    DiskLoc actualLoc = ext->firstRecord;
                    while (!actualLoc.isNull()) {
                        const Record* actualRec = em->recordForV1(actualLoc);
                        const int actualSize = actualRec->lengthWithHeaders();

                        dataSize += actualSize - Record::HeaderSize;
                        numRecs += 1;

                        ASSERT_EQUALS(actualLoc, records[recIdx].loc);
                        ASSERT_EQUALS(actualSize, records[recIdx].size);

                        ASSERT_EQUALS(actualRec->extentOfs(), extLoc.getOfs());
                        ASSERT_EQUALS(actualRec->prevOfs(), actualPrevOfs);
                        actualPrevOfs = actualLoc.getOfs();

                        recIdx++;
                        const int nextOfs = actualRec->nextOfs();
                        actualLoc = (nextOfs == DiskLoc::NullOfs ? DiskLoc()
                                                                 : DiskLoc(actualLoc.a(), nextOfs));
                    }

                    if (ext->xnext.isNull()) {
                        ASSERT_EQUALS(md->lastExtent(), extLoc);
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
                    while (!actualLoc.isNull()) {
                        const DeletedRecord* actualDrec = &em->recordForV1(actualLoc)->asDeleted();
                        const int actualSize = actualDrec->lengthWithHeaders();

                        ASSERT_EQUALS(actualLoc, drecs[drecIdx].loc);
                        ASSERT_EQUALS(actualSize, drecs[drecIdx].size);

                        // Make sure the drec is correct
                        ASSERT_EQUALS(actualDrec->extentOfs(), 0);
                        ASSERT_EQUALS(bucketIdx, RecordStoreV1Base::bucket(actualSize));

                        drecIdx++;
                        actualLoc = actualDrec->nextDeleted();
                    }
                }
                // both the expected and actual deleted lists must be done at this point
                ASSERT_EQUALS(drecs[drecIdx].loc, DiskLoc());
            }
        }
        catch (...) {
            // If a test fails, provide extra info to make debugging easier
            printRecList(em, md);
            printDRecList(em, md);
            throw;
        }
    }
}
