/*-
 *    Copyright (C) 2017 MongoDB Inc.
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

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog_entry.h"

namespace mongo {
class UUIDCatalog;
class CollectionImpl final : virtual public Collection::Impl,
                             virtual CappedCallback,
                             virtual UpdateNotifier {
private:
    static const int kMagicNumber = 1357924;

public:
    explicit CollectionImpl(Collection* _this,
                            OperationContext* opCtx,
                            StringData fullNS,
                            OptionalCollectionUUID uuid,
                            CollectionCatalogEntry* details,  // does not own
                            RecordStore* recordStore,         // does not own
                            DatabaseCatalogEntry* dbce);      // does not own

    ~CollectionImpl();

    void init(OperationContext* opCtx) final;

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
        return &_infoCache;
    }

    const CollectionInfoCache* infoCache() const final {
        return &_infoCache;
    }

    const NamespaceString& ns() const final {
        return _ns;
    }

    OptionalCollectionUUID uuid() const {
        return _uuid;
    }

    void refreshUUID(OperationContext* opCtx) final {
        auto options = getCatalogEntry()->getCollectionOptions(opCtx);
        _uuid = options.uuid;
    }

    const IndexCatalog* getIndexCatalog() const final {
        return &_indexCatalog;
    }

    IndexCatalog* getIndexCatalog() final {
        return &_indexCatalog;
    }

    const RecordStore* getRecordStore() const final {
        return _recordStore;
    }

    RecordStore* getRecordStore() final {
        return _recordStore;
    }

    CursorManager* getCursorManager() const final {
        return &_cursorManager;
    }

    bool requiresIdIndex() const final;

    Snapshotted<BSONObj> docFor(OperationContext* opCtx, const RecordId& loc) const final {
        return Snapshotted<BSONObj>(opCtx->recoveryUnit()->getSnapshotId(),
                                    _recordStore->dataFor(opCtx, loc).releaseToBson());
    }

    /**
     * @param out - contents set to the right docs if exists, or nothing.
     * @return true iff loc exists
     */
    bool findDoc(OperationContext* opCtx,
                 const RecordId& loc,
                 Snapshotted<BSONObj>* out) const final;

    std::unique_ptr<SeekableRecordCursor> getCursor(OperationContext* opCtx,
                                                    bool forward = true) const final;

    /**
     * Returns many cursors that partition the Collection into many disjoint sets. Iterating
     * all returned cursors is equivalent to iterating the full collection.
     */
    std::vector<std::unique_ptr<RecordCursor>> getManyCursors(OperationContext* opCtx) const final;

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
     */
    void deleteDocument(OperationContext* opCtx,
                        StmtId stmtId,
                        const RecordId& loc,
                        OpDebug* opDebug,
                        bool fromMigrate = false,
                        bool noWarn = false) final;

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
                           bool enforceQuota,
                           bool fromMigrate = false) final;

    /**
     * this does NOT modify the doc before inserting
     * i.e. will not add an _id field for documents that are missing it
     *
     * 'opDebug' Optional argument. When not null, will be used to record operation statistics.
     * 'enforceQuota' If false, quotas will be ignored.
     */
    Status insertDocument(OperationContext* opCtx,
                          const InsertStatement& doc,
                          OpDebug* opDebug,
                          bool enforceQuota,
                          bool fromMigrate = false) final;

    /**
     * Callers must ensure no document validation is performed for this collection when calling
     * this method.
     */
    Status insertDocumentsForOplog(OperationContext* opCtx,
                                   const DocWriter* const* docs,
                                   size_t nDocs) final;

    /**
     * Inserts a document into the record store and adds it to the MultiIndexBlocks passed in.
     *
     * NOTE: It is up to caller to commit the indexes.
     */
    Status insertDocument(OperationContext* opCtx,
                          const BSONObj& doc,
                          const std::vector<MultiIndexBlock*>& indexBlocks,
                          bool enforceQuota) final;

    /**
     * Updates the document @ oldLocation with newDoc.
     *
     * If the document fits in the old space, it is put there; if not, it is moved.
     * Sets 'args.updatedDoc' to the updated version of the document with damages applied, on
     * success.
     * 'opDebug' Optional argument. When not null, will be used to record operation statistics.
     * @return the post update location of the doc (may or may not be the same as oldLocation)
     */
    StatusWith<RecordId> updateDocument(OperationContext* opCtx,
                                        const RecordId& oldLocation,
                                        const Snapshotted<BSONObj>& oldDoc,
                                        const BSONObj& newDoc,
                                        bool enforceQuota,
                                        bool indexesAffected,
                                        OpDebug* opDebug,
                                        OplogUpdateEntryArgs* args) final;

    bool updateWithDamagesSupported() const final;

    /**
     * Not allowed to modify indexes.
     * Illegal to call if updateWithDamagesSupported() returns false.
     * Sets 'args.updatedDoc' to the updated version of the document with damages applied, on
     * success.
     * @return the contents of the updated record.
     */
    StatusWith<RecordData> updateDocumentWithDamages(OperationContext* opCtx,
                                                     const RecordId& loc,
                                                     const Snapshotted<RecordData>& oldRec,
                                                     const char* damageSource,
                                                     const mutablebson::DamageVector& damages,
                                                     OplogUpdateEntryArgs* args) final;

    // -----------

    StatusWith<CompactStats> compact(OperationContext* opCtx, const CompactOptions* options) final;

    /**
     * removes all documents as fast as possible
     * indexes before and after will be the same
     * as will other characteristics
     */
    Status truncate(OperationContext* opCtx) final;

    /**
     * @return OK if the validate run successfully
     *         OK will be returned even if corruption is found
     *         deatils will be in result
     */
    Status validate(OperationContext* opCtx,
                    ValidateCmdLevel level,
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
     */
    void cappedTruncateAfter(OperationContext* opCtx, RecordId end, bool inclusive) final;

    using ValidationAction = Collection::ValidationAction;

    using ValidationLevel = Collection::ValidationLevel;

    /**
     * Returns a non-ok Status if validator is not legal for this collection.
     */
    StatusWithMatchExpression parseValidator(const BSONObj& validator) const final;

    static StatusWith<ValidationLevel> parseValidationLevel(StringData);
    static StatusWith<ValidationAction> parseValidationAction(StringData);

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

    // -----------

    //
    // Stats
    //

    bool isCapped() const final;

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
                          int scale = 1) final;

    /**
     * If return value is not boost::none, reads with majority read concern using an older snapshot
     * must error.
     */
    boost::optional<SnapshotName> getMinimumVisibleSnapshot() final {
        return _minVisibleSnapshot;
    }

    void setMinimumVisibleSnapshot(SnapshotName name) final {
        _minVisibleSnapshot = name;
    }

    /**
     * Notify (capped collection) waiters of data changes, like an insert.
     */
    void notifyCappedWaitersIfNeeded() final;

    /**
     * Get a pointer to the collection's default collator. The pointer must not be used after this
     * Collection is destroyed.
     */
    const CollatorInterface* getDefaultCollator() const final;

