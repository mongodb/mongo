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
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/concurrency/d_concurrency.h"

namespace mongo {
class IndexConsistency;
class CollectionCatalog;
class CollectionImpl final : public Collection, public CappedCallback {
private:
    static const int kMagicNumber = 1357924;

public:
    enum ValidationAction { WARN, ERROR_V };
    enum ValidationLevel { OFF, MODERATE, STRICT_V };

    explicit CollectionImpl(OperationContext* opCtx,
                            StringData fullNS,
                            OptionalCollectionUUID uuid,
                            CollectionCatalogEntry* details,
                            RecordStore* recordStore);

    ~CollectionImpl();

    class FactoryImpl : public Factory {
    public:
        std::unique_ptr<Collection> make(
            OperationContext* opCtx,
            CollectionUUID uuid,
            CollectionCatalogEntry* collectionCatalogEntry) const final;
    };

    bool ok() const final {
        return _magic == kMagicNumber;
    }

    CollectionCatalogEntry* getCatalogEntry() final {
        return _details;
    }

    const CollectionCatalogEntry* getCatalogEntry() const final {
        return _details;
    }

    CollectionInfoCache* infoCache() final {
        return _infoCache.get();
    }

    const CollectionInfoCache* infoCache() const final {
        return _infoCache.get();
    }

    const NamespaceString& ns() const final {
        return _ns;
    }

    void setNs(NamespaceString nss) final;

    OptionalCollectionUUID uuid() const {
        return _uuid;
    }

    const IndexCatalog* getIndexCatalog() const final {
        return _indexCatalog.get();
    }

    IndexCatalog* getIndexCatalog() final {
        return _indexCatalog.get();
    }

    const RecordStore* getRecordStore() const final {
        return _recordStore;
    }

    RecordStore* getRecordStore() final {
        return _recordStore;
    }

    bool requiresIdIndex() const final;

    Snapshotted<BSONObj> docFor(OperationContext* opCtx, RecordId loc) const final {
        return Snapshotted<BSONObj>(opCtx->recoveryUnit()->getSnapshotId(),
                                    _recordStore->dataFor(opCtx, loc).releaseToBson());
    }

    /**
     * @param out - contents set to the right docs if exists, or nothing.
     * @return true iff loc exists
     */
    bool findDoc(OperationContext* opCtx, RecordId loc, Snapshotted<BSONObj>* out) const final;

    std::unique_ptr<SeekableRecordCursor> getCursor(OperationContext* opCtx,
                                                    bool forward = true) const final;

    /**
     * Deletes the document with the given RecordId from the collection.
     *
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
        StmtId stmtId,
        RecordId loc,
        OpDebug* opDebug,
        bool fromMigrate = false,
        bool noWarn = false,
        Collection::StoreDeletedDoc storeDeletedDoc = Collection::StoreDeletedDoc::Off) final;

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
                           bool fromMigrate = false) final;

    /**
     * this does NOT modify the doc before inserting
     * i.e. will not add an _id field for documents that are missing it
     *
     * 'opDebug' Optional argument. When not null, will be used to record operation statistics.
     */
    Status insertDocument(OperationContext* opCtx,
                          const InsertStatement& doc,
                          OpDebug* opDebug,
                          bool fromMigrate = false) final;

    /**
     * Callers must ensure no document validation is performed for this collection when calling
     * this method.
     */
    Status insertDocumentsForOplog(OperationContext* opCtx,
                                   const DocWriter* const* docs,
                                   Timestamp* timestamps,
                                   size_t nDocs) final;

