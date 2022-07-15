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
#include "mongo/util/assert_util.h"

namespace mongo {

/**
 * This class comprises a mock Collection for use by CollectionCatalog unit tests.
 */
class CollectionMock : public Collection {
public:
    explicit CollectionMock(const NamespaceString& nss)
        : CollectionMock(UUID::gen(), nss, std::unique_ptr<IndexCatalog>()) {}
    CollectionMock(const UUID& uuid, const NamespaceString& nss)
        : CollectionMock(uuid, nss, std::unique_ptr<IndexCatalog>()) {}
    CollectionMock(const UUID& uuid,
                   const NamespaceString& nss,
                   std::unique_ptr<IndexCatalog> indexCatalog)
        : _uuid(uuid), _nss(nss), _indexCatalog(std::move(indexCatalog)) {}
    CollectionMock(const NamespaceString& nss, RecordId catalogId)
        : _nss(nss), _catalogId(std::move(catalogId)) {}
    ~CollectionMock() = default;

    std::shared_ptr<Collection> clone() const {
        return std::make_shared<CollectionMock>(*this);
    }


    SharedCollectionDecorations* getSharedDecorations() const {
        return nullptr;
    }

    void init(OperationContext* opCtx) {
        MONGO_UNREACHABLE;
    }

    RecordId getCatalogId() const {
        return _catalogId;
    }

    void setCatalogId(RecordId catalogId) {
        _catalogId = std::move(catalogId);
    }

    const NamespaceString& ns() const {
        return _nss;
    }

    Status rename(OperationContext* opCtx, const NamespaceString& nss, bool stayTemp) final {
        _nss = std::move(nss);
        return Status::OK();
    }

    const IndexCatalog* getIndexCatalog() const {
        return _indexCatalog.get();
    }
    IndexCatalog* getIndexCatalog() {
        return _indexCatalog.get();
    }

    RecordStore* getRecordStore() const {
        MONGO_UNREACHABLE;
    }
    std::shared_ptr<Ident> getSharedIdent() const {
        MONGO_UNREACHABLE;
    }

    BSONObj getValidatorDoc() const {
        MONGO_UNREACHABLE;
    }

    std::pair<SchemaValidationResult, Status> checkValidation(OperationContext* opCtx,
                                                              const BSONObj& document) const {
        MONGO_UNREACHABLE;
    }

    bool requiresIdIndex() const {
        MONGO_UNREACHABLE;
    }

    Snapshotted<BSONObj> docFor(OperationContext* opCtx, const RecordId& loc) const {
        MONGO_UNREACHABLE;
    }

    bool findDoc(OperationContext* opCtx, const RecordId& loc, Snapshotted<BSONObj>* out) const {
        MONGO_UNREACHABLE;
    }

    std::unique_ptr<SeekableRecordCursor> getCursor(OperationContext* opCtx, bool forward) const {
        MONGO_UNREACHABLE;
    }

    void deleteDocument(OperationContext* opCtx,
                        StmtId stmtId,
                        const RecordId& loc,
                        OpDebug* opDebug,
                        bool fromMigrate,
                        bool noWarn,
                        Collection::StoreDeletedDoc storeDeletedDoc,
                        CheckRecordId checkRecordId) const {
        MONGO_UNREACHABLE;
    }

    void deleteDocument(
        OperationContext* opCtx,
        Snapshotted<BSONObj> doc,
        StmtId stmtId,
        const RecordId& loc,
        OpDebug* opDebug,
        bool fromMigrate = false,
        bool noWarn = false,
        Collection::StoreDeletedDoc storeDeletedDoc = Collection::StoreDeletedDoc::Off,
        CheckRecordId checkRecordId = CheckRecordId::Off) const {
        MONGO_UNREACHABLE;
    }

    Status insertDocuments(OperationContext* opCtx,
                           std::vector<InsertStatement>::const_iterator begin,
                           std::vector<InsertStatement>::const_iterator end,
                           OpDebug* opDebug,
                           bool fromMigrate) const {
        MONGO_UNREACHABLE;
    }

    Status insertDocument(OperationContext* opCtx,
                          const InsertStatement& doc,
                          OpDebug* opDebug,
                          bool fromMigrate) const {
        MONGO_UNREACHABLE;
    }

    Status insertDocumentsForOplog(OperationContext* opCtx,
                                   std::vector<Record>* records,
                                   const std::vector<Timestamp>& timestamps) const {
        MONGO_UNREACHABLE;
    }

    Status insertDocumentForBulkLoader(OperationContext* opCtx,
                                       const BSONObj& doc,
                                       const OnRecordInsertedFn& onRecordInserted) const {
        MONGO_UNREACHABLE;
    }

    RecordId updateDocument(OperationContext* opCtx,
                            const RecordId& oldLocation,
                            const Snapshotted<BSONObj>& oldDoc,
                            const BSONObj& newDoc,
                            bool indexesAffected,
                            OpDebug* opDebug,
                            CollectionUpdateArgs* args) const {
        MONGO_UNREACHABLE;
    }

