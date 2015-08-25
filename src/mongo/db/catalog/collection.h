// collection.h

/**
*    Copyright (C) 2012-2014 MongoDB Inc.
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

#include <cstdint>
#include <memory>
#include <string>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/mutable/damage_vector.h"
#include "mongo/db/catalog/collection_info_cache.h"
#include "mongo/db/catalog/cursor_manager.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/exec/collection_scan_common.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/capped_callback.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"

namespace mongo {

class CollectionCatalogEntry;
class DatabaseCatalogEntry;
class ExtentManager;
class IndexCatalog;
class MatchExpression;
class MultiIndexBlock;
class OpDebug;
class OperationContext;
class RecordCursor;
class RecordFetcher;
class UpdateDriver;
class UpdateRequest;

struct CompactOptions {
    CompactOptions() {
        paddingMode = NONE;
        validateDocuments = true;
        paddingFactor = 1;
        paddingBytes = 0;
    }

    // padding
    enum PaddingMode { PRESERVE, NONE, MANUAL } paddingMode;

    // only used if _paddingMode == MANUAL
    double paddingFactor;  // what to multiple document size by
    int paddingBytes;      // what to add to ducment size after multiplication
    unsigned computeRecordSize(unsigned recordSize) const {
        recordSize = static_cast<unsigned>(paddingFactor * recordSize);
        recordSize += paddingBytes;
        return recordSize;
    }

    // other
    bool validateDocuments;

    std::string toString() const;
};

struct CompactStats {
    CompactStats() {
        corruptDocuments = 0;
    }

    long long corruptDocuments;
};

/**
 * Queries with the awaitData option use this notifier object to wait for more data to be
 * inserted into the capped collection.
 */
class CappedInsertNotifier {
public:
    CappedInsertNotifier();

    /**
     * Wakes up threads waiting on this object for the arrival of new data.
     */
    void notifyOfInsert();

    /**
     * Get a counter value which is incremented on every insert into a capped collection.
     * The return value should be used as a reference value to pass into waitForCappedInsert().
     */
    uint64_t getCount() const;

    /**
     * Waits for 'timeout' microseconds, or until notifyAll() is called to indicate that new
     * data is available in the capped collection.
     */
    void waitForInsert(uint64_t referenceCount, Microseconds timeout) const;

private:
    // Signalled when a successful insert is made into a capped collection.
    mutable stdx::condition_variable _cappedNewDataNotifier;

    // Mutex used with '_cappedNewDataNotifier'. Protects access to '_cappedInsertCount'.
    mutable stdx::mutex _cappedNewDataMutex;

    // A counter, incremented on insertion of new data into the capped collection.
    //
    // The condition which '_cappedNewDataNotifier' is being notified of is an increment of this
    // counter. Access to this counter is synchronized with '_cappedNewDataMutex'.
    uint64_t _cappedInsertCount;
};

/**
 * this is NOT safe through a yield right now
 * not sure if it will be, or what yet
 */
class Collection : CappedDocumentDeleteCallback, UpdateNotifier {
public:
    Collection(OperationContext* txn,
               StringData fullNS,
               CollectionCatalogEntry* details,  // does not own
               RecordStore* recordStore,         // does not own
               DatabaseCatalogEntry* dbce);      // does not own

    ~Collection();

    bool ok() const {
        return _magic == 1357924;
    }

    CollectionCatalogEntry* getCatalogEntry() {
        return _details;
    }
    const CollectionCatalogEntry* getCatalogEntry() const {
        return _details;
    }

    CollectionInfoCache* infoCache() {
        return &_infoCache;
    }
    const CollectionInfoCache* infoCache() const {
        return &_infoCache;
    }

    const NamespaceString& ns() const {
        return _ns;
    }

    const IndexCatalog* getIndexCatalog() const {
        return &_indexCatalog;
    }
    IndexCatalog* getIndexCatalog() {
        return &_indexCatalog;
    }

    const RecordStore* getRecordStore() const {
        return _recordStore;
    }
    RecordStore* getRecordStore() {
        return _recordStore;
    }

