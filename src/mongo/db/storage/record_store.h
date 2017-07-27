// record_store.h

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

#pragma once

#include <boost/optional.hpp>

#include "mongo/base/owned_pointer_vector.h"
#include "mongo/bson/mutable/damage_vector.h"
#include "mongo/db/exec/collection_scan_common.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/record_fetcher.h"

namespace mongo {

class CappedCallback;
class Collection;
struct CompactOptions;
struct CompactStats;
class MAdvise;
class NamespaceDetails;
class OperationContext;
class RecordFetcher;

class RecordStoreCompactAdaptor;
class RecordStore;

struct ValidateResults;
class ValidateAdaptor;

/**
 * Allows inserting a Record "in-place" without creating a copy ahead of time.
 */
class DocWriter {
public:
    virtual void writeDocument(char* buf) const = 0;
    virtual size_t documentSize() const = 0;
    virtual bool addPadding() const {
        return true;
    }

protected:
    // Can't delete through base pointer.
    ~DocWriter() = default;
};

/**
 * @see RecordStore::updateRecord
 */
class UpdateNotifier {
public:
    virtual ~UpdateNotifier() {}
    virtual Status recordStoreGoingToUpdateInPlace(OperationContext* opCtx,
                                                   const RecordId& loc) = 0;
};

/**
 * The data items stored in a RecordStore.
 */
struct Record {
    RecordId id;
    RecordData data;
};

struct BsonRecord {
    RecordId id;
    const BSONObj* docPtr;
};

enum ValidateCmdLevel : int {
    kValidateIndex = 0x01,
    kValidateRecordStore = 0x02,
    kValidateFull = 0x03
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
 * Any method other than invalidate and the save methods may throw WriteConflictException. If
 * that happens, the cursor may not be used again until it has been saved and successfully
 * restored. If next() or restore() throw a WCE the cursor's position will be the same as before
 * the call (strong exception guarantee). All other methods leave the cursor in a valid state
 * but with an unspecified position (basic exception guarantee). If any exception other than
 * WCE is thrown, the cursor must be destroyed, which is guaranteed not to leak any resources.
 *
 * Any returned unowned BSON is only valid until the next call to any method on this
 * interface.
 *
 * Implementations may override any default implementation if they can provide a more
 * efficient implementation.
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
 *   - SeekableRecordCursor::seekExact() must ignore the visibility filter and return the requested
 *     document even if it is supposed to be invisible.
 * TODO SERVER-18934 Handle this above the storage engine layer so storage engines don't have to
 * deal with capped visibility.
 */
class RecordCursor {
public:
    virtual ~RecordCursor() = default;

    /**
     * Moves forward and returns the new data or boost::none if there is no more data.
     * Continues returning boost::none once it reaches EOF.
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
     * Returns false if it is invalid to continue using this iterator. This usually means that
     * capped deletes have caught up to the position of this iterator and continuing could
     * result in missed data.
     *
     * If the former position no longer exists, but it is safe to continue iterating, the
     * following call to next() will return the next closest position in the direction of the
     * scan, if any.
     *
     * This handles restoring after either save() or SeekableRecordCursor::saveUnpositioned().
     */
    virtual bool restore() = 0;

    /**
     * Detaches from the OperationContext and releases any storage-engine state.
     *
     * It is only legal to call this when in a "saved" state. While in the "detached" state, it is
     * only legal to call reattachToOperationContext or the destructor. It is not legal to call
     * detachFromOperationContext() while already in the detached state.
     */
    virtual void detachFromOperationContext() = 0;

    /**
     * Reattaches to the OperationContext and reacquires any storage-engine state.
     *
     * It is only legal to call this in the "detached" state. On return, the cursor is left in a
     * "saved" state, so callers must still call restoreState to use this object.
     */
    virtual void reattachToOperationContext(OperationContext* opCtx) = 0;

    /**
     * Inform the cursor that this id is being invalidated. Must be called between save and restore.
     * The opCtx is that of the operation causing the invalidation, not the opCtx using the cursor.
     *
     * WARNING: Storage engines other than MMAPv1 should use the default implementation,
     *          and not depend on this being called.
     */
    virtual void invalidate(OperationContext* opCtx, const RecordId& id) {}

