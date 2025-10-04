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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/compact_options.h"
#include "mongo/db/storage/container.h"
#include "mongo/db/storage/damage_vector.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/key_format.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class OperationContext;
class RecoveryUnit;
class ValidateResults;

namespace CollectionValidation {
class ValidationOptions;
}

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
    virtual bool restore(RecoveryUnit& ru, bool tolerateCappedRepositioning = true) = 0;

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
    mutable stdx::mutex _mutex;

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
 * methods must be thread safe, see MDBCatalog.
 */
class RecordStore {
public:
    class Capped;
    class Oplog;
    using RecordStoreContainer = std::variant<std::reference_wrapper<IntegerKeyedContainer>,
                                              std::reference_wrapper<StringKeyedContainer>>;

    /**
     * Options for generating a new RecordStore. Each RecordStore subclass is responsible for
     * parsing its applicable fields - not all fields apply to every RecordStore implementation.
     */
    struct Options {
        /**
         * The KeyFormat for RecordIds in the RecordStore.
         */
        KeyFormat keyFormat{KeyFormat::Long};

        /**
         * True if the RecordStore is for a capped collection.
         */
        bool isCapped{false};

        /*
         * True if the RecordStore is for the oplog collection.
         */
        bool isOplog{false};

        /**
         * The initial maximum size an Oplog's RecordStore should reach. Non-zero value is only
         * valid when 'isOplog' is true.
         */
        long long oplogMaxSize{0};

        /**
         * Whether or not the RecordStore allows allows writes to overwrite existing records with
         * the same RecordId.
         */
        bool allowOverwrite{true};

        /**
         * True if updates through the RecordStore must force updates to the full document.
         */
        bool forceUpdateWithFullDocument{false};

        /**
         * When not none, defines a block compression algorithm to use in liu of the default for
         * RecordStores which support block compression. Otherwise, the RecordStore should utilize
         * the default block compressor.
         */
        boost::optional<std::string> customBlockCompressor;

        /**
         * Empty by default. Holds collection-specific storage engine configuration options. For
         * example, the 'storageEngine' options passed into `db.createCollection()`. Expected to be
         * mirror the 'CollectionOptions::storageEngine' format { storageEngine: { <storage engine
         * name> : { configString: "<option>=<setting>,..."} } }.
         *
         * If fields in the 'configString' conflict with fields set either by global defaults or
         * other members of the 'RecordStore::Options' struct, RecordStores should prefer values
         * from the 'configString'. However, this is difficult to guarantee across RecordStores, and
         * any concerns should be validated through explicit testing.
         */
        BSONObj storageEngineCollectionOptions;
    };

    virtual ~RecordStore() {}

    virtual const char* name() const = 0;

    virtual boost::optional<UUID> uuid() const = 0;

    virtual bool isTemp() const = 0;

    virtual std::shared_ptr<Ident> getSharedIdent() const = 0;

    virtual StringData getIdent() const = 0;

    virtual void setIdent(std::shared_ptr<Ident>) = 0;

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
    virtual long long dataSize() const = 0;

    /**
     * Total number of records in the RecordStore. You may need to cache it, so this call
     * takes constant time, as it is called often.
     */
    virtual long long numRecords() const = 0;

    /**
     * @param extraInfo - optional more debug info
     * @param level - optional, level of debug info to put in (higher is more)
     * @return total estimate size (in bytes) on stable storage
     */
    virtual int64_t storageSize(RecoveryUnit&,
                                BSONObjBuilder* extraInfo = nullptr,
                                int infoLevel = 0) const = 0;

    /**
     * @return file bytes available for reuse
     * A return value of zero can mean either no bytes are available, or that the real value is
     * unknown.
     */
    virtual int64_t freeStorageSize(RecoveryUnit&) const = 0;

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
    virtual RecordData dataFor(OperationContext*, RecoveryUnit&, const RecordId&) const = 0;

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
    virtual bool findRecord(OperationContext*,
                            RecoveryUnit&,
                            const RecordId&,
                            RecordData*) const = 0;

    virtual void deleteRecord(OperationContext* opCtx, RecoveryUnit&, const RecordId& dl) = 0;

    /**
     * Inserts the specified records into this RecordStore by copying the passed-in record data and
     * updates 'inOutRecords' to contain the ids of the inserted records.
     */
    virtual Status insertRecords(OperationContext*,
                                 RecoveryUnit&,
                                 std::vector<Record>*,
                                 const std::vector<Timestamp>&) = 0;

