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

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>
// IWYU pragma: no_include "cxxabi.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/bson/util/builder.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/catalog/capped_visibility.h"
#include "mongo/db/catalog/clustered_collection_options_gen.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/collection_options_gen.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/change_stream_pre_and_post_images_options_gen.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/optime_with.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/s/collection_metadata.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/migration_chunk_cloner_source.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/shard_server_test_fixture.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/session/logical_session_id_helpers.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/shard_id.h"
#include "mongo/db/storage/bson_collection_catalog_entry.h"
#include "mongo/db/storage/durable_catalog_entry.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/network_test_env.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/sharding_catalog_client_mock.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/database_version.h"
#include "mongo/s/request_types/move_range_request_gen.h"
#include "mongo/s/resharding/type_collection_fields_gen.h"
#include "mongo/s/type_collection_common_types_gen.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/uuid.h"
#include "mongo/util/version/releases.h"

namespace mongo {
namespace {

using executor::RemoteCommandRequest;
using unittest::assertGet;

const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("TestDB", "TestColl");
const std::string kShardKey = "X";
const BSONObj kShardKeyPattern{BSON(kShardKey << 1)};
const ConnectionString kDonorConnStr =
    ConnectionString::forReplicaSet("Donor",
                                    {HostAndPort("DonorHost1:1234"),
                                     HostAndPort{"DonorHost2:1234"},
                                     HostAndPort{"DonorHost3:1234"}});
const ConnectionString kRecipientConnStr =
    ConnectionString::forReplicaSet("Recipient",
                                    {HostAndPort("RecipientHost1:1234"),
                                     HostAndPort("RecipientHost2:1234"),
                                     HostAndPort("RecipientHost3:1234")});

class CollectionWithFault : public Collection {
public:
    CollectionWithFault(const Collection* originalCollection) : _coll(originalCollection) {}

    void setFindDocStatus(Status newStatus) {
        _findDocStatus = newStatus;
    }

    //////////////////////////////////////////////////////////////////////////////////
    // Collection overrides

    std::shared_ptr<Collection> clone() const override {
        return _coll->clone();
    }

    SharedCollectionDecorations* getSharedDecorations() const override {
        return _coll->getSharedDecorations();
    }

    Status initFromExisting(OperationContext* opCtx,
                            const std::shared_ptr<const Collection>& collection,
                            const DurableCatalogEntry& catalogEntry,
                            boost::optional<Timestamp> readTimestamp) override {
        MONGO_UNREACHABLE;
    }

    const NamespaceString& ns() const override {
        return _coll->ns();
    }

    Status rename(OperationContext* opCtx, const NamespaceString& nss, bool stayTemp) override {
        MONGO_UNREACHABLE;
    }

    RecordId getCatalogId() const override {
        return _coll->getCatalogId();
    }

    UUID uuid() const override {
        return _coll->uuid();
    }

    const IndexCatalog* getIndexCatalog() const override {
        return _coll->getIndexCatalog();
    }

    IndexCatalog* getIndexCatalog() override {
        MONGO_UNREACHABLE;
    }

    RecordStore* getRecordStore() const {
        return _coll->getRecordStore();
    }

    std::shared_ptr<Ident> getSharedIdent() const override {
        return _coll->getSharedIdent();
    }

    void setIdent(std::shared_ptr<Ident> newIdent) override {
        MONGO_UNREACHABLE;
    }

    BSONObj getValidatorDoc() const override {
        return _coll->getValidatorDoc();
    }

    std::pair<SchemaValidationResult, Status> checkValidation(
        OperationContext* opCtx, const BSONObj& document) const override {
        return _coll->checkValidation(opCtx, document);
    }

    Status checkValidationAndParseResult(OperationContext* opCtx,
                                         const BSONObj& document) const override {
        return _coll->checkValidationAndParseResult(opCtx, document);
    }

    bool requiresIdIndex() const override {
        return _coll->requiresIdIndex();
    }

    Snapshotted<BSONObj> docFor(OperationContext* opCtx, const RecordId& loc) const override {
        return _coll->docFor(opCtx, loc);
    }

    bool findDoc(OperationContext* opCtx,
                 const RecordId& loc,
                 Snapshotted<BSONObj>* out) const override {
        uassertStatusOK(_findDocStatus);
        return _coll->findDoc(opCtx, loc, out);
    }

    std::unique_ptr<SeekableRecordCursor> getCursor(OperationContext* opCtx,
                                                    bool forward = true) const override {
        return _coll->getCursor(opCtx, forward);
    }

    bool updateWithDamagesSupported() const override {
        return _coll->updateWithDamagesSupported();
    }

    Status truncate(OperationContext* opCtx) override {
        MONGO_UNREACHABLE;
    }

    Validator parseValidator(OperationContext* opCtx,
                             const BSONObj& validator,
                             MatchExpressionParser::AllowedFeatureSet allowedFeatures,
                             boost::optional<multiversion::FeatureCompatibilityVersion>
                                 maxFeatureCompatibilityVersion) const override {
        return _coll->parseValidator(
            opCtx, validator, allowedFeatures, maxFeatureCompatibilityVersion);
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
        return _coll->getValidationLevel();
    }

    boost::optional<ValidationActionEnum> getValidationAction() const override {
        return _coll->getValidationAction();
    }

    Status updateValidator(OperationContext* opCtx,
                           BSONObj newValidator,
                           boost::optional<ValidationLevelEnum> newLevel,
                           boost::optional<ValidationActionEnum> newAction) override {
        MONGO_UNREACHABLE;
    }

    Status checkValidatorAPIVersionCompatability(OperationContext* opCtx) const override {
        return _coll->checkValidatorAPIVersionCompatability(opCtx);
    }

    bool isChangeStreamPreAndPostImagesEnabled() const override {
        return _coll->isChangeStreamPreAndPostImagesEnabled();
    }

    void setChangeStreamPreAndPostImages(OperationContext* opCtx,
                                         ChangeStreamPreAndPostImagesOptions val) override {
        MONGO_UNREACHABLE;
    }

    bool isTemporary() const override {
        return _coll->isTemporary();
    }

    boost::optional<bool> getTimeseriesBucketsMayHaveMixedSchemaData() const override {
        return _coll->getTimeseriesBucketsMayHaveMixedSchemaData();
    }

    void setTimeseriesBucketsMayHaveMixedSchemaData(OperationContext* opCtx,
                                                    boost::optional<bool> setting) override {
        MONGO_UNREACHABLE;
    }

    boost::optional<bool> timeseriesBucketingParametersHaveChanged() const override {
        return _coll->timeseriesBucketingParametersHaveChanged();
    }

    void setTimeseriesBucketingParametersChanged(OperationContext* opCtx,
                                                 boost::optional<bool> value) override {
        MONGO_UNREACHABLE;
    }

    bool areTimeseriesBucketsFixed() const override {
        auto tsOptions = getTimeseriesOptions();
        boost::optional<bool> parametersChanged = timeseriesBucketingParametersHaveChanged();
        return parametersChanged.has_value() && !parametersChanged.get() && tsOptions &&
            tsOptions->getBucketMaxSpanSeconds() == tsOptions->getBucketRoundingSeconds();
    }

    StatusWith<bool> doesTimeseriesBucketsDocContainMixedSchemaData(
        const BSONObj& bucketsDoc) const override {
        return _coll->doesTimeseriesBucketsDocContainMixedSchemaData(bucketsDoc);
    }

    bool getRequiresTimeseriesExtendedRangeSupport() const override {
        return _coll->getRequiresTimeseriesExtendedRangeSupport();
    }

