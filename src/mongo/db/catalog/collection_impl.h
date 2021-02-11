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

#include "mongo/bson/timestamp.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/commands/create_gen.h"
#include "mongo/db/concurrency/d_concurrency.h"

namespace mongo {
class IndexConsistency;
class CollectionCatalog;
class CollectionImpl final : public Collection {
public:
    explicit CollectionImpl(OperationContext* opCtx,
                            const NamespaceString& nss,
                            RecordId catalogId,
                            UUID uuid,
                            std::unique_ptr<RecordStore> recordStore);

    ~CollectionImpl();

    std::shared_ptr<Collection> clone() const final;

    class FactoryImpl : public Factory {
    public:
        std::shared_ptr<Collection> make(OperationContext* opCtx,
                                         const NamespaceString& nss,
                                         RecordId catalogId,
                                         CollectionUUID uuid,
                                         std::unique_ptr<RecordStore> rs) const final;
    };

    SharedCollectionDecorations* getSharedDecorations() const final;

    void init(OperationContext* opCtx) final;
    bool isInitialized() const final;
    bool isCommitted() const final;
    void setCommitted(bool val) final;

    const NamespaceString& ns() const final {
        return _ns;
    }

    void setNs(NamespaceString nss) final;

    RecordId getCatalogId() const {
        return _catalogId;
    }

    UUID uuid() const {
        return _uuid;
    }

    const IndexCatalog* getIndexCatalog() const final {
        return _indexCatalog.get();
    }

    IndexCatalog* getIndexCatalog() final {
        return _indexCatalog.get();
    }

    RecordStore* getRecordStore() const final {
        return _shared->_recordStore.get();
    }

    std::shared_ptr<Ident> getSharedIdent() const final {
        // Use shared_ptr's aliasing constructor so we can keep all shared state in a single
        // reference counted object
        return {_shared, _shared->_recordStore.get()};
    }

    const BSONObj getValidatorDoc() const final {
        return _validator.validatorDoc.getOwned();
    }

    bool requiresIdIndex() const final;

    Snapshotted<BSONObj> docFor(OperationContext* opCtx, RecordId loc) const final {
        return Snapshotted<BSONObj>(opCtx->recoveryUnit()->getSnapshotId(),
                                    _shared->_recordStore->dataFor(opCtx, loc).releaseToBson());
    }

    /**
     * @param out - contents set to the right docs if exists, or nothing.
     * @return true iff loc exists
     */
    bool findDoc(OperationContext* opCtx, RecordId loc, Snapshotted<BSONObj>* out) const final;

    std::unique_ptr<SeekableRecordCursor> getCursor(OperationContext* opCtx,
                                                    bool forward = true) const final;

    /**
     * Deletes the document with the given RecordId from the collection. For a description of
     * the parameters, see the overloaded function below.
     */
    void deleteDocument(
        OperationContext* opCtx,
        StmtId stmtId,
        RecordId loc,
        OpDebug* opDebug,
        bool fromMigrate = false,
        bool noWarn = false,
        Collection::StoreDeletedDoc storeDeletedDoc = Collection::StoreDeletedDoc::Off) const final;

    /**
     * Deletes the document from the collection.

     * 'doc' the document to be deleted.
     * 'stmtId' the statement id for this delete operation. Pass in kUninitializedStmtId if not
     * applicable.
     * 'fromMigrate' indicates whether the delete was induced by a chunk migration, and
     * so should be ignored by the user as an internal maintenance operation and not a
     * real delete.
     * 'loc' key to uniquely identify a record in a collection.
     * 'opDebug' Optional argument. When not null, will be used to record operation statistics.
     * 'cappedOK' if true, allows deletes on capped collections (Cloner::copyDB uses this).
     * 'noWarn' if unindexing the record causes an error, if noWarn is true the error
     * will not be logged.
     * 'storeDeletedDoc' whether to store the document deleted in the oplog.
     */
    void deleteDocument(
        OperationContext* opCtx,
        Snapshotted<BSONObj> doc,
        StmtId stmtId,
        RecordId loc,
        OpDebug* opDebug,
        bool fromMigrate = false,
        bool noWarn = false,
        Collection::StoreDeletedDoc storeDeletedDoc = Collection::StoreDeletedDoc::Off) const final;