    /**
     * A thin wrapper around insertRecords() to simplify handling of single document inserts.
     */
    virtual StatusWith<RecordId> insertRecord(
        OperationContext*, RecoveryUnit&, const char* data, int len, Timestamp) = 0;

    /**
     * A thin wrapper around insertRecords() to simplify handling of single document inserts.
     * If RecordId is null, the storage engine will generate one and return it.
     */
    virtual StatusWith<RecordId> insertRecord(OperationContext*,
                                              RecoveryUnit&,
                                              const RecordId&,
                                              const char* data,
                                              int len,
                                              Timestamp) = 0;

    /**
     * Updates the record with id 'recordId', replacing its contents with those described by
     * 'data' and 'len'.
     */
    virtual Status updateRecord(
        OperationContext*, RecoveryUnit&, const RecordId&, const char* data, int len) = 0;

    /**
     * @return Returns 'false' if this record store does not implement
     * 'updatewithDamages'. If this method returns false, 'updateWithDamages' must not be
     * called, and all updates must be routed through 'updateRecord' above. This allows the
     * update framework to avoid doing the work of damage tracking if the underlying record
     * store cannot utilize that information.
     */
    virtual bool updateWithDamagesSupported() const = 0;

    /**
     * Updates the record positioned at the provided id in-place using the deltas described by the
     * provided damages. The damages vector describes contiguous ranges of 'damageSource' from which
     * to copy and apply byte-level changes to the data. Behavior is undefined for calling this on a
     * non-existant loc.
     *
     * @return the updated version of the record. If unowned data is returned, then it is valid
     * until the next modification of this Record or the lock on the collection has been released.
     */
    virtual StatusWith<RecordData> updateWithDamages(OperationContext*,
                                                     RecoveryUnit&,
                                                     const RecordId&,
                                                     const RecordData&,
                                                     const char* damageSource,
                                                     const DamageVector&) = 0;

    /**
     * Prints any storage engine provided metadata for the record with 'recordId'.
     *
     * If provided, saves any valid timestamps (startTs, startDurableTs, stopTs, stopDurableTs)
     * related to this record in 'recordTimestamps'.
     */
    virtual void printRecordMetadata(const RecordId&, std::set<Timestamp>*) const = 0;

    /**
     * Returns a new cursor over this record store.
     *
     * The cursor is logically positioned before the first (or last if !forward) Record in the
     * collection so that Record will be returned on the first call to next(). Implementations
     * are allowed to lazily seek to the first Record when next() is called rather than doing
     * it on construction.
     */
    virtual std::unique_ptr<SeekableRecordCursor> getCursor(OperationContext*,
                                                            RecoveryUnit&,
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
    virtual std::unique_ptr<RecordCursor> getRandomCursor(OperationContext*,
                                                          RecoveryUnit&) const = 0;

    /**
     * Removes all Records.
     *
     * The operation context parameter is optional and, if non-null, will only be used to check the
     * "read-only" flag.
     */
    virtual Status truncate(OperationContext*, RecoveryUnit&) = 0;

    /**
     * Removes all Records in the range [minRecordId, maxRecordId] inclusive of both. The hint*
     * arguments serve as a hint to the record store of how much data will be truncated. This is
     * necessary for some implementations to avoid reading the data between the two RecordIds in
     * order to update numRecords and dataSize correctly. Implementations are free to ignore the
     * hints if they have a way of obtaining the correct values without the help of external
     * callers.
     *
     * The operation context parameter is optional and, if non-null, will only be used to check the
     * "read-only" flag.
     */
    virtual Status rangeTruncate(OperationContext*,
                                 RecoveryUnit&,
                                 const RecordId& minRecordId = RecordId(),
                                 const RecordId& maxRecordId = RecordId(),
                                 int64_t hintDataSizeIncrement = 0,
                                 int64_t hintNumRecordsIncrement = 0) = 0;

    /**
     * does this RecordStore support the compact operation?
     *
     * If you return true, you must provide implementations of all compact methods.
     */
    virtual bool compactSupported() const = 0;

    /**
     * Attempt to reduce the storage space used by this RecordStore.
     * Only called if compactSupported() returns true.
     * Returns an estimated number of bytes when doing a dry run.
     */
    virtual StatusWith<int64_t> compact(OperationContext*,
                                        RecoveryUnit&,
                                        const CompactOptions&) = 0;

    /**
     * Performs record store specific validation to ensure consistency of underlying data
     * structures. If corruption is found, details of the errors will be in the results parameter.
     */
    virtual void validate(RecoveryUnit&,
                          const CollectionValidation::ValidationOptions&,
                          ValidateResults*) = 0;

    /**
     * @param scaleSize - amount by which to scale size metrics
     * Appends any numeric custom stats from the RecordStore or other unique stats, it should
     * avoid any expensive calls
     */
    virtual void appendNumericCustomStats(RecoveryUnit&, BSONObjBuilder*, double scale) const = 0;


    /**
     * @param scaleSize - amount by which to scale size metrics
     * Appends all custom stats from the RecordStore or other unique stats, it can be more
     * expensive than RecordStore::appendNumericCustomStats
     */
    virtual void appendAllCustomStats(RecoveryUnit&, BSONObjBuilder*, double scale) const = 0;

    /**
     * Returns the largest RecordId in the RecordStore, regardless of visibility rules. If the store
     * is empty, returns a null RecordId.
     *
     * May throw WriteConflictException in certain cache-stuck scenarios even if the operation isn't
     * part of a WriteUnitOfWork.
     */
    virtual RecordId getLargestKey(OperationContext*, RecoveryUnit&) const = 0;

    /**
     * Reserve a range of contiguous RecordIds. Returns the first valid RecordId in the range. Must
     * only be called on a RecordStore with KeyFormat::Long.
     */
    virtual void reserveRecordIds(OperationContext*,
                                  RecoveryUnit&,
                                  std::vector<RecordId>*,
                                  size_t numRecords) = 0;

    /**
     * Called after a repair operation is run with the recomputed numRecords and dataSize.
     */
    virtual void updateStatsAfterRepair(long long numRecords, long long dataSize) = 0;

    /**
     * Returns nullptr if this record store is not capped.
     */
    virtual Capped* capped() = 0;

    /**
     * Returns nullptr if this record store is not the oplog.
     */
    virtual Oplog* oplog() = 0;

    /**
     * Returns the underlying container.
     */
    virtual RecordStoreContainer getContainer() = 0;
};

class RecordStore::Capped {
public:
    struct TruncateAfterResult {
        int64_t recordsRemoved = 0;
        int64_t bytesRemoved = 0;
        RecordId firstRemovedId;
    };

