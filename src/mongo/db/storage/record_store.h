/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/mutable/damage_vector.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/collection_scan_common.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/key_format.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

namespace mongo {

class Collection;
class CollectionPtr;
class MAdvise;
class OperationContext;
class RecordStore;

struct ValidateResults;
class ValidateAdaptor;

/**
 * The data items stored in a RecordStore.
 */
struct Record {
    RecordId id;
    RecordData data;
};

/**
 * Retrieves Records from a RecordStore.
 *
 * A cursor is constructed with a direction flag with the following effects:
 *      - The direction that next() moves.
 *      - If a restore cannot return to the saved position, cursors will be positioned on the
 *        closest position *after* the query in the direction of the scan.
 *
 * A cursor is tied to a transaction, such as the OperationContext or a WriteUnitOfWork
 * inside that context. Any cursor acquired inside a transaction is invalid outside
 * of that transaction, instead use the save and restore methods to reestablish the cursor.
 *
 * Any method other than the save method may throw WriteConflictException. If that happens, the
 * cursor may not be used again until it has been saved and successfully restored. If next() or
 * restore() throw a WCE the cursor's position will be the same as before the call (strong exception
 * guarantee). All other methods leave the cursor in a valid state but with an unspecified position
 * (basic exception guarantee). If any exception other than WCE is thrown, the cursor must be
 * destroyed, which is guaranteed not to leak any resources.
 *
 * Any returned unowned BSON is only valid until the next call to any method on this
 * interface.
 *
 * Implementations may override any default implementation if they can provide a more
 * efficient implementation.
 *
 * Any interface that performs writes must validate that we are not in 'readOnly' mode.
 *
 * Storage engines only need to implement the derived SeekableRecordCursor, but may choose
 * to implement this simpler interface for cursors used for repair or random traversal.
 *
 * IMPORTANT NOTE FOR DOCUMENT-LOCKING ENGINES: If you implement capped collections with a
 * "visibility" system such that documents that exist in your snapshot but were inserted after
 * the last uncommitted document are hidden, you must follow the following rules:
 *   - next() on forward cursors must never return invisible documents.
 *   - If next() on a forward cursor hits an invisible document, it should behave as if it hit
 *     the end of the collection.
 *   - Reverse cursors must ignore the visibility filter. That means that they initially return the
 *     newest committed record in the collection and may skip over uncommitted records.
 *   - SeekableRecordCursor::seek() and SeekableRecordCursor::seekExact() on forward cursors must
 *     never return invisible documents.
 * TODO SERVER-18934 Handle this above the storage engine layer so storage engines don't have to
 * deal with capped visibility.
 */
class RecordCursor {
public:
    virtual ~RecordCursor() = default;

    /**
     * Moves forward and returns the new data or boost::none if there is no more data.
     * Continues returning boost::none once it reaches EOF unlike stl iterators.
     * Returns records in RecordId order.
     */
    virtual boost::optional<Record> next() = 0;

    //
    // Saving and restoring state
    //

    /**
     * Prepares for state changes in underlying data in a way that allows the cursor's
     * current position to be restored.
     *
     * It is safe to call save multiple times in a row.
     * No other method (excluding destructor) may be called until successfully restored.
     */
    virtual void save() = 0;

    /**
     * Recovers from potential state changes in underlying data.
     *
     * Returns false if it is invalid to continue using this Cursor. This usually means that
     * capped deletes have caught up to the position of this Cursor and continuing could
     * result in missed data. Note that Cursors, unlike iterators can continue to iterate past the
     * "end".
     *
     * If the former position no longer exists, but it is safe to continue iterating, the
     * following call to next() will return the next closest position in the direction of the
     * scan, if any.
     *
     * This handles restoring after either save() or SeekableRecordCursor::saveUnpositioned().
     *
     * 'tolerateCappedRepositioning' allows repositioning a capped cursor, which is useful for
     * range writes.
     */
    virtual bool restore(bool tolerateCappedRepositioning = true) = 0;

