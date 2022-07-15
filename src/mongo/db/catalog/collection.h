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
#include "mongo/db/catalog/collection_operation_source.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/storage/bson_collection_catalog_entry.h"
#include "mongo/db/storage/capped_callback.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/db/yieldable.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/decorable.h"

namespace mongo {

class CappedCallback;
class CollectionPtr;
class IndexCatalog;
class IndexCatalogEntry;
class MatchExpression;
class OpDebug;
class OperationContext;
class RecordCursor;

/**
 * Holds information update an update operation.
 */
struct CollectionUpdateArgs {
    enum class StoreDocOption { None, PreImage, PostImage };

    std::vector<StmtId> stmtIds = {kUninitializedStmtId};

    // The document before modifiers were applied.
    boost::optional<BSONObj> preImageDoc;

    // Fully updated document with damages (update modifiers) applied.
    BSONObj updatedDoc;

    // Document describing the update.
    BSONObj update;

    // Document containing the _id field of the doc being updated.
    BSONObj criteria;

    // Type of update. See OperationSource definition for more details.
    OperationSource source = OperationSource::kStandard;

    StoreDocOption storeDocOption = StoreDocOption::None;
    bool preImageRecordingEnabledForCollection = false;
    bool changeStreamPreAndPostImagesEnabledForCollection = false;

    // Set if OpTimes were reserved for the update ahead of time.
    std::vector<OplogSlot> oplogSlots;
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
 * A decorable object that is shared across all Collection instances for the same collection. There
 * may be several Collection instances simultaneously in existence representing different versions
 * of a collection's persisted state. A single instance of SharedCollectionDecorations will be
 * associated with all of the Collection instances for a collection, sharing whatever data may
 * decorate it across all point in time views of the collection.
 */
class SharedCollectionDecorations : public Decorable<SharedCollectionDecorations> {
public:
    SharedCollectionDecorations() = default;
    SharedCollectionDecorations(const SharedCollectionDecorations&) = delete;
    SharedCollectionDecorations& operator=(const SharedCollectionDecorations&) = delete;
};

class Collection : public Decorable<Collection> {
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
        virtual std::shared_ptr<Collection> make(OperationContext* opCtx,
                                                 const NamespaceString& nss,
                                                 RecordId catalogId,
                                                 const CollectionOptions& options,
                                                 std::unique_ptr<RecordStore> rs) const = 0;

        /**
         * Constructs a Collection object. This does not persist any state to the storage engine,
         * only constructs an in-memory representation of what already exists on disk.
         */
        virtual std::shared_ptr<Collection> make(
            OperationContext* opCtx,
            const NamespaceString& nss,
            RecordId catalogId,
            std::shared_ptr<BSONCollectionCatalogEntry::MetaData> metadata,
            std::unique_ptr<RecordStore> rs) const = 0;
    };

    /**
     * A Collection::Validator represents a filter that is applied to all documents that are
     * inserted. Enforcement of Validators being well formed is done lazily, so the 'Validator'
     * class may represent a validator which is not well formed.
     */
    struct Validator {

        /**
         * Returns whether the validator's filter is well formed.
         */
        bool isOK() const {
            return filter.isOK();
        }

        /**
         * Returns OK or the error encounter when parsing the validator.
         */
        Status getStatus() const {
            return filter.getStatus();
        }

        /**
         * Empty means no validator. This must outlive 'filter'.
         */
        BSONObj validatorDoc;

        /**
         * A special ExpressionContext used to evaluate the filter match expression. This should
         * outlive 'filter'.
         */
        boost::intrusive_ptr<ExpressionContext> expCtxForFilter;

        /**
         * The collection validator MatchExpression. This is stored as a StatusWith, as we lazily
         * enforce that collection validators are well formed.
         *
         * -A non-OK Status indicates that the validator is not well formed, and any attempts to
         * enforce the validator should error.
         *
         * -A value of Status::OK/nullptr indicates that there is no validator.
         *
         * -Anything else indicates a well formed validator. The MatchExpression will maintain
         * pointers into _validatorDoc.
         *
         * Note: this is shared state across cloned Collection instances
         */
        StatusWith<std::shared_ptr<MatchExpression>> filter = {nullptr};
    };