private:
    inline DatabaseCatalogEntry* dbce() const final {
        return this->_dbce;
    }

    inline CollectionCatalogEntry* details() const final {
        return this->_details;
    }

    /**
     * Returns a non-ok Status if document does not pass this collection's validator.
     */
    Status checkValidation(OperationContext* opCtx, const BSONObj& document) const;

    Status recordStoreGoingToUpdateInPlace(OperationContext* opCtx, const RecordId& loc);

    Status aboutToDeleteCapped(OperationContext* opCtx, const RecordId& loc, RecordData data);

    /**
     * same semantics as insertDocument, but doesn't do:
     *  - some user error checks
     *  - adjust padding
     */
    Status _insertDocument(OperationContext* opCtx, const BSONObj& doc, bool enforceQuota);

    Status _insertDocuments(OperationContext* opCtx,
                            std::vector<InsertStatement>::const_iterator begin,
                            std::vector<InsertStatement>::const_iterator end,
                            bool enforceQuota,
                            OpDebug* opDebug);


    /**
     * Perform update when document move will be required.
     */
    StatusWith<RecordId> _updateDocumentWithMove(OperationContext* opCtx,
                                                 const RecordId& oldLocation,
                                                 const Snapshotted<BSONObj>& oldDoc,
                                                 const BSONObj& newDoc,
                                                 bool enforceQuota,
                                                 OpDebug* opDebug,
                                                 OplogUpdateEntryArgs* args,
                                                 const SnapshotId& sid);

    bool _enforceQuota(bool userEnforeQuota) const;

    int _magic;

    const NamespaceString _ns;
    OptionalCollectionUUID _uuid;
    CollectionCatalogEntry* const _details;
    RecordStore* const _recordStore;
    DatabaseCatalogEntry* const _dbce;
    const bool _needCappedLock;
    CollectionInfoCache _infoCache;
    IndexCatalog _indexCatalog;

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

    // this is mutable because read only users of the Collection class
    // use it keep state.  This seems valid as const correctness of Collection
    // should be about the data.
    mutable CursorManager _cursorManager;

    // Notifier object for awaitData. Threads polling a capped collection for new data can wait
    // on this object until notified of the arrival of new data.
    //
    // This is non-null if and only if the collection is a capped collection.
    const std::shared_ptr<CappedInsertNotifier> _cappedNotifier;

    const bool _mustTakeCappedLockOnInsert;

    // The earliest snapshot that is allowed to use this collection.
    boost::optional<SnapshotName> _minVisibleSnapshot;

    Collection* _this;

    friend class NamespaceDetails;
};
}  // namespace mongo