    /**
     * Detaches from the OperationContext. Releases storage-engine resources, unless
     * setSaveStorageCursorOnDetachFromOperationContext() has been set to true.
     */
    virtual void detachFromOperationContext() = 0;

    /**
     * Reattaches to the OperationContext and reacquires any storage-engine state if necessary.
     *
     * It is only legal to call this in the "detached" state. On return, the cursor may still be a
     * "saved" state if there was a prior call to save(). In this case, callers must still call
     * restore() to use this object.
     */
    virtual void reattachToOperationContext(OperationContext* opCtx) = 0;

    /**
     * Toggles behavior on whether to give up the underlying storage cursor (and any record pointed
     * to by it) on detachFromOperationContext(). This supports the query layer retaining valid and
     * positioned cursors across commands.
     */
    virtual void setSaveStorageCursorOnDetachFromOperationContext(bool) = 0;
};

/**
 * Adds explicit seeking of records. This functionality is separated out from RecordCursor, because
 * some cursors are not required to support seeking. All storage engines must support detecting the
 * existence of Records.
 */
class SeekableRecordCursor : public RecordCursor {
public:
    /**
     * Tells bounded 'seek' whether the bound excludes or includes the bound 'start'.
     */
    enum class BoundInclusion {
        kExclude,
        kInclude,
    };

    /**
     * Seeks to a Record with the provided bound 'start'.
     *
     * For forward cursors, 'start' is a lower bound.
     * For reverse cursors, 'start' is an upper bound.
     *
     * When 'boundInclusion' is 'kInclusive', positions the cursor at 'start' or the next record,
     * if one exists. When 'boundInclusion' is 'kExclusive', positions the cursor at the first
     * record after 'start', if one exists.
     *
     * Returns the Record at the cursor or boost::none if no matching records are found.
     */
    virtual boost::optional<Record> seek(const RecordId& start, BoundInclusion boundInclusion) = 0;

    /**
     * Seeks to a Record with the provided id.
     *
     * If an exact match can't be found, boost::none will be returned and the resulting position
     * of the cursor is unspecified.
     */
    virtual boost::optional<Record> seekExact(const RecordId& id) = 0;

    /**
     * Positions this cursor near 'start' or an adjacent record if 'start' does not exist. If there
     * is not an exact match, the cursor is positioned on the directionally previous Record. If no
     * earlier record exists, the cursor is positioned on the directionally following record.
     * Returns boost::none if the RecordStore is empty.
     *
     * For forward cursors, returns the Record with the highest RecordId less than or equal to
     * 'start'. If no such record exists, positions on the next highest RecordId after 'start'.
     *
     * For reverse cursors, returns the Record with the lowest RecordId greater than or equal to
     * 'start'. If no such record exists, positions on the next lowest RecordId before 'start'.
     *
     * Note: Only supported on capped collections.
     */
    virtual boost::optional<Record> seekNear(const RecordId& start) = 0;

    /**
     * Prepares for state changes in underlying data without necessarily saving the current
     * state.
     *
     * The cursor's position when restored is unspecified. Caller is expected to seek rather
     * than call next() following the restore.
     *
     * It is safe to call saveUnpositioned multiple times in a row.
     * No other method (excluding destructor) may be called until successfully restored.
     */
    virtual void saveUnpositioned() {
        save();
    }

    virtual uint64_t getCheckpointId() const {
        uasserted(ErrorCodes::CommandNotSupported,
                  "The current storage engine does not support checkpoint ids");
    }
};

/**
 * Queries with the awaitData option use this notifier object to wait for more data to be
 * inserted into the capped collection.
 */
class CappedInsertNotifier {
public:
    /**
     * Wakes up all threads waiting.
     */
    void notifyAll() const;

    /**
     * Waits until 'deadline', or until notifyAll() is called to indicate that new
     * data is available in the capped collection.
     *
     * NOTE: Waiting threads can be signaled by calling kill or notify* methods.
     */
    void waitUntil(OperationContext* opCtx, uint64_t prevVersion, Date_t deadline) const;

