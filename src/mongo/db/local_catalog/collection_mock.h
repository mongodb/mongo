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

#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/index_catalog.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_yield_policy.h"
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
    ~CollectionMock() override = default;

    std::shared_ptr<Collection> clone() const override {
        std::unique_ptr<IndexCatalog> indexCatalogCopy =
            _indexCatalog ? _indexCatalog->clone() : nullptr;
        auto copy = std::make_shared<CollectionMock>(_uuid, _nss, std::move(indexCatalogCopy));
        copy->_catalogId = _catalogId;
        copy->_committed = _committed;
        copy->_options = _options;
        return copy;
    }

    SharedCollectionDecorations* getSharedDecorations() const override {
        return &_sharedCollectionDecorations;
    }

    void init(OperationContext* opCtx) override {
        MONGO_UNREACHABLE;
    }

    Status initFromExisting(OperationContext* opCtx,
                            const std::shared_ptr<const Collection>& collection,
                            const durable_catalog::CatalogEntry& catalogEntry,
                            boost::optional<Timestamp> readTimestamp) override {
        MONGO_UNREACHABLE;
    }

    RecordId getCatalogId() const override {
        return _catalogId;
    }

    void setCatalogId(RecordId catalogId) {
        _catalogId = std::move(catalogId);
    }

    const NamespaceString& ns() const override {
        return _nss;
    }

    Status rename(OperationContext* opCtx, const NamespaceString& nss, bool stayTemp) final {
        _nss = std::move(nss);
        return Status::OK();
    }

    const IndexCatalog* getIndexCatalog() const override {
        return _indexCatalog.get();
    }
    IndexCatalog* getIndexCatalog() override {
        return _indexCatalog.get();
    }

    RecordStore* getRecordStore() const override {
        MONGO_UNREACHABLE;
    }
    std::shared_ptr<Ident> getSharedIdent() const override {
        return std::make_shared<Ident>(_nss.toString_forTest());
    }
    void setIdent(std::shared_ptr<Ident> newIdent) override {
        MONGO_UNREACHABLE;
    }

    BSONObj getValidatorDoc() const override {
        return BSONObj();
    }

    std::pair<SchemaValidationResult, Status> checkValidation(
        OperationContext* opCtx, const BSONObj& document) const override {
        MONGO_UNREACHABLE;
    }

    Status checkValidationAndParseResult(OperationContext* opCtx,
                                         const BSONObj& document) const override {
        MONGO_UNREACHABLE;
    }

    bool requiresIdIndex() const override {
        MONGO_UNREACHABLE;
    }

    Snapshotted<BSONObj> docFor(OperationContext* opCtx, const RecordId& loc) const override {
        MONGO_UNREACHABLE;
    }

    bool findDoc(OperationContext* opCtx,
                 const RecordId& loc,
                 Snapshotted<BSONObj>* out) const override {
        MONGO_UNREACHABLE;
    }

    std::unique_ptr<SeekableRecordCursor> getCursor(OperationContext* opCtx,
                                                    bool forward) const override {
        MONGO_UNREACHABLE;
    }

    bool updateWithDamagesSupported() const override {
        MONGO_UNREACHABLE;
    }

    Status truncate(OperationContext* opCtx) override {
        MONGO_UNREACHABLE;
    }

    Validator parseValidator(
        OperationContext* opCtx,
        const BSONObj& validator,
        MatchExpressionParser::AllowedFeatureSet allowedFeatures) const override {
        MONGO_UNREACHABLE;
    }

    void setValidator(OperationContext* opCtx, Validator validator) override {
        MONGO_UNREACHABLE;
    }

    Status setValidationLevel(OperationContext* opCtx, ValidationLevelEnum newLevel) override {
        MONGO_UNREACHABLE;
    }
    Status setValidationAction(OperationContext* opCtx, ValidationActionEnum newAction) override {
        MONGO_UNREACHABLE;
    }

    boost::optional<ValidationLevelEnum> getValidationLevel() const override {
        MONGO_UNREACHABLE;
    }
    boost::optional<ValidationActionEnum> getValidationAction() const override {
        MONGO_UNREACHABLE;
    }

    Status updateValidator(OperationContext* opCtx,
                           BSONObj newValidator,
                           boost::optional<ValidationLevelEnum> newLevel,
                           boost::optional<ValidationActionEnum> newAction) override {
        MONGO_UNREACHABLE;
    }

    Status checkValidatorAPIVersionCompatability(OperationContext* opCtx) const final {
        MONGO_UNREACHABLE;
    }

    bool isTemporary() const override {
        MONGO_UNREACHABLE;
    }

    bool isTimeseriesCollection() const override {
        return getTimeseriesOptions().has_value();
    }

    bool isNewTimeseriesWithoutView() const override {
        MONGO_UNREACHABLE;
    }

    timeseries::MixedSchemaBucketsState getTimeseriesMixedSchemaBucketsState() const override {
        MONGO_UNREACHABLE;
    }

    void setTimeseriesBucketsMayHaveMixedSchemaData(OperationContext* opCtx,
                                                    boost::optional<bool> setting) override {
        MONGO_UNREACHABLE;
    }

    boost::optional<bool> timeseriesBucketingParametersHaveChanged() const override {
        MONGO_UNREACHABLE;
    }

    void setTimeseriesBucketingParametersChanged(OperationContext* opCtx,
                                                 boost::optional<bool> value) override {
        MONGO_UNREACHABLE;
    }

    void removeLegacyTimeseriesBucketingParametersHaveChanged(OperationContext* opCtx) final {
        MONGO_UNREACHABLE;
    }

    StatusWith<bool> doesTimeseriesBucketsDocContainMixedSchemaData(
        const BSONObj& bucketsDoc) const override {
        MONGO_UNREACHABLE;
    }

    bool getRequiresTimeseriesExtendedRangeSupport() const override {
        MONGO_UNREACHABLE;
    }

    void setRequiresTimeseriesExtendedRangeSupport(OperationContext* opCtx) const override {
        MONGO_UNREACHABLE;
    }

    bool areTimeseriesBucketsFixed() const override {
        MONGO_UNREACHABLE;
    }

    bool isClustered() const override {
        return false;
    }

    boost::optional<ClusteredCollectionInfo> getClusteredInfo() const override {
        MONGO_UNREACHABLE;
    }

    void updateClusteredIndexTTLSetting(OperationContext* opCtx,
                                        boost::optional<int64_t> expireAfterSeconds) override {
        MONGO_UNREACHABLE;
    }

    Status updateCappedSize(OperationContext* opCtx,
                            boost::optional<long long> newCappedSize,
                            boost::optional<long long> newCappedMax) override {
        MONGO_UNREACHABLE;
    }

    void unsetRecordIdsReplicated(OperationContext* opCtx) final {
        MONGO_UNREACHABLE;
    }

    bool isChangeStreamPreAndPostImagesEnabled() const override {
        MONGO_UNREACHABLE;
    }

    void setChangeStreamPreAndPostImages(OperationContext* opCtx,
                                         ChangeStreamPreAndPostImagesOptions val) override {
        MONGO_UNREACHABLE;
    }

    bool areRecordIdsReplicated() const override {
        return false;
    }

    bool isCapped() const override {
        return false;
    }

    long long getCappedMaxDocs() const override {
        MONGO_UNREACHABLE;
    }

    long long getCappedMaxSize() const override {
        MONGO_UNREACHABLE;
    }

    long long numRecords(OperationContext* opCtx) const override {
        return 0LL;
    }

    long long dataSize(OperationContext* opCtx) const override {
        MONGO_UNREACHABLE;
    }

    int64_t sizeOnDisk(OperationContext* opCtx, const StorageEngine& storageEngine) const override {
        MONGO_UNREACHABLE;
    }

    bool isEmpty(OperationContext* opCtx) const override {
        MONGO_UNREACHABLE;
    }

    int averageObjectSize(OperationContext* const opCtx) const override {
        MONGO_UNREACHABLE;
    }

    uint64_t getIndexSize(OperationContext* opCtx,
                          BSONObjBuilder* details,
                          int scale) const override {
        MONGO_UNREACHABLE;
    }

    uint64_t getIndexFreeStorageBytes(OperationContext* const opCtx) const override {
        MONGO_UNREACHABLE;
    }

    boost::optional<Timestamp> getMinimumValidSnapshot() const override {
        MONGO_UNREACHABLE;
    }

    void setMinimumValidSnapshot(Timestamp name) override {
        // no-op, called by unittests
    }

    const boost::optional<TimeseriesOptions>& getTimeseriesOptions() const override {
        return _options.timeseries;
    }

    void setTimeseriesOptions(OperationContext* opCtx,
                              const TimeseriesOptions& tsOptions) override {
        MONGO_UNREACHABLE;
    }

    const CollatorInterface* getDefaultCollator() const override {
        MONGO_UNREACHABLE;
    }

    const CollectionOptions& getCollectionOptions() const override {
        return _options;
    }

    StatusWith<BSONObj> addCollationDefaultsToIndexSpecsForCreate(
        OperationContext* opCtx, const BSONObj& indexSpecs) const override {
        MONGO_UNREACHABLE;
    }

    StatusWith<std::vector<BSONObj>> addCollationDefaultsToIndexSpecsForCreate(
        OperationContext* opCtx, const std::vector<BSONObj>& indexSpecs) const override {
        MONGO_UNREACHABLE;
    }

    void onDeregisterFromCatalog(ServiceContext* svcCtx) override {}

    UUID uuid() const override {
        return _uuid;
    }

    void indexBuildSuccess(OperationContext* opCtx, IndexCatalogEntry* index) override {
        MONGO_UNREACHABLE;
    }

    StatusWith<int> checkMetaDataForIndex(const std::string& indexName,
                                          const BSONObj& spec) const override {
        return 1;
    }

    void updateTTLSetting(OperationContext* opCtx,
                          StringData idxName,
                          long long newExpireSeconds) override {
        MONGO_UNREACHABLE;
    }

    void updateHiddenSetting(OperationContext* opCtx, StringData idxName, bool hidden) override {
        MONGO_UNREACHABLE;
    }

    void updateUniqueSetting(OperationContext* opCtx, StringData idxName, bool unique) override {
        MONGO_UNREACHABLE;
    }

    void updatePrepareUniqueSetting(OperationContext* opCtx,
                                    StringData idxName,
                                    bool prepareUnique) override {
        MONGO_UNREACHABLE;
    }

    std::vector<std::string> repairInvalidIndexOptions(OperationContext* opCtx,
                                                       bool removeDeprecatedFields) override {
        MONGO_UNREACHABLE;
    }

    void setIsTemp(OperationContext* opCtx, bool isTemp) override {
        MONGO_UNREACHABLE;
    }

    void removeIndex(OperationContext* opCtx, StringData indexName) override {
        MONGO_UNREACHABLE;
    }

    Status prepareForIndexBuild(OperationContext* opCtx,
                                const IndexDescriptor* spec,
                                StringData indexIdent,
                                boost::optional<UUID> buildUUID) override {
        MONGO_UNREACHABLE;
    }

    boost::optional<UUID> getIndexBuildUUID(StringData indexName) const override {
        MONGO_UNREACHABLE;
    }

    bool isIndexMultikey(OperationContext* opCtx,
                         StringData indexName,
                         MultikeyPaths* multikeyPaths,
                         int indexOffset) const override {
        MONGO_UNREACHABLE;
    }

    bool setIndexIsMultikey(OperationContext* opCtx,
                            StringData indexName,
                            const MultikeyPaths& multikeyPaths,
                            int indexOffset) const override {
        MONGO_UNREACHABLE;
    }

    void forceSetIndexIsMultikey(OperationContext* opCtx,
                                 const IndexDescriptor* desc,
                                 bool isMultikey,
                                 const MultikeyPaths& multikeyPaths) const final {
        MONGO_UNREACHABLE;
    }

    int getTotalIndexCount() const override {
        MONGO_UNREACHABLE;
    }

    int getCompletedIndexCount() const override {
        MONGO_UNREACHABLE;
    }

    BSONObj getIndexSpec(StringData indexName) const override {
        MONGO_UNREACHABLE;
    }

    void getAllIndexes(std::vector<std::string>* names) const override {
        MONGO_UNREACHABLE;
    }

    void getReadyIndexes(std::vector<std::string>* names) const override {
        MONGO_UNREACHABLE;
    }

    bool isIndexPresent(StringData indexName) const override {
        MONGO_UNREACHABLE;
    }

    bool isIndexReady(StringData indexName) const override {
        return true;
    }

    void replaceMetadata(OperationContext* opCtx,
                         std::shared_ptr<durable_catalog::CatalogEntryMetaData> md) override {
        MONGO_UNREACHABLE;
    }

    bool isMetadataEqual(const BSONObj& otherMetadata) const override {
        MONGO_UNREACHABLE;
    }

    bool needsCappedLock() const override {
        MONGO_UNREACHABLE;
    }

    bool isCappedAndNeedsDelete(OperationContext* opCtx) const override {
        MONGO_UNREACHABLE;
    }

private:
    UUID _uuid = UUID::gen();
    NamespaceString _nss;
    RecordId _catalogId{0};
    clonable_ptr<IndexCatalog> _indexCatalog;
    bool _committed = true;
    CollectionOptions _options;
    mutable SharedCollectionDecorations _sharedCollectionDecorations;
};

}  // namespace mongo