    CursorManager* getCursorManager() const {
        return &_cursorManager;
    }

    bool requiresIdIndex() const;

    Snapshotted<BSONObj> docFor(OperationContext* txn, const RecordId& loc) const;

    /**
     * @param out - contents set to the right docs if exists, or nothing.
     * @return true iff loc exists
     */
    bool findDoc(OperationContext* txn, const RecordId& loc, Snapshotted<BSONObj>* out) const;

    std::unique_ptr<RecordCursor> getCursor(OperationContext* txn, bool forward = true) const;

    /**
     * Returns many cursors that partition the Collection into many disjoint sets. Iterating
     * all returned cursors is equivalent to iterating the full collection.
     */
    std::vector<std::unique_ptr<RecordCursor>> getManyCursors(OperationContext* txn) const;

    void deleteDocument(OperationContext* txn,
                        const RecordId& loc,
                        bool cappedOK = false,
                        bool noWarn = false,
                        BSONObj* deletedId = 0);

    /**
     * this does NOT modify the doc before inserting
     * i.e. will not add an _id field for documents that are missing it
     *
     * If enforceQuota is false, quotas will be ignored.
     */
    StatusWith<RecordId> insertDocument(OperationContext* txn,
                                        const BSONObj& doc,
                                        bool enforceQuota,
                                        bool fromMigrate = false);

    /**
     * Callers must ensure no document validation is performed for this collection when calling
     * this method.
     */
    StatusWith<RecordId> insertDocument(OperationContext* txn,
                                        const DocWriter* doc,
                                        bool enforceQuota);

    StatusWith<RecordId> insertDocument(OperationContext* txn,
                                        const BSONObj& doc,
                                        MultiIndexBlock* indexBlock,
                                        bool enforceQuota);

    /**
     * updates the document @ oldLocation with newDoc
     * if the document fits in the old space, it is put there
     * if not, it is moved
     * @return the post update location of the doc (may or may not be the same as oldLocation)
     */
    StatusWith<RecordId> updateDocument(OperationContext* txn,
                                        const RecordId& oldLocation,
                                        const Snapshotted<BSONObj>& oldDoc,
                                        const BSONObj& newDoc,
                                        bool enforceQuota,
                                        bool indexesAffected,
                                        OpDebug* debug,
                                        oplogUpdateEntryArgs& args);

    bool updateWithDamagesSupported() const;

    /**
     * Not allowed to modify indexes.
     * Illegal to call if updateWithDamagesSupported() returns false.
     * @return the contents of the updated record.
     */
    StatusWith<RecordData> updateDocumentWithDamages(OperationContext* txn,
                                                     const RecordId& loc,
                                                     const Snapshotted<RecordData>& oldRec,
                                                     const char* damageSource,
                                                     const mutablebson::DamageVector& damages,
                                                     oplogUpdateEntryArgs& args);

    // -----------

    StatusWith<CompactStats> compact(OperationContext* txn, const CompactOptions* options);

    /**
     * removes all documents as fast as possible
     * indexes before and after will be the same
     * as will other characteristics
     */
    Status truncate(OperationContext* txn);

    /**
     * @param full - does more checks
     * @param scanData - scans each document
     * @return OK if the validate run successfully
     *         OK will be returned even if corruption is found
     *         deatils will be in result
     */
    Status validate(OperationContext* txn,
                    bool full,
                    bool scanData,
                    ValidateResults* results,
                    BSONObjBuilder* output);

    /**
     * forces data into cache
     */
    Status touch(OperationContext* txn,
                 bool touchData,
                 bool touchIndexes,
                 BSONObjBuilder* output) const;

    /**
     * Truncate documents newer than the document at 'end' from the capped
     * collection.  The collection cannot be completely emptied using this
     * function.  An assertion will be thrown if that is attempted.
     * @param inclusive - Truncate 'end' as well iff true
     * XXX: this will go away soon, just needed to move for now
     */
    void temp_cappedTruncateAfter(OperationContext* txn, RecordId end, bool inclusive);

