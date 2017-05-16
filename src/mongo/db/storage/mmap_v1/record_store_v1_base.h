// record_store_v1_base.h

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

#pragma once

#include "mongo/platform/unordered_set.h"
#include "mongo/util/concurrency/spin_lock.h"

#include "mongo/db/storage/mmap_v1/diskloc.h"
#include "mongo/db/storage/record_store.h"

namespace mongo {

class DeletedRecord;
class ExtentManager;
class MmapV1RecordHeader;
class OperationContext;

struct Extent;

class RecordStoreV1MetaData {
public:
    virtual ~RecordStoreV1MetaData() {}

    virtual const DiskLoc& capExtent() const = 0;
    virtual void setCapExtent(OperationContext* opCtx, const DiskLoc& loc) = 0;

    virtual const DiskLoc& capFirstNewRecord() const = 0;
    virtual void setCapFirstNewRecord(OperationContext* opCtx, const DiskLoc& loc) = 0;

    bool capLooped() const {
        return capFirstNewRecord().isValid();
    }

    virtual long long dataSize() const = 0;
    virtual long long numRecords() const = 0;

    virtual void incrementStats(OperationContext* opCtx,
                                long long dataSizeIncrement,
                                long long numRecordsIncrement) = 0;

    virtual void setStats(OperationContext* opCtx, long long dataSize, long long numRecords) = 0;

    virtual DiskLoc deletedListEntry(int bucket) const = 0;
    virtual void setDeletedListEntry(OperationContext* opCtx, int bucket, const DiskLoc& loc) = 0;

    virtual DiskLoc deletedListLegacyGrabBag() const = 0;
    virtual void setDeletedListLegacyGrabBag(OperationContext* opCtx, const DiskLoc& loc) = 0;

    virtual void orphanDeletedList(OperationContext* opCtx) = 0;

    virtual const DiskLoc& firstExtent(OperationContext* opCtx) const = 0;
    virtual void setFirstExtent(OperationContext* opCtx, const DiskLoc& loc) = 0;

    virtual const DiskLoc& lastExtent(OperationContext* opCtx) const = 0;
    virtual void setLastExtent(OperationContext* opCtx, const DiskLoc& loc) = 0;

    virtual bool isCapped() const = 0;

    virtual bool isUserFlagSet(int flag) const = 0;
    virtual int userFlags() const = 0;
    virtual bool setUserFlag(OperationContext* opCtx, int flag) = 0;
    virtual bool clearUserFlag(OperationContext* opCtx, int flag) = 0;
    virtual bool replaceUserFlags(OperationContext* opCtx, int flags) = 0;

    virtual int lastExtentSize(OperationContext* opCtx) const = 0;
    virtual void setLastExtentSize(OperationContext* opCtx, int newMax) = 0;

    virtual long long maxCappedDocs() const = 0;
};

/**
 * Class that stores active cursors that have been saved (as part of yielding) to
 * allow them to be invalidated if the thing they pointed at goes away. The registry is
 * thread-safe, as readers may concurrently register and remove their cursors. Contention is
 * expected to be very low, as yielding is infrequent. This logically belongs to the
 * RecordStore, but is not contained in it to facilitate unit testing.
 */
class SavedCursorRegistry {
public:
    /**
     * The destructor ensures the cursor is unregistered when an exception is thrown.
     * Note that the SavedCursor may outlive the registry it was saved in.
     */
    struct SavedCursor {
        SavedCursor() : _registry(NULL) {}
        virtual ~SavedCursor() {
            if (_registry)
                _registry->unregisterCursor(this);
        }
        DiskLoc bucket;
        BSONObj key;
        DiskLoc loc;

    private:
        friend class SavedCursorRegistry;
        // Non-null iff registered. Accessed by owner or writer with MODE_X collection lock
        SavedCursorRegistry* _registry;
    };

    ~SavedCursorRegistry();

    /**
     * Adds given saved cursor to SavedCursorRegistry. Doesn't take ownership.
     */
    void registerCursor(SavedCursor* cursor);