    /**
     * Callback function for callers of insertDocumentForBulkLoader().
     */
    using OnRecordInsertedFn = std::function<Status(const RecordId& loc)>;

    Collection() = default;
    virtual ~Collection() = default;

    /**
     * Clones this Collection instance. Some members are deep copied and some are shallow copied.
     * This should only be be called from the CollectionCatalog when it needs a writable collection.
     */
    virtual std::shared_ptr<Collection> clone() const = 0;

    /**
     * Fetches the shared state across Collection instances for the a collection. Returns an object
     * decorated by state shared across Collection instances for the same namespace. Its decorations
     * are unversioned (not associated with any point in time view of the collection) data related
     * to the collection.
     */
    virtual SharedCollectionDecorations* getSharedDecorations() const = 0;

    virtual void init(OperationContext* opCtx) {}

    virtual bool isCommitted() const {
        return true;
    }

    /**
     * Update the visibility of this collection in the Collection Catalog. Updates to this value
     * are not idempotent, as successive updates with the same `val` should not occur.
     */
    virtual void setCommitted(bool val) {}

    virtual bool isInitialized() const {
        return false;
    }

    virtual const NamespaceString& ns() const = 0;

    /**
     * Sets a new namespace on this Collection, in the case that the Collection is being renamed.
     * In general, reads and writes to Collection objects are synchronized using locks from the lock
     * manager. However, there is special synchronization for ns() and rename() so that the
     * CollectionCatalog can perform UUID to namespace lookup without holding a Collection lock. See
     * CollectionCatalog::onCollectionRename().
     */
    virtual Status rename(OperationContext* opCtx, const NamespaceString& nss, bool stayTemp) = 0;

    virtual RecordId getCatalogId() const = 0;

    virtual UUID uuid() const = 0;

    virtual const IndexCatalog* getIndexCatalog() const = 0;
    virtual IndexCatalog* getIndexCatalog() = 0;

    virtual RecordStore* getRecordStore() const = 0;

    /**
     * Fetches the Ident for this collection.
     */
    virtual std::shared_ptr<Ident> getSharedIdent() const = 0;

    virtual BSONObj getValidatorDoc() const = 0;

    /**
     * Returns 'kPass' with an ok Status if the document passes this collection's schema validator.
     *
     * Returns a non-ok Status if the document does not pass this collection's schema validator and
     * returns 'kWarn' or 'kError' based on the validation action.
     *
     * The validation action is set during collection creation and can be modified as part of
     * collMod. It determines whether we should error for documents that violate the schema
     * validation rules, or warn about, but allow invalid documents.
     */
    enum class SchemaValidationResult { kPass, kWarn, kError };
    virtual std::pair<SchemaValidationResult, Status> checkValidation(
        OperationContext* opCtx, const BSONObj& document) const = 0;

    virtual bool requiresIdIndex() const = 0;

    virtual Snapshotted<BSONObj> docFor(OperationContext* opCtx, const RecordId& loc) const = 0;

    /**
     * @param out - contents set to the right docs if exists, or nothing.
     * @return true iff loc exists
     */
    virtual bool findDoc(OperationContext* opCtx,
                         const RecordId& loc,
                         Snapshotted<BSONObj>* out) const = 0;

    virtual std::unique_ptr<SeekableRecordCursor> getCursor(OperationContext* opCtx,
                                                            bool forward = true) const = 0;

    /**
     * Deletes the document with the given RecordId from the collection. For a description of the
     * parameters, see the overloaded function below.
     */
    virtual void deleteDocument(OperationContext* opCtx,
                                StmtId stmtId,
                                const RecordId& loc,
                                OpDebug* opDebug,
                                bool fromMigrate = false,
                                bool noWarn = false,
                                StoreDeletedDoc storeDeletedDoc = StoreDeletedDoc::Off,
                                CheckRecordId checkRecordId = CheckRecordId::Off) const = 0;

