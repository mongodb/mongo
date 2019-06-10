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

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/mutable/damage_vector.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/catalog/collection_info_cache.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/storage/capped_callback.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"

namespace mongo {
class CappedCallback;
class CollectionCatalogEntry;
class ExtentManager;
class IndexCatalog;
class IndexCatalogEntry;
class IndexDescriptor;
class DatabaseImpl;
class MatchExpression;
class OpDebug;
class OperationContext;
class RecordCursor;
class UpdateDriver;
class UpdateRequest;

/**
 * Holds information update an update operation.
 */
struct CollectionUpdateArgs {
    enum class StoreDocOption { None, PreImage, PostImage };

    StmtId stmtId = kUninitializedStmtId;

    // The document before modifiers were applied.
    boost::optional<BSONObj> preImageDoc;

    // Fully updated document with damages (update modifiers) applied.
    BSONObj updatedDoc;

    // Document containing update modifiers -- e.g. $set and $unset
    BSONObj update;

    // Document containing the _id field of the doc being updated.
    BSONObj criteria;

    // True if this update comes from a chunk migration.
    bool fromMigrate = false;

    StoreDocOption storeDocOption = StoreDocOption::None;
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
    void notifyAll();

    /**
     * Waits until 'deadline', or until notifyAll() is called to indicate that new
     * data is available in the capped collection.
     *
     * NOTE: Waiting threads can be signaled by calling kill or notify* methods.
     */
    void waitUntil(uint64_t prevVersion, Date_t deadline) const;

    /**
     * Returns the version for use as an additional wake condition when used above.
     */
    uint64_t getVersion() const {
        return _version;
    }

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
    uint64_t _version = 0;

    // True once the notifier is dead.
    bool _dead = false;
};

class Collection {
public:
    enum class StoreDeletedDoc { Off, On };

    /**
     * Direction of collection scan plan executor returned by makePlanExecutor().
     */
    enum class ScanDirection {
        kForward = 1,
        kBackward = -1,
    };

    /**
     * A Collection::Factory is a factory class that constructs Collection objects.
     */
    class Factory {
    public:
        Factory() = default;
        virtual ~Factory() = default;

        static Factory* get(ServiceContext* service);
        static Factory* get(OperationContext* opCtx);
        static void set(ServiceContext* service, std::unique_ptr<Factory> factory);

        /**
         * Constructs a Collection object. This does not persist any state to the storage engine,
         * only constructs an in-memory representation of what already exists on disk.
         */
        virtual std::unique_ptr<Collection> make(
            OperationContext* opCtx, CollectionCatalogEntry* collectionCatalogEntry) const = 0;
    };

    /**
     * Callback function for callers of insertDocumentForBulkLoader().
     */
    using OnRecordInsertedFn = std::function<Status(const RecordId& loc)>;

    Collection() = default;
    virtual ~Collection() = default;

    virtual bool ok() const = 0;

    virtual CollectionCatalogEntry* getCatalogEntry() = 0;
    virtual const CollectionCatalogEntry* getCatalogEntry() const = 0;

    virtual CollectionInfoCache* infoCache() = 0;
    virtual const CollectionInfoCache* infoCache() const = 0;

    virtual const NamespaceString& ns() const = 0;

    /**
     * Sets a new namespace on this Collection, in the case that the Collection is being renamed.
     * In general, reads and writes to Collection objects are synchronized using locks from the lock
     * manager. However, there is special synchronization for ns() and setNs() so that the
     * CollectionCatalog can perform UUID to namespace lookup without holding a Collection lock. See
     * CollectionCatalog::setCollectionNamespace().
     */
    virtual void setNs(NamespaceString nss) = 0;

    virtual OptionalCollectionUUID uuid() const = 0;

    virtual const IndexCatalog* getIndexCatalog() const = 0;
    virtual IndexCatalog* getIndexCatalog() = 0;

    virtual const RecordStore* getRecordStore() const = 0;
    virtual RecordStore* getRecordStore() = 0;

    virtual bool requiresIdIndex() const = 0;

    virtual Snapshotted<BSONObj> docFor(OperationContext* const opCtx, RecordId loc) const = 0;

    /**
     * @param out - contents set to the right docs if exists, or nothing.
     * @return true iff loc exists
     */
    virtual bool findDoc(OperationContext* const opCtx,
                         RecordId loc,
                         Snapshotted<BSONObj>* const out) const = 0;

    virtual std::unique_ptr<SeekableRecordCursor> getCursor(OperationContext* const opCtx,
                                                            const bool forward = true) const = 0;