    /**
     * Removes given saved cursor. Returns true if the cursor was still present, and false
     * if it had already been removed due to invalidation. Doesn't take ownership.
     */
    bool unregisterCursor(SavedCursor* cursor);

    /**
     * When a btree-bucket disappears due to merge/split or similar, this invalidates all
     * cursors that point at the same bucket by removing them from the registry.
     */
    void invalidateCursorsForBucket(DiskLoc bucket);

private:
    SpinLock _mutex;
    typedef unordered_set<SavedCursor*> SavedCursorSet;  // SavedCursor pointers not owned here
    SavedCursorSet _cursors;
};

class RecordStoreV1Base : public RecordStore {
public:
    static const int Buckets = 26;
    static const int MaxAllowedAllocation = 16 * 1024 * 1024 + 512 * 1024;

    static const int bucketSizes[];

    // ------------

    class IntraExtentIterator;

    /**
     * @param details - takes ownership
     * @param em - does NOT take ownership
     */
    RecordStoreV1Base(StringData ns,
                      RecordStoreV1MetaData* details,
                      ExtentManager* em,
                      bool isSystemIndexes);

    virtual ~RecordStoreV1Base();

    virtual long long dataSize(OperationContext* opCtx) const {
        return _details->dataSize();
    }
    virtual long long numRecords(OperationContext* opCtx) const {
        return _details->numRecords();
    }

    virtual int64_t storageSize(OperationContext* opCtx,
                                BSONObjBuilder* extraInfo = NULL,
                                int level = 0) const;

    virtual RecordData dataFor(OperationContext* opCtx, const RecordId& loc) const;

    virtual bool findRecord(OperationContext* opCtx, const RecordId& loc, RecordData* rd) const;

    void deleteRecord(OperationContext* opCtx, const RecordId& dl);

    StatusWith<RecordId> insertRecord(OperationContext* opCtx,
                                      const char* data,
                                      int len,
                                      bool enforceQuota);

    Status insertRecordsWithDocWriter(OperationContext* opCtx,
                                      const DocWriter* const* docs,
                                      size_t nDocs,
                                      RecordId* idsOut) final;

    virtual Status updateRecord(OperationContext* opCtx,
                                const RecordId& oldLocation,
                                const char* data,
                                int len,
                                bool enforceQuota,
                                UpdateNotifier* notifier);

    virtual bool updateWithDamagesSupported() const;

    virtual StatusWith<RecordData> updateWithDamages(OperationContext* opCtx,
                                                     const RecordId& loc,
                                                     const RecordData& oldRec,
                                                     const char* damageSource,
                                                     const mutablebson::DamageVector& damages);

    virtual std::unique_ptr<RecordCursor> getCursorForRepair(OperationContext* opCtx) const;

    void increaseStorageSize(OperationContext* opCtx, int size, bool enforceQuota);

    virtual Status validate(OperationContext* opCtx,
                            ValidateCmdLevel level,
                            ValidateAdaptor* adaptor,
                            ValidateResults* results,
                            BSONObjBuilder* output);

    virtual void appendCustomStats(OperationContext* opCtx,
                                   BSONObjBuilder* result,
                                   double scale) const;

    virtual Status touch(OperationContext* opCtx, BSONObjBuilder* output) const;

    const RecordStoreV1MetaData* details() const {
        return _details.get();
    }

    // This keeps track of cursors saved during yielding, for invalidation purposes.
    SavedCursorRegistry savedCursors;

    DiskLoc getExtentLocForRecord(OperationContext* opCtx, const DiskLoc& loc) const;

    DiskLoc getNextRecord(OperationContext* opCtx, const DiskLoc& loc) const;
    DiskLoc getPrevRecord(OperationContext* opCtx, const DiskLoc& loc) const;

    DiskLoc getNextRecordInExtent(OperationContext* opCtx, const DiskLoc& loc) const;
    DiskLoc getPrevRecordInExtent(OperationContext* opCtx, const DiskLoc& loc) const;

