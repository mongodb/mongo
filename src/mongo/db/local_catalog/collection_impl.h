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

#include "mongo/base/clonable_ptr.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/local_catalog/clustered_collection_options_gen.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/local_catalog/collection_options_gen.h"
#include "mongo/db/local_catalog/durable_catalog_entry.h"
#include "mongo/db/local_catalog/durable_catalog_entry_metadata.h"
#include "mongo/db/local_catalog/index_catalog.h"
#include "mongo/db/local_catalog/index_catalog_entry.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/change_stream_pre_and_post_images_options_gen.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/uuid.h"
#include "mongo/util/version/releases.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class CollectionImpl final : public Collection {
public:
    // Uses the collator factory to convert the BSON representation of a collator to a
    // CollatorInterface. Returns null if the BSONObj is empty. We expect the stored collation to be
    // valid, since it gets validated on collection create.
    static std::unique_ptr<CollatorInterface> parseCollation(OperationContext* opCtx,
                                                             const NamespaceString& nss,
                                                             BSONObj collationSpec);


    explicit CollectionImpl(OperationContext* opCtx,
                            const NamespaceString& nss,
                            RecordId catalogId,
                            std::shared_ptr<durable_catalog::CatalogEntryMetaData> metadata,
                            std::unique_ptr<RecordStore> recordStore);

    ~CollectionImpl() override;

    std::shared_ptr<Collection> clone() const final;

    class FactoryImpl : public Factory {
    public:
        std::shared_ptr<Collection> make(
            OperationContext* opCtx,
            const NamespaceString& nss,
            RecordId catalogId,
            std::shared_ptr<durable_catalog::CatalogEntryMetaData> metadata,
            std::unique_ptr<RecordStore> rs) const final;
    };

    SharedCollectionDecorations* getSharedDecorations() const final;

    void init(OperationContext* opCtx) final;
    Status initFromExisting(OperationContext* opCtx,
                            const std::shared_ptr<const Collection>& collection,
                            const durable_catalog::CatalogEntry& catalogEntry,
                            boost::optional<Timestamp> readTimestamp) final;
    bool isInitialized() const final;

    const NamespaceString& ns() const final {
        return _ns;
    }

    Status rename(OperationContext* opCtx, const NamespaceString& nss, bool stayTemp) final;

    RecordId getCatalogId() const final {
        return _catalogId;
    }

    UUID uuid() const final {
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
        return _shared->_recordStore->getSharedIdent();
    }

    void setIdent(std::shared_ptr<Ident> newIdent) final {
        _shared->_recordStore->setIdent(std::move(newIdent));
    }

    BSONObj getValidatorDoc() const final {
        return _validator.validatorDoc.getOwned();
    }

    std::pair<SchemaValidationResult, Status> checkValidation(OperationContext* opCtx,
                                                              const BSONObj& document) const final;

    Status checkValidationAndParseResult(OperationContext* opCtx,
                                         const BSONObj& document) const final;

    bool requiresIdIndex() const final;

    Snapshotted<BSONObj> docFor(OperationContext* opCtx, const RecordId& loc) const final {
        return Snapshotted<BSONObj>(
            shard_role_details::getRecoveryUnit(opCtx)->getSnapshotId(),
            _shared->_recordStore->dataFor(opCtx, *shard_role_details::getRecoveryUnit(opCtx), loc)
                .releaseToBson());
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

    bool updateWithDamagesSupported() const final;

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
     * Returns a non-ok Status if validator is not legal for this collection.
     */
    Validator parseValidator(OperationContext* opCtx,
                             const BSONObj& validator,
                             MatchExpressionParser::AllowedFeatureSet allowedFeatures) const final;

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

    bool isChangeStreamPreAndPostImagesEnabled() const final;
    void setChangeStreamPreAndPostImages(OperationContext* opCtx,
                                         ChangeStreamPreAndPostImagesOptions val) final;

    bool isTimeseriesCollection() const final;

    bool isNewTimeseriesWithoutView() const final;

    bool isTemporary() const final;

    timeseries::MixedSchemaBucketsState getTimeseriesMixedSchemaBucketsState() const final;

    void setTimeseriesBucketsMayHaveMixedSchemaData(OperationContext* opCtx,
                                                    boost::optional<bool> setting) final;

    boost::optional<bool> timeseriesBucketingParametersHaveChanged() const final;

    void setTimeseriesBucketingParametersChanged(OperationContext* opCtx,
                                                 boost::optional<bool> value) final;

    void removeLegacyTimeseriesBucketingParametersHaveChanged(OperationContext* opCtx) final;

    StatusWith<bool> doesTimeseriesBucketsDocContainMixedSchemaData(
        const BSONObj& bucketsDoc) const final;

    bool getRequiresTimeseriesExtendedRangeSupport() const final;
    void setRequiresTimeseriesExtendedRangeSupport(OperationContext* opCtx) const final;

    bool areTimeseriesBucketsFixed() const final;

    /**
     * isClustered() relies on the object returned from getClusteredInfo(). If
     * ClusteredCollectionInfo exists, the collection is clustered.
     */
    bool isClustered() const final;
    boost::optional<ClusteredCollectionInfo> getClusteredInfo() const final;
    void updateClusteredIndexTTLSetting(OperationContext* opCtx,
                                        boost::optional<int64_t> expireAfterSeconds) final;

    Status updateCappedSize(OperationContext* opCtx,
                            boost::optional<long long> newCappedSize,
                            boost::optional<long long> newCappedMax) final;

    void unsetRecordIdsReplicated(OperationContext* opCtx) final;

    //
    // Stats
    //

    bool areRecordIdsReplicated() const final;

    bool isCapped() const final;
    long long getCappedMaxDocs() const final;
    long long getCappedMaxSize() const final;

    long long numRecords(OperationContext* opCtx) const final;

    long long dataSize(OperationContext* opCtx) const final;

    int64_t sizeOnDisk(OperationContext* opCtx, const StorageEngine& storageEngine) const final;

    /**
     * Currently fast counts are prone to false negative as it is not tolerant to unclean shutdowns.
     * So, verify that the collection is really empty by opening the collection cursor and reading
     * the first document.
     * Expects to hold at least collection lock in mode IS.
     * TODO SERVER-24266: After making fast counts tolerant to unclean shutdowns, we can make use of
     * fast count to determine whether the collection is empty and remove cursor checking logic.
     */
    bool isEmpty(OperationContext* opCtx) const final;

    inline int averageObjectSize(OperationContext* opCtx) const override {
        uint64_t n = numRecords(opCtx);

        if (n == 0)
            return 0;
        return static_cast<int>(dataSize(opCtx) / n);
    }

    uint64_t getIndexSize(OperationContext* opCtx,
                          BSONObjBuilder* details = nullptr,
                          int scale = 1) const final;

    uint64_t getIndexFreeStorageBytes(OperationContext* opCtx) const final;

    boost::optional<Timestamp> getMinimumValidSnapshot() const final {
        return _minValidSnapshot;
    }

    /**
     * Updates the minimum valid snapshot. The 'newMinimumValidSnapshot' is ignored if it would
     * set the minimum valid snapshot backwards in time.
     */
    void setMinimumValidSnapshot(Timestamp newMinimumValidSnapshot) final;

    const boost::optional<TimeseriesOptions>& getTimeseriesOptions() const final;
    void setTimeseriesOptions(OperationContext* opCtx, const TimeseriesOptions& tsOptions) final;

    /**
     * Get a pointer to the collection's default collator. The pointer must not be used after this
     * Collection is destroyed.
     */
    const CollatorInterface* getDefaultCollator() const final;

    const CollectionOptions& getCollectionOptions() const final;

    StatusWith<BSONObj> addCollationDefaultsToIndexSpecsForCreate(
        OperationContext* opCtx, const BSONObj& indexSpecs) const final;
    StatusWith<std::vector<BSONObj>> addCollationDefaultsToIndexSpecsForCreate(
        OperationContext* opCtx, const std::vector<BSONObj>& indexSpecs) const final;

    void indexBuildSuccess(OperationContext* opCtx, IndexCatalogEntry* index) final;

    void onDeregisterFromCatalog(ServiceContext* svcCtx) final;

    StatusWith<int> checkMetaDataForIndex(const std::string& indexName,
                                          const BSONObj& spec) const final;

    void updateTTLSetting(OperationContext* opCtx,
                          StringData idxName,
                          long long newExpireSeconds) final;

    void updateHiddenSetting(OperationContext* opCtx, StringData idxName, bool hidden) final;

    void updateUniqueSetting(OperationContext* opCtx, StringData idxName, bool unique) final;

    void updatePrepareUniqueSetting(OperationContext* opCtx,
                                    StringData idxName,
                                    bool prepareUnique) final;

    std::vector<std::string> repairInvalidIndexOptions(OperationContext* opCtx,
                                                       bool removeDeprecatedFields) final;

    void setIsTemp(OperationContext* opCtx, bool isTemp) final;

    void removeIndex(OperationContext* opCtx, StringData indexName) final;

    Status prepareForIndexBuild(OperationContext* opCtx,
                                const IndexDescriptor* spec,
                                StringData indexIdent,
                                boost::optional<UUID> buildUUID) final;

    boost::optional<UUID> getIndexBuildUUID(StringData indexName) const final;

    bool isIndexMultikey(OperationContext* opCtx,
                         StringData indexName,
                         MultikeyPaths* multikeyPaths,
                         int indexOffset) const final;

    bool setIndexIsMultikey(OperationContext* opCtx,
                            StringData indexName,
                            const MultikeyPaths& multikeyPaths,
                            int indexOffset) const final;

    void forceSetIndexIsMultikey(OperationContext* opCtx,
                                 const IndexDescriptor* desc,
                                 bool isMultikey,
                                 const MultikeyPaths& multikeyPaths) const final;

    int getTotalIndexCount() const final;

    int getCompletedIndexCount() const final;

    BSONObj getIndexSpec(StringData indexName) const final;

    void getAllIndexes(std::vector<std::string>* names) const final;

    void getReadyIndexes(std::vector<std::string>* names) const final;

    bool isIndexPresent(StringData indexName) const final;

    bool isIndexReady(StringData indexName) const final;

    void replaceMetadata(OperationContext* opCtx,
                         std::shared_ptr<durable_catalog::CatalogEntryMetaData> md) final;

    bool isMetadataEqual(const BSONObj& otherMetadata) const final;

    bool needsCappedLock() const final;

    bool isCappedAndNeedsDelete(OperationContext* opCtx) const final;

private:
    /**
     * Writes metadata through durable_catalog. Func should have the function signature
     * 'void(durable_catalog::CatalogEntryMetaData&)'
     */
    template <typename Func>
    void _writeMetadata(OperationContext* opCtx, Func func);

    /**
     * Creates a mutable copy of the current metadata, including uncommitted multikey changes if
     * applicable.
     */
    std::shared_ptr<durable_catalog::CatalogEntryMetaData> _copyMetadataForWrite(
        OperationContext* opCtx);

    /**
     * Helper for init() and initFromExisting() to initialize shared state.
     */
    void _initShared(OperationContext* opCtx, const CollectionOptions& options);

    /**
     * Helper for init() and initFromExisting() to initialize common state.
     */
    void _initCommon(OperationContext* opCtx);

    /**
     * Helper to set the _metadata field. Every set must check for the storageEngine option to work
     * around the issue described in SERVER-91193 and SERVER-91194.
     */
    void _setMetadata(OperationContext* opCtx,
                      std::shared_ptr<durable_catalog::CatalogEntryMetaData>&& metadata);

    /**
     * Holder of shared state between CollectionImpl clones and snapshots at a point in time.
     */
    struct SharedState {
        SharedState(OperationContext* opCtx,
                    CollectionImpl* collection,
                    std::unique_ptr<RecordStore> recordStore,
                    const CollectionOptions& options);
        ~SharedState();

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

        const bool _needCappedLock;

        // Time-series collections are allowed to contain measurements with arbitrary dates;
        // however, many of our query optimizations only work properly with dates that can be stored
        // as an offset in seconds from the Unix epoch within 31 bits (roughly 1970-2038). When this
        // flag is set to true, these optimizations will be disabled. It must be set to true if the
        // collection contains any measurements with dates outside this normal range.
        //
        // This is set from the write path where we only hold an IX lock, so we want to be able to
        // set it from a const method on the Collection. In order to do this, we need to make it
        // mutable. Given that the value may only transition from false to true, but never back
        // again, and that we store and retrieve it atomically, this should be safe.
        mutable AtomicWord<bool> _requiresTimeseriesExtendedRangeSupport{false};
    };

    NamespaceString _ns;
    RecordId _catalogId;
    UUID _uuid;
    std::shared_ptr<SharedState> _shared;

    // Collection metadata cached from the durable_catalog. Is kept separate from the SharedState
    // because it may be updated.
    std::shared_ptr<const durable_catalog::CatalogEntryMetaData> _metadata;

    clonable_ptr<IndexCatalog> _indexCatalog;

    // The validator is using shared state internally. Collections share validator until a new
    // validator is set in setValidator which sets a new instance.
    Validator _validator;

    // The earliest snapshot that is allowed to use this collection.
    boost::optional<Timestamp> _minValidSnapshot;

    bool _initialized = false;
};

}  // namespace mongo