    /**
     * Deletes the document from the collection.
     *
     * 'doc' the document to be deleted.
     * 'fromMigrate' indicates whether the delete was induced by a chunk migration, and
     * so should be ignored by the user as an internal maintenance operation and not a
     * real delete.
     * 'loc' key to uniquely identify a record in a collection.
     * 'opDebug' Optional argument. When not null, will be used to record operation statistics.
     * 'noWarn' if unindexing the record causes an error, if noWarn is true the error
     * will not be logged.
     * 'storeDeletedDoc' whether to store the document deleted in the oplog.
     * 'checkRecordId' whether to confirm the recordId matches the record we are removing when
     * unindexing.
     */
    virtual void deleteDocument(OperationContext* opCtx,
                                Snapshotted<BSONObj> doc,
                                StmtId stmtId,
                                const RecordId& loc,
                                OpDebug* opDebug,
                                bool fromMigrate = false,
                                bool noWarn = false,
                                StoreDeletedDoc storeDeletedDoc = StoreDeletedDoc::Off,
                                CheckRecordId checkRecordId = CheckRecordId::Off) const = 0;

    /*
     * Inserts all documents inside one WUOW.
     * Caller should ensure vector is appropriately sized for this.
     * If any errors occur (including WCE), caller should retry documents individually.
     *
     * 'opDebug' Optional argument. When not null, will be used to record operation statistics.
     */
    virtual Status insertDocuments(OperationContext* opCtx,
                                   std::vector<InsertStatement>::const_iterator begin,
                                   std::vector<InsertStatement>::const_iterator end,
                                   OpDebug* opDebug,
                                   bool fromMigrate = false) const = 0;

    /**
     * this does NOT modify the doc before inserting
     * i.e. will not add an _id field for documents that are missing it
     *
     * 'opDebug' Optional argument. When not null, will be used to record operation statistics.
     */
    virtual Status insertDocument(OperationContext* opCtx,
                                  const InsertStatement& doc,
                                  OpDebug* opDebug,
                                  bool fromMigrate = false) const = 0;

    /**
     * Callers must ensure no document validation is performed for this collection when calling
     * this method.
     */
    virtual Status insertDocumentsForOplog(OperationContext* opCtx,
                                           std::vector<Record>* records,
                                           const std::vector<Timestamp>& timestamps) const = 0;

    /**
     * Inserts a document into the record store for a bulk loader that manages the index building
     * outside this Collection. The bulk loader is notified with the RecordId of the document
     * inserted into the RecordStore.
     *
     * NOTE: It is up to caller to commit the indexes.
     */
    virtual Status insertDocumentForBulkLoader(
        OperationContext* opCtx,
        const BSONObj& doc,
        const OnRecordInsertedFn& onRecordInserted) const = 0;

    /**
     * Updates the document @ oldLocation with newDoc.
     *
     * If the document fits in the old space, it is put there; if not, it is moved.
     * Sets 'args.updatedDoc' to the updated version of the document with damages applied, on
     * success.
     * 'opDebug' Optional argument. When not null, will be used to record operation statistics.
     * @return the post update location of the doc (may or may not be the same as oldLocation)
     */
    virtual RecordId updateDocument(OperationContext* opCtx,
                                    const RecordId& oldLocation,
                                    const Snapshotted<BSONObj>& oldDoc,
                                    const BSONObj& newDoc,
                                    bool indexesAffected,
                                    OpDebug* opDebug,
                                    CollectionUpdateArgs* args) const = 0;

    virtual bool updateWithDamagesSupported() const = 0;

    /**
     * Not allowed to modify indexes.
     * Illegal to call if updateWithDamagesSupported() returns false.
     * Sets 'args.updatedDoc' to the updated version of the document with damages applied, on
     * success.
     * @return the contents of the updated record.
     */
    virtual StatusWith<RecordData> updateDocumentWithDamages(
        OperationContext* opCtx,
        const RecordId& loc,
        const Snapshotted<RecordData>& oldRec,
        const char* damageSource,
        const mutablebson::DamageVector& damages,
        CollectionUpdateArgs* args) const = 0;