    /**
     * Quantize 'minSize' to the nearest allocation size.
     */
    static int quantizeAllocationSpace(int minSize);

    static bool isQuantized(int recordSize);

    /* return which "deleted bucket" for this size object */
    static int bucket(int size);

    void waitForAllEarlierOplogWritesToBeVisible(OperationContext* opCtx) const override {}

    virtual void updateStatsAfterRepair(OperationContext* opCtx,
                                        long long numRecords,
                                        long long dataSize) {
        invariant(false);  // MMAPv1 has its own repair which doesn't call this.
    }

protected:
    virtual MmapV1RecordHeader* recordFor(const DiskLoc& loc) const;

    const DeletedRecord* deletedRecordFor(const DiskLoc& loc) const;

    virtual bool isCapped() const = 0;

    virtual bool shouldPadInserts() const = 0;

    virtual StatusWith<DiskLoc> allocRecord(OperationContext* opCtx,
                                            int lengthWithHeaders,
                                            bool enforceQuota) = 0;

    // TODO: document, remove, what have you
    virtual void addDeletedRec(OperationContext* opCtx, const DiskLoc& dloc) = 0;

    // TODO: another sad one
    virtual DeletedRecord* drec(const DiskLoc& loc) const;

    // just a wrapper for _extentManager->getExtent( loc );
    Extent* _getExtent(OperationContext* opCtx, const DiskLoc& loc) const;

    DiskLoc _getExtentLocForRecord(OperationContext* opCtx, const DiskLoc& loc) const;

    DiskLoc _getNextRecord(OperationContext* opCtx, const DiskLoc& loc) const;
    DiskLoc _getPrevRecord(OperationContext* opCtx, const DiskLoc& loc) const;

    DiskLoc _getNextRecordInExtent(OperationContext* opCtx, const DiskLoc& loc) const;
    DiskLoc _getPrevRecordInExtent(OperationContext* opCtx, const DiskLoc& loc) const;

    /**
     * finds the first suitable DiskLoc for data
     * will return the DiskLoc of a newly created DeletedRecord
     */
    DiskLoc _findFirstSpot(OperationContext* opCtx, const DiskLoc& extDiskLoc, Extent* e);

    /** add a record to the end of the linked list chain within this extent.
        require: you must have already declared write intent for the record header.
    */
    void _addRecordToRecListInExtent(OperationContext* opCtx, MmapV1RecordHeader* r, DiskLoc loc);

    /**
     * internal
     * doesn't check inputs or change padding
     */
    StatusWith<RecordId> _insertRecord(OperationContext* opCtx,
                                       const char* data,
                                       int len,
                                       bool enforceQuota);

    std::unique_ptr<RecordStoreV1MetaData> _details;
    ExtentManager* _extentManager;
    bool _isSystemIndexes;

    friend class RecordStoreV1RepairCursor;
};

/**
 * Iterates over all records within a single extent.
 *
 * EOF at end of extent, even if there are more extents.
 */
class RecordStoreV1Base::IntraExtentIterator final : public RecordCursor {
public:
    IntraExtentIterator(OperationContext* opCtx,
                        DiskLoc start,
                        const RecordStoreV1Base* rs,
                        bool forward = true)
        : _opCtx(opCtx), _curr(start), _rs(rs), _forward(forward) {}

    boost::optional<Record> next() final;
    void invalidate(OperationContext* opCtx, const RecordId& dl) final;
    void save() final {}
    bool restore() final {
        return true;
    }
    void detachFromOperationContext() final {
        _opCtx = nullptr;
    }
    void reattachToOperationContext(OperationContext* opCtx) final {
        _opCtx = opCtx;
    }
    std::unique_ptr<RecordFetcher> fetcherForNext() const final;

private:
    virtual const MmapV1RecordHeader* recordFor(const DiskLoc& loc) const {
        return _rs->recordFor(loc);
    }

    void advance();

    OperationContext* _opCtx;
    DiskLoc _curr;
    const RecordStoreV1Base* _rs;
    bool _forward;
};
}