    //
    // RecordFetchers
    //
    // Storage engines which do not support document-level locking hold locks at collection or
    // database granularity. As an optimization, these locks can be yielded when a record needs
    // to be fetched from secondary storage. If this method returns non-NULL, then it indicates
    // that the query system layer should yield its locks, following the protocol defined by the
    // RecordFetcher class, so that a potential page fault is triggered out of the lock.
    //
    // Storage engines which support document-level locking need not implement this.
    //
    // TODO see if these can be replaced by WriteConflictException.
    //

    /**
     * Returns a RecordFetcher if needed for a call to next() or none if unneeded.
     */
    virtual std::unique_ptr<RecordFetcher> fetcherForNext() const {
        return {};
    }
};

/**
 * Adds explicit seeking of records. This functionality is separated out from RecordCursor,
 * because some cursors, such as repair cursors, are not required to support seeking.
 *
 * Warning: MMAPv1 cannot detect if RecordIds are valid. Therefore callers should only pass
 * potentially deleted RecordIds to seek methods if they know that MMAPv1 is not the current
 * storage engine. All new storage engines must support detecting the existence of Records.
 *
 */
class SeekableRecordCursor : public RecordCursor {
public:
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

    /**
     * Returns a RecordFetcher if needed to fetch the provided Record or none if unneeded.
     */
    virtual std::unique_ptr<RecordFetcher> fetcherForId(const RecordId& id) const {
        return {};
    }
};

/**
 * A RecordStore provides an abstraction used for storing documents in a collection,
 * or entries in an index. In storage engines implementing the KVEngine, record stores
 * are also used for implementing catalogs.
 *
 * Many methods take an OperationContext parameter. This contains the RecoveryUnit, with
 * all RecordStore specific transaction information, as well as the LockState. Methods that take
 * an OperationContext may throw a WriteConflictException.
 *
 * This class must be thread-safe for document-level locking storage engines. In addition, for
 * storage engines implementing the KVEngine some methods must be thread safe, see KVCatalog. Only
 * for MMAPv1 is this class not thread-safe.
 */
class RecordStore {
    MONGO_DISALLOW_COPYING(RecordStore);

public:
    RecordStore(StringData ns) : _ns(ns.toString()) {}

    virtual ~RecordStore() {}

    // META

    // name of the RecordStore implementation
    virtual const char* name() const = 0;

    virtual const std::string& ns() const {
        return _ns;
    }

    /**
     * The dataSize is an approximation of the sum of the sizes (in bytes) of the
     * documents or entries in the recordStore.
     */
    virtual long long dataSize(OperationContext* opCtx) const = 0;

    /**
     * Total number of record in the RecordStore. You may need to cache it, so this call
     * takes constant time, as it is called often.
     */
    virtual long long numRecords(OperationContext* opCtx) const = 0;

    virtual bool isCapped() const = 0;

    virtual void setCappedCallback(CappedCallback*) {
        invariant(false);
    }

    /**
     * @param extraInfo - optional more debug info
     * @param level - optional, level of debug info to put in (higher is more)
     * @return total estimate size (in bytes) on stable storage
     */
    virtual int64_t storageSize(OperationContext* opCtx,
                                BSONObjBuilder* extraInfo = NULL,
                                int infoLevel = 0) const = 0;

    // CRUD related