    /**
     * Returns the version for use as an additional wake condition when used above.
     */
    uint64_t getVersion() const;

    /**
     * Cancels the notifier if the collection is dropped/invalidated, and wakes all waiting.
     */
    void kill();

    /**
     * Returns true if no new insert notification will occur.
     */
    bool isDead();

private:
    // Signalled when a successful insert is made into a capped collection.
    mutable stdx::condition_variable _notifier;

    // Mutex used with '_notifier'. Protects access to '_version'.
    mutable Mutex _mutex = MONGO_MAKE_LATCH("CappedInsertNotifier::_mutex");

    // A counter, incremented on insertion of new data into the capped collection.
    //
    // The condition which '_cappedNewDataNotifier' is being notified of is an increment of this
    // counter. Access to this counter is synchronized with '_mutex'.
    mutable uint64_t _version = 0;

    // True once the notifier is dead.
    bool _dead = false;
};

/**
 * An abstraction used for storing documents in a collection or entries in an index.
 *
 * In storage engines implementing the KVEngine, record stores are also used for implementing
 * catalogs.
 *
 * Many methods take an OperationContext parameter. This contains the RecoveryUnit, with
 * all RecordStore specific transaction information, as well as the LockState. Methods that take
 * an OperationContext may throw a WriteConflictException.
 *
 * This class must be thread-safe. In addition, for storage engines implementing the KVEngine some
 * methods must be thread safe, see DurableCatalog.
 */
class RecordStore {
    RecordStore(const RecordStore&) = delete;
    RecordStore& operator=(const RecordStore&) = delete;

public:
    RecordStore(boost::optional<UUID> uuid, StringData identName, bool isCapped);
    virtual ~RecordStore() {}

    // META

    // name of the RecordStore implementation
    virtual const char* name() const = 0;

    boost::optional<UUID> uuid() const {
        return _uuid;
    }

    bool isTemp() const {
        return !_uuid.has_value();
    }

    std::shared_ptr<Ident> getSharedIdent() const {
        return _ident;
    }

    const std::string& getIdent() const {
        return _ident->getIdent();
    }

    void setIdent(std::shared_ptr<Ident> newIdent) {
        _ident = std::move(newIdent);
    }

    /**
     * Get the namespace this RecordStore is associated with.
     */
    virtual NamespaceString ns(OperationContext* opCtx) const = 0;

    /**
     * The key format for this RecordStore's RecordIds.
     *
     * Clustered collections may use the String format, however most
     * RecordStores use Long. RecordStores with the String format require callers to provide
     * RecordIds and will not generate them automatically.
     */
    virtual KeyFormat keyFormat() const = 0;

    /**
     * The dataSize is an approximation of the sum of the sizes (in bytes) of the
     * documents or entries in the recordStore.
     */
    virtual long long dataSize(OperationContext* opCtx) const = 0;

    /**
     * Total number of records in the RecordStore. You may need to cache it, so this call
     * takes constant time, as it is called often.
     */
    virtual long long numRecords(OperationContext* opCtx) const = 0;

    /**
     * Storage engines can manage oplog truncation internally as opposed to having higher layers
     * manage it for them.
     */
    virtual bool selfManagedOplogTruncation() const {
        return false;
    }

    /**
     * Get a pointer to a capped insert notifier object. The caller can wait on this object
     * until it is notified of a new insert into the capped collection.
     *
     * It is invalid to call this method unless the owning collection is capped.
     */
    std::shared_ptr<CappedInsertNotifier> getCappedInsertNotifier() const {
        return _cappedInsertNotifier;
    }

    /**
     * Uses the reference counter of the capped insert notifier shared pointer to decide whether
     * anyone is waiting in order to optimise notifications on a potentially hot path. It is
     * acceptable for this function to return 'true' even if there are no more waiters, but the
     * inverse is not allowed.
     */
    bool haveCappedWaiters() const;