    // -----------

    /**
     * removes all documents as fast as possible
     * indexes before and after will be the same
     * as will other characteristics.
     *
     * The caller should hold a collection X lock and ensure there are no index builds in progress
     * on the collection.
     */
    virtual Status truncate(OperationContext* opCtx) = 0;

    /**
     * Truncate documents newer than the document at 'end' from the capped
     * collection.  The collection cannot be completely emptied using this
     * function.  An assertion will be thrown if that is attempted.
     * @param inclusive - Truncate 'end' as well iff true
     *
     * The caller should hold a collection X lock and ensure there are no index builds in progress
     * on the collection.
     */
    virtual void cappedTruncateAfter(OperationContext* opCtx,
                                     const RecordId& end,
                                     bool inclusive) const = 0;

    /**
     * Returns a non-ok Status if validator is not legal for this collection.
     */
    virtual Validator parseValidator(OperationContext* opCtx,
                                     const BSONObj& validator,
                                     MatchExpressionParser::AllowedFeatureSet allowedFeatures,
                                     boost::optional<multiversion::FeatureCompatibilityVersion>
                                         maxFeatureCompatibilityVersion) const = 0;

    /**
     * Sets the validator for this collection.
     *
     * An empty validator removes all validation.
     * Requires an exclusive lock on the collection.
     */
    virtual void setValidator(OperationContext* opCtx, Validator validator) = 0;

    virtual Status setValidationLevel(OperationContext* opCtx, ValidationLevelEnum newLevel) = 0;
    virtual Status setValidationAction(OperationContext* opCtx, ValidationActionEnum newAction) = 0;

    virtual boost::optional<ValidationLevelEnum> getValidationLevel() const = 0;
    virtual boost::optional<ValidationActionEnum> getValidationAction() const = 0;

    virtual Status updateValidator(OperationContext* opCtx,
                                   BSONObj newValidator,
                                   boost::optional<ValidationLevelEnum> newLevel,
                                   boost::optional<ValidationActionEnum> newAction) = 0;

    virtual Status checkValidatorAPIVersionCompatability(OperationContext* opCtx) const = 0;

    virtual bool getRecordPreImages() const = 0;
    virtual void setRecordPreImages(OperationContext* opCtx, bool val) = 0;

    virtual bool isChangeStreamPreAndPostImagesEnabled() const = 0;
    virtual void setChangeStreamPreAndPostImages(OperationContext* opCtx,
                                                 ChangeStreamPreAndPostImagesOptions val) = 0;

    /**
     * Returns true if this is a temporary collection.
     */
    virtual bool isTemporary() const = 0;

    /**
     * Returns true if the time-series collection may have mixed-schema data.
     *
     * If FCV < 5.2 or if this is not a time-series collection, returns boost::none.
     */
    virtual boost::optional<bool> getTimeseriesBucketsMayHaveMixedSchemaData() const = 0;

    /**
     * Sets the 'timeseriesBucketsMayHaveMixedSchemaData' catalog entry flag to 'setting' for this
     * collection.
     *
     * Throws if this is not a time-series collection.
     */
    virtual void setTimeseriesBucketsMayHaveMixedSchemaData(OperationContext* opCtx,
                                                            boost::optional<bool> setting) = 0;

    /**
     * Returns true if the passed in time-series bucket document contains mixed-schema data.
     */
    virtual bool doesTimeseriesBucketsDocContainMixedSchemaData(
        const BSONObj& bucketsDoc) const = 0;

    /*
     * Returns true if this collection is clustered. That is, its RecordIds store the value of the
     * cluster key. If the collection is clustered on _id, there is no separate _id index.
     */
    virtual bool isClustered() const = 0;

    virtual boost::optional<ClusteredCollectionInfo> getClusteredInfo() const = 0;