    /**
     * Sets the validator for this collection.
     *
     * An empty validator removes all validation.
     * Requires an exclusive lock on the collection.
     */
    Status setValidator(OperationContext* txn, BSONObj validator);

    Status setValidationLevel(OperationContext* txn, StringData newLevel);
    Status setValidationAction(OperationContext* txn, StringData newAction);

    StringData getValidationLevel() const;
    StringData getValidationAction() const;

    // -----------

    //
    // Stats
    //

    bool isCapped() const;

    /**
     * Get a pointer to a capped insert notifier object. The caller can wait on this object
     * until it is notified of a new insert into the capped collection.
     *
     * It is invalid to call this method unless the collection is capped.
     */
    std::shared_ptr<CappedInsertNotifier> getCappedInsertNotifier() const;

    uint64_t numRecords(OperationContext* txn) const;

    uint64_t dataSize(OperationContext* txn) const;

    int averageObjectSize(OperationContext* txn) const {
        uint64_t n = numRecords(txn);
        if (n == 0)
            return 5;
        return static_cast<int>(dataSize(txn) / n);
    }

    uint64_t getIndexSize(OperationContext* opCtx, BSONObjBuilder* details = NULL, int scale = 1);

    /**
     * If return value is not boost::none, reads with majority read concern using an older snapshot
     * must error.
     */
    boost::optional<SnapshotName> getMinimumVisibleSnapshot() {
        return _minVisibleSnapshot;
    }

    void setMinimumVisibleSnapshot(SnapshotName name) {
        _minVisibleSnapshot = name;
    }

private:
    /**
     * Returns a non-ok Status if document does not pass this collection's validator.
     */
    Status checkValidation(OperationContext* txn, const BSONObj& document) const;

    /**
     * Returns a non-ok Status if validator is not legal for this collection.
     */
    StatusWithMatchExpression parseValidator(const BSONObj& validator) const;

    Status recordStoreGoingToMove(OperationContext* txn,
                                  const RecordId& oldLocation,
                                  const char* oldBuffer,
                                  size_t oldSize);

    Status recordStoreGoingToUpdateInPlace(OperationContext* txn, const RecordId& loc);

    Status aboutToDeleteCapped(OperationContext* txn, const RecordId& loc, RecordData data);

    /**
     * same semantics as insertDocument, but doesn't do:
     *  - some user error checks
     *  - adjust padding
     */
    StatusWith<RecordId> _insertDocument(OperationContext* txn,
                                         const BSONObj& doc,
                                         bool enforceQuota);

    bool _enforceQuota(bool userEnforeQuota) const;

    int _magic;

    NamespaceString _ns;
    CollectionCatalogEntry* _details;
    RecordStore* _recordStore;
    DatabaseCatalogEntry* _dbce;
    CollectionInfoCache _infoCache;
    IndexCatalog _indexCatalog;

    // Empty means no filter.
    BSONObj _validatorDoc;
    // Points into _validatorDoc. Null means no filter.
    std::unique_ptr<MatchExpression> _validator;
    enum ValidationAction { WARN, ERROR_V } _validationAction;
    enum ValidationLevel { OFF, MODERATE, STRICT_V } _validationLevel;

    static StatusWith<ValidationLevel> _parseValidationLevel(StringData);
    static StatusWith<ValidationAction> _parseValidationAction(StringData);

    // this is mutable because read only users of the Collection class
    // use it keep state.  This seems valid as const correctness of Collection
    // should be about the data.
    mutable CursorManager _cursorManager;

    // Notifier object for awaitData. Threads polling a capped collection for new data can wait
    // on this object until notified of the arrival of new data.
    //
    // This is non-null if and only if the collection is a capped collection.
    std::shared_ptr<CappedInsertNotifier> _cappedNotifier;

    const bool _mustTakeCappedLockOnInsert;

    // The earliest snapshot that is allowed to use this collection.
    boost::optional<SnapshotName> _minVisibleSnapshot;

    friend class Database;
    friend class IndexCatalog;
    friend class NamespaceDetails;
};
}