    /**
     * If the record store is capped and there are listeners waiting for notifications for capped
     * inserts, notifies them.
     */
    void notifyCappedWaitersIfNeeded();

    /**
     * @param extraInfo - optional more debug info
     * @param level - optional, level of debug info to put in (higher is more)
     * @return total estimate size (in bytes) on stable storage
     */
    virtual int64_t storageSize(OperationContext* opCtx,
                                BSONObjBuilder* extraInfo = nullptr,
                                int infoLevel = 0) const = 0;

    /**
     * @return file bytes available for reuse
     * A return value of zero can mean either no bytes are available, or that the real value is
     * unknown.
     */
    virtual int64_t freeStorageSize(OperationContext* opCtx) const {
        return 0;
    }

    // CRUD related

    /**
     * Get the RecordData at loc, which must exist.
     *
     * If unowned data is returned, it is only valid until either of these happens:
     *  - The record is modified
     *  - The snapshot from which it was obtained is abandoned
     *  - The lock on the collection is released
     *
     * In general, prefer findRecord or RecordCursor::seekExact since they can tell you if a record
     * has been removed.
     */
    RecordData dataFor(OperationContext* opCtx, const RecordId& loc) const {
        RecordData data;
        invariant(findRecord(opCtx, loc, &data),
                  str::stream() << "Didn't find RecordId " << loc << " in record store "
                                << ns(opCtx).toStringForErrorMsg());
        return data;
    }

    /**
     * @param out - If the record exists, the contents of this are set.
     * @return true iff there is a Record for loc
     *
     * If unowned data is returned, it is valid until the next modification of this Record or
     * the lock on this collection is released.
     *
     * In general prefer RecordCursor::seekExact since it can avoid copying data in more
     * storageEngines.
     */
    virtual bool findRecord(OperationContext* opCtx, const RecordId& loc, RecordData* out) const {
        auto cursor = getCursor(opCtx);
        auto record = cursor->seekExact(loc);
        if (!record)
            return false;

        record->data.makeOwned();  // Unowned data expires when cursor goes out of scope.
        *out = std::move(record->data);
        return true;
    }

    void deleteRecord(OperationContext* opCtx, const RecordId& dl);

    /**
     * Inserts the specified records into this RecordStore by copying the passed-in record data and
     * updates 'inOutRecords' to contain the ids of the inserted records.
     */
    Status insertRecords(OperationContext* opCtx,
                         std::vector<Record>* inOutRecords,
                         const std::vector<Timestamp>& timestamps);

    /**
     * A thin wrapper around insertRecords() to simplify handling of single document inserts.
     */
    StatusWith<RecordId> insertRecord(OperationContext* opCtx,
                                      const char* data,
                                      int len,
                                      Timestamp timestamp) {
        // Record stores with the Long key format accept a null RecordId, as the storage engine will
        // generate one.
        invariant(keyFormat() == KeyFormat::Long);
        return insertRecord(opCtx, RecordId(), data, len, timestamp);
    }

    /**
     * A thin wrapper around insertRecords() to simplify handling of single document inserts.
     * If RecordId is null, the storage engine will generate one and return it.
     */
    StatusWith<RecordId> insertRecord(OperationContext* opCtx,
                                      const RecordId& rid,
                                      const char* data,
                                      int len,
                                      Timestamp timestamp) {
        std::vector<Record> inOutRecords{Record{rid, RecordData(data, len)}};
        Status status = insertRecords(opCtx, &inOutRecords, std::vector<Timestamp>{timestamp});
        if (!status.isOK())
            return status;
        return std::move(inOutRecords.front().id);
    }

    /**
     * Updates the record with id 'recordId', replacing its contents with those described by
     * 'data' and 'len'.
     */
    Status updateRecord(OperationContext* opCtx,
                        const RecordId& recordId,
                        const char* data,
                        int len);

    /**
     * @return Returns 'false' if this record store does not implement
     * 'updatewithDamages'. If this method returns false, 'updateWithDamages' must not be
     * called, and all updates must be routed through 'updateRecord' above. This allows the
     * update framework to avoid doing the work of damage tracking if the underlying record
     * store cannot utilize that information.
     */
    virtual bool updateWithDamagesSupported() const = 0;