    bool updateWithDamagesSupported() const {
        MONGO_UNREACHABLE;
    }

    StatusWith<RecordData> updateDocumentWithDamages(OperationContext* opCtx,
                                                     const RecordId& loc,
                                                     const Snapshotted<RecordData>& oldRec,
                                                     const char* damageSource,
                                                     const mutablebson::DamageVector& damages,
                                                     CollectionUpdateArgs* args) const {
        MONGO_UNREACHABLE;
    }

    Status truncate(OperationContext* opCtx) {
        MONGO_UNREACHABLE;
    }

    void cappedTruncateAfter(OperationContext* opCtx, const RecordId& end, bool inclusive) const {
        MONGO_UNREACHABLE;
    }

    Validator parseValidator(OperationContext* opCtx,
                             const BSONObj& validator,
                             MatchExpressionParser::AllowedFeatureSet allowedFeatures,
                             boost::optional<multiversion::FeatureCompatibilityVersion>
                                 maxFeatureCompatibilityVersion) const {
        MONGO_UNREACHABLE;
    }

    void setValidator(OperationContext* opCtx, Validator validator) {
        MONGO_UNREACHABLE;
    }

    Status setValidationLevel(OperationContext* opCtx, ValidationLevelEnum newLevel) {
        MONGO_UNREACHABLE;
    }
    Status setValidationAction(OperationContext* opCtx, ValidationActionEnum newAction) {
        MONGO_UNREACHABLE;
    }

    boost::optional<ValidationLevelEnum> getValidationLevel() const {
        MONGO_UNREACHABLE;
    }
    boost::optional<ValidationActionEnum> getValidationAction() const {
        MONGO_UNREACHABLE;
    }

    Status updateValidator(OperationContext* opCtx,
                           BSONObj newValidator,
                           boost::optional<ValidationLevelEnum> newLevel,
                           boost::optional<ValidationActionEnum> newAction) {
        MONGO_UNREACHABLE;
    }

    Status checkValidatorAPIVersionCompatability(OperationContext* opCtx) const final {
        MONGO_UNREACHABLE;
    }

    bool isTemporary() const {
        MONGO_UNREACHABLE;
    }

    boost::optional<bool> getTimeseriesBucketsMayHaveMixedSchemaData() const {
        MONGO_UNREACHABLE;
    }

    void setTimeseriesBucketsMayHaveMixedSchemaData(OperationContext* opCtx,
                                                    boost::optional<bool> setting) {
        MONGO_UNREACHABLE;
    }

    bool doesTimeseriesBucketsDocContainMixedSchemaData(const BSONObj& bucketsDoc) const {
        MONGO_UNREACHABLE;
    }

    bool isClustered() const {
        return false;
    }

    boost::optional<ClusteredCollectionInfo> getClusteredInfo() const {
        MONGO_UNREACHABLE;
    }

    void updateClusteredIndexTTLSetting(OperationContext* opCtx,
                                        boost::optional<int64_t> expireAfterSeconds) {
        MONGO_UNREACHABLE;
    }

    Status updateCappedSize(OperationContext* opCtx,
                            boost::optional<long long> newCappedSize,
                            boost::optional<long long> newCappedMax) {
        MONGO_UNREACHABLE;
    }

    bool getRecordPreImages() const {
        MONGO_UNREACHABLE;
    }

    void setRecordPreImages(OperationContext* opCtx, bool val) {
        MONGO_UNREACHABLE;
    }

    bool isChangeStreamPreAndPostImagesEnabled() const {
        MONGO_UNREACHABLE;
    }

    void setChangeStreamPreAndPostImages(OperationContext* opCtx,
                                         ChangeStreamPreAndPostImagesOptions val) {
        MONGO_UNREACHABLE;
    }

    bool isCapped() const {
        return false;
    }

    long long getCappedMaxDocs() const {
        MONGO_UNREACHABLE;
    }

    long long getCappedMaxSize() const {
        MONGO_UNREACHABLE;
    }

    CappedCallback* getCappedCallback() {
        MONGO_UNREACHABLE;
    }
    const CappedCallback* getCappedCallback() const {
        MONGO_UNREACHABLE;
    }

    std::shared_ptr<CappedInsertNotifier> getCappedInsertNotifier() const {
        MONGO_UNREACHABLE;
    }

    long long numRecords(OperationContext* opCtx) const {
        MONGO_UNREACHABLE;
    }

    long long dataSize(OperationContext* opCtx) const {
        MONGO_UNREACHABLE;
    }

    bool isEmpty(OperationContext* opCtx) const {
        MONGO_UNREACHABLE;
    }

    int averageObjectSize(OperationContext* const opCtx) const {
        MONGO_UNREACHABLE;
    }

    uint64_t getIndexSize(OperationContext* opCtx, BSONObjBuilder* details, int scale) const {
        MONGO_UNREACHABLE;
    }