    /**
     * Deletes the document with the given RecordId from the collection.
     *
     * 'fromMigrate' indicates whether the delete was induced by a chunk migration, and
     * so should be ignored by the user as an internal maintenance operation and not a
     * real delete.
     * 'loc' key to uniquely identify a record in a collection.
     * 'opDebug' Optional argument. When not null, will be used to record operation statistics.
     * 'noWarn' if unindexing the record causes an error, if noWarn is true the error
     * will not be logged.
     */
    virtual void deleteDocument(OperationContext* const opCtx,
                                StmtId stmtId,
                                RecordId loc,
                                OpDebug* const opDebug,
                                const bool fromMigrate = false,
                                const bool noWarn = false,
                                StoreDeletedDoc storeDeletedDoc = StoreDeletedDoc::Off) = 0;

    /*
     * Inserts all documents inside one WUOW.
     * Caller should ensure vector is appropriately sized for this.
     * If any errors occur (including WCE), caller should retry documents individually.
     *
     * 'opDebug' Optional argument. When not null, will be used to record operation statistics.
     */
    virtual Status insertDocuments(OperationContext* const opCtx,
                                   const std::vector<InsertStatement>::const_iterator begin,
                                   const std::vector<InsertStatement>::const_iterator end,
                                   OpDebug* const opDebug,
                                   const bool fromMigrate = false) = 0;

    /**
     * this does NOT modify the doc before inserting
     * i.e. will not add an _id field for documents that are missing it
     *
     * 'opDebug' Optional argument. When not null, will be used to record operation statistics.
     */
    virtual Status insertDocument(OperationContext* const opCtx,
                                  const InsertStatement& doc,
                                  OpDebug* const opDebug,
                                  const bool fromMigrate = false) = 0;

    /**
     * Callers must ensure no document validation is performed for this collection when calling
     * this method.
     */
    virtual Status insertDocumentsForOplog(OperationContext* const opCtx,
                                           const DocWriter* const* const docs,
                                           Timestamp* timestamps,
                                           const size_t nDocs) = 0;

    /**
     * Inserts a document into the record store for a bulk loader that manages the index building
     * outside this Collection. The bulk loader is notified with the RecordId of the document
     * inserted into the RecordStore.
     *
     * NOTE: It is up to caller to commit the indexes.
     */
    virtual Status insertDocumentForBulkLoader(OperationContext* const opCtx,
                                               const BSONObj& doc,
                                               const OnRecordInsertedFn& onRecordInserted) = 0;

    /**
     * Updates the document @ oldLocation with newDoc.
     *
     * If the document fits in the old space, it is put there; if not, it is moved.
     * Sets 'args.updatedDoc' to the updated version of the document with damages applied, on
     * success.
     * 'opDebug' Optional argument. When not null, will be used to record operation statistics.
     * @return the post update location of the doc (may or may not be the same as oldLocation)
     */
    virtual RecordId updateDocument(OperationContext* const opCtx,
                                    RecordId oldLocation,
                                    const Snapshotted<BSONObj>& oldDoc,
                                    const BSONObj& newDoc,
                                    const bool indexesAffected,
                                    OpDebug* const opDebug,
                                    CollectionUpdateArgs* const args) = 0;

    virtual bool updateWithDamagesSupported() const = 0;

    /**
     * Not allowed to modify indexes.
     * Illegal to call if updateWithDamagesSupported() returns false.
     * Sets 'args.updatedDoc' to the updated version of the document with damages applied, on
     * success.
     * @return the contents of the updated record.
     */
    virtual StatusWith<RecordData> updateDocumentWithDamages(
        OperationContext* const opCtx,
        RecordId loc,
        const Snapshotted<RecordData>& oldRec,
        const char* const damageSource,
        const mutablebson::DamageVector& damages,
        CollectionUpdateArgs* const args) = 0;

    // -----------

    /**
     * removes all documents as fast as possible
     * indexes before and after will be the same
     * as will other characteristics.
     *
     * The caller should hold a collection X lock and ensure there are no index builds in progress
     * on the collection.
     */
    virtual Status truncate(OperationContext* const opCtx) = 0;

    /**
     * @return OK if the validate run successfully
     *         OK will be returned even if corruption is found
     *         deatils will be in result.
     */
    virtual Status validate(OperationContext* const opCtx,
                            const ValidateCmdLevel level,
                            bool background,
                            ValidateResults* const results,
                            BSONObjBuilder* const output) = 0;

    /**
     * forces data into cache.
     */
    virtual Status touch(OperationContext* const opCtx,
                         const bool touchData,
                         const bool touchIndexes,
                         BSONObjBuilder* const output) const = 0;