    /**
     * Get a pointer to a capped insert notifier object. The caller can wait on this object
     * until it is notified of a new insert into the capped collection.
     *
     * It is invalid to call this method unless the owning collection is capped.
     */
    virtual std::shared_ptr<CappedInsertNotifier> getInsertNotifier() const = 0;

    /**
     * Uses the reference counter of the capped insert notifier shared pointer to decide whether
     * anyone is waiting in order to optimise notifications on a potentially hot path. It is
     * acceptable for this function to return 'true' even if there are no more waiters, but the
     * inverse is not allowed.
     */
    virtual bool hasWaiters() const = 0;

    /**
     * If the record store is capped and there are listeners waiting for notifications for capped
     * inserts, notifies them.
     */
    virtual void notifyWaitersIfNeeded() = 0;

    /**
     * Truncate documents newer than the document at 'end' from the capped
     * collection.  The collection cannot be completely emptied using this
     * function.  An assertion will be thrown if that is attempted.
     * @param inclusive - Truncate 'end' as well iff true
     */
    virtual TruncateAfterResult truncateAfter(OperationContext*,
                                              RecoveryUnit&,
                                              const RecordId&,
                                              bool inclusive) = 0;
};

class RecordStore::Oplog {
public:
    /**
     * Storage engines can choose whether to support changing the oplog size online.
     */
    virtual Status updateSize(long long size) = 0;

    virtual int64_t getMaxSize() const = 0;

    /**
     * Returns a new cursor on the oplog, ignoring any visibility semantics specific to forward
     * cursors.
     */
    virtual std::unique_ptr<SeekableRecordCursor> getRawCursor(OperationContext* opCtx,
                                                               RecoveryUnit&,
                                                               bool forward = true) const = 0;
    /**
     * If supported, this method returns the timestamp value for the latest storage engine committed
     * oplog document. Note that this method should not be called within a UnitOfWork.
     *
     * If there is an active transaction, that transaction is used and its snapshot determines
     * visibility. Otherwise, a new transaction will be created and destroyed to service this call.
     *
     * Unsupported RecordStores return the OplogOperationUnsupported error code.
     */
    virtual StatusWith<Timestamp> getLatestTimestamp(RecoveryUnit&) const = 0;

    /**
     * If supported, this method returns the timestamp value for the earliest storage engine
     * committed oplog document.
     *
     * Unsupported RecordStores return the OplogOperationUnsupported error code.
     */
    virtual StatusWith<Timestamp> getEarliestTimestamp(RecoveryUnit&) = 0;
};

}  // namespace mongo