    uint64_t getIndexFreeStorageBytes(OperationContext* const opCtx) const {
        MONGO_UNREACHABLE;
    }

    boost::optional<Timestamp> getMinimumVisibleSnapshot() const {
        MONGO_UNREACHABLE;
    }

    void setMinimumVisibleSnapshot(Timestamp name) {
        MONGO_UNREACHABLE;
    }

    boost::optional<TimeseriesOptions> getTimeseriesOptions() const {
        MONGO_UNREACHABLE;
    }

    void setTimeseriesOptions(OperationContext* opCtx, const TimeseriesOptions& tsOptions) {
        MONGO_UNREACHABLE;
    }

    const CollatorInterface* getDefaultCollator() const {
        MONGO_UNREACHABLE;
    }

    const CollectionOptions& getCollectionOptions() const {
        MONGO_UNREACHABLE;
    }

    StatusWith<std::vector<BSONObj>> addCollationDefaultsToIndexSpecsForCreate(
        OperationContext* opCtx, const std::vector<BSONObj>& indexSpecs) const {
        MONGO_UNREACHABLE;
    }

    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> makePlanExecutor(
        OperationContext* opCtx,
        const CollectionPtr& yieldableCollection,
        PlanYieldPolicy::YieldPolicy yieldPolicy,
        ScanDirection scanDirection,
        const boost::optional<RecordId>& resumeAfterRecordId) const {
        MONGO_UNREACHABLE;
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
        MONGO_UNREACHABLE;
    }

    StatusWith<int> checkMetaDataForIndex(const std::string& indexName, const BSONObj& spec) const {
        MONGO_UNREACHABLE;
    }

    void updateTTLSetting(OperationContext* opCtx, StringData idxName, long long newExpireSeconds) {
        MONGO_UNREACHABLE;
    }

    void updateHiddenSetting(OperationContext* opCtx, StringData idxName, bool hidden) {
        MONGO_UNREACHABLE;
    }

    void updateUniqueSetting(OperationContext* opCtx, StringData idxName, bool unique) {
        MONGO_UNREACHABLE;
    }

    void updatePrepareUniqueSetting(OperationContext* opCtx,
                                    StringData idxName,
                                    bool prepareUnique) {
        MONGO_UNREACHABLE;
    }

    std::vector<std::string> repairInvalidIndexOptions(OperationContext* opCtx) {
        MONGO_UNREACHABLE;
    }

    void setIsTemp(OperationContext* opCtx, bool isTemp) {
        MONGO_UNREACHABLE;
    }

    void removeIndex(OperationContext* opCtx, StringData indexName) {
        MONGO_UNREACHABLE;
    }

    Status prepareForIndexBuild(OperationContext* opCtx,
                                const IndexDescriptor* spec,
                                boost::optional<UUID> buildUUID,
                                bool isBackgroundSecondaryBuild) {
        MONGO_UNREACHABLE;
    }

    boost::optional<UUID> getIndexBuildUUID(StringData indexName) const {
        MONGO_UNREACHABLE;
    }

    bool isIndexMultikey(OperationContext* opCtx,
                         StringData indexName,
                         MultikeyPaths* multikeyPaths,
                         int indexOffset) const {
        MONGO_UNREACHABLE;
    }

    bool setIndexIsMultikey(OperationContext* opCtx,
                            StringData indexName,
                            const MultikeyPaths& multikeyPaths,
                            int indexOffset) const {
        MONGO_UNREACHABLE;
    }

    void forceSetIndexIsMultikey(OperationContext* opCtx,
                                 const IndexDescriptor* desc,
                                 bool isMultikey,
                                 const MultikeyPaths& multikeyPaths) const final {
        MONGO_UNREACHABLE;
    }

    int getTotalIndexCount() const {
        MONGO_UNREACHABLE;
    }

    int getCompletedIndexCount() const {
        MONGO_UNREACHABLE;
    }

    BSONObj getIndexSpec(StringData indexName) const {
        MONGO_UNREACHABLE;
    }

    void getAllIndexes(std::vector<std::string>* names) const {
        MONGO_UNREACHABLE;
    }

    void getReadyIndexes(std::vector<std::string>* names) const {
        MONGO_UNREACHABLE;
    }

    bool isIndexPresent(StringData indexName) const {
        MONGO_UNREACHABLE;
    }

    bool isIndexReady(StringData indexName) const {
        MONGO_UNREACHABLE;
    }

    void replaceMetadata(OperationContext* opCtx,
                         std::shared_ptr<BSONCollectionCatalogEntry::MetaData> md) {
        MONGO_UNREACHABLE;
    }

private:
    UUID _uuid = UUID::gen();
    NamespaceString _nss;
    RecordId _catalogId{0};
    clonable_ptr<IndexCatalog> _indexCatalog;
    bool _committed = true;
};

}  // namespace mongo