    /**
     * Updates the record positioned at 'loc' in-place using the deltas described by 'damages'. The
     * 'damages' vector describes contiguous ranges of 'damageSource' from which to copy and apply
     * byte-level changes to the data. Behavior is undefined for calling this on a non-existant loc.
     *
     * @return the updated version of the record. If unowned data is returned, then it is valid
     * until the next modification of this Record or the lock on the collection has been released.
     */
    StatusWith<RecordData> updateWithDamages(OperationContext* opCtx,
                                             const RecordId& loc,
                                             const RecordData& oldRec,
                                             const char* damageSource,
                                             const mutablebson::DamageVector& damages);

    /**
     * Prints any storage engine provided metadata for the record with 'recordId'.
     *
     * If provided, saves any valid timestamps (startTs, startDurableTs, stopTs, stopDurableTs)
     * related to this record in 'recordTimestamps'.
     */
    virtual void printRecordMetadata(OperationContext* opCtx,
                                     const RecordId& recordId,
                                     std::set<Timestamp>* recordTimestamps) const = 0;

    /**
     * Returns a new cursor over this record store.
     *
     * The cursor is logically positioned before the first (or last if !forward) Record in the
     * collection so that Record will be returned on the first call to next(). Implementations
     * are allowed to lazily seek to the first Record when next() is called rather than doing
     * it on construction.
     */
    virtual std::unique_ptr<SeekableRecordCursor> getCursor(OperationContext* opCtx,
                                                            bool forward = true) const = 0;

    /**
     * Constructs a cursor over a record store that returns documents in a randomized order, and
     * allows storage engines to provide a more efficient way of random sampling of a record store
     * than MongoDB's default sampling methods, which is used when this method returns {}.
     *
     * This method may be implemented using a pseudo-random walk over B-trees or a similar approach.
     * Different cursors should return documents in a different order. Random cursors may return
     * the same document more than once and, as a result, may return more documents than exist in
     * the record store. Implementations should avoid obvious biases toward older, newer, larger
     * smaller or other specific classes of documents.
     */
    virtual std::unique_ptr<RecordCursor> getRandomCursor(OperationContext* opCtx) const {
        return {};
    }

    // higher level

    /**
     * Removes all Records.
     */
    Status truncate(OperationContext* opCtx);

    /**
     * Removes all Records in the range [minRecordId, maxRecordId] inclusive of both. The hint*
     * arguments serve as a hint to the record store of how much data will be truncated. This is
     * necessary for some implementations to avoid reading the data between the two RecordIds in
     * order to update numRecords and dataSize correctly. Implementations are free to ignore the
     * hints if they have a way of obtaining the correct values without the help of external
     * callers.
     */
    Status rangeTruncate(OperationContext* opCtx,
                         const RecordId& minRecordId = RecordId(),
                         const RecordId& maxRecordId = RecordId(),
                         int64_t hintDataSizeIncrement = 0,
                         int64_t hintNumRecordsIncrement = 0);

    /**
     * Truncate documents newer than the document at 'end' from the capped
     * collection.  The collection cannot be completely emptied using this
     * function.  An assertion will be thrown if that is attempted.
     * @param inclusive - Truncate 'end' as well iff true
     */
    using AboutToDeleteRecordCallback =
        std::function<void(OperationContext* opCtx, const RecordId& loc, RecordData data)>;
    void cappedTruncateAfter(OperationContext* opCtx,
                             const RecordId& end,
                             bool inclusive,
                             const AboutToDeleteRecordCallback& aboutToDelete);

    /**
     * does this RecordStore support the compact operation?
     *
     * If you return true, you must provide implementations of all compact methods.
     */
    virtual bool compactSupported() const {
        return false;
    }