    /**
     * Inserts a document into the record store for a bulk loader that manages the index building
     * outside this Collection. The bulk loader is notified with the RecordId of the document
     * inserted into the RecordStore.
     *
     * NOTE: It is up to caller to commit the indexes.
     */
    Status insertDocumentForBulkLoader(OperationContext* opCtx,
                                       const BSONObj& doc,
                                       const OnRecordInsertedFn& onRecordInserted) final;

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
                            CollectionUpdateArgs* args) final;

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
                                                     CollectionUpdateArgs* args) final;

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
     * @return OK if the validate run successfully
     *         OK will be returned even if corruption is found
     *         deatils will be in result
     */
    Status validate(OperationContext* opCtx,
                    ValidateCmdLevel level,
                    bool background,
                    ValidateResults* results,
                    BSONObjBuilder* output) final;

    /**
     * forces data into cache
     */
    Status touch(OperationContext* opCtx,
                 bool touchData,
                 bool touchIndexes,
                 BSONObjBuilder* output) const final;

    /**
     * Truncate documents newer than the document at 'end' from the capped
     * collection.  The collection cannot be completely emptied using this
     * function.  An assertion will be thrown if that is attempted.
     * @param inclusive - Truncate 'end' as well iff true
     *
     * The caller should hold a collection X lock and ensure there are no index builds in progress
     * on the collection.
     */
    void cappedTruncateAfter(OperationContext* opCtx, RecordId end, bool inclusive) final;

    /**
     * Returns a non-ok Status if validator is not legal for this collection.
     */
    StatusWithMatchExpression parseValidator(
        OperationContext* opCtx,
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
    Status setValidator(OperationContext* opCtx, BSONObj validator) final;

    Status setValidationLevel(OperationContext* opCtx, StringData newLevel) final;
    Status setValidationAction(OperationContext* opCtx, StringData newAction) final;

    StringData getValidationLevel() const final;
    StringData getValidationAction() const final;

    /**
     * Sets the validator to exactly what's provided. If newLevel or newAction are empty, this
     * sets them to the defaults. Any error Status returned by this function should be considered
     * fatal.
     */
    Status updateValidator(OperationContext* opCtx,
                           BSONObj newValidator,
                           StringData newLevel,
                           StringData newAction) final;

    bool isTemporary(OperationContext* opCtx) const final;

    //
    // Stats
    //

    bool isCapped() const final;

    CappedCallback* getCappedCallback() final;

    /**
     * Get a pointer to a capped insert notifier object. The caller can wait on this object
     * until it is notified of a new insert into the capped collection.
     *
     * It is invalid to call this method unless the collection is capped.
     */
    std::shared_ptr<CappedInsertNotifier> getCappedInsertNotifier() const final;

    uint64_t numRecords(OperationContext* opCtx) const final;

    uint64_t dataSize(OperationContext* opCtx) const final;

    inline int averageObjectSize(OperationContext* opCtx) const {
        uint64_t n = numRecords(opCtx);

        if (n == 0)
            return 5;
        return static_cast<int>(dataSize(opCtx) / n);
    }

    uint64_t getIndexSize(OperationContext* opCtx,
                          BSONObjBuilder* details = NULL,
                          int scale = 1) const final;

    /**
     * If return value is not boost::none, reads with majority read concern using an older snapshot
     * must error.
     */
    boost::optional<Timestamp> getMinimumVisibleSnapshot() final {
        return _minVisibleSnapshot;
    }

    /**
     * Updates the minimum visible snapshot. The 'newMinimumVisibleSnapshot' is ignored if it would
     * set the minimum visible snapshot backwards in time.
     */
    void setMinimumVisibleSnapshot(Timestamp newMinimumVisibleSnapshot) final;

    bool haveCappedWaiters() final;

    /**
     * Notify (capped collection) waiters of data changes, like an insert.
     */
    void notifyCappedWaitersIfNeeded() final;

    /**
     * Get a pointer to the collection's default collator. The pointer must not be used after this
     * Collection is destroyed.
     */
    const CollatorInterface* getDefaultCollator() const final;

    StatusWith<std::vector<BSONObj>> addCollationDefaultsToIndexSpecsForCreate(
        OperationContext* opCtx, const std::vector<BSONObj>& indexSpecs) const final;

    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> makePlanExecutor(
        OperationContext* opCtx,
        PlanExecutor::YieldPolicy yieldPolicy,
        ScanDirection scanDirection) final;

    void indexBuildSuccess(OperationContext* opCtx, IndexCatalogEntry* index) final;

    void establishOplogCollectionForLogging(OperationContext* opCtx) final;

    void init(OperationContext* opCtx) final;
    bool isInitialized() const final;

private:
    /**
     * Returns a non-ok Status if document does not pass this collection's validator.
     */
    Status checkValidation(OperationContext* opCtx, const BSONObj& document) const;

    Status aboutToDeleteCapped(OperationContext* opCtx, const RecordId& loc, RecordData data);

    /**
     * same semantics as insertDocument, but doesn't do:
     *  - some user error checks
     *  - adjust padding
     */
    Status _insertDocument(OperationContext* opCtx, const BSONObj& doc);

    Status _insertDocuments(OperationContext* opCtx,
                            std::vector<InsertStatement>::const_iterator begin,
                            std::vector<InsertStatement>::const_iterator end,
                            OpDebug* opDebug);

    int _magic;

    NamespaceString _ns;
    OptionalCollectionUUID _uuid;
    CollectionCatalogEntry* const _details;

    // The RecordStore may be null during a repair operation.
    RecordStore* const _recordStore;
    const bool _needCappedLock;
    std::unique_ptr<CollectionInfoCache> _infoCache;
    std::unique_ptr<IndexCatalog> _indexCatalog;


    // The default collation which is applied to operations and indices which have no collation of
    // their own. The collection's validator will respect this collation.
    //
    // If null, the default collation is simple binary compare.
    std::unique_ptr<CollatorInterface> _collator;

    // Empty means no filter.
    BSONObj _validatorDoc;

    // Points into _validatorDoc. Null means no filter.
    std::unique_ptr<MatchExpression> _validator;

    ValidationAction _validationAction;
    ValidationLevel _validationLevel;

    // Notifier object for awaitData. Threads polling a capped collection for new data can wait
    // on this object until notified of the arrival of new data.
    //
    // This is non-null if and only if the collection is a capped collection.
    const std::shared_ptr<CappedInsertNotifier> _cappedNotifier;

    // The earliest snapshot that is allowed to use this collection.
    boost::optional<Timestamp> _minVisibleSnapshot;

    bool _initialized = false;
};
}  // namespace mongo