    /*
     * Inserts all documents inside one WUOW.
     * Caller should ensure vector is appropriately sized for this.
     * If any errors occur (including WCE), caller should retry documents individually.
     *
     * 'opDebug' Optional argument. When not null, will be used to record operation statistics.
     */
    Status insertDocuments(OperationContext* opCtx,
                           std::vector<InsertStatement>::const_iterator begin,
                           std::vector<InsertStatement>::const_iterator end,
                           OpDebug* opDebug,
                           bool fromMigrate = false) const final;

    /**
     * this does NOT modify the doc before inserting
     * i.e. will not add an _id field for documents that are missing it
     *
     * 'opDebug' Optional argument. When not null, will be used to record operation statistics.
     */
    Status insertDocument(OperationContext* opCtx,
                          const InsertStatement& doc,
                          OpDebug* opDebug,
                          bool fromMigrate = false) const final;

    /**
     * Callers must ensure no document validation is performed for this collection when calling
     * this method.
     */
    Status insertDocumentsForOplog(OperationContext* opCtx,
                                   std::vector<Record>* records,
                                   const std::vector<Timestamp>& timestamps) const final;

    /**
     * Inserts a document into the record store for a bulk loader that manages the index building
     * outside this Collection. The bulk loader is notified with the RecordId of the document
     * inserted into the RecordStore.
     *
     * NOTE: It is up to caller to commit the indexes.
     */
    Status insertDocumentForBulkLoader(OperationContext* opCtx,
                                       const BSONObj& doc,
                                       const OnRecordInsertedFn& onRecordInserted) const final;

    /**
     * Updates the document @ oldLocation with newDoc.
     *
     * If the document fits in the old space, it is put there; if not, it is moved.
     * Sets 'args.updatedDoc' to the updated version of the document with damages applied, on
     * success.
     * 'opDebug' Optional argument. When not null, will be used to record operation statistics.
     * @return the post update location of the doc (may or may not be the same as oldLocation)
     */
    RecordId updateDocument(OperationContext* opCtx,
                            RecordId oldLocation,
                            const Snapshotted<BSONObj>& oldDoc,
                            const BSONObj& newDoc,
                            bool indexesAffected,
                            OpDebug* opDebug,
                            CollectionUpdateArgs* args) const final;

    bool updateWithDamagesSupported() const final;

    /**
     * Not allowed to modify indexes.
     * Illegal to call if updateWithDamagesSupported() returns false.
     * Sets 'args.updatedDoc' to the updated version of the document with damages applied, on
     * success.
     * @return the contents of the updated record.
     */
    StatusWith<RecordData> updateDocumentWithDamages(OperationContext* opCtx,
                                                     RecordId loc,
                                                     const Snapshotted<RecordData>& oldRec,
                                                     const char* damageSource,
                                                     const mutablebson::DamageVector& damages,
                                                     CollectionUpdateArgs* args) const final;

    // -----------

    /**
     * removes all documents as fast as possible
     * indexes before and after will be the same
     * as will other characteristics
     *
     * The caller should hold a collection X lock and ensure there are no index builds in progress
     * on the collection.
     */
    Status truncate(OperationContext* opCtx) final;

    /**
     * Truncate documents newer than the document at 'end' from the capped
     * collection.  The collection cannot be completely emptied using this
     * function.  An assertion will be thrown if that is attempted.
     * @param inclusive - Truncate 'end' as well iff true
     *
     * The caller should hold a collection X lock and ensure there are no index builds in progress
     * on the collection.
     */
    void cappedTruncateAfter(OperationContext* opCtx, RecordId end, bool inclusive) const final;

    /**
     * Returns a non-ok Status if validator is not legal for this collection.
     */
    Validator parseValidator(OperationContext* opCtx,
                             const BSONObj& validator,
                             MatchExpressionParser::AllowedFeatureSet allowedFeatures,
                             boost::optional<ServerGlobalParams::FeatureCompatibility::Version>
                                 maxFeatureCompatibilityVersion = boost::none) const final;

