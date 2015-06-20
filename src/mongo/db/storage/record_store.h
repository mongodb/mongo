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

class CappedDocumentDeleteCallback;
class Collection;
struct CompactOptions;
struct CompactStats;
class DocWriter;
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
    virtual ~DocWriter() {}
    virtual void writeDocument(char* buf) const = 0;
    virtual size_t documentSize() const = 0;
    virtual bool addPadding() const {
        return true;
    }
};

/**
 * @see RecordStore::updateRecord
 */
class UpdateNotifier {
public:
    virtual ~UpdateNotifier() {}
    virtual Status recordStoreGoingToMove(OperationContext* txn,
                                          const RecordId& oldLocation,
                                          const char* oldBuffer,
                                          size_t oldSize) = 0;
    virtual Status recordStoreGoingToUpdateInPlace(OperationContext* txn, const RecordId& loc) = 0;
};

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
 * Any method other than invalidate and the save methods may throw WriteConflict exception. If
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
    // Seeking
    //
    // Warning: MMAPv1 cannot detect if RecordIds are valid. Therefore callers should only pass
    // potentially deleted RecordIds to seek methods if they know that MMAPv1 is not the current
    // storage engine. All new storage engines must support detecting the existence of Records.
    //

    /**
     * Seeks to a Record with the provided id.
     *
     * If an exact match can't be found, boost::none will be returned and the resulting position
     * of the cursor is unspecified.
     */
    virtual boost::optional<Record> seekExact(const RecordId& id) = 0;

    //
    // Saving and restoring state
    //

    /**
     * Prepares for state changes in underlying data in a way that allows the cursor's
     * current position to be restored.
     *
     * It is safe to call savePositioned multiple times in a row.
     * No other method (excluding destructor) may be called until successfully restored.
     */
    virtual void savePositioned() = 0;

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
        savePositioned();
    }

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
     * This handles restoring after either savePositioned() or saveUnpositioned().
     */
    virtual bool restore(OperationContext* txn) = 0;

    /**
     * Inform the cursor that this id is being invalidated.
     * Must be called between save and restore.
     *
     * WARNING: Storage engines other than MMAPv1 should not depend on this being called.
     */
    virtual void invalidate(const RecordId& id){};

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
    virtual long long dataSize(OperationContext* txn) const = 0;

    /**
     * Total number of record in the RecordStore. You may need to cache it, so this call
     * takes constant time, as it is called often.
     */
    virtual long long numRecords(OperationContext* txn) const = 0;

    virtual bool isCapped() const = 0;

    virtual void setCappedDeleteCallback(CappedDocumentDeleteCallback*) {
        invariant(false);
    }

    /**
     * @param extraInfo - optional more debug info
     * @param level - optional, level of debug info to put in (higher is more)
     * @return total estimate size (in bytes) on stable storage
     */
    virtual int64_t storageSize(OperationContext* txn,
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
    virtual RecordData dataFor(OperationContext* txn, const RecordId& loc) const {
        RecordData data;
        invariant(findRecord(txn, loc, &data));
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
    virtual bool findRecord(OperationContext* txn, const RecordId& loc, RecordData* out) const {
        auto cursor = getCursor(txn);
        auto record = cursor->seekExact(loc);
        if (!record)
            return false;

        record->data.makeOwned();  // Unowned data expires when cursor goes out of scope.
        *out = std::move(record->data);
        return true;
    }

    virtual void deleteRecord(OperationContext* txn, const RecordId& dl) = 0;

    virtual StatusWith<RecordId> insertRecord(OperationContext* txn,
                                              const char* data,
                                              int len,
                                              bool enforceQuota) = 0;

    virtual StatusWith<RecordId> insertRecord(OperationContext* txn,
                                              const DocWriter* doc,
                                              bool enforceQuota) = 0;

    /**
     * @param notifier - Only used by record stores which do not support doc-locking.
     *                   In the case of a document move, this is called after the document
     *                   has been written to the new location, but before it is deleted from
     *                   the old location.
     *                   In the case of an in-place update, this is called just before the
     *                   in-place write occurs.
     * @return Status or RecordId, RecordId might be different
     */
    virtual StatusWith<RecordId> updateRecord(OperationContext* txn,
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

    virtual Status updateWithDamages(OperationContext* txn,
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
    virtual std::unique_ptr<RecordCursor> getCursor(OperationContext* txn,
                                                    bool forward = true) const = 0;

    /**
     * Constructs a cursor over a potentially corrupted store, which can be used to salvage
     * damaged records. The iterator might return every record in the store if all of them
     * are reachable and not corrupted.  Returns NULL if not supported.
     *
     * Repair cursors are only required to support forward scanning, so it is illegal to call
     * seekExact() on the returned cursor.
     */
    virtual std::unique_ptr<RecordCursor> getCursorForRepair(OperationContext* txn) const {
        return {};
    }

    /**
     * Returns many RecordCursors that partition the RecordStore into many disjoint sets.
     * Iterating all returned RecordCursors is equivalent to iterating the full store.
     *
     * Partition cursors are only required to support forward scanning, so it is illegal to call
     * seekExact() on any of the returned cursors.
     *
     * WARNING: the first call to restore() on each cursor may (but is not guaranteed to) be on
     * a different RecoveryUnit than the initial save. This will be made more sane as part of
     * SERVER-17364.
     */
    virtual std::vector<std::unique_ptr<RecordCursor>> getManyCursors(OperationContext* txn) const {
        std::vector<std::unique_ptr<RecordCursor>> out(1);
        out[0] = getCursor(txn);
        return out;
    }

    // higher level


    /**
     * removes all Records
     */
    virtual Status truncate(OperationContext* txn) = 0;

    /**
     * Truncate documents newer than the document at 'end' from the capped
     * collection.  The collection cannot be completely emptied using this
     * function.  An assertion will be thrown if that is attempted.
     * @param inclusive - Truncate 'end' as well iff true
     * XXX: this will go away soon, just needed to move for now
     */
    virtual void temp_cappedTruncateAfter(OperationContext* txn, RecordId end, bool inclusive) = 0;

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
    virtual Status compact(OperationContext* txn,
                           RecordStoreCompactAdaptor* adaptor,
                           const CompactOptions* options,
                           CompactStats* stats) {
        invariant(false);
    }

    /**
     * @param full - does more checks
     * @param scanData - scans each document
     * @return OK if the validate run successfully
     *         OK will be returned even if corruption is found
     *         deatils will be in result
     */
    virtual Status validate(OperationContext* txn,
                            bool full,
                            bool scanData,
                            ValidateAdaptor* adaptor,
                            ValidateResults* results,
                            BSONObjBuilder* output) = 0;

    /**
     * @param scaleSize - amount by which to scale size metrics
     * appends any custom stats from the RecordStore or other unique stats
     */
    virtual void appendCustomStats(OperationContext* txn,
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
    virtual Status touch(OperationContext* txn, BSONObjBuilder* output) const {
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
    virtual boost::optional<RecordId> oplogStartHack(OperationContext* txn,
                                                     const RecordId& startingPosition) const {
        return boost::none;
    }

    /**
     * When we write to an oplog, we call this so that if the storage engine
     * supports doc locking, it can manage the visibility of oplog entries to ensure
     * they are ordered.
     */
    virtual Status oplogDiskLocRegister(OperationContext* txn, const Timestamp& opTime) {
        return Status::OK();
    }

    /**
     * Called after a repair operation is run with the recomputed numRecords and dataSize.
     */
    virtual void updateStatsAfterRepair(OperationContext* txn,
                                        long long numRecords,
                                        long long dataSize) = 0;

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
};

/**
 * This is so when a RecordStore is validating all records
 * it can call back to someone to check if a record is valid.
 * The actual data contained in a Record is totally opaque to the implementation.
 */
class ValidateAdaptor {
public:
    virtual ~ValidateAdaptor() {}

    virtual Status validate(const RecordData& recordData, size_t* dataSize) = 0;
};
}