    void setRequiresTimeseriesExtendedRangeSupport(OperationContext* opCtx) const override {
        return _coll->setRequiresTimeseriesExtendedRangeSupport(opCtx);
    }

    bool isClustered() const override {
        return _coll->isClustered();
    }

    boost::optional<ClusteredCollectionInfo> getClusteredInfo() const override {
        return _coll->getClusteredInfo();
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

    void unsetRecordIdsReplicated(OperationContext* opCtx) override {
        MONGO_UNREACHABLE;
    }

    StatusWith<int> checkMetaDataForIndex(const std::string& indexName,
                                          const BSONObj& spec) const override {
        return _coll->checkMetaDataForIndex(indexName, spec);
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

    std::vector<std::string> repairInvalidIndexOptions(OperationContext* opCtx) override {
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
                                boost::optional<UUID> buildUUID,
                                bool isBackgroundSecondaryBuild) override {
        MONGO_UNREACHABLE;
    }

    boost::optional<UUID> getIndexBuildUUID(StringData indexName) const override {
        return _coll->getIndexBuildUUID(indexName);
    }

    bool isIndexMultikey(OperationContext* opCtx,
                         StringData indexName,
                         MultikeyPaths* multikeyPaths,
                         int indexOffset = -1) const override {
        return _coll->isIndexMultikey(opCtx, indexName, multikeyPaths, indexOffset);
    }

    bool setIndexIsMultikey(OperationContext* opCtx,
                            StringData indexName,
                            const MultikeyPaths& multikeyPaths,
                            int indexOffset = -1) const override {
        return _coll->setIndexIsMultikey(opCtx, indexName, multikeyPaths, indexOffset);
    }

    void forceSetIndexIsMultikey(OperationContext* opCtx,
                                 const IndexDescriptor* desc,
                                 bool isMultikey,
                                 const MultikeyPaths& multikeyPaths) const override {
        return _coll->forceSetIndexIsMultikey(opCtx, desc, isMultikey, multikeyPaths);
    }

    int getTotalIndexCount() const override {
        return _coll->getTotalIndexCount();
    }

    int getCompletedIndexCount() const override {
        return _coll->getCompletedIndexCount();
    }

    BSONObj getIndexSpec(StringData indexName) const override {
        return _coll->getIndexSpec(indexName);
    }

    void getAllIndexes(std::vector<std::string>* names) const override {
        return _coll->getAllIndexes(names);
    }

    void getReadyIndexes(std::vector<std::string>* names) const override {
        return _coll->getReadyIndexes(names);
    }

    bool isIndexPresent(StringData indexName) const override {
        return _coll->isIndexPresent(indexName);
    }

    bool isIndexReady(StringData indexName) const override {
        return _coll->isIndexReady(indexName);
    }

    void replaceMetadata(OperationContext* opCtx,
                         std::shared_ptr<BSONCollectionCatalogEntry::MetaData> md) override {
        MONGO_UNREACHABLE;
    }

    bool isMetadataEqual(const BSONObj& otherMetadata) const override {
        return _coll->isMetadataEqual(otherMetadata);
    }

    void sanitizeCollectionOptions(OperationContext* opCtx) override {
        MONGO_UNREACHABLE;
    }

    bool needsCappedLock() const override {
        return _coll->needsCappedLock();
    }

    bool isCappedAndNeedsDelete(OperationContext* opCtx) const override {
        return _coll->isCappedAndNeedsDelete(opCtx);
    }

    bool usesCappedSnapshots() const override {
        return _coll->usesCappedSnapshots();
    }

    std::vector<RecordId> reserveCappedRecordIds(OperationContext* opCtx,
                                                 size_t nIds) const override {
        return _coll->reserveCappedRecordIds(opCtx, nIds);
    }

    void registerCappedInserts(OperationContext* opCtx,
                               const RecordId& minRecord,
                               const RecordId& maxRecord) const override {
        return _coll->registerCappedInserts(opCtx, minRecord, maxRecord);
    }

    CappedVisibilityObserver* getCappedVisibilityObserver() const override {
        return _coll->getCappedVisibilityObserver();
    }

    CappedVisibilitySnapshot takeCappedVisibilitySnapshot() const override {
        return _coll->takeCappedVisibilitySnapshot();
    }

    bool areRecordIdsReplicated() const override {
        return _coll->areRecordIdsReplicated();
    }

    bool isCapped() const override {
        return _coll->isCapped();
    }

    long long getCappedMaxDocs() const override {
        return _coll->getCappedMaxDocs();
    }

    long long getCappedMaxSize() const override {
        return _coll->getCappedMaxSize();
    }

    long long numRecords(OperationContext* opCtx) const override {
        return _coll->numRecords(opCtx);
    }

    long long dataSize(OperationContext* opCtx) const override {
        return _coll->dataSize(opCtx);
    }

    bool isEmpty(OperationContext* opCtx) const override {
        return _coll->isEmpty(opCtx);
    }

    int averageObjectSize(OperationContext* opCtx) const override {
        return _coll->averageObjectSize(opCtx);
    }

    uint64_t getIndexSize(OperationContext* opCtx,
                          BSONObjBuilder* details = nullptr,
                          int scale = 1) const {
        return _coll->getIndexSize(opCtx, details, scale);
    }

    uint64_t getIndexFreeStorageBytes(OperationContext* opCtx) const override {
        return _coll->getIndexFreeStorageBytes(opCtx);
    }

    boost::optional<Timestamp> getMinimumValidSnapshot() const override {
        return _coll->getMinimumValidSnapshot();
    }

    void setMinimumValidSnapshot(Timestamp name) override {
        MONGO_UNREACHABLE;
    }

    boost::optional<TimeseriesOptions> getTimeseriesOptions() const override {
        return _coll->getTimeseriesOptions();
    }

    void setTimeseriesOptions(OperationContext* opCtx,
                              const TimeseriesOptions& tsOptions) override {
        MONGO_UNREACHABLE;
    }

    const CollatorInterface* getDefaultCollator() const override {
        return _coll->getDefaultCollator();
    }

    const CollectionOptions& getCollectionOptions() const override {
        return _coll->getCollectionOptions();
    }

    StatusWith<std::vector<BSONObj>> addCollationDefaultsToIndexSpecsForCreate(
        OperationContext* opCtx, const std::vector<BSONObj>& indexSpecs) const {
        return _coll->addCollationDefaultsToIndexSpecsForCreate(opCtx, indexSpecs);
    }

    StatusWith<BSONObj> addCollationDefaultsToIndexSpecsForCreate(OperationContext* opCtx,
                                                                  const BSONObj& indexSpecs) const {
        return _coll->addCollationDefaultsToIndexSpecsForCreate(opCtx, indexSpecs);
    }

    void indexBuildSuccess(OperationContext* opCtx, IndexCatalogEntry* index) override {
        MONGO_UNREACHABLE;
    }

    void onDeregisterFromCatalog(OperationContext* opCtx) override {
        MONGO_UNREACHABLE;
    }

private:
    const Collection* _coll;

    Status _findDocStatus{Status::OK()};
};

class MigrationChunkClonerSourceTest : public ShardServerTestFixture {
protected:
    MigrationChunkClonerSourceTest() : ShardServerTestFixture(Options{}.useMockClock(true)) {}

    void setUp() override {
        ShardServerTestFixture::setUp();

        auto opCtx = operationContext();
        DBDirectClient client(opCtx);
        client.createCollection(NamespaceString::kSessionTransactionsTableNamespace);
        client.createIndexes(NamespaceString::kSessionTransactionsTableNamespace,
                             {MongoDSessionCatalog::getConfigTxnPartialIndexSpec()});

        // TODO: SERVER-26919 set the flag on the mock repl coordinator just for the window where it
        // actually needs to bypass the op observer.
        replicationCoordinator()->alwaysAllowWrites(true);

        _client.emplace(operationContext());

        {
            auto donorShard = assertGet(
                shardRegistry()->getShard(operationContext(), kDonorConnStr.getSetName()));
            RemoteCommandTargeterMock::get(donorShard->getTargeter())
                ->setConnectionStringReturnValue(kDonorConnStr);
            RemoteCommandTargeterMock::get(donorShard->getTargeter())
                ->setFindHostReturnValue(kDonorConnStr.getServers()[0]);
        }

        {
            auto recipientShard = assertGet(
                shardRegistry()->getShard(operationContext(), kRecipientConnStr.getSetName()));
            RemoteCommandTargeterMock::get(recipientShard->getTargeter())
                ->setConnectionStringReturnValue(kRecipientConnStr);
            RemoteCommandTargeterMock::get(recipientShard->getTargeter())
                ->setFindHostReturnValue(kRecipientConnStr.getServers()[0]);
        }

        _lsid = makeLogicalSessionId(operationContext());
    }

    void tearDown() override {
        _client.reset();

        ShardServerTestFixture::tearDown();
    }

    /**
     * Returns the DBDirectClient instance to use for writes to the database.
     */
    DBDirectClient* client() {
        invariant(_client);
        return _client.get_ptr();
    }

    /**
     * Inserts the specified docs in 'kNss' and ensures the insert succeeded.
     */
    void insertDocsInShardedCollection(const std::vector<BSONObj>& docs) {
        if (docs.empty())
            return;

        std::deque<BSONObj> docsToInsert;
        std::copy(docs.cbegin(), docs.cend(), std::back_inserter(docsToInsert));

        while (!docsToInsert.empty()) {
            std::vector<BSONObj> batchToInsert;

            size_t sizeInBatch = 0;
            while (!docsToInsert.empty()) {
                auto next = docsToInsert.front();
                sizeInBatch += next.objsize();

                if (sizeInBatch > BSONObjMaxUserSize) {
                    break;
                }

                batchToInsert.push_back(next);
                docsToInsert.pop_front();
            }

            auto response = client()->insertAcknowledged(kNss, batchToInsert);
            ASSERT_OK(getStatusFromWriteCommandReply(response));
            ASSERT_GT(response["n"].Int(), 0);
        }
    }

    void deleteDocsInShardedCollection(BSONObj query) {
        auto response = client()->removeAcknowledged(kNss, query);
        ASSERT_OK(getStatusFromWriteCommandReply(response));
        ASSERT_GT(response["n"].Int(), 0);
    }

    void updateDocsInShardedCollection(BSONObj filter, BSONObj updated) {
        auto response = client()->updateAcknowledged(kNss, filter, updated);
        ASSERT_OK(getStatusFromWriteCommandReply(response));
        ASSERT_GT(response["n"].Int(), 0);
    }

    /**
     * Creates a collection, which contains an index corresponding to kShardKeyPattern and inserts
     * the specified initial documents.
     */
    void createShardedCollection(const std::vector<BSONObj>& initialDocs) {
        {
            OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE
                unsafeCreateCollection(operationContext());
            uassertStatusOK(
                createCollection(operationContext(), kNss.dbName(), BSON("create" << kNss.coll())));
        }

        const auto uuid = [&] {
            AutoGetCollection autoColl(operationContext(), kNss, MODE_IX);
            return autoColl.getCollection()->uuid();
        }();

        [&] {
            const OID epoch = OID::gen();
            const Timestamp timestamp(1);

            auto rt = RoutingTableHistory::makeNew(
                kNss,
                uuid,
                kShardKeyPattern,
                false, /* unsplittable */
                nullptr,
                false,
                epoch,
                timestamp,
                boost::none /* timeseriesFields */,
                boost::none /* resharding Fields */,
                true,
                {ChunkType{uuid,
                           ChunkRange{BSON(kShardKey << MINKEY), BSON(kShardKey << MAXKEY)},
                           ChunkVersion({epoch, timestamp}, {1, 0}),
                           ShardId("dummyShardId")}});

            AutoGetDb autoDb(operationContext(), kNss.dbName(), MODE_IX);
            Lock::CollectionLock collLock(operationContext(), kNss, MODE_IX);
            CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(operationContext(),
                                                                                 kNss)
                ->setFilteringMetadata(
                    operationContext(),
                    CollectionMetadata(
                        ChunkManager(ShardId("dummyShardId"),
                                     DatabaseVersion(UUID::gen(), Timestamp(1, 1)),
                                     makeStandaloneRoutingTableHistory(std::move(rt)),
                                     boost::none),
                        ShardId("dummyShardId")));
        }();

        client()->createIndex(kNss, kShardKeyPattern);
        insertDocsInShardedCollection(initialDocs);
    }

    /**
     * Shortcut to create BSON represenation of a moveChunk request for the specified range with
     * fixed kDonorConnStr and kRecipientConnStr, respectively.
     */
    static ShardsvrMoveRange createMoveRangeRequest(const ChunkRange& chunkRange) {
        ShardsvrMoveRange req(kNss);
        req.setEpoch(OID::gen());
        req.setFromShard(ShardId(kDonorConnStr.getSetName()));
        req.setMaxChunkSizeBytes(1024);
        req.getMoveRangeRequestBase().setToShard(ShardId(kRecipientConnStr.getSetName()));
        req.getMoveRangeRequestBase().setMin(chunkRange.getMin());
        req.getMoveRangeRequestBase().setMax(chunkRange.getMax());
        return req;
    }

    /**
     * Instantiates a BSON object in which both "_id" and "X" are set to value.
     */
    static BSONObj createCollectionDocument(int value) {
        return BSON("_id" << value << "X" << value);
    }

    /**
     * Instantiates a BSON object with different "_id" and "X" values.
     */
    static BSONObj createCollectionDocumentForUpdate(int id, int value) {
        return BSON("_id" << id << "X" << value);
    }

    /**
     * Instantiates a BSON object with objsize close to size.
     */
    static BSONObj createSizedCollectionDocument(int id, long long size) {
        std::string value(size, 'x');
        return BSON("_id" << id << "X" << id << "Y" << value);
    }

protected:
    LogicalSessionId _lsid;
    TxnNumber _txnNumber{0};

private:
    std::unique_ptr<ShardingCatalogClient> makeShardingCatalogClient() override {
        class StaticCatalogClient final : public ShardingCatalogClientMock {
        public:
            StaticCatalogClient() = default;

            StatusWith<repl::OpTimeWith<std::vector<ShardType>>> getAllShards(
                OperationContext* opCtx,
                repl::ReadConcernLevel readConcern,
                bool excludeDraining) override {

                ShardType donorShard;
                donorShard.setName(kDonorConnStr.getSetName());
                donorShard.setHost(kDonorConnStr.toString());

                ShardType recipientShard;
                recipientShard.setName(kRecipientConnStr.getSetName());
                recipientShard.setHost(kRecipientConnStr.toString());

                return repl::OpTimeWith<std::vector<ShardType>>({donorShard, recipientShard});
            }
        };

        return std::make_unique<StaticCatalogClient>();
    }

    boost::optional<DBDirectClient> _client;
};

TEST_F(MigrationChunkClonerSourceTest, CorrectDocumentsFetched) {
    const std::vector<BSONObj> contents = {createCollectionDocument(99),
                                           createCollectionDocument(100),
                                           createCollectionDocument(199),
                                           createCollectionDocument(200)};
    const ShardKeyPattern shardKeyPattern(kShardKeyPattern);

    createShardedCollection(contents);

    const ShardsvrMoveRange req =
        createMoveRangeRequest(ChunkRange(BSON("X" << 100), BSON("X" << 200)));
    MigrationChunkClonerSource cloner(operationContext(),
                                      req,
                                      WriteConcernOptions(),
                                      kShardKeyPattern,
                                      kDonorConnStr,
                                      kRecipientConnStr.getServers()[0]);

    {
        auto futureStartClone = launchAsync([&]() {
            onCommand([&](const RemoteCommandRequest& request) { return BSON("ok" << true); });
        });

        ASSERT_OK(cloner.startClone(operationContext(), UUID::gen(), _lsid, _txnNumber));
        futureStartClone.default_timed_get();
    }

    // Ensure the initial clone documents are available
    {
        AutoGetCollection autoColl(operationContext(), kNss, MODE_IS);

        {
            BSONArrayBuilder arrBuilder;
            ASSERT_OK(
                cloner.nextCloneBatch(operationContext(), autoColl.getCollection(), &arrBuilder));
            ASSERT_EQ(2, arrBuilder.arrSize());

            const auto arr = arrBuilder.arr();
            ASSERT_BSONOBJ_EQ(contents[1], arr[0].Obj());
            ASSERT_BSONOBJ_EQ(contents[2], arr[1].Obj());
        }

        {
            BSONArrayBuilder arrBuilder;
            ASSERT_OK(
                cloner.nextCloneBatch(operationContext(), autoColl.getCollection(), &arrBuilder));
            ASSERT_EQ(0, arrBuilder.arrSize());
        }
    }

    // Insert some documents in the chunk range to be included for migration
    insertDocsInShardedCollection({createCollectionDocument(150)});
    insertDocsInShardedCollection({createCollectionDocument(151)});

    // Insert some documents which are outside of the chunk range and should not be included for
    // migration
    insertDocsInShardedCollection({createCollectionDocument(90)});
    insertDocsInShardedCollection({createCollectionDocument(210)});

    // Normally the insert above and the onInsert/onDelete callbacks below will happen under the
    // same lock and write unit of work
    {
        AutoGetCollection autoColl(operationContext(), kNss, MODE_IX);

        WriteUnitOfWork wuow(operationContext());

        cloner.onInsertOp(operationContext(), createCollectionDocument(90), {});
        cloner.onInsertOp(operationContext(), createCollectionDocument(150), {});
        cloner.onInsertOp(operationContext(), createCollectionDocument(151), {});
        cloner.onInsertOp(operationContext(), createCollectionDocument(210), {});

        cloner.onDeleteOp(
            operationContext(), getDocumentKey(shardKeyPattern, createCollectionDocument(80)), {});
        cloner.onDeleteOp(
            operationContext(), getDocumentKey(shardKeyPattern, createCollectionDocument(199)), {});
        cloner.onDeleteOp(
            operationContext(), getDocumentKey(shardKeyPattern, createCollectionDocument(220)), {});

        wuow.commit();
    }

    {
        AutoGetCollection autoColl(operationContext(), kNss, MODE_IS);

        {
            BSONArrayBuilder arrBuilder;
            ASSERT_OK(
                cloner.nextCloneBatch(operationContext(), autoColl.getCollection(), &arrBuilder));
            ASSERT_EQ(0, arrBuilder.arrSize());
        }

        {
            BSONObjBuilder modsBuilder;
            ASSERT_OK(cloner.nextModsBatch(operationContext(), &modsBuilder));

            const auto modsObj = modsBuilder.obj();
            ASSERT_EQ(2U, modsObj["reload"].Array().size());
            ASSERT_BSONOBJ_EQ(createCollectionDocument(150), modsObj["reload"].Array()[0].Obj());
            ASSERT_BSONOBJ_EQ(createCollectionDocument(151), modsObj["reload"].Array()[1].Obj());

            ASSERT_EQ(1U, modsObj["deleted"].Array().size());
            ASSERT_BSONOBJ_EQ(BSON("_id" << 199), modsObj["deleted"].Array()[0].Obj());
        }
    }

    auto futureCommit = launchAsync([&]() {
        onCommand([&](const RemoteCommandRequest& request) { return BSON("ok" << true); });
    });

    ASSERT_OK(cloner.commitClone(operationContext()));
    futureCommit.default_timed_get();
}

TEST_F(MigrationChunkClonerSourceTest, RemoveDuplicateDocuments) {
    const std::vector<BSONObj> contents = {createCollectionDocument(100),
                                           createCollectionDocument(199)};
    const ShardKeyPattern shardKeyPattern(kShardKeyPattern);

    createShardedCollection(contents);

    const ShardsvrMoveRange req =
        createMoveRangeRequest(ChunkRange(BSON("X" << 100), BSON("X" << 200)));
    MigrationChunkClonerSource cloner(operationContext(),
                                      req,
                                      WriteConcernOptions(),
                                      kShardKeyPattern,
                                      kDonorConnStr,
                                      kRecipientConnStr.getServers()[0]);

    {
        auto futureStartClone = launchAsync([&]() {
            onCommand([&](const RemoteCommandRequest& request) { return BSON("ok" << true); });
        });

        ASSERT_OK(cloner.startClone(operationContext(), UUID::gen(), _lsid, _txnNumber));
        futureStartClone.default_timed_get();
    }

    // Ensure the initial clone documents are available
    {
        AutoGetCollection autoColl(operationContext(), kNss, MODE_IS);

        {
            BSONArrayBuilder arrBuilder;
            ASSERT_OK(
                cloner.nextCloneBatch(operationContext(), autoColl.getCollection(), &arrBuilder));
            ASSERT_EQ(2, arrBuilder.arrSize());

            const auto arr = arrBuilder.arr();
            ASSERT_BSONOBJ_EQ(contents[0], arr[0].Obj());
            ASSERT_BSONOBJ_EQ(contents[1], arr[1].Obj());
        }
    }

    {
        AutoGetCollection autoColl(operationContext(), kNss, MODE_IX);

        deleteDocsInShardedCollection(createCollectionDocument(100));
        insertDocsInShardedCollection({createCollectionDocument(100)});
        deleteDocsInShardedCollection(createCollectionDocument(100));

        updateDocsInShardedCollection(createCollectionDocument(199),
                                      createCollectionDocumentForUpdate(199, 198));
        updateDocsInShardedCollection(createCollectionDocumentForUpdate(199, 198),
                                      createCollectionDocumentForUpdate(199, 197));

        WriteUnitOfWork wuow(operationContext());

        cloner.onDeleteOp(
            operationContext(), getDocumentKey(shardKeyPattern, createCollectionDocument(100)), {});
        cloner.onInsertOp(operationContext(), createCollectionDocument(100), {});
        cloner.onDeleteOp(
            operationContext(), getDocumentKey(shardKeyPattern, createCollectionDocument(100)), {});

        cloner.onUpdateOp(operationContext(),
                          createCollectionDocument(199),
                          createCollectionDocumentForUpdate(199, 198),
                          {});
        cloner.onUpdateOp(operationContext(),
                          createCollectionDocument(199),
                          createCollectionDocumentForUpdate(199, 197),
                          {});

        wuow.commit();
    }

    {
        AutoGetCollection autoColl(operationContext(), kNss, MODE_IS);
        {
            BSONObjBuilder modsBuilder;
            ASSERT_OK(cloner.nextModsBatch(operationContext(), &modsBuilder));

            const auto modsObj = modsBuilder.obj();
            ASSERT_EQ(1U, modsObj["reload"].Array().size());
            ASSERT_BSONOBJ_EQ(createCollectionDocumentForUpdate(199, 197),
                              modsObj["reload"].Array()[0].Obj());
            ASSERT_EQ(1U, modsObj["deleted"].Array().size());
            ASSERT_BSONOBJ_EQ(BSON("_id" << 100), modsObj["deleted"].Array()[0].Obj());
        }
    }

    auto futureCommit = launchAsync([&]() {
        onCommand([&](const RemoteCommandRequest& request) { return BSON("ok" << true); });
    });

    ASSERT_OK(cloner.commitClone(operationContext()));
    futureCommit.default_timed_get();
}


TEST_F(MigrationChunkClonerSourceTest, OneLargeDocumentTransferMods) {
    const std::vector<BSONObj> contents = {createCollectionDocument(1)};

    createShardedCollection(contents);

    const ShardsvrMoveRange req =
        createMoveRangeRequest(ChunkRange(BSON("X" << 1), BSON("X" << 100)));
    MigrationChunkClonerSource cloner(operationContext(),
                                      req,
                                      WriteConcernOptions(),
                                      kShardKeyPattern,
                                      kDonorConnStr,
                                      kRecipientConnStr.getServers()[0]);

    {
        auto futureStartClone = launchAsync([&]() {
            onCommand([&](const RemoteCommandRequest& request) { return BSON("ok" << true); });
        });

        ASSERT_OK(cloner.startClone(operationContext(), UUID::gen(), _lsid, _txnNumber));
        futureStartClone.default_timed_get();
    }

    {
        AutoGetCollection autoColl(operationContext(), kNss, MODE_IS);

        {
            BSONArrayBuilder arrBuilder;
            ASSERT_OK(
                cloner.nextCloneBatch(operationContext(), autoColl.getCollection(), &arrBuilder));
            ASSERT_EQ(1, arrBuilder.arrSize());
        }
    }

    {
        AutoGetCollection autoColl(operationContext(), kNss, MODE_IX);
        BSONObj insertDoc =
            createSizedCollectionDocument(2, BSONObjMaxUserSize - kFixedCommandOverhead + 2 * 1024);
        insertDocsInShardedCollection({insertDoc});
        WriteUnitOfWork wuow(operationContext());
        cloner.onInsertOp(operationContext(), insertDoc, {});
        wuow.commit();
    }

    {
        AutoGetCollection autoColl(operationContext(), kNss, MODE_IS);
        {
            BSONObjBuilder modsBuilder;
            ASSERT_OK(cloner.nextModsBatch(operationContext(), &modsBuilder));

            const auto modsObj = modsBuilder.obj();
            ASSERT_EQ(1, modsObj["reload"].Array().size());
        }
    }

    auto futureCommit = launchAsync([&]() {
        onCommand([&](const RemoteCommandRequest& request) { return BSON("ok" << true); });
    });

    ASSERT_OK(cloner.commitClone(operationContext()));
    futureCommit.default_timed_get();
}

TEST_F(MigrationChunkClonerSourceTest, ManySmallDocumentsTransferMods) {
    const std::vector<BSONObj> contents = {createCollectionDocument(1)};

    createShardedCollection(contents);

    const ShardsvrMoveRange req =
        createMoveRangeRequest(ChunkRange(BSON("X" << 1), BSON("X" << 1000000)));
    MigrationChunkClonerSource cloner(operationContext(),
                                      req,
                                      WriteConcernOptions(),
                                      kShardKeyPattern,
                                      kDonorConnStr,
                                      kRecipientConnStr.getServers()[0]);

    {
        auto futureStartClone = launchAsync([&]() {
            onCommand([&](const RemoteCommandRequest& request) { return BSON("ok" << true); });
        });

        ASSERT_OK(cloner.startClone(operationContext(), UUID::gen(), _lsid, _txnNumber));
        futureStartClone.default_timed_get();
    }

    {
        AutoGetCollection autoColl(operationContext(), kNss, MODE_IS);

        {
            BSONArrayBuilder arrBuilder;
            ASSERT_OK(
                cloner.nextCloneBatch(operationContext(), autoColl.getCollection(), &arrBuilder));
            ASSERT_EQ(1, arrBuilder.arrSize());
        }
    }

    long long numDocuments = 0;
    {
        AutoGetCollection autoColl(operationContext(), kNss, MODE_IX);

        std::vector<BSONObj> insertDocs;
        long long totalSize = 0;
        long long id = 2;
        while (true) {
            BSONObj add = createSizedCollectionDocument(id++, 4 * 1024);
            // The overhead for a BSONObjBuilder with 4KB documents is ~ 22 * 1024, so this is the
            // max documents to fit in one batch
            if (totalSize + add.objsize() > BSONObjMaxUserSize - kFixedCommandOverhead - 22 * 1024)
                break;
            insertDocs.push_back(add);
            totalSize += add.objsize();
            numDocuments++;
            insertDocsInShardedCollection({add});
        }

        WriteUnitOfWork wuow(operationContext());
        for (const BSONObj& add : insertDocs) {
            cloner.onInsertOp(operationContext(), add, {});
        }
        wuow.commit();
    }

    {
        AutoGetCollection autoColl(operationContext(), kNss, MODE_IS);
        {
            BSONObjBuilder modsBuilder;
            ASSERT_OK(cloner.nextModsBatch(operationContext(), &modsBuilder));
            const auto modsObj = modsBuilder.obj();
            ASSERT_EQ(modsObj["reload"].Array().size(), numDocuments);
        }
    }

    auto futureCommit = launchAsync([&]() {
        onCommand([&](const RemoteCommandRequest& request) { return BSON("ok" << true); });
    });

    ASSERT_OK(cloner.commitClone(operationContext()));
    futureCommit.default_timed_get();
}

TEST_F(MigrationChunkClonerSourceTest, CollectionNotFound) {
    const ShardsvrMoveRange req =
        createMoveRangeRequest(ChunkRange(BSON("X" << 100), BSON("X" << 200)));
    MigrationChunkClonerSource cloner(operationContext(),
                                      req,
                                      WriteConcernOptions(),
                                      kShardKeyPattern,
                                      kDonorConnStr,
                                      kRecipientConnStr.getServers()[0]);

    ASSERT_NOT_OK(cloner.startClone(operationContext(), UUID::gen(), _lsid, _txnNumber));
    cloner.cancelClone(operationContext());
}

TEST_F(MigrationChunkClonerSourceTest, ShardKeyIndexNotFound) {
    {
        OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE unsafeCreateCollection(
            operationContext());
        uassertStatusOK(
            createCollection(operationContext(), kNss.dbName(), BSON("create" << kNss.coll())));
    }

    const ShardsvrMoveRange req =
        createMoveRangeRequest(ChunkRange(BSON("X" << 100), BSON("X" << 200)));
    MigrationChunkClonerSource cloner(operationContext(),
                                      req,
                                      WriteConcernOptions(),
                                      kShardKeyPattern,
                                      kDonorConnStr,
                                      kRecipientConnStr.getServers()[0]);

    ASSERT_NOT_OK(cloner.startClone(operationContext(), UUID::gen(), _lsid, _txnNumber));
    cloner.cancelClone(operationContext());
}

TEST_F(MigrationChunkClonerSourceTest, FailedToEngageRecipientShard) {
    const std::vector<BSONObj> contents = {createCollectionDocument(99),
                                           createCollectionDocument(100),
                                           createCollectionDocument(199),
                                           createCollectionDocument(200)};

    createShardedCollection(contents);

    const ShardsvrMoveRange req =
        createMoveRangeRequest(ChunkRange(BSON("X" << 100), BSON("X" << 200)));
    MigrationChunkClonerSource cloner(operationContext(),
                                      req,
                                      WriteConcernOptions(),
                                      kShardKeyPattern,
                                      kDonorConnStr,
                                      kRecipientConnStr.getServers()[0]);

    {
        auto futureStartClone = launchAsync([&]() {
            onCommand([&](const RemoteCommandRequest& request) {
                return Status(ErrorCodes::NetworkTimeout,
                              "Did not receive confirmation from donor");
            });
        });

        auto startCloneStatus =
            cloner.startClone(operationContext(), UUID::gen(), _lsid, _txnNumber);
        ASSERT_EQ(ErrorCodes::NetworkTimeout, startCloneStatus.code());
        futureStartClone.default_timed_get();
    }

    // Ensure that if the recipient tries to fetch some documents, the cloner won't crash
    {
        AutoGetCollection autoColl(operationContext(), kNss, MODE_IS);

        {
            BSONArrayBuilder arrBuilder;
            ASSERT_OK(
                cloner.nextCloneBatch(operationContext(), autoColl.getCollection(), &arrBuilder));
            ASSERT_EQ(2, arrBuilder.arrSize());
        }

        {
            BSONArrayBuilder arrBuilder;
            ASSERT_OK(
                cloner.nextCloneBatch(operationContext(), autoColl.getCollection(), &arrBuilder));
            ASSERT_EQ(0, arrBuilder.arrSize());
        }
    }

    // Cancel clone should not send a cancellation request to the donor because we failed to engage
    // it (see comment in the startClone method)
    cloner.cancelClone(operationContext());
}

TEST_F(MigrationChunkClonerSourceTest, CloneFetchThatOverflows) {
    const auto kBigSize = 10 * 1024 * 1024;
    const std::vector<BSONObj> contents = {createSizedCollectionDocument(100, kBigSize),
                                           createSizedCollectionDocument(120, kBigSize),
                                           createSizedCollectionDocument(199, kBigSize)};

    createShardedCollection(contents);

    ShardsvrMoveRange req = createMoveRangeRequest(ChunkRange(BSON("X" << 100), BSON("X" << 200)));
    req.setMaxChunkSizeBytes(64 * 1024 * 1024);

    MigrationChunkClonerSource cloner(operationContext(),
                                      req,
                                      WriteConcernOptions(),
                                      kShardKeyPattern,
                                      kDonorConnStr,
                                      kRecipientConnStr.getServers()[0]);

    {
        auto futureStartClone = launchAsync([&]() {
            onCommand([&](const RemoteCommandRequest& request) { return BSON("ok" << true); });
        });

        ASSERT_OK(cloner.startClone(operationContext(), UUID::gen(), _lsid, _txnNumber));
        futureStartClone.default_timed_get();
    }

    // Ensure the initial clone documents are available
    {
        AutoGetCollection autoColl(operationContext(), kNss, MODE_IS);

        {
            BSONArrayBuilder arrBuilder;
            ASSERT_OK(
                cloner.nextCloneBatch(operationContext(), autoColl.getCollection(), &arrBuilder));
            ASSERT_EQ(1, arrBuilder.arrSize());

            const auto arr = arrBuilder.arr();
            ASSERT_BSONOBJ_EQ(contents[0], arr[0].Obj());
        }

        {
            BSONArrayBuilder arrBuilder;
            ASSERT_OK(
                cloner.nextCloneBatch(operationContext(), autoColl.getCollection(), &arrBuilder));
            ASSERT_EQ(1, arrBuilder.arrSize());

            const auto arr = arrBuilder.arr();
            ASSERT_BSONOBJ_EQ(contents[1], arr[0].Obj());
        }

        {
            BSONArrayBuilder arrBuilder;
            ASSERT_OK(
                cloner.nextCloneBatch(operationContext(), autoColl.getCollection(), &arrBuilder));
            ASSERT_EQ(1, arrBuilder.arrSize());

            const auto arr = arrBuilder.arr();
            ASSERT_BSONOBJ_EQ(contents[2], arr[0].Obj());
        }

        {
            BSONArrayBuilder arrBuilder;
            ASSERT_OK(
                cloner.nextCloneBatch(operationContext(), autoColl.getCollection(), &arrBuilder));
            ASSERT_EQ(0, arrBuilder.arrSize());
        }
    }

    auto futureCommit = launchAsync([&]() {
        onCommand([&](const RemoteCommandRequest& request) { return BSON("ok" << true); });
    });

    ASSERT_OK(cloner.commitClone(operationContext()));
    futureCommit.default_timed_get();
}

TEST_F(MigrationChunkClonerSourceTest, CloneShouldNotCrashWhenNextCloneBatchThrows) {
    const std::vector<BSONObj> contents = {createCollectionDocument(100),
                                           createCollectionDocument(150),
                                           createCollectionDocument(199)};

    createShardedCollection(contents);

    const ShardsvrMoveRange req =
        createMoveRangeRequest(ChunkRange(BSON("X" << 100), BSON("X" << 200)));
    MigrationChunkClonerSource cloner(operationContext(),
                                      req,
                                      WriteConcernOptions(),
                                      kShardKeyPattern,
                                      kDonorConnStr,
                                      kRecipientConnStr.getServers()[0]);

    {
        auto futureStartClone = launchAsync([&]() {
            onCommand([&](const RemoteCommandRequest& request) { return BSON("ok" << true); });
        });

        ASSERT_OK(cloner.startClone(operationContext(), UUID::gen(), _lsid, _txnNumber));
        futureStartClone.default_timed_get();
    }

    {
        AutoGetCollection autoColl(operationContext(), kNss, MODE_IS);

        {
            auto collWithFault =
                std::make_unique<CollectionWithFault>(autoColl.getCollection().get());
            CollectionPtr collPtrWithFault(collWithFault.get());

            // Note: findDoc currently doesn't have any interruption points, this test simulates
            // an exception being thrown while it is being called.
            collWithFault->setFindDocStatus({ErrorCodes::Interrupted, "fake interrupt"});

            BSONArrayBuilder arrBuilder;

            ASSERT_THROWS_CODE(
                cloner.nextCloneBatch(operationContext(), collPtrWithFault, &arrBuilder),
                DBException,
                ErrorCodes::Interrupted);
            ASSERT_EQ(0, arrBuilder.arrSize());
        }

        // The first document was lost and returned an error during nextCloneBatch. This would
        // cause the migration destination to abort, but it is still possible for other
        // threads to be in the middle of calling nextCloneBatch and the next nextCloneBatch
        // calls simulate calls from other threads after the first call threw.
        {
            BSONArrayBuilder arrBuilder;
            ASSERT_OK(
                cloner.nextCloneBatch(operationContext(), autoColl.getCollection(), &arrBuilder));

            const auto arr = arrBuilder.arr();
            ASSERT_EQ(2, arrBuilder.arrSize());

            ASSERT_BSONOBJ_EQ(contents[1], arr[0].Obj());
            ASSERT_BSONOBJ_EQ(contents[2], arr[1].Obj());
        }

        {
            BSONArrayBuilder arrBuilder;
            ASSERT_OK(
                cloner.nextCloneBatch(operationContext(), autoColl.getCollection(), &arrBuilder));

            const auto arr = arrBuilder.arr();
            ASSERT_EQ(0, arrBuilder.arrSize());
        }
    }

    auto futureCommit = launchAsync([&]() {
        // Simulate destination returning an error.
        onCommand([&](const RemoteCommandRequest& request) { return BSON("ok" << false); });

        // This is the return response for recvChunkAbort.
        onCommand([&](const RemoteCommandRequest& request) { return BSON("ok" << true); });
    });

    ASSERT_NOT_OK(cloner.commitClone(operationContext()));
    futureCommit.default_timed_get();
}

TEST_F(MigrationChunkClonerSourceTest, CorrectDocumentsFetchedWithDottedShardKeyPattern) {
    const ShardKeyPattern dottedShardKeyPattern(BSON("x.a" << 1 << "x.b" << 1));

    auto createDoc =
        ([](int val) { return BSON("_id" << val << "x" << BSON("a" << val << "b" << val)); });

    const ShardsvrMoveRange req = createMoveRangeRequest(
        ChunkRange(BSON("x.a" << 100 << "x.b" << 100), BSON("x.a" << 200 << "x.b" << 200)));
    MigrationChunkClonerSource cloner(operationContext(),
                                      req,
                                      WriteConcernOptions(),
                                      dottedShardKeyPattern.toBSON(),
                                      kDonorConnStr,
                                      kRecipientConnStr.getServers()[0]);

    // Insert some documents in the chunk range to be included for migration
    insertDocsInShardedCollection({createDoc(150)});
    insertDocsInShardedCollection({createDoc(151)});

    // Insert some documents which are outside of the chunk range and should not be included for
    // migration
    insertDocsInShardedCollection({createDoc(90)});
    insertDocsInShardedCollection({createDoc(210)});

    {
        AutoGetCollection autoColl(operationContext(), kNss, MODE_IX);

        WriteUnitOfWork wuow(operationContext());

        cloner.onInsertOp(operationContext(), createDoc(90), {});
        cloner.onInsertOp(operationContext(), createDoc(150), {});
        cloner.onInsertOp(operationContext(), createDoc(151), {});
        cloner.onInsertOp(operationContext(), createDoc(210), {});

        cloner.onDeleteOp(
            operationContext(), getDocumentKey(dottedShardKeyPattern, createDoc(80)), {});
        cloner.onDeleteOp(
            operationContext(), getDocumentKey(dottedShardKeyPattern, createDoc(199)), {});
        cloner.onDeleteOp(
            operationContext(), getDocumentKey(dottedShardKeyPattern, createDoc(220)), {});

        wuow.commit();
    }

    {
        AutoGetCollection autoColl(operationContext(), kNss, MODE_IS);

        {
            BSONArrayBuilder arrBuilder;
            ASSERT_OK(
                cloner.nextCloneBatch(operationContext(), autoColl.getCollection(), &arrBuilder));
            ASSERT_EQ(0, arrBuilder.arrSize());
        }

        {
            BSONObjBuilder modsBuilder;
            ASSERT_OK(cloner.nextModsBatch(operationContext(), &modsBuilder));

            const auto modsObj = modsBuilder.obj();
            ASSERT_EQ(2U, modsObj["reload"].Array().size());
            ASSERT_BSONOBJ_EQ(createDoc(150), modsObj["reload"].Array()[0].Obj());
            ASSERT_BSONOBJ_EQ(createDoc(151), modsObj["reload"].Array()[1].Obj());

            ASSERT_EQ(1U, modsObj["deleted"].Array().size());
            ASSERT_BSONOBJ_EQ(BSON("_id" << 199), modsObj["deleted"].Array()[0].Obj());
        }
    }

    cloner.cancelClone(operationContext());
}

TEST_F(MigrationChunkClonerSourceTest, CorrectDocumentsFetchedWithHasheddShardKeyPattern) {
    const ShardKeyPattern hashedShardKeyPattern(BSON("X"
                                                     << "hashed"));

    const ShardsvrMoveRange req = createMoveRangeRequest(
        ChunkRange(BSON("X" << 6000000000000000000ll), BSON("X" << 9003000000000000000ll)));
    MigrationChunkClonerSource cloner(operationContext(),
                                      req,
                                      WriteConcernOptions(),
                                      hashedShardKeyPattern.toBSON(),
                                      kDonorConnStr,
                                      kRecipientConnStr.getServers()[0]);

    // Insert some documents in the chunk range to be included for migration
    insertDocsInShardedCollection({createCollectionDocument(150)});
    insertDocsInShardedCollection({createCollectionDocument(151)});

    // Insert some documents which are outside of the chunk range and should not be included for
    // migration
    insertDocsInShardedCollection({createCollectionDocument(90)});
    insertDocsInShardedCollection({createCollectionDocument(210)});

    {
        AutoGetCollection autoColl(operationContext(), kNss, MODE_IX);

        WriteUnitOfWork wuow(operationContext());

        cloner.onInsertOp(
            operationContext(), createCollectionDocument(90), {});  // hashed = 1348713528393582036
        cloner.onInsertOp(
            operationContext(), createCollectionDocument(150), {});  // hashed = 9002984030040611364
        cloner.onInsertOp(
            operationContext(), createCollectionDocument(151), {});  // hashed = 6186237390842619770
        cloner.onInsertOp(
            operationContext(), createCollectionDocument(210), {});  // hashed = 4420792252088815836

        cloner.onDeleteOp(operationContext(),
                          getDocumentKey(hashedShardKeyPattern, createCollectionDocument(80)),
                          {});  // hashed = 6910253216116676730
        cloner.onDeleteOp(operationContext(),
                          getDocumentKey(hashedShardKeyPattern, createCollectionDocument(199)),
                          {});  // hashed = 3000073935277689405
        cloner.onDeleteOp(operationContext(),
                          getDocumentKey(hashedShardKeyPattern, createCollectionDocument(220)),
                          {});  // hashed = -6432749148213749320

        wuow.commit();
    }

    {
        AutoGetCollection autoColl(operationContext(), kNss, MODE_IS);

        {
            BSONArrayBuilder arrBuilder;
            ASSERT_OK(
                cloner.nextCloneBatch(operationContext(), autoColl.getCollection(), &arrBuilder));
            ASSERT_EQ(0, arrBuilder.arrSize());
        }

        {
            BSONObjBuilder modsBuilder;
            ASSERT_OK(cloner.nextModsBatch(operationContext(), &modsBuilder));

            const auto modsObj = modsBuilder.obj();
            ASSERT_EQ(2U, modsObj["reload"].Array().size());
            ASSERT_BSONOBJ_EQ(createCollectionDocument(150), modsObj["reload"].Array()[0].Obj());
            ASSERT_BSONOBJ_EQ(createCollectionDocument(151), modsObj["reload"].Array()[1].Obj());

            ASSERT_EQ(1U, modsObj["deleted"].Array().size());
            ASSERT_BSONOBJ_EQ(BSON("_id" << 80), modsObj["deleted"].Array()[0].Obj());
        }
    }

    cloner.cancelClone(operationContext());
}

TEST_F(MigrationChunkClonerSourceTest, UpdatedDocumentsFetched) {
    const ShardKeyPattern shardKeyPattern(kShardKeyPattern);

    const ShardsvrMoveRange req =
        createMoveRangeRequest(ChunkRange(BSON("X" << 100), BSON("X" << 200)));
    MigrationChunkClonerSource cloner(operationContext(),
                                      req,
                                      WriteConcernOptions(),
                                      kShardKeyPattern,
                                      kDonorConnStr,
                                      kRecipientConnStr.getServers()[0]);

    // Insert some documents in the chunk range to be included for migration
    insertDocsInShardedCollection({createCollectionDocument(150)});
    insertDocsInShardedCollection({createCollectionDocument(151)});

    {
        AutoGetCollection autoColl(operationContext(), kNss, MODE_IX);

        WriteUnitOfWork wuow(operationContext());

        cloner.onUpdateOp(operationContext(), boost::none, createCollectionDocument(150), {});
        cloner.onUpdateOp(
            operationContext(), createCollectionDocument(80), createCollectionDocument(151), {});

        // From in doc in chunk range to outside of range will be converted to a delete xferMods.
        cloner.onUpdateOp(
            operationContext(), createCollectionDocument(199), createCollectionDocument(90), {});

        wuow.commit();
    }

    {
        AutoGetCollection autoColl(operationContext(), kNss, MODE_IS);

        {
            BSONArrayBuilder arrBuilder;
            ASSERT_OK(
                cloner.nextCloneBatch(operationContext(), autoColl.getCollection(), &arrBuilder));
            ASSERT_EQ(0, arrBuilder.arrSize());
        }

        {
            BSONObjBuilder modsBuilder;
            ASSERT_OK(cloner.nextModsBatch(operationContext(), &modsBuilder));

            const auto modsObj = modsBuilder.obj();
            ASSERT_EQ(2U, modsObj["reload"].Array().size());
            ASSERT_BSONOBJ_EQ(createCollectionDocument(150), modsObj["reload"].Array()[0].Obj());
            ASSERT_BSONOBJ_EQ(createCollectionDocument(151), modsObj["reload"].Array()[1].Obj());

            ASSERT_EQ(1U, modsObj["deleted"].Array().size());
            ASSERT_BSONOBJ_EQ(BSON("_id" << 199), modsObj["deleted"].Array()[0].Obj());
        }
    }

    cloner.cancelClone(operationContext());
}

TEST_F(MigrationChunkClonerSourceTest, UpdatedDocumentsFetchedWithHashedShardKey) {
    const ShardKeyPattern shardKeyPattern(BSON("X"
                                               << "hashed"));

    const ShardsvrMoveRange req = createMoveRangeRequest(
        ChunkRange(BSON("X" << 6000000000000000000ll), BSON("X" << 9003000000000000000ll)));
    MigrationChunkClonerSource cloner(operationContext(),
                                      req,
                                      WriteConcernOptions(),
                                      shardKeyPattern.toBSON(),
                                      kDonorConnStr,
                                      kRecipientConnStr.getServers()[0]);

    // Insert some documents in the chunk range to be included for migration
    insertDocsInShardedCollection({createCollectionDocument(150)});
    insertDocsInShardedCollection({createCollectionDocument(151)});

    {
        AutoGetCollection autoColl(operationContext(), kNss, MODE_IX);

        WriteUnitOfWork wuow(operationContext());

        cloner.onUpdateOp(operationContext(),
                          boost::none,
                          createCollectionDocument(150),
                          {});  // hashed = 9002984030040611364
        cloner.onUpdateOp(operationContext(),
                          createCollectionDocument(90),
                          createCollectionDocument(151),
                          {});  // hashed = 1348713528393582036 -> 6186237390842619770

        // From in doc in chunk range to outside of range will be converted to a delete xferMods.
        cloner.onUpdateOp(operationContext(),
                          createCollectionDocument(80),
                          createCollectionDocument(199),
                          {});  // hashed = 6910253216116676730 -> 3000073935277689405

        wuow.commit();
    }

    {
        AutoGetCollection autoColl(operationContext(), kNss, MODE_IS);

        {
            BSONArrayBuilder arrBuilder;
            ASSERT_OK(
                cloner.nextCloneBatch(operationContext(), autoColl.getCollection(), &arrBuilder));
            ASSERT_EQ(0, arrBuilder.arrSize());
        }

        {
            BSONObjBuilder modsBuilder;
            ASSERT_OK(cloner.nextModsBatch(operationContext(), &modsBuilder));

            const auto modsObj = modsBuilder.obj();
            ASSERT_EQ(2U, modsObj["reload"].Array().size());
            ASSERT_BSONOBJ_EQ(createCollectionDocument(150), modsObj["reload"].Array()[0].Obj());
            ASSERT_BSONOBJ_EQ(createCollectionDocument(151), modsObj["reload"].Array()[1].Obj());

            ASSERT_EQ(1U, modsObj["deleted"].Array().size());
            ASSERT_BSONOBJ_EQ(BSON("_id" << 80), modsObj["deleted"].Array()[0].Obj());
        }
    }

    cloner.cancelClone(operationContext());
}

TEST_F(MigrationChunkClonerSourceTest, UpdatedDocumentsFetchedWithDottedShardKeyPattern) {
    const ShardKeyPattern dottedShardKeyPattern(BSON("x.a" << 1 << "x.b" << 1));

    auto createDoc =
        ([](int val) { return BSON("_id" << val << "x" << BSON("a" << val << "b" << val)); });

    const ShardsvrMoveRange req = createMoveRangeRequest(
        ChunkRange(BSON("x.a" << 100 << "x.b" << 100), BSON("x.a" << 200 << "x.b" << 200)));
    MigrationChunkClonerSource cloner(operationContext(),
                                      req,
                                      WriteConcernOptions(),
                                      dottedShardKeyPattern.toBSON(),
                                      kDonorConnStr,
                                      kRecipientConnStr.getServers()[0]);

    // Insert some documents in the chunk range to be included for migration
    insertDocsInShardedCollection({createDoc(150)});
    insertDocsInShardedCollection({createDoc(151)});

    {
        AutoGetCollection autoColl(operationContext(), kNss, MODE_IX);

        WriteUnitOfWork wuow(operationContext());

        cloner.onUpdateOp(operationContext(), boost::none, createDoc(150), {});
        cloner.onUpdateOp(operationContext(), createDoc(80), createDoc(151), {});

        // From in doc in chunk range to outside of range will be converted to a delete xferMods.
        cloner.onUpdateOp(operationContext(), createDoc(199), createDoc(90), {});

        wuow.commit();
    }

    {
        AutoGetCollection autoColl(operationContext(), kNss, MODE_IS);

        {
            BSONArrayBuilder arrBuilder;
            ASSERT_OK(
                cloner.nextCloneBatch(operationContext(), autoColl.getCollection(), &arrBuilder));
            ASSERT_EQ(0, arrBuilder.arrSize());
        }

        {
            BSONObjBuilder modsBuilder;
            ASSERT_OK(cloner.nextModsBatch(operationContext(), &modsBuilder));

            const auto modsObj = modsBuilder.obj();
            ASSERT_EQ(2U, modsObj["reload"].Array().size());
            ASSERT_BSONOBJ_EQ(createDoc(150), modsObj["reload"].Array()[0].Obj());
            ASSERT_BSONOBJ_EQ(createDoc(151), modsObj["reload"].Array()[1].Obj());

            ASSERT_EQ(1U, modsObj["deleted"].Array().size());
            ASSERT_BSONOBJ_EQ(BSON("_id" << 199), modsObj["deleted"].Array()[0].Obj());
        }
    }

    cloner.cancelClone(operationContext());
}

}  // namespace
}  // namespace mongo