    /**
     * Updates the expireAfterSeconds setting for a clustered TTL index in this Collection and the
     * durable catalog.
     */
    virtual void updateClusteredIndexTTLSetting(OperationContext* opCtx,
                                                boost::optional<int64_t> expireAfterSeconds) = 0;

    virtual Status updateCappedSize(OperationContext* opCtx,
                                    boost::optional<long long> newCappedSize,
                                    boost::optional<long long> newCappedMax) = 0;

    //
    // Index
    //

    /**
     * Checks that the metadata for the index exists and matches the given spec.
     * Returns offset in metadata on success.
     */
    virtual StatusWith<int> checkMetaDataForIndex(const std::string& indexName,
                                                  const BSONObj& spec) const = 0;

    /*
     * Updates the expireAfterSeconds field of the given index to the value in newExpireSecs.
     * The specified index must already contain an expireAfterSeconds field, and the value in
     * that field and newExpireSecs must both be numeric.
     */
    virtual void updateTTLSetting(OperationContext* opCtx,
                                  StringData idxName,
                                  long long newExpireSeconds) = 0;

    /*
     * Hide or unhide the given index. A hidden index will not be considered for use by the
     * query planner.
     */
    virtual void updateHiddenSetting(OperationContext* opCtx, StringData idxName, bool hidden) = 0;

    /*
     * Converts the the given index to be unique or non-unique.
     */
    virtual void updateUniqueSetting(OperationContext* opCtx, StringData idxName, bool unique) = 0;

    /*
     * Disallows or allows new duplicates in the given index.
     */
    virtual void updatePrepareUniqueSetting(OperationContext* opCtx,
                                            StringData idxName,
                                            bool prepareUnique) = 0;

    /**
     * Repairs invalid index options on all indexes in this collection. Returns a list of
     * index names that were repaired.
     */
    virtual std::vector<std::string> repairInvalidIndexOptions(OperationContext* opCtx) = 0;

    /**
     * Updates the 'temp' setting for this collection.
     */
    virtual void setIsTemp(OperationContext* opCtx, bool isTemp) = 0;

    /**
     * Removes the index 'indexName' from the persisted collection catalog entry identified by
     * 'catalogId'.
     */
    virtual void removeIndex(OperationContext* opCtx, StringData indexName) = 0;

    /**
     * Updates the persisted catalog entry for 'ns' with the new index and creates the index on
     * disk.
     *
     * A passed 'buildUUID' implies that the index is part of a two-phase index build.
     */
    virtual Status prepareForIndexBuild(OperationContext* opCtx,
                                        const IndexDescriptor* spec,
                                        boost::optional<UUID> buildUUID,
                                        bool isBackgroundSecondaryBuild) = 0;

    /**
     * Returns a UUID if the index is being built with the two-phase index build procedure.
     */
    virtual boost::optional<UUID> getIndexBuildUUID(StringData indexName) const = 0;

    /**
     * Returns true if the index identified by 'indexName' is multikey, and returns false otherwise.
     *
     * If the 'multikeyPaths' pointer is non-null, then it must point to an empty vector. If this
     * index type supports tracking path-level multikey information in the catalog, then this
     * function sets 'multikeyPaths' as the path components that cause this index to be multikey.
     *
     * In particular, if this function returns false and the index supports tracking path-level
     * multikey information, then 'multikeyPaths' is initialized as a vector with size equal to the
     * number of elements in the index key pattern of empty sets.
     */
    virtual bool isIndexMultikey(OperationContext* opCtx,
                                 StringData indexName,
                                 MultikeyPaths* multikeyPaths,
                                 int indexOffset = -1) const = 0;

    /**
     * Sets the index identified by 'indexName' to be multikey.
     *
     * If 'multikeyPaths' is non-empty, then it must be a vector with size equal to the number of
     * elements in the index key pattern. Additionally, at least one path component of the indexed
     * fields must cause this index to be multikey.
     *
     * This function returns true if the index metadata has changed, and returns false otherwise.
     */
    virtual bool setIndexIsMultikey(OperationContext* opCtx,
                                    StringData indexName,
                                    const MultikeyPaths& multikeyPaths,
                                    int indexOffset = -1) const = 0;