    /**
     * Get the RecordData at loc, which must exist.
     *
     * If unowned data is returned, it is valid until the next modification of this Record or
     * the lock on this collection is released.
     *
     * In general, prefer findRecord or RecordCursor::seekExact since they can tell you if a
     * record has been removed.
     */
    virtual RecordData dataFor(OperationContext* opCtx, const RecordId& loc) const {
        RecordData data;
        invariant(findRecord(opCtx, loc, &data));
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
     *
     * Warning: MMAPv1 cannot detect if RecordIds are valid. Therefore callers should only pass
     * potentially deleted RecordIds to seek methods if they know that MMAPv1 is not the current
     * storage engine. All new storage engines must support detecting the existence of Records.
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

    virtual void deleteRecord(OperationContext* opCtx, const RecordId& dl) = 0;

    virtual StatusWith<RecordId> insertRecord(OperationContext* opCtx,
                                              const char* data,
                                              int len,
                                              bool enforceQuota) = 0;

    virtual Status insertRecords(OperationContext* opCtx,
                                 std::vector<Record>* records,
                                 bool enforceQuota) {
        for (auto& record : *records) {
            StatusWith<RecordId> res =
                insertRecord(opCtx, record.data.data(), record.data.size(), enforceQuota);
            if (!res.isOK())
                return res.getStatus();

            record.id = res.getValue();
        }
        return Status::OK();
    }

    /**
     * Inserts nDocs documents into this RecordStore using the DocWriter interface.
     *
     * This allows the storage engine to reserve space for a record and have it built in-place
     * rather than building the record then copying it into its destination.
     *
     * On success, if idsOut is non-null the RecordIds of the inserted records will be written into
     * it. It must have space for nDocs RecordIds.
     */
    virtual Status insertRecordsWithDocWriter(OperationContext* opCtx,
                                              const DocWriter* const* docs,
                                              size_t nDocs,
                                              RecordId* idsOut = nullptr) = 0;

    /**
     * A thin wrapper around insertRecordsWithDocWriter() to simplify handling of single DocWriters.
     */
    StatusWith<RecordId> insertRecordWithDocWriter(OperationContext* opCtx, const DocWriter* doc) {
        RecordId out;
        Status status = insertRecordsWithDocWriter(opCtx, &doc, 1, &out);
        if (!status.isOK())
            return status;
        return out;
    }

    /**
     * @param notifier - Only used by record stores which do not support doc-locking. Called only
     *                   in the case of an in-place update. Called just before the in-place write
     *                   occurs.
     * @return Status  - If a document move is required (MMAPv1 only) then a status of
     *                   ErrorCodes::NeedsDocumentMove will be returned. On receipt of this status
     *                   no update will be performed. It is the caller's responsibility to:
     *                     1. Remove the existing document and associated index keys.
     *                     2. Insert a new document and index keys.
     *
     * For capped record stores, the record size will never change.
     */
    virtual Status updateRecord(OperationContext* opCtx,
                                const RecordId& oldLocation,
                                const char* data,
                                int len,
                                bool enforceQuota,
                                UpdateNotifier* notifier) = 0;

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
     * byte-level changes to the data.
     *
     * @return the updated version of the record. If unowned data is returned, then it is valid
     * until the next modification of this Record or the lock on the collection has been released.
     */
    virtual StatusWith<RecordData> updateWithDamages(OperationContext* opCtx,
                                                     const RecordId& loc,
                                                     const RecordData& oldRec,
                                                     const char* damageSource,
                                                     const mutablebson::DamageVector& damages) = 0;

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
     * Constructs a cursor over a potentially corrupted store, which can be used to salvage
     * damaged records. The iterator might return every record in the store if all of them
     * are reachable and not corrupted.  Returns NULL if not supported.
     */
    virtual std::unique_ptr<RecordCursor> getCursorForRepair(OperationContext* opCtx) const {
        return {};
    }

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

    /**
     * Returns many RecordCursors that partition the RecordStore into many disjoint sets.
     * Iterating all returned RecordCursors is equivalent to iterating the full store.
     */
    virtual std::vector<std::unique_ptr<RecordCursor>> getManyCursors(
        OperationContext* opCtx) const {
        std::vector<std::unique_ptr<RecordCursor>> out(1);
        out[0] = getCursor(opCtx);
        return out;
    }

    // higher level


    /**
     * removes all Records
     */
    virtual Status truncate(OperationContext* opCtx) = 0;

    /**
     * Truncate documents newer than the document at 'end' from the capped
     * collection.  The collection cannot be completely emptied using this
     * function.  An assertion will be thrown if that is attempted.
     * @param inclusive - Truncate 'end' as well iff true
     */
    virtual void cappedTruncateAfter(OperationContext* opCtx, RecordId end, bool inclusive) = 0;

    /**
     * does this RecordStore support the compact operation?
     *
     * If you return true, you must provide implementations of all compact methods.
     */
    virtual bool compactSupported() const {
        return false;
    }

    /**
     * Does compact() leave RecordIds alone or can they change.
     *
     * Only called if compactSupported() returns true.
     */
    virtual bool compactsInPlace() const {
        invariant(false);
    }

    /**
     * Attempt to reduce the storage space used by this RecordStore.
     *
     * Only called if compactSupported() returns true.
     * No RecordStoreCompactAdaptor will be passed if compactsInPlace() returns true.
     */
    virtual Status compact(OperationContext* opCtx,
                           RecordStoreCompactAdaptor* adaptor,
                           const CompactOptions* options,
                           CompactStats* stats) {
        invariant(false);
    }

    /**
     * Does the RecordStore cursor retrieve its document in RecordId Order?
     *
     * If a subclass overrides the default value to true, the RecordStore cursor must retrieve
     * its documents in RecordId order.
     *
     * This enables your storage engine to run collection validation in the
     * background.
     */
    virtual bool isInRecordIdOrder() const {
        return false;
    }

    /**
     * @return OK if the validate run successfully
     *         OK will be returned even if corruption is found
     *         deatils will be in result
     */
    virtual Status validate(OperationContext* opCtx,
                            ValidateCmdLevel level,
                            ValidateAdaptor* adaptor,
                            ValidateResults* results,
                            BSONObjBuilder* output) = 0;

    /**
     * @param scaleSize - amount by which to scale size metrics
     * appends any custom stats from the RecordStore or other unique stats
     */
    virtual void appendCustomStats(OperationContext* opCtx,
                                   BSONObjBuilder* result,
                                   double scale) const = 0;

    /**
     * Load all data into cache.
     * What cache depends on implementation.
     *
     * If the underlying storage engine does not support the operation,
     * returns ErrorCodes::CommandNotSupported
     *
     * @param output (optional) - where to put detailed stats
     */
    virtual Status touch(OperationContext* opCtx, BSONObjBuilder* output) const {
        return Status(ErrorCodes::CommandNotSupported,
                      "this storage engine does not support touch");
    }

    /**
     * Return the RecordId of an oplog entry as close to startingPosition as possible without
     * being higher. If there are no entries <= startingPosition, return RecordId().
     *
     * If you don't implement the oplogStartHack, just use the default implementation which
     * returns boost::none.
     */
    virtual boost::optional<RecordId> oplogStartHack(OperationContext* opCtx,
                                                     const RecordId& startingPosition) const {
        return boost::none;
    }

    /**
     * When we write to an oplog, we call this so that if the storage engine
     * supports doc locking, it can manage the visibility of oplog entries to ensure
     * they are ordered.
     *
     * Since this is called inside of a WriteUnitOfWork while holding a std::mutex, it is
     * illegal to acquire any LockManager locks inside of this function.
     */
    virtual Status oplogDiskLocRegister(OperationContext* opCtx, const Timestamp& opTime) {
        return Status::OK();
    }

    /**
     * Waits for all writes that completed before this call to be visible to forward scans.
     * See the comment on RecordCursor for more details about the visibility rules.
     *
     * It is only legal to call this on an oplog. It is illegal to call this inside a
     * WriteUnitOfWork.
     */
    virtual void waitForAllEarlierOplogWritesToBeVisible(OperationContext* opCtx) const = 0;

    /**
     * Called after a repair operation is run with the recomputed numRecords and dataSize.
     */
    virtual void updateStatsAfterRepair(OperationContext* opCtx,
                                        long long numRecords,
                                        long long dataSize) = 0;

    /**
     * used to support online change oplog size.
     */
    virtual Status updateCappedSize(OperationContext* opCtx, long long cappedSize) {
        return Status(ErrorCodes::CommandNotSupported,
                      "this storage engine does not support updateCappedSize");
    }

protected:
    std::string _ns;
};

class RecordStoreCompactAdaptor {
public:
    virtual ~RecordStoreCompactAdaptor() {}
    virtual bool isDataValid(const RecordData& recData) = 0;
    virtual size_t dataSize(const RecordData& recData) = 0;
    virtual void inserted(const RecordData& recData, const RecordId& newLocation) = 0;
};

struct ValidateResults {
    ValidateResults() {
        valid = true;
    }
    bool valid;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

/**
 * This is so when a RecordStore is validating all records
 * it can call back to someone to check if a record is valid.
 * The actual data contained in a Record is totally opaque to the implementation.
 */
class ValidateAdaptor {
public:
    virtual ~ValidateAdaptor() {}

    virtual Status validate(const RecordId& recordId,
                            const RecordData& recordData,
                            size_t* dataSize) = 0;
};
}
