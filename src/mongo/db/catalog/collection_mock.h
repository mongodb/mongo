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

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/platform/atomic_word.h"

namespace mongo {

/**
 * This class comprises a mock Collection for use by CollectionCatalog unit tests.
 */
class CollectionMock : public Collection {
public:
    CollectionMock(const NamespaceString& ns)
        : CollectionMock(ns, std::unique_ptr<IndexCatalog>()) {}
    CollectionMock(const NamespaceString& ns, std::unique_ptr<IndexCatalog> indexCatalog)
        : _ns(ns), _indexCatalog(std::move(indexCatalog)) {}
    CollectionMock(const NamespaceString& ns, RecordId catalogId)
        : _ns(ns), _catalogId(catalogId) {}
    ~CollectionMock() = default;

    std::shared_ptr<Collection> clone() const {
        return std::make_shared<CollectionMock>(*this);
    }


    SharedCollectionDecorations* getSharedDecorations() const {
        return nullptr;
    }

    void init(OperationContext* opCtx) {
        std::abort();
    }

    RecordId getCatalogId() const {
        return _catalogId;
    }

    void setCatalogId(RecordId catalogId) {
        _catalogId = catalogId;
    }

    const NamespaceString& ns() const {
        return _ns;
    }

    void setNs(NamespaceString nss) final {
        _ns = std::move(nss);
    }

    const IndexCatalog* getIndexCatalog() const {
        return _indexCatalog.get();
    }
    IndexCatalog* getIndexCatalog() {
        return _indexCatalog.get();
    }

    RecordStore* getRecordStore() const {
        std::abort();
    }
    std::shared_ptr<Ident> getSharedIdent() const {
        std::abort();
    }

    const BSONObj getValidatorDoc() const {
        std::abort();
    }

    bool requiresIdIndex() const {
        std::abort();
    }

    Snapshotted<BSONObj> docFor(OperationContext* opCtx, RecordId loc) const {
        std::abort();
    }

    bool findDoc(OperationContext* opCtx, RecordId loc, Snapshotted<BSONObj>* out) const {
        std::abort();
    }

    std::unique_ptr<SeekableRecordCursor> getCursor(OperationContext* opCtx, bool forward) const {
        std::abort();
    }

    void deleteDocument(OperationContext* opCtx,
                        StmtId stmtId,
                        RecordId loc,
                        OpDebug* opDebug,
                        bool fromMigrate,
                        bool noWarn,
                        Collection::StoreDeletedDoc storeDeletedDoc) const {
        std::abort();
    }

    void deleteDocument(
        OperationContext* opCtx,
        Snapshotted<BSONObj> doc,
        StmtId stmtId,
        RecordId loc,
        OpDebug* opDebug,
        bool fromMigrate = false,
        bool noWarn = false,
        Collection::StoreDeletedDoc storeDeletedDoc = Collection::StoreDeletedDoc::Off) const {
        std::abort();
    }

    Status insertDocuments(OperationContext* opCtx,
                           std::vector<InsertStatement>::const_iterator begin,
                           std::vector<InsertStatement>::const_iterator end,
                           OpDebug* opDebug,
                           bool fromMigrate) const {
        std::abort();
    }

    Status insertDocument(OperationContext* opCtx,
                          const InsertStatement& doc,
                          OpDebug* opDebug,
                          bool fromMigrate) const {
        std::abort();
    }

    Status insertDocumentsForOplog(OperationContext* opCtx,
                                   std::vector<Record>* records,
                                   const std::vector<Timestamp>& timestamps) const {
        std::abort();
    }

    Status insertDocumentForBulkLoader(OperationContext* opCtx,
                                       const BSONObj& doc,
                                       const OnRecordInsertedFn& onRecordInserted) const {
        std::abort();
    }

    RecordId updateDocument(OperationContext* opCtx,
                            RecordId oldLocation,
                            const Snapshotted<BSONObj>& oldDoc,
                            const BSONObj& newDoc,
                            bool indexesAffected,
                            OpDebug* opDebug,
                            CollectionUpdateArgs* args) const {
        std::abort();
    }

    bool updateWithDamagesSupported() const {
        std::abort();
    }