    /**
     * Sets the index to be multikey with the provided paths. This performs minimal validation of
     * the inputs and is intended to be used internally to "correct" multikey metadata that drifts
     * from the underlying collection data.
     *
     * When isMultikey is false, ignores multikeyPaths and resets the metadata appropriately based
     * on the index descriptor. Otherwise, overwrites the existing multikeyPaths with the ones
     * provided. This only writes multikey paths if the index type supports path-level tracking, and
     * only sets the multikey boolean flag otherwise.
     */
    virtual void forceSetIndexIsMultikey(OperationContext* opCtx,
                                         const IndexDescriptor* desc,
                                         bool isMultikey,
                                         const MultikeyPaths& multikeyPaths) const = 0;

    virtual int getTotalIndexCount() const = 0;

    virtual int getCompletedIndexCount() const = 0;

    virtual BSONObj getIndexSpec(StringData indexName) const = 0;

    virtual void getAllIndexes(std::vector<std::string>* names) const = 0;

    virtual void getReadyIndexes(std::vector<std::string>* names) const = 0;

    virtual bool isIndexPresent(StringData indexName) const = 0;

    virtual bool isIndexReady(StringData indexName) const = 0;

    virtual void replaceMetadata(OperationContext* opCtx,
                                 std::shared_ptr<BSONCollectionCatalogEntry::MetaData> md) = 0;

    //
    // Stats
    //

    virtual bool isCapped() const = 0;
    virtual long long getCappedMaxDocs() const = 0;
    virtual long long getCappedMaxSize() const = 0;

    /**
     * Returns a pointer to a capped callback object.
     * The storage engine interacts with capped collections through a CappedCallback interface.
     */
    virtual CappedCallback* getCappedCallback() = 0;
    virtual const CappedCallback* getCappedCallback() const = 0;

    /**
     * Get a pointer to a capped insert notifier object. The caller can wait on this object
     * until it is notified of a new insert into the capped collection.
     *
     * It is invalid to call this method unless the collection is capped.
     */
    virtual std::shared_ptr<CappedInsertNotifier> getCappedInsertNotifier() const = 0;

    virtual long long numRecords(OperationContext* opCtx) const = 0;

    /**
     * Return uncompressed collection data size in bytes
     */
    virtual long long dataSize(OperationContext* opCtx) const = 0;


    /**
     * Returns true if the collection does not contain any records.
     */
    virtual bool isEmpty(OperationContext* opCtx) const = 0;

    /**
     * Return the average object size in bytes
     */
    virtual int averageObjectSize(OperationContext* opCtx) const = 0;

    virtual uint64_t getIndexSize(OperationContext* opCtx,
                                  BSONObjBuilder* details = nullptr,
                                  int scale = 1) const = 0;

    /**
     * Returns the number of unused, free bytes used by all indexes on disk.
     */
    virtual uint64_t getIndexFreeStorageBytes(OperationContext* opCtx) const = 0;

    /**
     * If return value is not boost::none, reads with majority read concern using an older snapshot
     * must error.
     */
    virtual boost::optional<Timestamp> getMinimumVisibleSnapshot() const = 0;

    virtual void setMinimumVisibleSnapshot(Timestamp name) = 0;

    /**
     * Returns the time-series options for this buckets collection, or boost::none if not a
     * time-series buckets collection.
     */
    virtual boost::optional<TimeseriesOptions> getTimeseriesOptions() const = 0;

    /**
     * Sets the time-series options for this buckets collection.
     */
    virtual void setTimeseriesOptions(OperationContext* opCtx,
                                      const TimeseriesOptions& tsOptions) = 0;

    /**
     * Get a pointer to the collection's default collator. The pointer must not be used after this
     * Collection is destroyed.
     */
    virtual const CollatorInterface* getDefaultCollator() const = 0;