    /**
     * If compact() supports online compaction.
     *
     * Only called if compactSupported() returns true.
     */
    virtual bool supportsOnlineCompaction() const {
        MONGO_UNREACHABLE;
    }

    /**
     * Attempt to reduce the storage space used by this RecordStore. If the freeSpaceTargetMB is
     * provided, compaction will only proceed if the free storage space available is greater than
     * the provided value.
     *
     * Only called if compactSupported() returns true.
     */
    Status compact(OperationContext* opCtx, boost::optional<int64_t> freeSpaceTargetMB);

    /**
     * Performs record store specific validation to ensure consistency of underlying data
     * structures. If corruption is found, details of the errors will be in the results parameter.
     */
    virtual void validate(OperationContext* opCtx, bool full, ValidateResults* results) {}

    /**
     * @param scaleSize - amount by which to scale size metrics
     * Appends any numeric custom stats from the RecordStore or other unique stats, it should
     * avoid any expensive calls
     */
    virtual void appendNumericCustomStats(OperationContext* opCtx,
                                          BSONObjBuilder* result,
                                          double scale) const = 0;


    /**
     * @param scaleSize - amount by which to scale size metrics
     * Appends all custom stats from the RecordStore or other unique stats, it can be more
     * expensive than RecordStore::appendNumericCustomStats
     */
    virtual void appendAllCustomStats(OperationContext* opCtx,
                                      BSONObjBuilder* result,
                                      double scale) const {
        appendNumericCustomStats(opCtx, result, scale);
    };

    /**
     * When we write to an oplog, we call this so that that the storage engine can manage the
     * visibility of oplog entries to ensure they are ordered.
     *
     * Since this is called inside of a WriteUnitOfWork while holding a std::mutex, it is
     * illegal to acquire any LockManager locks inside of this function.
     *
     * If `orderedCommit` is true, the storage engine can assume the input `opTime` has become
     * visible in the oplog. Otherwise the storage engine must continue to maintain its own
     * visibility management. Calls with `orderedCommit` true will not be concurrent with calls of
     * `orderedCommit` false.
     */
    Status oplogDiskLocRegister(OperationContext* opCtx,
                                const Timestamp& opTime,
                                bool orderedCommit);

    /**
     * Waits for all writes that completed before this call to be visible to forward scans.
     * See the comment on RecordCursor for more details about the visibility rules.
     *
     * It is only legal to call this on an oplog. It is illegal to call this inside a
     * WriteUnitOfWork.
     */
    void waitForAllEarlierOplogWritesToBeVisible(OperationContext* opCtx) const;

    /**
     * Returns the largest RecordId in the RecordStore, regardless of visibility rules. If the store
     * is empty, returns a null RecordId.
     *
     * May throw WriteConflictException in certain cache-stuck scenarios even if the operation isn't
     * part of a WriteUnitOfWork.
     */
    virtual RecordId getLargestKey(OperationContext* opCtx) const = 0;

    /**
     * Reserve a range of contiguous RecordIds. Returns the first valid RecordId in the range. Must
     * only be called on a RecordStore with KeyFormat::Long.
     */
    virtual void reserveRecordIds(OperationContext* opCtx,
                                  std::vector<RecordId>* out,
                                  size_t nRecords) = 0;

    /**
     * Called after a repair operation is run with the recomputed numRecords and dataSize.
     */
    virtual void updateStatsAfterRepair(OperationContext* opCtx,
                                        long long numRecords,
                                        long long dataSize) = 0;

    /**
     * Storage engines can choose whether to support changing the oplog size online.
     */
    virtual Status updateOplogSize(OperationContext* opCtx, long long newOplogSize) {
        return Status(ErrorCodes::CommandNotSupported,
                      "This storage engine does not support updateOplogSize");
    }

    /**
     * Returns false if the oplog was dropped while waiting for a deletion request.
     * This should only be called if StorageEngine::supportsOplogTruncateMarkers() is true.
     * Storage engines supporting oplog truncate markers must implement this function.
     */
    virtual bool yieldAndAwaitOplogDeletionRequest(OperationContext* opCtx) {
        MONGO_UNREACHABLE;
    }

