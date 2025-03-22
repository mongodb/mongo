/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

namespace mongo {

/**
 * This class comprises a mock Collection for use by timeseries rewrite unit tests. Notably, this is
 * distinct from the CollectionMock used for CollectionCatalog unit tests.
 */
class TimeseriesRewritesCollectionMock : public Collection {
public:
    explicit TimeseriesRewritesCollectionMock(
        const TimeseriesOptions timeseriesOptions,
        const timeseries::MixedSchemaBucketsState timeseriesMixedSchemaBucketsState,
        const boost::optional<bool> timeseriesBucketingParametersHaveChanged,
        const bool isTimeseriesCollection,
        const bool isNewTimeseriesWithoutView)
        : _timeseriesOptions(timeseriesOptions),
          _timeseriesMixedSchemaBucketsState(timeseriesMixedSchemaBucketsState),
          _isTimeseriesCollection(isTimeseriesCollection),
          _isNewTimeseriesWithoutView(isNewTimeseriesWithoutView) {}
    ~TimeseriesRewritesCollectionMock() override = default;

    const boost::optional<TimeseriesOptions>& getTimeseriesOptions() const override {
        return _timeseriesOptions;
    }

    timeseries::MixedSchemaBucketsState getTimeseriesMixedSchemaBucketsState() const override {
        return _timeseriesMixedSchemaBucketsState;
    }

    boost::optional<bool> timeseriesBucketingParametersHaveChanged() const override {
        return _timeseriesBucketingParametersHaveChanged;
    }

    bool isTimeseriesCollection() const override {
        return _isTimeseriesCollection;
    }

    bool isNewTimeseriesWithoutView() const override {
        return _isNewTimeseriesWithoutView;
    }

    // Everything below this is MONGO_UNREACHABLE overrides of virtual functions.

    std::shared_ptr<Collection> clone() const override {
        MONGO_UNREACHABLE;
    }

    SharedCollectionDecorations* getSharedDecorations() const override {
        MONGO_UNREACHABLE;
    }

    void init(OperationContext* opCtx) override {
        MONGO_UNREACHABLE;
    }

    Status initFromExisting(OperationContext*,
                            const std::shared_ptr<const Collection>&,
                            const DurableCatalogEntry&,
                            boost::optional<Timestamp>) override {
        MONGO_UNREACHABLE;
    }

    RecordId getCatalogId() const override {
        MONGO_UNREACHABLE;
    }

    void setCatalogId(RecordId catalogId) {
        MONGO_UNREACHABLE;
    }

    const NamespaceString& ns() const override {
        MONGO_UNREACHABLE;
    }

    Status rename(OperationContext*, const NamespaceString&, bool) final {
        MONGO_UNREACHABLE;
    }

    const IndexCatalog* getIndexCatalog() const override {
        MONGO_UNREACHABLE;
    }
    IndexCatalog* getIndexCatalog() override {
        MONGO_UNREACHABLE;
    }

    RecordStore* getRecordStore() const override {
        MONGO_UNREACHABLE;
    }
    std::shared_ptr<Ident> getSharedIdent() const override {
        MONGO_UNREACHABLE;
    }
    void setIdent(std::shared_ptr<Ident>) override {
        MONGO_UNREACHABLE;
    }

    BSONObj getValidatorDoc() const override {
        MONGO_UNREACHABLE;
    }

    std::pair<SchemaValidationResult, Status> checkValidation(OperationContext*,
                                                              const BSONObj&) const override {
        MONGO_UNREACHABLE;
    }

    Status checkValidationAndParseResult(OperationContext*, const BSONObj&) const override {
        MONGO_UNREACHABLE;
    }

    bool requiresIdIndex() const override {
        MONGO_UNREACHABLE;
    }

    Snapshotted<BSONObj> docFor(OperationContext*, const RecordId&) const override {
        MONGO_UNREACHABLE;
    }

    bool findDoc(OperationContext*, const RecordId&, Snapshotted<BSONObj>*) const override {
        MONGO_UNREACHABLE;
    }

    std::unique_ptr<SeekableRecordCursor> getCursor(OperationContext*, bool) const override {
        MONGO_UNREACHABLE;
    }

    bool updateWithDamagesSupported() const override {
        MONGO_UNREACHABLE;
    }

    Status truncate(OperationContext*) override {
        MONGO_UNREACHABLE;
    }

    void cappedTruncateAfter(OperationContext*, const RecordId&, bool) const {
        MONGO_UNREACHABLE;
    }

    Validator parseValidator(
        OperationContext*,
        const BSONObj&,
        MatchExpressionParser::AllowedFeatureSet,
        boost::optional<multiversion::FeatureCompatibilityVersion>) const override {
        MONGO_UNREACHABLE;
    }

