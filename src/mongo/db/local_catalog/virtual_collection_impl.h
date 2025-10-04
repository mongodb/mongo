/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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
#include "mongo/base/error_codes.h"
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
#include "mongo/db/local_catalog/virtual_collection_options.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/change_stream_pre_and_post_images_options_gen.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/virtual_collection/external_record_store.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/util/assert_util.h"
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
class VirtualCollectionImpl final : public Collection {
public:
    static std::shared_ptr<Collection> make(OperationContext* opCtx,
                                            const NamespaceString& nss,
                                            const CollectionOptions& options,
                                            const VirtualCollectionOptions& vopts);

    // Constructor for a virtual collection.
    explicit VirtualCollectionImpl(OperationContext* opCtx,
                                   const NamespaceString& nss,
                                   const CollectionOptions& options,
                                   std::unique_ptr<ExternalRecordStore> recordStore);

    VirtualCollectionImpl(const VirtualCollectionImpl&) = default;

    ~VirtualCollectionImpl() override = default;

    const VirtualCollectionOptions& getVirtualCollectionOptions() const {
        return _shared->_recordStore->getOptions();
    }

    std::shared_ptr<Collection> clone() const final {
        return std::make_shared<VirtualCollectionImpl>(*this);
    }

    SharedCollectionDecorations* getSharedDecorations() const final {
        return &_shared->_sharedDecorations;
    }

    Status initFromExisting(OperationContext* opCtx,
                            const std::shared_ptr<const Collection>& collection,
                            const durable_catalog::CatalogEntry& catalogEntry,
                            boost::optional<Timestamp> readTimestamp) final {
        unimplementedTasserted();
        return Status(ErrorCodes::UnknownError, "unknown");
    };

    bool isInitialized() const final {
        return true;
    };

    const NamespaceString& ns() const final {
        return _nss;
    }

    Status rename(OperationContext* opCtx, const NamespaceString& nss, bool stayTemp) final {
        unimplementedTasserted();
        return Status(ErrorCodes::UnknownError, "unknown");
    }

    RecordId getCatalogId() const final {
        return RecordId();
    }