    /**
     * Sets the validator for this collection.
     *
     * An empty validator removes all validation.
     * Requires an exclusive lock on the collection.
     */
    void setValidator(OperationContext* opCtx, Validator validator) final;

    Status setValidationLevel(OperationContext* opCtx, ValidationLevelEnum newLevel) final;
    Status setValidationAction(OperationContext* opCtx, ValidationActionEnum newAction) final;

    boost::optional<ValidationLevelEnum> getValidationLevel() const final;
    boost::optional<ValidationActionEnum> getValidationAction() const final;

    /**
     * Sets the validator to exactly what's provided. Any error Status returned by this function
     * should be considered fatal.
     */
    Status updateValidator(OperationContext* opCtx,
                           BSONObj newValidator,
                           boost::optional<ValidationLevelEnum> newLevel,
                           boost::optional<ValidationActionEnum> newAction) final;

    /**
     * Returns non-OK status if the collection validator does not comply with stable API
     * requirements.
     */
    Status checkValidatorAPIVersionCompatability(OperationContext* opCtx) const final;

    bool getRecordPreImages() const final;
    void setRecordPreImages(OperationContext* opCtx, bool val) final;

    bool isTemporary(OperationContext* opCtx) const final;

    bool isClustered() const final;

    //
    // Stats
    //

    bool isCapped() const final;

    CappedCallback* getCappedCallback() final;
    const CappedCallback* getCappedCallback() const final;

    /**
     * Get a pointer to a capped insert notifier object. The caller can wait on this object
     * until it is notified of a new insert into the capped collection.
     *
     * It is invalid to call this method unless the collection is capped.
     */
    std::shared_ptr<CappedInsertNotifier> getCappedInsertNotifier() const final;

    uint64_t numRecords(OperationContext* opCtx) const final;

    uint64_t dataSize(OperationContext* opCtx) const final;

    /**
     * Currently fast counts are prone to false negative as it is not tolerant to unclean shutdowns.
     * So, verify that the collection is really empty by opening the collection cursor and reading
     * the first document.
     * Expects to hold at least collection lock in mode IS.
     * TODO SERVER-24266: After making fast counts tolerant to unclean shutdowns, we can make use of
     * fast count to determine whether the collection is empty and remove cursor checking logic.
     */
    bool isEmpty(OperationContext* opCtx) const final;

    inline int averageObjectSize(OperationContext* opCtx) const {
        uint64_t n = numRecords(opCtx);

        if (n == 0)
            return 5;
        return static_cast<int>(dataSize(opCtx) / n);
    }

    uint64_t getIndexSize(OperationContext* opCtx,
                          BSONObjBuilder* details = nullptr,
                          int scale = 1) const final;

    uint64_t getIndexFreeStorageBytes(OperationContext* const opCtx) const final;

    /**
     * If return value is not boost::none, reads with majority read concern using an older snapshot
     * must error.
     */
    boost::optional<Timestamp> getMinimumVisibleSnapshot() const final {
        return _minVisibleSnapshot;
    }

    /**
     * Updates the minimum visible snapshot. The 'newMinimumVisibleSnapshot' is ignored if it would
     * set the minimum visible snapshot backwards in time.
     */
    void setMinimumVisibleSnapshot(Timestamp newMinimumVisibleSnapshot) final;

    /**
     * Get a pointer to the collection's default collator. The pointer must not be used after this
     * Collection is destroyed.
     */
    const CollatorInterface* getDefaultCollator() const final;

    StatusWith<std::vector<BSONObj>> addCollationDefaultsToIndexSpecsForCreate(
        OperationContext* opCtx, const std::vector<BSONObj>& indexSpecs) const final;

    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> makePlanExecutor(
        OperationContext* opCtx,
        const CollectionPtr& yieldableCollection,
        PlanYieldPolicy::YieldPolicy yieldPolicy,
        ScanDirection scanDirection,
        boost::optional<RecordId> resumeAfterRecordId) const final;