    /**
     * This should only be called if StorageEngine::supportsOplogTruncateMarkers() is true.
     * Storage engines supporting oplog truncate markers must implement this function.
     */
    virtual void reclaimOplog(OperationContext* opCtx) {
        MONGO_UNREACHABLE;
    }

    /**
     * This should only be called if StorageEngine::supportsOplogTruncateMarkers() is true.
     * Storage engines supporting oplog truncate markers must implement this function.
     * Populates `builder` with various statistics pertaining to oplog truncate markers and oplog
     * truncation.
     */
    virtual void getOplogTruncateStats(BSONObjBuilder& builder) const {
        MONGO_UNREACHABLE;
    }

    /**
     * If supported, this method returns the timestamp value for the latest storage engine committed
     * oplog document. Note that this method should not be called within a UnitOfWork.
     *
     * If there is an active transaction, that transaction is used and its snapshot determines
     * visibility. Otherwise, a new transaction will be created and destroyed to service this call.
     *
     * Unsupported RecordStores return the OplogOperationUnsupported error code.
     */
    virtual StatusWith<Timestamp> getLatestOplogTimestamp(OperationContext* opCtx) const {
        return Status(ErrorCodes::OplogOperationUnsupported,
                      "The current storage engine doesn't support an optimized implementation for "
                      "getting the latest oplog timestamp.");
    }

    /**
     * If supported, this method returns the timestamp value for the earliest storage engine
     * committed oplog document.
     *
     * Unsupported RecordStores return the OplogOperationUnsupported error code.
     */
    virtual StatusWith<Timestamp> getEarliestOplogTimestamp(OperationContext* opCtx) {
        return Status(ErrorCodes::OplogOperationUnsupported,
                      "The current storage engine doesn't support an optimized implementation for "
                      "getting the earliest oplog timestamp.");
    }

protected:
    // Functions derived classes need to override to implement this interface. Any write needs to be
    // first checked so we are not in read only mode in this base class and then redirected to the
    // derived class if allowed to perform the write.
    virtual void doDeleteRecord(OperationContext* opCtx, const RecordId& dl) = 0;
    virtual Status doInsertRecords(OperationContext* opCtx,
                                   std::vector<Record>* inOutRecords,
                                   const std::vector<Timestamp>& timestamps) = 0;
    virtual Status doUpdateRecord(OperationContext* opCtx,
                                  const RecordId& recordId,
                                  const char* data,
                                  int len) = 0;
    virtual StatusWith<RecordData> doUpdateWithDamages(
        OperationContext* opCtx,
        const RecordId& loc,
        const RecordData& oldRec,
        const char* damageSource,
        const mutablebson::DamageVector& damages) = 0;
    virtual Status doTruncate(OperationContext* opCtx) = 0;
    virtual Status doRangeTruncate(OperationContext* opCtx,
                                   const RecordId& minRecordId,
                                   const RecordId& maxRecordId,
                                   int64_t hintDataSizeDiff,
                                   int64_t hintNumRecordsDiff) = 0;
    virtual void doCappedTruncateAfter(OperationContext* opCtx,
                                       const RecordId& end,
                                       bool inclusive,
                                       const AboutToDeleteRecordCallback& aboutToDelete) = 0;
    virtual Status doCompact(OperationContext* opCtx, boost::optional<int64_t> freeSpaceTargetMB) {
        MONGO_UNREACHABLE;
    }

    virtual Status oplogDiskLocRegisterImpl(OperationContext* opCtx,
                                            const Timestamp& opTime,
                                            bool orderedCommit) {
        return Status::OK();
    }

    virtual void waitForAllEarlierOplogWritesToBeVisibleImpl(OperationContext* opCtx) const = 0;

    std::shared_ptr<Ident> _ident;

    boost::optional<UUID> _uuid;

    std::shared_ptr<CappedInsertNotifier> _cappedInsertNotifier;
};

}  // namespace mongo