    UUID uuid() const final {
        return *_options.uuid;
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

    // A virtual collection can't have an 'ident' because 'ident' is an identifier to a WT table
    // which a virtual collection does not have. So returns nullptr.
    std::shared_ptr<Ident> getSharedIdent() const final {
        return nullptr;
    }

    void setIdent(std::shared_ptr<Ident> newIdent) final {
        unimplementedTasserted();
    }

    BSONObj getValidatorDoc() const final {
        return BSONObj();
    }

    std::pair<SchemaValidationResult, Status> checkValidation(OperationContext* opCtx,
                                                              const BSONObj& document) const final {
        unimplementedTasserted();
        return {SchemaValidationResult::kError, Status(ErrorCodes::UnknownError, "unknown")};
    }

    Status checkValidationAndParseResult(OperationContext* opCtx,
                                         const BSONObj& document) const final {
        unimplementedTasserted();
        return Status(ErrorCodes::UnknownError, "unknown");
    }

    bool requiresIdIndex() const final {
        return false;
    };

    Snapshotted<BSONObj> docFor(OperationContext* opCtx, const RecordId& loc) const final {
        unimplementedTasserted();
        return Snapshotted<BSONObj>();
    }

    bool findDoc(OperationContext* opCtx,
                 const RecordId& loc,
                 Snapshotted<BSONObj>* out) const final {
        unimplementedTasserted();
        return false;
    }

    std::unique_ptr<SeekableRecordCursor> getCursor(OperationContext* opCtx,
                                                    bool forward = true) const final {
        return _shared->_recordStore->getCursor(
            opCtx, *shard_role_details::getRecoveryUnit(opCtx), forward);
    }

    bool updateWithDamagesSupported() const final {
        unimplementedTasserted();
        return false;
    }

    Status truncate(OperationContext* opCtx) final {
        unimplementedTasserted();
        return Status(ErrorCodes::UnknownError, "unknown");
    }

    Validator parseValidator(OperationContext* opCtx,
                             const BSONObj& validator,
                             MatchExpressionParser::AllowedFeatureSet allowedFeatures) const final {
        unimplementedTasserted();
        return Validator();
    }

    void setValidator(OperationContext* opCtx, Validator validator) final {
        unimplementedTasserted();
    }

    Status setValidationLevel(OperationContext* opCtx, ValidationLevelEnum newLevel) final {
        unimplementedTasserted();
        return Status(ErrorCodes::UnknownError, "unknown");
    }

    Status setValidationAction(OperationContext* opCtx, ValidationActionEnum newAction) final {
        unimplementedTasserted();
        return Status(ErrorCodes::UnknownError, "unknown");
    }

    boost::optional<ValidationLevelEnum> getValidationLevel() const final {
        unimplementedTasserted();
        return boost::none;
    }

    boost::optional<ValidationActionEnum> getValidationAction() const final {
        unimplementedTasserted();
        return boost::none;
    }

    Status updateValidator(OperationContext* opCtx,
                           BSONObj newValidator,
                           boost::optional<ValidationLevelEnum> newLevel,
                           boost::optional<ValidationActionEnum> newAction) final {
        unimplementedTasserted();
        return Status(ErrorCodes::UnknownError, "unknown");
    }

    Status checkValidatorAPIVersionCompatability(OperationContext* opCtx) const final {
        unimplementedTasserted();
        return Status(ErrorCodes::UnknownError, "unknown");
    }

    bool isChangeStreamPreAndPostImagesEnabled() const final {
        unimplementedTasserted();
        return false;
    }

    void setChangeStreamPreAndPostImages(OperationContext* opCtx,
                                         ChangeStreamPreAndPostImagesOptions val) final {
        unimplementedTasserted();
    }

    bool isTemporary() const final {
        return true;
    }

    bool isTimeseriesCollection() const final {
        return false;
    }

    bool isNewTimeseriesWithoutView() const final {
        return isTimeseriesCollection() && !ns().isTimeseriesBucketsCollection();
    }

    timeseries::MixedSchemaBucketsState getTimeseriesMixedSchemaBucketsState() const final {
        unimplementedTasserted();
        return timeseries::MixedSchemaBucketsState::Invalid;
    }

    void setTimeseriesBucketsMayHaveMixedSchemaData(OperationContext* opCtx,
                                                    boost::optional<bool> setting) final {
        unimplementedTasserted();
    }

    StatusWith<bool> doesTimeseriesBucketsDocContainMixedSchemaData(
        const BSONObj& bucketsDoc) const final {
        unimplementedTasserted();
        return false;
    }

    boost::optional<bool> timeseriesBucketingParametersHaveChanged() const final {
        unimplementedTasserted();
        return false;
    }

    void setTimeseriesBucketingParametersChanged(OperationContext* opCtx,
                                                 boost::optional<bool> value) final {
        unimplementedTasserted();
    }

    void removeLegacyTimeseriesBucketingParametersHaveChanged(OperationContext* opCtx) final {
        unimplementedTasserted();
    }

    bool getRequiresTimeseriesExtendedRangeSupport() const final {
        // A virtual collection is never a time-series collection, so it never requires
        // extended-range support.
        return false;
    }

    void setRequiresTimeseriesExtendedRangeSupport(OperationContext* opCtx) const final {
        unimplementedTasserted();
    }

    bool areTimeseriesBucketsFixed() const final {
        // A virtual collection is never a time-series collection, so it can never have fixed
        // buckets.
        return false;
    }

    bool isClustered() const final {
        return false;
    }

    boost::optional<ClusteredCollectionInfo> getClusteredInfo() const final {
        unimplementedTasserted();
        return boost::none;
    }

    void updateClusteredIndexTTLSetting(OperationContext* opCtx,
                                        boost::optional<int64_t> expireAfterSeconds) final {
        unimplementedTasserted();
    }

    Status updateCappedSize(OperationContext* opCtx,
                            boost::optional<long long> newCappedSize,
                            boost::optional<long long> newCappedMax) final {
        unimplementedTasserted();
        return Status(ErrorCodes::UnknownError, "unknown");
    }

    void unsetRecordIdsReplicated(OperationContext* opCtx) final {
        unimplementedTasserted();
    }

    StatusWith<int> checkMetaDataForIndex(const std::string& indexName,
                                          const BSONObj& spec) const final {
        unimplementedTasserted();
        return Status(ErrorCodes::UnknownError, "unknown");
    }

    void updateTTLSetting(OperationContext* opCtx,
                          StringData idxName,
                          long long newExpireSeconds) final {
        unimplementedTasserted();
    }

    void updateHiddenSetting(OperationContext* opCtx, StringData idxName, bool hidden) final {
        unimplementedTasserted();
    }

    void updateUniqueSetting(OperationContext* opCtx, StringData idxName, bool unique) final {
        unimplementedTasserted();
    }

    void updatePrepareUniqueSetting(OperationContext* opCtx,
                                    StringData idxName,
                                    bool prepareUnique) final {
        unimplementedTasserted();
    }

    std::vector<std::string> repairInvalidIndexOptions(OperationContext* opCtx,
                                                       bool removeDeprecatedFields) final {
        unimplementedTasserted();
        return {};
    }

    void setIsTemp(OperationContext* opCtx, bool isTemp) final {
        unimplementedTasserted();
    }

    void removeIndex(OperationContext* opCtx, StringData indexName) final {
        unimplementedTasserted();
    }

    Status prepareForIndexBuild(OperationContext* opCtx,
                                const IndexDescriptor* spec,
                                StringData indexIdent,
                                boost::optional<UUID> buildUUID) final {
        unimplementedTasserted();
        return Status(ErrorCodes::UnknownError, "unknown");
    }

    boost::optional<UUID> getIndexBuildUUID(StringData indexName) const final {
        unimplementedTasserted();
        return boost::none;
    }

    bool isIndexMultikey(OperationContext* opCtx,
                         StringData indexName,
                         MultikeyPaths* multikeyPaths,
                         int indexOffset) const final {
        unimplementedTasserted();
        return false;
    }

    bool setIndexIsMultikey(OperationContext* opCtx,
                            StringData indexName,
                            const MultikeyPaths& multikeyPaths,
                            int indexOffset) const final {
        unimplementedTasserted();
        return false;
    }

    void forceSetIndexIsMultikey(OperationContext* opCtx,
                                 const IndexDescriptor* desc,
                                 bool isMultikey,
                                 const MultikeyPaths& multikeyPaths) const final {
        unimplementedTasserted();
    }

    int getTotalIndexCount() const final {
        return 0;
    }

    int getCompletedIndexCount() const final {
        return 0;
    }

    BSONObj getIndexSpec(StringData indexName) const final {
        return BSONObj();
    }

    void getAllIndexes(std::vector<std::string>* names) const final {}

    void getReadyIndexes(std::vector<std::string>* names) const final {}

    bool isIndexPresent(StringData indexName) const final {
        return false;
    }

    bool isIndexReady(StringData indexName) const final {
        return false;
    }

    void replaceMetadata(OperationContext* opCtx,
                         std::shared_ptr<durable_catalog::CatalogEntryMetaData> md) final {
        unimplementedTasserted();
    }

    bool isMetadataEqual(const BSONObj& otherMetadata) const final {
        unimplementedTasserted();
        return false;
    }

    bool needsCappedLock() const final {
        unimplementedTasserted();
        return false;
    }

    bool isCappedAndNeedsDelete(OperationContext* opCtx) const final {
        unimplementedTasserted();
        return false;
    }

    /**
     * Virtual collections represent external data sources that are outside the control of the
     * replication system. RecordIds being replicated are a property specific to collections
     * managed by the replication & storage layers.
     */
    bool areRecordIdsReplicated() const final {
        return false;
    }

    bool isCapped() const final {
        return false;
    }

    long long getCappedMaxDocs() const final {
        return 0;
    }

    long long getCappedMaxSize() const final {
        return 0;
    }

    long long numRecords(OperationContext* opCtx) const final {
        return _shared->_recordStore->numRecords();
    }

    long long dataSize(OperationContext* opCtx) const final {
        return _shared->_recordStore->dataSize();
    }

    int64_t sizeOnDisk(OperationContext* opCtx, const StorageEngine& storageEngine) const final {
        return 0;
    }

    bool isEmpty(OperationContext* opCtx) const final {
        return _shared->_recordStore->dataSize() == 0LL;
    }

    inline int averageObjectSize(OperationContext* opCtx) const override {
        return 0;
    }

    uint64_t getIndexSize(OperationContext* opCtx,
                          BSONObjBuilder* details = nullptr,
                          int scale = 1) const final {
        return 0;
    }

    uint64_t getIndexFreeStorageBytes(OperationContext* opCtx) const final {
        return 0;
    }

    boost::optional<Timestamp> getMinimumValidSnapshot() const final {
        return boost::none;
    }

    void setMinimumValidSnapshot(Timestamp newMinimumValidSnapshot) final {}

    const boost::optional<TimeseriesOptions>& getTimeseriesOptions() const final {
        return _options.timeseries;
    }

    void setTimeseriesOptions(OperationContext* opCtx, const TimeseriesOptions& tsOptions) final {
        unimplementedTasserted();
    }

    /**
     * Get a pointer to the collection's default collator. The pointer must not be used after this
     * Collection is destroyed.
     */
    const CollatorInterface* getDefaultCollator() const final {
        return _shared->_collator.get();
    }

    const CollectionOptions& getCollectionOptions() const final {
        return _options;
    }

    StatusWith<BSONObj> addCollationDefaultsToIndexSpecsForCreate(
        OperationContext* opCtx, const BSONObj& indexSpecs) const final {
        unimplementedTasserted();
        return Status(ErrorCodes::UnknownError, "unknown");
    }

    StatusWith<std::vector<BSONObj>> addCollationDefaultsToIndexSpecsForCreate(
        OperationContext* opCtx, const std::vector<BSONObj>& indexSpecs) const final {
        unimplementedTasserted();
        return Status(ErrorCodes::UnknownError, "unknown");
    }

    void indexBuildSuccess(OperationContext* opCtx, IndexCatalogEntry* index) final {
        unimplementedTasserted();
    }

    void onDeregisterFromCatalog(ServiceContext* svcCtx) final {}

private:
    void unimplementedTasserted() const {
        MONGO_UNIMPLEMENTED_TASSERT(6968506);
    }

    struct SharedState {
        SharedState(std::unique_ptr<ExternalRecordStore> recordStore,
                    std::unique_ptr<CollatorInterface> collator)
            : _recordStore(std::move(recordStore)), _collator(std::move(collator)) {}

        ~SharedState() = default;

        std::unique_ptr<ExternalRecordStore> _recordStore;

        // This object is decorable and decorated with unversioned data related to the collection.
        // Not associated with any particular Collection instance for the collection, but shared
        // across all instances for the same collection. This is a vehicle for users of a collection
        // to cache unversioned state for a collection that is accessible across all of the
        // Collection instances.
        SharedCollectionDecorations _sharedDecorations;

        // The default collation which is applied to operations and indices which have no collation
        // of their own. The collection's validator will respect this collation. If null, the
        // default collation is simple binary compare.
        std::unique_ptr<CollatorInterface> _collator;
    };

    NamespaceString _nss;
    CollectionOptions _options;

    std::shared_ptr<SharedState> _shared;
    clonable_ptr<IndexCatalog> _indexCatalog;
};
}  // namespace mongo