    void setValidator(OperationContext*, Validator) override {
        MONGO_UNREACHABLE;
    }

    Status setValidationLevel(OperationContext*, ValidationLevelEnum) override {
        MONGO_UNREACHABLE;
    }
    Status setValidationAction(OperationContext*, ValidationActionEnum) override {
        MONGO_UNREACHABLE;
    }

    boost::optional<ValidationLevelEnum> getValidationLevel() const override {
        MONGO_UNREACHABLE;
    }
    boost::optional<ValidationActionEnum> getValidationAction() const override {
        MONGO_UNREACHABLE;
    }

    Status updateValidator(OperationContext*,
                           BSONObj,
                           boost::optional<ValidationLevelEnum>,
                           boost::optional<ValidationActionEnum>) override {
        MONGO_UNREACHABLE;
    }

    Status checkValidatorAPIVersionCompatability(OperationContext*) const final {
        MONGO_UNREACHABLE;
    }

    bool isTemporary() const override {
        MONGO_UNREACHABLE;
    }

    void setTimeseriesBucketsMayHaveMixedSchemaData(OperationContext*,
                                                    boost::optional<bool>) override {
        MONGO_UNREACHABLE;
    }

    void setTimeseriesBucketingParametersChanged(OperationContext*,
                                                 boost::optional<bool>) override {
        MONGO_UNREACHABLE;
    }

    void removeLegacyTimeseriesBucketingParametersHaveChanged(OperationContext*) override {
        MONGO_UNREACHABLE;
    }

    StatusWith<bool> doesTimeseriesBucketsDocContainMixedSchemaData(const BSONObj&) const override {
        MONGO_UNREACHABLE;
    }

    bool getRequiresTimeseriesExtendedRangeSupport() const override {
        MONGO_UNREACHABLE;
    }

    void setRequiresTimeseriesExtendedRangeSupport(OperationContext*) const override {
        MONGO_UNREACHABLE;
    }

    bool areTimeseriesBucketsFixed() const override {
        MONGO_UNREACHABLE;
    }

    bool isClustered() const override {
        MONGO_UNREACHABLE;
    }

    boost::optional<ClusteredCollectionInfo> getClusteredInfo() const override {
        MONGO_UNREACHABLE;
    }

    void updateClusteredIndexTTLSetting(OperationContext*, boost::optional<int64_t>) override {
        MONGO_UNREACHABLE;
    }

    Status updateCappedSize(OperationContext*,
                            boost::optional<long long>,
                            boost::optional<long long>) override {
        MONGO_UNREACHABLE;
    }

    void unsetRecordIdsReplicated(OperationContext*) final {
        MONGO_UNREACHABLE;
    }

    bool isChangeStreamPreAndPostImagesEnabled() const override {
        MONGO_UNREACHABLE;
    }

    void setChangeStreamPreAndPostImages(OperationContext*,
                                         ChangeStreamPreAndPostImagesOptions) override {
        MONGO_UNREACHABLE;
    }

    bool areRecordIdsReplicated() const override {
        MONGO_UNREACHABLE;
    }

    bool isCapped() const override {
        MONGO_UNREACHABLE;
    }

    long long getCappedMaxDocs() const override {
        MONGO_UNREACHABLE;
    }

    long long getCappedMaxSize() const override {
        MONGO_UNREACHABLE;
    }

    bool usesCappedSnapshots() const override {
        MONGO_UNREACHABLE;
    }

    std::vector<RecordId> reserveCappedRecordIds(OperationContext*, size_t) const final {
        MONGO_UNREACHABLE;
    }

    void registerCappedInserts(OperationContext*, const RecordId&, const RecordId&) const override {
        MONGO_UNREACHABLE;
    }

    CappedVisibilityObserver* getCappedVisibilityObserver() const override {
        MONGO_UNREACHABLE;
    }

    CappedVisibilitySnapshot takeCappedVisibilitySnapshot() const override {
        MONGO_UNREACHABLE;
    }

    long long numRecords(OperationContext*) const override {
        MONGO_UNREACHABLE;
    }

    long long dataSize(OperationContext*) const override {
        MONGO_UNREACHABLE;
    }

    int64_t sizeOnDisk(OperationContext* opCtx, const StorageEngine& storageEngine) const override {
        MONGO_UNREACHABLE;
    }

    bool isEmpty(OperationContext*) const override {
        MONGO_UNREACHABLE;
    }

    int averageObjectSize(OperationContext* const) const override {
        MONGO_UNREACHABLE;
    }

    uint64_t getIndexSize(OperationContext*, BSONObjBuilder*, int) const override {
        MONGO_UNREACHABLE;
    }

    uint64_t getIndexFreeStorageBytes(OperationContext* const) const override {
        MONGO_UNREACHABLE;
    }