    void indexBuildSuccess(OperationContext* opCtx, IndexCatalogEntry* index) final;

    void establishOplogCollectionForLogging(OperationContext* opCtx) final;
    void onDeregisterFromCatalog(OperationContext* opCtx) final;

private:
    /**
     * Returns a non-ok Status if document does not pass this collection's validator.
     */
    Status checkValidation(OperationContext* opCtx, const BSONObj& document) const;

    /**
     * same semantics as insertDocument, but doesn't do:
     *  - some user error checks
     *  - adjust padding
     */
    Status _insertDocument(OperationContext* opCtx, const BSONObj& doc);

    Status _insertDocuments(OperationContext* opCtx,
                            std::vector<InsertStatement>::const_iterator begin,
                            std::vector<InsertStatement>::const_iterator end,
                            OpDebug* opDebug) const;

    /**
     * Holder of shared state between CollectionImpl clones. Also implements CappedCallback, a
     * pointer to which is given to the RecordStore, so that the CappedCallback logic can always be
     * performed on the latest CollectionImpl instance without needing to know about copy-on-write
     * on CollectionImpl instances.
     */
    struct SharedState : public CappedCallback {
        SharedState(CollectionImpl* collection, std::unique_ptr<RecordStore> recordStore);
        ~SharedState();

        /**
         * The Collection instance that need to be notified through the CappedCallback changes when
         * the Collection is cloned for a write. When the constructor and destructor is run for
         * CollectionImpl it notifies this class through this interface so we can keep track of the
         * most recent Collection instance to be used when implementing CappedCallback.
         */
        void instanceCreated(CollectionImpl* collection);
        void instanceDeleted(CollectionImpl* collection);

        bool haveCappedWaiters() const final;
        void notifyCappedWaitersIfNeeded() const final;
        Status aboutToDeleteCapped(OperationContext* opCtx,
                                   const RecordId& loc,
                                   RecordData data) final;

        // As we're holding a MODE_X lock when cloning Collections we may have up to two current
        // Collection instances at the same time if there's a pending clone that is not commited to
        // the catalog yet. We need to keep track of the previous instance in case of a rollback.
        // When we delete from capped, operate on the latest collection.
        CollectionImpl* _collectionLatest = nullptr;
        CollectionImpl* _collectionPrev = nullptr;

        // The RecordStore may be null during a repair operation.
        std::unique_ptr<RecordStore> _recordStore;

        // This object is decorable and decorated with unversioned data related to the collection.
        // Not associated with any particular Collection instance for the collection, but shared
        // across all all instances for the same collection. This is a vehicle for users of a
        // collection to cache unversioned state for a collection that is accessible across all of
        // the Collection instances.
        SharedCollectionDecorations _sharedDecorations;

        // The default collation which is applied to operations and indices which have no collation
        // of their own. The collection's validator will respect this collation. If null, the
        // default collation is simple binary compare.
        std::unique_ptr<CollatorInterface> _collator;

        // Notifier object for awaitData. Threads polling a capped collection for new data can wait
        // on this object until notified of the arrival of new data.
        //
        // This is non-null if and only if the collection is a capped collection.
        const std::shared_ptr<CappedInsertNotifier> _cappedNotifier;

        const bool _needCappedLock;

        AtomicWord<bool> _committed{true};
    };

    NamespaceString _ns;
    RecordId _catalogId;
    UUID _uuid;
    bool _cachedCommitted = true;
    std::shared_ptr<SharedState> _shared;

    clonable_ptr<IndexCatalog> _indexCatalog;

    // The validator is using shared state internally. Collections share validator until a new
    // validator is set in setValidator which sets a new instance.
    Validator _validator;
    boost::optional<ValidationActionEnum> _validationAction;
    boost::optional<ValidationLevelEnum> _validationLevel;

    // Whether or not this collection is clustered on _id values.
    bool _clustered = false;

    bool _recordPreImages = false;

    // The earliest snapshot that is allowed to use this collection.
    boost::optional<Timestamp> _minVisibleSnapshot;

    bool _initialized = false;
};
}  // namespace mongo