    /**
     * Truncate documents newer than the document at 'end' from the capped
     * collection.  The collection cannot be completely emptied using this
     * function.  An assertion will be thrown if that is attempted.
     * @param inclusive - Truncate 'end' as well iff true
     *
     * The caller should hold a collection X lock and ensure there are no index builds in progress
     * on the collection.
     */
    virtual void cappedTruncateAfter(OperationContext* const opCtx,
                                     RecordId end,
                                     const bool inclusive) = 0;

    /**
     * Returns a non-ok Status if validator is not legal for this collection.
     */
    virtual StatusWithMatchExpression parseValidator(
        OperationContext* opCtx,
        const BSONObj& validator,
        MatchExpressionParser::AllowedFeatureSet allowedFeatures,
        boost::optional<ServerGlobalParams::FeatureCompatibility::Version>
            maxFeatureCompatibilityVersion) const = 0;

    static Status parseValidationLevel(StringData level);
    static Status parseValidationAction(StringData action);

    /**
     * Sets the validator for this collection.
     *
     * An empty validator removes all validation.
     * Requires an exclusive lock on the collection.
     */
    virtual Status setValidator(OperationContext* const opCtx, const BSONObj validator) = 0;

    virtual Status setValidationLevel(OperationContext* const opCtx, const StringData newLevel) = 0;
    virtual Status setValidationAction(OperationContext* const opCtx,
                                       const StringData newAction) = 0;

    virtual StringData getValidationLevel() const = 0;
    virtual StringData getValidationAction() const = 0;

    virtual Status updateValidator(OperationContext* opCtx,
                                   BSONObj newValidator,
                                   StringData newLevel,
                                   StringData newAction) = 0;

    /**
     * Returns true if this is a temporary collection.
     *
     * Calling this function is somewhat costly because it requires accessing the storage engine's
     * cache of collection information.
     */
    virtual bool isTemporary(OperationContext* opCtx) const = 0;

    //
    // Stats
    //

    virtual bool isCapped() const = 0;

    /**
     * Returns a pointer to a capped callback object.
     * The storage engine interacts with capped collections through a CappedCallback interface.
     */
    virtual CappedCallback* getCappedCallback() = 0;

    /**
     * Get a pointer to a capped insert notifier object. The caller can wait on this object
     * until it is notified of a new insert into the capped collection.
     *
     * It is invalid to call this method unless the collection is capped.
     */
    virtual std::shared_ptr<CappedInsertNotifier> getCappedInsertNotifier() const = 0;

    virtual uint64_t numRecords(OperationContext* const opCtx) const = 0;

    virtual uint64_t dataSize(OperationContext* const opCtx) const = 0;

    virtual int averageObjectSize(OperationContext* const opCtx) const = 0;

    virtual uint64_t getIndexSize(OperationContext* const opCtx,
                                  BSONObjBuilder* const details = nullptr,
                                  const int scale = 1) const = 0;

    /**
     * If return value is not boost::none, reads with majority read concern using an older snapshot
     * must error.
     */
    virtual boost::optional<Timestamp> getMinimumVisibleSnapshot() = 0;

    virtual void setMinimumVisibleSnapshot(const Timestamp name) = 0;

    /**
     * Get a pointer to the collection's default collator. The pointer must not be used after this
     * Collection is destroyed.
     */
    virtual const CollatorInterface* getDefaultCollator() const = 0;

    /**
     * Fills in each index specification with collation information from this collection and returns
     * the new index specifications.
     *
     * The returned index specifications will not be equivalent to the ones specified in
     * 'indexSpecs' if any missing collation information were filled in; however, the returned index
     * specifications will match the form stored in the IndexCatalog should any of these indexes
     * already exist.
     */
    virtual StatusWith<std::vector<BSONObj>> addCollationDefaultsToIndexSpecsForCreate(
        OperationContext* opCtx, const std::vector<BSONObj>& indexSpecs) const = 0;

    /**
     * Returns a plan executor for a collection scan over this collection.
     */
    virtual std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> makePlanExecutor(
        OperationContext* opCtx,
        PlanExecutor::YieldPolicy yieldPolicy,
        ScanDirection scanDirection) = 0;

    virtual void indexBuildSuccess(OperationContext* opCtx, IndexCatalogEntry* index) = 0;

    /**
     * Use this Collection as the new cached pointer to the local oplog.
     *
     * Called by catalog::openCatalog() to re-establish the oplog collection pointer while holding
     * onto the global lock in exclusive mode.
     */
    virtual void establishOplogCollectionForLogging(OperationContext* opCtx) = 0;

    virtual void init(OperationContext* opCtx) {}

    virtual bool isInitialized() const {
        return false;
    }
};

}  // namespace mongo