    StatusWith<RecordData> updateDocumentWithDamages(OperationContext* opCtx,
                                                     RecordId loc,
                                                     const Snapshotted<RecordData>& oldRec,
                                                     const char* damageSource,
                                                     const mutablebson::DamageVector& damages,
                                                     CollectionUpdateArgs* args) const {
        std::abort();
    }

    Status truncate(OperationContext* opCtx) {
        std::abort();
    }

    void cappedTruncateAfter(OperationContext* opCtx, RecordId end, bool inclusive) const {
        std::abort();
    }

    Validator parseValidator(OperationContext* opCtx,
                             const BSONObj& validator,
                             MatchExpressionParser::AllowedFeatureSet allowedFeatures,
                             boost::optional<ServerGlobalParams::FeatureCompatibility::Version>
                                 maxFeatureCompatibilityVersion) const {
        std::abort();
    }

    void setValidator(OperationContext* opCtx, Validator validator) {
        std::abort();
    }

    Status setValidationLevel(OperationContext* opCtx, ValidationLevelEnum newLevel) {
        std::abort();
    }
    Status setValidationAction(OperationContext* opCtx, ValidationActionEnum newAction) {
        std::abort();
    }

    boost::optional<ValidationLevelEnum> getValidationLevel() const {
        std::abort();
    }
    boost::optional<ValidationActionEnum> getValidationAction() const {
        std::abort();
    }

    Status updateValidator(OperationContext* opCtx,
                           BSONObj newValidator,
                           boost::optional<ValidationLevelEnum> newLevel,
                           boost::optional<ValidationActionEnum> newAction) {
        std::abort();
    }

    Status checkValidatorAPIVersionCompatability(OperationContext* opCtx) const final {
        std::abort();
    }

    bool isTemporary(OperationContext* opCtx) const {
        std::abort();
    }

    bool isClustered() const {
        std::abort();
    }

    bool getRecordPreImages() const {
        std::abort();
    }

    void setRecordPreImages(OperationContext* opCtx, bool val) {
        std::abort();
    }

    bool isCapped() const {
        std::abort();
    }

    CappedCallback* getCappedCallback() {
        std::abort();
    }
    const CappedCallback* getCappedCallback() const {
        std::abort();
    }

    std::shared_ptr<CappedInsertNotifier> getCappedInsertNotifier() const {
        std::abort();
    }

    uint64_t numRecords(OperationContext* opCtx) const {
        std::abort();
    }

    uint64_t dataSize(OperationContext* opCtx) const {
        std::abort();
    }

    bool isEmpty(OperationContext* opCtx) const {
        std::abort();
    }

    int averageObjectSize(OperationContext* const opCtx) const {
        std::abort();
    }

    uint64_t getIndexSize(OperationContext* opCtx, BSONObjBuilder* details, int scale) const {
        std::abort();
    }

    uint64_t getIndexFreeStorageBytes(OperationContext* const opCtx) const {
        std::abort();
    }

    boost::optional<Timestamp> getMinimumVisibleSnapshot() const {
        std::abort();
    }

    void setMinimumVisibleSnapshot(Timestamp name) {
        std::abort();
    }

    const CollatorInterface* getDefaultCollator() const {
        std::abort();
    }

    StatusWith<std::vector<BSONObj>> addCollationDefaultsToIndexSpecsForCreate(
        OperationContext* opCtx, const std::vector<BSONObj>& indexSpecs) const {
        std::abort();
    }

    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> makePlanExecutor(
        OperationContext* opCtx,
        const CollectionPtr& yieldableCollection,
        PlanYieldPolicy::YieldPolicy yieldPolicy,
        ScanDirection scanDirection,
        boost::optional<RecordId> resumeAfterRecordId) const {
        std::abort();
    }

    void establishOplogCollectionForLogging(OperationContext* opCtx) {
        std::abort();
    }

    void onDeregisterFromCatalog(OperationContext* opCtx) {}

    UUID uuid() const {
        return _uuid;
    }

    bool isCommitted() const final {
        return _committed;
    }

    void setCommitted(bool val) final {
        _committed = val;
    }

    void indexBuildSuccess(OperationContext* opCtx, IndexCatalogEntry* index) {
        std::abort();
    }

private:
    UUID _uuid = UUID::gen();
    NamespaceString _ns;
    RecordId _catalogId{0};
    clonable_ptr<IndexCatalog> _indexCatalog;
    bool _committed = true;
};

}  // namespace mongo