    /**
     * Returns a cached version of the Collection MetaData that matches the version of this
     * Collection instance.
     */
    virtual const CollectionOptions& getCollectionOptions() const = 0;

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
        const CollectionPtr& yieldableCollection,
        PlanYieldPolicy::YieldPolicy yieldPolicy,
        ScanDirection scanDirection,
        const boost::optional<RecordId>& resumeAfterRecordId = boost::none) const = 0;

    virtual void indexBuildSuccess(OperationContext* opCtx, IndexCatalogEntry* index) = 0;

    /**
     * Called when this Collection is deregistered from the catalog
     */
    virtual void onDeregisterFromCatalog(OperationContext* opCtx) = 0;

    friend auto logAttrs(const Collection& col) {
        return logv2::multipleAttrs(col.ns(), col.uuid());
    }
};

/**
 * Smart-pointer'esque type to handle yielding of Collection lock that may invalidate pointers when
 * resuming. CollectionPtr will re-load the Collection from the Catalog when restoring from a yield
 * that dropped locks. The yield and restore behavior can be disabled by constructing this type from
 * a writable Collection or by specifying NoYieldTag.
 */
class CollectionPtr : public Yieldable {
public:
    static CollectionPtr null;

    // Function for the implementation on how we load a new Collection pointer when restoring from
    // yield
    using RestoreFn = std::function<const Collection*(OperationContext*, UUID)>;

    CollectionPtr();

    // Creates a Yieldable CollectionPtr that reloads the Collection pointer from the catalog when
    // restoring from yield
    CollectionPtr(OperationContext* opCtx, const Collection* collection, RestoreFn restoreFn);

    // Creates non-yieldable CollectionPtr, performing yield/restore will be a no-op.
    struct NoYieldTag {};
    CollectionPtr(const Collection* collection, NoYieldTag);
    CollectionPtr(Collection* collection);

    CollectionPtr(const CollectionPtr&) = delete;
    CollectionPtr(CollectionPtr&&);
    ~CollectionPtr();

    CollectionPtr& operator=(const CollectionPtr&) = delete;
    CollectionPtr& operator=(CollectionPtr&&);

    explicit operator bool() const {
        return static_cast<bool>(_collection);
    }

    bool operator==(const CollectionPtr& other) const {
        return get() == other.get();
    }
    bool operator!=(const CollectionPtr& other) const {
        return !operator==(other);
    }
    const Collection* operator->() const {
        return _collection;
    }
    const Collection* get() const {
        return _collection;
    }

    void reset() {
        *this = CollectionPtr();
    }

    void yield() const override;
    void restore() const override;

    friend std::ostream& operator<<(std::ostream& os, const CollectionPtr& coll);

    void setShardKeyPattern(const BSONObj& shardKeyPattern) {
        _shardKeyPattern = shardKeyPattern.getOwned();
    }
    const BSONObj& getShardKeyPattern() const;

    bool isSharded() const {
        return static_cast<bool>(_shardKeyPattern);
    }

private:
    bool _canYield() const;

    // These members needs to be mutable so the yield/restore interface can be const. We don't want
    // yield/restore to require a non-const instance when it otherwise could be const.
    mutable const Collection* _collection;

    // If the collection is currently in the 'yielded' state (i.e. yield() has been called), this
    // field will contain what was the UUID of the collection at the time of yield.
    mutable boost::optional<UUID> _yieldedUUID;

    OperationContext* _opCtx;
    RestoreFn _restoreFn;

    // Stores a consistent view of shard key with the collection that will be needed during the
    // operation. If _shardKeyPattern is set, that indicates that the collection is sharded.
    boost::optional<BSONObj> _shardKeyPattern = boost::none;
};

inline std::ostream& operator<<(std::ostream& os, const CollectionPtr& coll) {
    os << coll.get();
    return os;
}

inline ValidationActionEnum validationActionOrDefault(
    boost::optional<ValidationActionEnum> action) {
    return action.value_or(ValidationActionEnum::error);
}

inline ValidationLevelEnum validationLevelOrDefault(boost::optional<ValidationLevelEnum> level) {
    return level.value_or(ValidationLevelEnum::strict);
}
}  // namespace mongo