    boost::optional<Timestamp> getMinimumValidSnapshot() const override {
        MONGO_UNREACHABLE;
    }

    void setMinimumValidSnapshot(Timestamp) override {
        MONGO_UNREACHABLE;
    }

    void setTimeseriesOptions(OperationContext*, const TimeseriesOptions&) override {
        MONGO_UNREACHABLE;
    }

    const CollatorInterface* getDefaultCollator() const override {
        MONGO_UNREACHABLE;
    }

    const CollectionOptions& getCollectionOptions() const override {
        MONGO_UNREACHABLE;
    }

    StatusWith<BSONObj> addCollationDefaultsToIndexSpecsForCreate(OperationContext*,
                                                                  const BSONObj&) const override {
        MONGO_UNREACHABLE;
    }

    StatusWith<std::vector<BSONObj>> addCollationDefaultsToIndexSpecsForCreate(
        OperationContext*, const std::vector<BSONObj>&) const override {
        MONGO_UNREACHABLE;
    }

    void onDeregisterFromCatalog(OperationContext*) override {
        MONGO_UNREACHABLE;
    }

    UUID uuid() const override {
        MONGO_UNREACHABLE;
    }

    void indexBuildSuccess(OperationContext*, IndexCatalogEntry*) override {
        MONGO_UNREACHABLE;
    }

    StatusWith<int> checkMetaDataForIndex(const std::string&, const BSONObj&) const override {
        MONGO_UNREACHABLE;
    }

    void updateTTLSetting(OperationContext*, StringData, long long) override {
        MONGO_UNREACHABLE;
    }

    void updateHiddenSetting(OperationContext*, StringData, bool) override {
        MONGO_UNREACHABLE;
    }

    void updateUniqueSetting(OperationContext*, StringData, bool) override {
        MONGO_UNREACHABLE;
    }

    void updatePrepareUniqueSetting(OperationContext*, StringData, bool) override {
        MONGO_UNREACHABLE;
    }

    std::vector<std::string> repairInvalidIndexOptions(OperationContext*) override {
        MONGO_UNREACHABLE;
    }

    void setIsTemp(OperationContext*, bool) override {
        MONGO_UNREACHABLE;
    }

    void removeIndex(OperationContext*, StringData) override {
        MONGO_UNREACHABLE;
    }

    Status prepareForIndexBuild(OperationContext*,
                                const IndexDescriptor*,
                                boost::optional<UUID>) override {
        MONGO_UNREACHABLE;
    }

    boost::optional<UUID> getIndexBuildUUID(StringData) const override {
        MONGO_UNREACHABLE;
    }

    bool isIndexMultikey(OperationContext*, StringData, MultikeyPaths*, int) const override {
        MONGO_UNREACHABLE;
    }

    bool setIndexIsMultikey(OperationContext*,
                            StringData,
                            const MultikeyPaths&,
                            int) const override {
        MONGO_UNREACHABLE;
    }

    void forceSetIndexIsMultikey(OperationContext*,
                                 const IndexDescriptor*,
                                 bool,
                                 const MultikeyPaths&) const final {
        MONGO_UNREACHABLE;
    }

    int getTotalIndexCount() const override {
        MONGO_UNREACHABLE;
    }

    int getCompletedIndexCount() const override {
        MONGO_UNREACHABLE;
    }

    BSONObj getIndexSpec(StringData) const override {
        MONGO_UNREACHABLE;
    }

    void getAllIndexes(std::vector<std::string>*) const override {
        MONGO_UNREACHABLE;
    }

    void getReadyIndexes(std::vector<std::string>*) const override {
        MONGO_UNREACHABLE;
    }

    bool isIndexPresent(StringData) const override {
        MONGO_UNREACHABLE;
    }

    bool isIndexReady(StringData) const override {
        MONGO_UNREACHABLE;
    }

    void replaceMetadata(OperationContext*,
                         std::shared_ptr<BSONCollectionCatalogEntry::MetaData>) override {
        MONGO_UNREACHABLE;
    }

    bool isMetadataEqual(const BSONObj&) const override {
        MONGO_UNREACHABLE;
    }

    bool needsCappedLock() const override {
        MONGO_UNREACHABLE;
    }

    bool isCappedAndNeedsDelete(OperationContext*) const override {
        MONGO_UNREACHABLE;
    }

private:
    const boost::optional<TimeseriesOptions> _timeseriesOptions;
    const timeseries::MixedSchemaBucketsState _timeseriesMixedSchemaBucketsState;
    const boost::optional<bool> _timeseriesBucketingParametersHaveChanged;
    const bool _isTimeseriesCollection;
    const bool _isNewTimeseriesWithoutView;
};

}  // namespace mongo
