/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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


#include <absl/container/node_hash_map.h>
#include <boost/cstdint.hpp>
#include <boost/none.hpp>
#include <initializer_list>
#include <memory>
#include <ostream>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/index_builds/index_builds_coordinator_mock.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/primary_only_service_test_fixture.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/s/migration_destination_manager.h"
#include "mongo/db/s/resharding/donor_oplog_id_gen.h"
#include "mongo/db/s/resharding/resharding_change_event_o2_field_gen.h"
#include "mongo/db/s/resharding/resharding_data_copy_util.h"
#include "mongo/db/s/resharding/resharding_data_replication.h"
#include "mongo/db/s/resharding/resharding_oplog_applier_progress_gen.h"
#include "mongo/db/s/resharding/resharding_oplog_fetcher_progress_gen.h"
#include "mongo/db/s/resharding/resharding_recipient_service.h"
#include "mongo/db/s/resharding/resharding_recipient_service_external_state.h"
#include "mongo/db/s/resharding/resharding_service_test_helpers.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/task_executor.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_index_catalog_gen.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/database_version.h"
#include "mongo/s/index_version.h"
#include "mongo/s/sharding_index_catalog_cache.h"
#include "mongo/s/sharding_test_fixture_common.h"
#include "mongo/s/type_collection_common_types_gen.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/functional.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(failToCreateReshardingDataReplicationForTest);

using RecipientStateTransitionController =
    resharding_service_test_helpers::StateTransitionController<RecipientStateEnum>;
using PauseDuringStateTransitions =
    resharding_service_test_helpers::PauseDuringStateTransitions<RecipientStateEnum>;
using OpObserverForTest = resharding_service_test_helpers::
    StateTransitionControllerOpObserver<RecipientStateEnum, ReshardingRecipientDocument>;
const ShardId recipientShardId{"myShardId"};
const long approxBytesToCopy = 10000;
const long approxDocumentsToCopy = 100;

const BSONObj sourceCollectionOptions = BSONObj();
BSONObj tempReshardingCollectionOptions = BSONObj();

class ExternalStateForTest : public ReshardingRecipientService::RecipientStateMachineExternalState {
public:
    ShardId myShardId(ServiceContext* serviceContext) const override {
        return recipientShardId;
    }

    void refreshCatalogCache(OperationContext* opCtx, const NamespaceString& nss) override {}

    CollectionRoutingInfo getTrackedCollectionRoutingInfo(OperationContext* opCtx,
                                                          const NamespaceString& nss) override {
        invariant(nss == _sourceNss);

        const OID epoch = OID::gen();
        std::vector<ChunkType> chunks = {ChunkType{
            _sourceUUID,
            ChunkRange{BSON(_currentShardKey << MINKEY), BSON(_currentShardKey << MAXKEY)},
            ChunkVersion({epoch, Timestamp(1, 1)}, {100, 0}),
            _someDonorId}};

        auto rt = RoutingTableHistory::makeNew(_sourceNss,
                                               _sourceUUID,
                                               BSON(_currentShardKey << 1),
                                               false, /* unsplittable */
                                               nullptr /* defaultCollator */,
                                               false /* unique */,
                                               epoch,
                                               Timestamp(1, 1),
                                               boost::none /* timeseriesFields */,
                                               boost::none /* reshardingFields */,
                                               true /* allowMigrations */,
                                               chunks);
        IndexCatalogTypeMap shardingIndexesCatalogMap;
        shardingIndexesCatalogMap.emplace(
            "randomKey_1",
            IndexCatalogType(
                "randomKey_1", BSON("randomKey" << 1), BSONObj(), Timestamp(1, 0), _sourceUUID));

        return CollectionRoutingInfo{
            ChunkManager(
                _someDonorId,
                DatabaseVersion(UUID::gen(), Timestamp(1, 1)),
                ShardingTestFixtureCommon::makeStandaloneRoutingTableHistory(std::move(rt)),
                boost::none /* clusterTime */),
            ShardingIndexesCatalogCache(CollectionIndexes(_sourceUUID, Timestamp(1, 0)),
                                        std::move(shardingIndexesCatalogMap))};
    }

    MigrationDestinationManager::CollectionOptionsAndUUID getCollectionOptions(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const UUID& uuid,
        boost::optional<Timestamp> afterClusterTime,
        StringData reason) override {
        if (nss == _sourceNss) {
            return {sourceCollectionOptions, uuid};
        } else {
            return {tempReshardingCollectionOptions, uuid};
        }
    }

    MigrationDestinationManager::CollectionOptionsAndUUID getCollectionOptions(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const UUID& uuid,
        boost::optional<Timestamp> afterClusterTime,
        StringData reason,
        const ShardId& fromShardId) override {
        return getCollectionOptions(opCtx, nss, uuid, afterClusterTime, reason);
    }

    MigrationDestinationManager::IndexesAndIdIndex getCollectionIndexes(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const UUID& uuid,
        Timestamp afterClusterTime,
        StringData reason,
        bool expandSimpleCollation) override {
        invariant(nss == _sourceNss);
        return {std::vector<BSONObj>{}, BSONObj()};
    }

    boost::optional<ShardingIndexesCatalogCache> getCollectionIndexInfoWithRefresh(
        OperationContext* opCtx, const NamespaceString& nss) override {
        return boost::none;
    }

    void withShardVersionRetry(OperationContext* opCtx,
                               const NamespaceString& nss,
                               StringData reason,
                               unique_function<void()> callback) override {
        callback();
    }

    void updateCoordinatorDocument(OperationContext* opCtx,
                                   const BSONObj& query,
                                   const BSONObj& update) override {
        auto coll = acquireCollection(opCtx,
                                      CollectionAcquisitionRequest::fromOpCtx(
                                          opCtx,
                                          NamespaceString::kConfigReshardingOperationsNamespace,
                                          AcquisitionPrerequisites::kWrite),
                                      MODE_IX);
        Helpers::update(opCtx, coll, query, update);
    }

    void clearFilteringMetadataOnTempReshardingCollection(
        OperationContext* opCtx, const NamespaceString& tempReshardingNss) override {}

private:
    const StringData _currentShardKey = "oldKey";

    const NamespaceString _sourceNss =
        NamespaceString::createNamespaceString_forTest("sourcedb", "sourcecollection");
    const UUID _sourceUUID = UUID::gen();

    const ShardId _someDonorId{"myDonorId"};
};

class DataReplicationForTest : public ReshardingDataReplicationInterface {
public:
    DataReplicationForTest() {
        if (failToCreateReshardingDataReplicationForTest.shouldFail()) {
            uasserted(ErrorCodes::InternalError, "Failed to create DataReplicationForTest");
        }
    }
    SemiFuture<void> runUntilStrictlyConsistent(
        std::shared_ptr<executor::TaskExecutor> executor,
        std::shared_ptr<executor::TaskExecutor> cleanupExecutor,
        CancellationToken cancelToken,
        CancelableOperationContextFactory opCtxFactory,
        const mongo::Date_t& startConfigTxnCloneTime) override {
        return makeReadyFutureWith([] {}).semi();
    };

    void startOplogApplication() override{};

    SharedSemiFuture<void> awaitCloningDone() override {
        return makeReadyFutureWith([] {}).share();
    };

    SharedSemiFuture<void> awaitStrictlyConsistent() override {
        return makeReadyFutureWith([] {}).share();
    };

    void shutdown() override {}

    void join() override {}
};

class ReshardingRecipientServiceForTest : public ReshardingRecipientService {
public:
    explicit ReshardingRecipientServiceForTest(ServiceContext* serviceContext)
        : ReshardingRecipientService(serviceContext), _serviceContext(serviceContext) {}

    std::shared_ptr<repl::PrimaryOnlyService::Instance> constructInstance(
        BSONObj initialState) override {
        return std::make_shared<RecipientStateMachine>(
            this,
            ReshardingRecipientDocument::parse(
                IDLParserContext{"ReshardingRecipientServiceForTest"}, initialState),
            std::make_unique<ExternalStateForTest>(),
            [](auto...) { return std::make_unique<DataReplicationForTest>(); },
            _serviceContext);
    }

private:
    ServiceContext* _serviceContext;
};

struct TestOptions {
    bool isAlsoDonor;
    bool skipCloningAndApplying;
    bool noChunksToCopy;
    bool storeOplogFetcherProgress = true;
    bool performVerification = true;

    BSONObj toBSON() const {
        BSONObjBuilder bob;
        bob.append("isAlsoDonor", isAlsoDonor);
        bob.append("skipCloningAndApplying", skipCloningAndApplying);
        bob.append("noChunksToCopy", noChunksToCopy);
        bob.append("storeOplogFetcherProgress", storeOplogFetcherProgress);
        bob.append("performVerification", performVerification);
        return bob.obj();
    }
};

std::vector<TestOptions> makeBasicTestOptions() {
    std::vector<TestOptions> testOptions;
    for (bool isAlsoDonor : {false, true}) {
        for (bool skipCloningAndApplying : {false, true}) {
            testOptions.push_back({isAlsoDonor, skipCloningAndApplying});
        }
    }
    return testOptions;
}

std::vector<TestOptions> makeAllTestOptions() {
    std::vector<TestOptions> testOptions;
    for (bool isAlsoDonor : {false, true}) {
        for (bool skipCloningAndApplying : {false, true}) {
            for (bool noChunksToCopy : {false, true}) {
                for (bool storeOplogFetcherProgress : {false, true}) {
                    for (bool performVerification : {false, true}) {
                        if (skipCloningAndApplying && !noChunksToCopy) {
                            // This is an invalid combination.
                            continue;
                        }
                        testOptions.push_back({isAlsoDonor,
                                               skipCloningAndApplying,
                                               noChunksToCopy,
                                               storeOplogFetcherProgress,
                                               performVerification});
                    }
                }
            }
        }
    }
    return testOptions;
}

BSONObj makeTestDocument(int i) {
    auto id = UUID::gen();
    return BSON("_id" << id.toBSON() << "x" << i << "y" << i);
}

struct RecipientMetricsCommon {
    int64_t docsCopied = 0;
    int64_t bytesCopied = 0;
    int64_t oplogFetched = 0;
    int64_t oplogApplied = 0;
};

struct RecipientMetricsForDonor {
    ShardId shardId;
    RecipientMetricsCommon metrics;
};

struct TestRecipientMetrics {
public:
    void add(const RecipientMetricsForDonor& metricsForDonor) {
        _metricsTotal.docsCopied += metricsForDonor.metrics.docsCopied;
        _metricsTotal.bytesCopied += metricsForDonor.metrics.bytesCopied;
        _metricsTotal.oplogFetched += metricsForDonor.metrics.oplogFetched;
        _metricsTotal.oplogApplied += metricsForDonor.metrics.oplogApplied;
        _metricsForDonors.push_back(metricsForDonor);
    }

    const RecipientMetricsCommon& getMetricsTotal() const {
        return _metricsTotal;
    }

    const std::vector<RecipientMetricsForDonor>& getMetricsForDonors() const {
        return _metricsForDonors;
    }

private:
    RecipientMetricsCommon _metricsTotal;
    std::vector<RecipientMetricsForDonor> _metricsForDonors;
};

TestRecipientMetrics makeTestRecipientMetrics(const TestOptions& testOptions,
                                              const std::vector<DonorShardFetchTimestamp>& donors) {
    if (testOptions.noChunksToCopy) {
        return {};
    }

    ASSERT_GT(donors.size(), 1);
    TestRecipientMetrics testRecipientMetrics;

    for (unsigned long i = 0; i < donors.size(); i++) {
        if (i == 0) {
            // Make the recipient not have any documents to copy or oplog entries to fetch/apply
            // from donor0.
            continue;
        }

        RecipientMetricsForDonor metricsForDonor;
        metricsForDonor.shardId = donors[i].getShardId();

        auto bytesPerDoc = makeTestDocument(0).objsize();
        metricsForDonor.metrics.docsCopied = i;
        metricsForDonor.metrics.bytesCopied = bytesPerDoc * metricsForDonor.metrics.docsCopied;
        metricsForDonor.metrics.oplogFetched = i * 2;
        metricsForDonor.metrics.oplogApplied = metricsForDonor.metrics.oplogFetched;

        testRecipientMetrics.add(metricsForDonor);
    }
    return testRecipientMetrics;
}

/**
 * Tests the behavior of the ReshardingRecipientService upon recovery from failover.
 */
class ReshardingRecipientServiceTest : public repl::PrimaryOnlyServiceMongoDTest {
public:
    using RecipientStateMachine = ReshardingRecipientService::RecipientStateMachine;

    ReshardingRecipientServiceTest()
        : repl::PrimaryOnlyServiceMongoDTest(
              Options{}.useIndexBuildsCoordinator(std::make_unique<IndexBuildsCoordinatorMock>())) {
    }

    std::unique_ptr<repl::PrimaryOnlyService> makeService(ServiceContext* serviceContext) override {
        return std::make_unique<ReshardingRecipientServiceForTest>(serviceContext);
    }

    void setUp() override {
        repl::PrimaryOnlyServiceMongoDTest::setUp();

        auto serviceContext = getServiceContext();
        auto storageMock = std::make_unique<repl::StorageInterfaceMock>();
        repl::StorageInterface::set(serviceContext, std::move(storageMock));

        _controller = std::make_shared<RecipientStateTransitionController>();
        _opObserverRegistry->addObserver(std::make_unique<OpObserverForTest>(
            _controller,
            NamespaceString::kRecipientReshardingOperationsNamespace,
            [](const ReshardingRecipientDocument& stateDoc) {
                return stateDoc.getMutableState().getState();
            }));
    }

    RecipientStateTransitionController* controller() {
        return _controller.get();
    }

    BSONObj newShardKeyPattern() {
        return BSON("newKey" << 1);
    }

    ReshardingRecipientDocument makeRecipientDocument(const TestOptions& testOptions) {
        RecipientShardContext recipientCtx;
        recipientCtx.setState(RecipientStateEnum::kUnused);

        ReshardingRecipientDocument doc(
            std::move(recipientCtx),
            {ShardId{"donor1"},
             testOptions.isAlsoDonor ? recipientShardId : ShardId{"donor2"},
             ShardId{"donor3"}},
            durationCount<Milliseconds>(Milliseconds{5}));

        NamespaceString sourceNss =
            NamespaceString::createNamespaceString_forTest("sourcedb", "sourcecollection");
        auto sourceUUID = UUID::gen();
        auto commonMetadata = CommonReshardingMetadata(
            UUID::gen(),
            sourceNss,
            sourceUUID,
            resharding::constructTemporaryReshardingNss(sourceNss, sourceUUID),
            newShardKeyPattern());
        commonMetadata.setStartTime(getServiceContext()->getFastClockSource()->now());

        doc.setCommonReshardingMetadata(std::move(commonMetadata));
        doc.setSkipCloningAndApplying(testOptions.skipCloningAndApplying);
        doc.setStoreOplogFetcherProgress(testOptions.storeOplogFetcherProgress);
        doc.setPerformVerification(testOptions.performVerification);
        return doc;
    }

    ReshardingCoordinatorDocument makeCoordinatorDocument(
        const ReshardingRecipientDocument& recipientDoc) {
        ReshardingCoordinatorDocument coordinatorDoc;
        coordinatorDoc.setState(CoordinatorStateEnum::kUnused);
        coordinatorDoc.setCommonReshardingMetadata(recipientDoc.getCommonReshardingMetadata());

        std::vector<DonorShardEntry> donorShards;
        std::transform(recipientDoc.getDonorShards().begin(),
                       recipientDoc.getDonorShards().end(),
                       std::back_inserter(donorShards),
                       [](auto donorShard) {
                           return DonorShardEntry{donorShard.getShardId(), {}};
                       });
        coordinatorDoc.setDonorShards(donorShards);

        std::vector<RecipientShardEntry> recipientShards;
        recipientShards.push_back({recipientShardId, recipientDoc.getMutableState()});
        coordinatorDoc.setRecipientShards(recipientShards);

        return coordinatorDoc;
    }

    void createSourceCollection(OperationContext* opCtx,
                                const ReshardingRecipientDocument& recipientDoc) {
        CollectionOptions options;
        options.uuid = recipientDoc.getSourceUUID();
        resharding::data_copy::ensureCollectionDropped(opCtx, recipientDoc.getSourceNss());
        resharding::data_copy::ensureCollectionExists(opCtx, recipientDoc.getSourceNss(), options);
    }

    void createTempReshardingCollection(OperationContext* opCtx,
                                        const ReshardingRecipientDocument& recipientDoc) {
        CollectionOptions options;
        options.uuid = recipientDoc.getReshardingUUID();
        resharding::data_copy::ensureCollectionDropped(opCtx, recipientDoc.getTempReshardingNss());
        resharding::data_copy::ensureCollectionExists(
            opCtx, recipientDoc.getTempReshardingNss(), options);
    }

    void notifyToStartCloning(OperationContext* opCtx,
                              RecipientStateMachine& recipient,
                              const ReshardingRecipientDocument& recipientDoc) {
        _onReshardingFieldsChanges(opCtx, recipient, recipientDoc, CoordinatorStateEnum::kCloning);
    }

    void notifyReshardingCommitting(OperationContext* opCtx,
                                    RecipientStateMachine& recipient,
                                    const ReshardingRecipientDocument& recipientDoc) {
        _onReshardingFieldsChanges(
            opCtx, recipient, recipientDoc, CoordinatorStateEnum::kCommitting);
    }

    void checkRecipientDocumentRemoved(OperationContext* opCtx) {
        AutoGetCollection recipientColl(
            opCtx, NamespaceString::kRecipientReshardingOperationsNamespace, MODE_IS);
        ASSERT_TRUE(bool(recipientColl));
        ASSERT_TRUE(bool(recipientColl->isEmpty(opCtx)));
    }

    template <typename DocumentType>
    DocumentType getPersistedStateDocument(OperationContext* opCtx,
                                           const NamespaceString& nss,
                                           UUID reshardingUUID) {
        boost::optional<DocumentType> persistedDoc;
        PersistentTaskStore<DocumentType> store(nss);
        store.forEach(opCtx,
                      BSON(DocumentType::kReshardingUUIDFieldName << reshardingUUID),
                      [&](const auto& doc) {
                          persistedDoc.emplace(doc);
                          return false;
                      });
        ASSERT(persistedDoc);
        return persistedDoc.get();
    }

    ReshardingRecipientDocument getPersistedRecipientDocument(OperationContext* opCtx,
                                                              UUID reshardingUUID) {
        return getPersistedStateDocument<ReshardingRecipientDocument>(
            opCtx, NamespaceString::kRecipientReshardingOperationsNamespace, reshardingUUID);
    }

    ReshardingCoordinatorDocument getPersistedCoordinatorDocument(OperationContext* opCtx,
                                                                  UUID reshardingUUID) {
        return getPersistedStateDocument<ReshardingCoordinatorDocument>(
            opCtx, NamespaceString::kConfigReshardingOperationsNamespace, reshardingUUID);
    }

protected:
    void setNoChunksToCopy(const TestOptions& testOptions) {
        _noChunksToCopy = testOptions.noChunksToCopy;
    }

    void insertDocuments(OperationContext* opCtx,
                         const NamespaceString nss,
                         const std::vector<BSONObj>& docs) {
        resharding::data_copy::ensureCollectionExists(opCtx, nss, CollectionOptions());
        std::vector<InsertStatement> inserts;
        for (const auto& doc : docs) {
            inserts.emplace_back(doc);
        }
        resharding::data_copy::insertBatch(opCtx, nss, inserts);
    }

    void mockCollectionClonerStateForDonor(
        OperationContext* opCtx,
        const TestOptions testOptions,
        const RecipientMetricsForDonor& testRecipientMetricsForDonor,
        const CommonReshardingMetadata& metadata,
        ReshardingMetrics& metrics) {
        auto docsCopied = testRecipientMetricsForDonor.metrics.docsCopied;
        auto bytesCopied = testRecipientMetricsForDonor.metrics.bytesCopied;

        if (docsCopied == 0) {
            ASSERT_EQ(bytesCopied, 0);
            return;
        }
        ASSERT_GT(bytesCopied, 0);

        bool storeCollectionClonerProgress = testOptions.performVerification;
        if (storeCollectionClonerProgress) {
            // Set up the cloner resume data collection. To verify that the metrics are restored
            // from the counts in this collection instead of the fast count of documents the
            // temporary collection, do not insert any documents to the temporary collection.
            ReshardingRecipientResumeDataId resumeDataId(
                {metadata.getReshardingUUID(), testRecipientMetricsForDonor.shardId});
            ReshardingRecipientResumeData resumeDataDoc(resumeDataId);
            resumeDataDoc.setDocumentsCopied(docsCopied);
            resumeDataDoc.setBytesCopied(bytesCopied);
            insertDocuments(opCtx,
                            NamespaceString::kRecipientReshardingResumeDataNamespace,
                            {resumeDataDoc.toBSON()});
        } else {
            // Set up the temporary collection.
            std::vector<BSONObj> docs;
            for (int i = 0; i < docsCopied; i++) {
                docs.push_back(makeTestDocument(i));
            }
            insertDocuments(opCtx, metadata.getTempReshardingNss(), docs);
        }

        metrics.onDocumentsProcessed(docsCopied, bytesCopied, Milliseconds(1));
    }

    std::vector<BSONObj> makeFetchedOplogEntryDocuments(long num) {
        std::vector<BSONObj> oplogEntries;

        Timestamp timestamp{1, 1};
        for (auto i = 0; i < num; i++) {
            // Only mock the necessary fields.
            ReshardingDonorOplogId oplogId{timestamp, timestamp};
            oplogEntries.push_back(BSON("_id" << oplogId.toBSON()));
            timestamp = timestamp + 1ULL;
        }
        return oplogEntries;
    }

    void mockOplogFetcherAndApplierStateForDonor(
        OperationContext* opCtx,
        const TestOptions testOptions,
        const RecipientMetricsForDonor& testRecipientMetricsForDonor,
        const CommonReshardingMetadata& metadata,
        boost::optional<ReshardingMetrics&> metrics = boost::none) {
        auto oplogEntriesFetched = testRecipientMetricsForDonor.metrics.oplogFetched;
        auto oplogEntriesApplied = testRecipientMetricsForDonor.metrics.oplogApplied;
        ASSERT_EQ(oplogEntriesFetched, oplogEntriesApplied);

        if (oplogEntriesApplied == 0) {
            return;
        }

        auto fetchedOplogEntries = makeFetchedOplogEntryDocuments(oplogEntriesApplied);

        if (testOptions.storeOplogFetcherProgress) {
            // Set up the fetcher progress collection. To verify that the metrics are restored
            // from the counts in this collection instead of the fast count of documents the oplog
            // buffer collection, do not insert any documents to the buffer collection.
            ReshardingOplogFetcherProgress fetcherProgressDoc(
                {metadata.getReshardingUUID(), testRecipientMetricsForDonor.shardId},
                oplogEntriesApplied);
            insertDocuments(opCtx,
                            NamespaceString::kReshardingFetcherProgressNamespace,
                            {fetcherProgressDoc.toBSON()});
        } else {
            // Set up the oplog buffer collection.
            insertDocuments(opCtx,
                            resharding::getLocalOplogBufferNamespace(
                                metadata.getSourceUUID(), testRecipientMetricsForDonor.shardId),
                            fetchedOplogEntries);
        }

        // Set up the applier progress collection.
        auto donorOplogId =
            ReshardingDonorOplogId::parse(IDLParserContext("ReshardingRecipientServiceTest"),
                                          fetchedOplogEntries.back()["_id"].Obj());
        ReshardingOplogApplierProgress applierProgressDoc(
            {metadata.getReshardingUUID(), testRecipientMetricsForDonor.shardId},
            donorOplogId,
            oplogEntriesApplied);
        insertDocuments(opCtx,
                        NamespaceString::kReshardingApplierProgressNamespace,
                        {applierProgressDoc.toBSON()});

        if (metrics) {
            metrics->onOplogEntriesFetched(oplogEntriesFetched);
            metrics->onOplogEntriesApplied(oplogEntriesApplied);
        }
    }

    void mockDataReplicationStateIfNeeded(OperationContext* opCtx,
                                          const TestOptions testOptions,
                                          const TestRecipientMetrics& testRecipientMetrics,
                                          const CommonReshardingMetadata& metadata,
                                          RecipientStateEnum prevState,
                                          ReshardingMetrics& metrics) {
        if (testOptions.skipCloningAndApplying) {
            return;
        }

        if (prevState == RecipientStateEnum::kCloning) {
            for (const auto& testMetricsForDonor : testRecipientMetrics.getMetricsForDonors()) {
                mockCollectionClonerStateForDonor(
                    opCtx, testOptions, testMetricsForDonor, metadata, metrics);
            }
        } else if (prevState == RecipientStateEnum::kApplying) {
            for (const auto& testMetricsForDonor : testRecipientMetrics.getMetricsForDonors()) {
                mockOplogFetcherAndApplierStateForDonor(
                    opCtx, testOptions, testMetricsForDonor, metadata, metrics);
            }
        }
    }

    void checkRecipientToCopyMetrics(const TestOptions& testOptions,
                                     const ReshardingRecipientDocument& recipientDoc,
                                     const boost::optional<BSONObj>& currOp) {
        auto state = recipientDoc.getMutableState().getState();

        if (state <= RecipientStateEnum::kCloning) {
            return;
        }

        auto expectedApproxDocsToCopy = testOptions.noChunksToCopy ? 0 : approxDocumentsToCopy;
        auto expectedApproxBytesToCopy = testOptions.noChunksToCopy ? 0 : approxBytesToCopy;

        ASSERT_EQ(*recipientDoc.getMetrics()->getApproxDocumentsToCopy(), expectedApproxDocsToCopy);
        ASSERT_EQ(*recipientDoc.getMetrics()->getApproxBytesToCopy(), expectedApproxBytesToCopy);

        if (currOp) {
            ASSERT_EQ(currOp->getField("approxDocumentsToCopy").numberLong(),
                      expectedApproxDocsToCopy);
            ASSERT_EQ(currOp->getField("approxBytesToCopy").numberLong(),
                      expectedApproxBytesToCopy);
        }
    }

    void checkRecipientCopiedMetrics(const TestOptions& testOptions,
                                     const TestRecipientMetrics& testRecipientMetrics,
                                     const ReshardingRecipientDocument& recipientDoc,
                                     const boost::optional<BSONObj>& currOp) {
        auto mutableState = recipientDoc.getMutableState();
        auto state = mutableState.getState();

        if (state < RecipientStateEnum::kApplying) {
            return;
        }

        auto expectedDocsCopied = testRecipientMetrics.getMetricsTotal().docsCopied;
        auto expectedBytesCopied = testRecipientMetrics.getMetricsTotal().bytesCopied;

        ASSERT_EQ(*recipientDoc.getMetrics()->getFinalDocumentsCopiedCount(), expectedDocsCopied);
        ASSERT_EQ(*recipientDoc.getMetrics()->getFinalBytesCopiedCount(), expectedBytesCopied);

        // There are separate metrics in the recipient doc that are shared with the coordinator doc
        // (via RecipientShardEntry/ReshardingRecipientContext).

        // The metrics below are populated upon transitioning to the "done" state.
        if (state == RecipientStateEnum::kDone) {
            // There is currently no 'documentsCopied'.
            ASSERT_EQ(*mutableState.getBytesCopied(), expectedBytesCopied);
        } else {
            ASSERT_FALSE(mutableState.getBytesCopied());
        }

        // If verification is enabled, the metrics below are populated upon transitioning to the
        // "applying" state after cloning completes and are updated as oplog entries are applied. If
        // verification is disabled, they are populated upon transitioning to the "done" state.
        if (testOptions.performVerification || state == RecipientStateEnum::kDone) {
            ASSERT_EQ(*mutableState.getTotalNumDocuments(), expectedDocsCopied);
            // Upon transitioning to the "done" state, 'totalDocumentSize' is set to the fast count
            // size. This test deliberately leaves the temporary collection empty when testing
            // verification so this is expected to be equal to 0.
            bool expectInaccurateSize =
                testOptions.performVerification && state == RecipientStateEnum::kDone;
            ASSERT_EQ(*mutableState.getTotalDocumentSize(),
                      expectInaccurateSize ? 0 : expectedBytesCopied);
        } else {
            ASSERT_FALSE(mutableState.getTotalNumDocuments());
            ASSERT_FALSE(mutableState.getTotalDocumentSize());
        }

        if (currOp) {
            ASSERT_EQ(currOp->getField("documentsCopied").numberLong(), expectedDocsCopied);
            ASSERT_EQ(currOp->getField("bytesCopied").numberLong(), expectedBytesCopied);
        }
    }

    void checkRecipientOplogMetrics(const TestRecipientMetrics& testRecipientMetrics,
                                    const ReshardingRecipientDocument& recipientDoc,
                                    const boost::optional<BSONObj>& currOp) {
        auto mutableState = recipientDoc.getMutableState();
        auto state = mutableState.getState();

        if (state < RecipientStateEnum::kStrictConsistency) {
            return;
        }

        auto expectedOplogFetched = testRecipientMetrics.getMetricsTotal().oplogFetched;
        auto expectedOplogApplied = testRecipientMetrics.getMetricsTotal().oplogApplied;

        // The oplog metrics in the recipient doc (in ReshardingRecipientContext) are populated upon
        // transitioning to the "done" state.
        if (state == RecipientStateEnum::kDone) {
            ASSERT_EQ(*mutableState.getOplogFetched(), expectedOplogFetched);
            ASSERT_EQ(*mutableState.getOplogApplied(), expectedOplogApplied);
        } else {
            ASSERT_FALSE(mutableState.getOplogFetched());
            ASSERT_FALSE(mutableState.getOplogApplied());
        }

        if (currOp) {
            ASSERT_EQ(currOp->getField("oplogEntriesFetched").numberLong(), expectedOplogFetched);
            ASSERT_EQ(currOp->getField("oplogEntriesApplied").numberLong(), expectedOplogApplied);
        }
    }

    void checkRecipientMetrics(const TestOptions& testOptions,
                               const TestRecipientMetrics& testRecipientMetrics,
                               const ReshardingRecipientDocument& recipientDoc,
                               const boost::optional<BSONObj>& currOp) {
        checkRecipientToCopyMetrics(testOptions, recipientDoc, currOp);
        checkRecipientCopiedMetrics(testOptions, testRecipientMetrics, recipientDoc, currOp);
        checkRecipientOplogMetrics(testRecipientMetrics, recipientDoc, currOp);
    }

    void checkCoordinatorCopiedMetrics(const TestOptions& testOptions,
                                       const TestRecipientMetrics& testRecipientMetrics,
                                       const ReshardingCoordinatorDocument& coordinatorDoc,
                                       RecipientStateEnum recipientState) {
        if (recipientState < RecipientStateEnum::kApplying) {
            return;
        }

        bool checked = false;
        for (const auto& recipientShard : coordinatorDoc.getRecipientShards()) {
            if (recipientShard.getId() == recipientShardId) {
                auto mutableState = recipientShard.getMutableState();

                auto expectedDocsCopied = testRecipientMetrics.getMetricsTotal().docsCopied;
                auto expectedBytesCopied = testRecipientMetrics.getMetricsTotal().bytesCopied;

                // The metrics below are populated upon transitioning to the "strict-consistency"
                // state.
                if (recipientState >= RecipientStateEnum::kStrictConsistency) {
                    // There is currently no 'documentsCopied'.
                    ASSERT_EQ(*mutableState.getBytesCopied(), expectedBytesCopied);
                } else {
                    ASSERT_FALSE(mutableState.getBytesCopied());
                }

                // If verification is enabled, the metrics below are populated upon transitioning to
                // the "applying" state after cloning completes and are updated upon transitioning
                // to the "strict-consistency" state. If verification is disabled, they are
                // populated upon transitioning to the "strict-consistency" state.
                if (testOptions.performVerification ||
                    recipientState >= RecipientStateEnum::kStrictConsistency) {

                    ASSERT_EQ(*mutableState.getTotalNumDocuments(), expectedDocsCopied);
                    // Upon transitioning to the "strict-consistency" state, 'totalDocumentSize' is
                    // set to the fast count size. This test deliberately leaves the temporary
                    // collection empty when testing verification so this is expected to be equal to
                    // 0.
                    bool expectInaccurateSize = testOptions.performVerification &&
                        recipientState >= RecipientStateEnum::kStrictConsistency;
                    ASSERT_EQ(*mutableState.getTotalDocumentSize(),
                              expectInaccurateSize ? 0 : expectedBytesCopied);
                } else {
                    ASSERT_FALSE(mutableState.getTotalNumDocuments());
                    ASSERT_FALSE(mutableState.getTotalDocumentSize());
                }
                checked = true;
                break;
            }
        }
        ASSERT(checked);
    }

    void checkCoordinatorOplogMetrics(const TestRecipientMetrics& testRecipientMetrics,
                                      const ReshardingCoordinatorDocument& coordinatorDoc,
                                      RecipientStateEnum recipientState) {
        if (recipientState < RecipientStateEnum::kStrictConsistency) {
            return;
        }

        auto expectedOplogFetched = testRecipientMetrics.getMetricsTotal().oplogFetched;
        auto expectedOplogApplied = testRecipientMetrics.getMetricsTotal().oplogApplied;

        bool checked = false;
        for (const auto& recipientShard : coordinatorDoc.getRecipientShards()) {
            if (recipientShard.getId() == recipientShardId) {
                auto mutableState = recipientShard.getMutableState();
                ASSERT_EQ(*mutableState.getOplogFetched(), expectedOplogFetched);
                ASSERT_EQ(*mutableState.getOplogApplied(), expectedOplogApplied);

                checked = true;
                break;
            }
        }
        ASSERT(checked);
    }

    void checkCoordinatorMetrics(const TestOptions& testOptions,
                                 const TestRecipientMetrics& testRecipientMetrics,
                                 const ReshardingCoordinatorDocument& coordinatorDoc,
                                 RecipientStateEnum recipientState) {
        checkCoordinatorCopiedMetrics(
            testOptions, testRecipientMetrics, coordinatorDoc, recipientState);
        checkCoordinatorOplogMetrics(testRecipientMetrics, coordinatorDoc, recipientState);
    }

private:
    TypeCollectionRecipientFields _makeRecipientFields(
        const ReshardingRecipientDocument& recipientDoc) {
        TypeCollectionRecipientFields recipientFields{
            recipientDoc.getDonorShards(),
            recipientDoc.getSourceUUID(),
            recipientDoc.getSourceNss(),
            recipientDoc.getMinimumOperationDurationMillis()};

        auto donorShards = recipientFields.getDonorShards();
        for (unsigned i = 0; i < donorShards.size(); ++i) {
            auto minFetchTimestamp = Timestamp{10 + i, i};
            donorShards[i].setMinFetchTimestamp(minFetchTimestamp);
            recipientFields.setCloneTimestamp(minFetchTimestamp);
        }
        recipientFields.setDonorShards(std::move(donorShards));

        ReshardingApproxCopySize approxCopySize;
        approxCopySize.setApproxBytesToCopy(approxBytesToCopy);
        approxCopySize.setApproxDocumentsToCopy(approxDocumentsToCopy);
        recipientFields.setReshardingApproxCopySizeStruct(std::move(approxCopySize));

        return recipientFields;
    }

    void _onReshardingFieldsChanges(OperationContext* opCtx,
                                    RecipientStateMachine& recipient,
                                    const ReshardingRecipientDocument& recipientDoc,
                                    CoordinatorStateEnum coordinatorState) {
        auto reshardingFields = TypeCollectionReshardingFields{recipientDoc.getReshardingUUID()};
        reshardingFields.setRecipientFields(_makeRecipientFields(recipientDoc));
        reshardingFields.setState(coordinatorState);
        bool noChunksToCopy = recipientDoc.getSkipCloningAndApplying().value_or(false) ||
            _noChunksToCopy.value_or(false);
        recipient.onReshardingFieldsChanges(opCtx, reshardingFields, noChunksToCopy);
    }

    std::shared_ptr<RecipientStateTransitionController> _controller;
    boost::optional<bool> _noChunksToCopy;
};

TEST_F(ReshardingRecipientServiceTest, CanTransitionThroughEachStateToCompletion) {
    for (const auto& testOptions : makeBasicTestOptions()) {
        LOGV2(5551105,
              "Running case",
              "test"_attr = _agent.getTestName(),
              "testOptions"_attr = testOptions);
        auto removeRecipientDocFailpoint =
            globalFailPointRegistry().find("removeRecipientDocFailpoint");
        auto timesEnteredFailPoint = removeRecipientDocFailpoint->setMode(FailPoint::alwaysOn);
        auto doc = makeRecipientDocument(testOptions);
        auto opCtx = makeOperationContext();
        RecipientStateMachine::insertStateDocument(opCtx.get(), doc);
        auto recipient = RecipientStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());

        notifyToStartCloning(opCtx.get(), *recipient, doc);

        notifyReshardingCommitting(opCtx.get(), *recipient, doc);

        removeRecipientDocFailpoint->waitForTimesEntered(timesEnteredFailPoint + 1);

        // Search metrics in the state document and verify they are valid and the same as the
        // ones in memory.
        auto persistedDoc = getPersistedRecipientDocument(opCtx.get(), doc.getReshardingUUID());

        Date_t copyBegin = recipient->getMetrics()
                               .getStartFor(ReshardingMetrics::TimedPhase::kCloning)
                               .value_or(Date_t::min());
        Date_t copyEnd = recipient->getMetrics()
                             .getEndFor(ReshardingMetrics::TimedPhase::kCloning)
                             .value_or(Date_t::min());
        Date_t buildIndexBegin = recipient->getMetrics()
                                     .getStartFor(ReshardingMetrics::TimedPhase::kBuildingIndex)
                                     .value_or(Date_t::min());
        Date_t buildIndexEnd = recipient->getMetrics()
                                   .getEndFor(ReshardingMetrics::TimedPhase::kBuildingIndex)
                                   .value_or(Date_t::min());
        Date_t applyBegin = recipient->getMetrics()
                                .getStartFor(ReshardingMetrics::TimedPhase::kApplying)
                                .value_or(Date_t::min());
        Date_t applyEnd = recipient->getMetrics()
                              .getEndFor(ReshardingMetrics::TimedPhase::kApplying)
                              .value_or(Date_t::min());

        auto copyBeginDoc = persistedDoc.getMetrics()->getDocumentCopy()->getStart();
        auto copyEndDoc = persistedDoc.getMetrics()->getDocumentCopy()->getStop();
        auto buildIndexBeginDoc = persistedDoc.getMetrics()->getIndexBuildTime()->getStart();
        auto buildIndexEndDoc = persistedDoc.getMetrics()->getIndexBuildTime()->getStop();
        auto applyBeginDoc = persistedDoc.getMetrics()->getOplogApplication()->getStart();
        auto applyEndDoc = persistedDoc.getMetrics()->getOplogApplication()->getStop();

        ASSERT_NE(copyBegin, Date_t::min());
        ASSERT_NE(copyEnd, Date_t::min());
        ASSERT_NE(buildIndexBegin, Date_t::min());
        ASSERT_NE(buildIndexEnd, Date_t::min());
        ASSERT_NE(applyBegin, Date_t::min());
        ASSERT_NE(applyEnd, Date_t::min());
        ASSERT_LTE(copyBegin, copyEnd);
        ASSERT_LTE(buildIndexBegin, buildIndexEnd);
        ASSERT_LTE(applyBegin, applyEnd);

        ASSERT_TRUE(copyBeginDoc.has_value());
        ASSERT_EQ(copyBegin, copyBeginDoc.get());

        ASSERT_TRUE(copyEndDoc.has_value());
        ASSERT_EQ(copyEnd, copyEndDoc.get());

        ASSERT_TRUE(buildIndexBeginDoc.has_value());
        ASSERT_EQ(buildIndexBegin, buildIndexBeginDoc.get());

        ASSERT_TRUE(buildIndexEndDoc.has_value());
        ASSERT_EQ(buildIndexEnd, buildIndexEndDoc.get());

        ASSERT_TRUE(applyBeginDoc.has_value());
        ASSERT_EQ(applyBegin, applyBeginDoc.get());

        ASSERT_TRUE(applyEndDoc.has_value());
        ASSERT_EQ(applyEnd, applyEndDoc.get());

        removeRecipientDocFailpoint->setMode(FailPoint::off);
        ASSERT_OK(recipient->getCompletionFuture().getNoThrow());
        checkRecipientDocumentRemoved(opCtx.get());
    }
}

TEST_F(ReshardingRecipientServiceTest, StepDownStepUpEachTransition) {
    const std::vector<RecipientStateEnum> recipientStates{RecipientStateEnum::kCreatingCollection,
                                                          RecipientStateEnum::kCloning,
                                                          RecipientStateEnum::kBuildingIndex,
                                                          RecipientStateEnum::kApplying,
                                                          RecipientStateEnum::kStrictConsistency,
                                                          RecipientStateEnum::kDone};
    for (const auto& testOptions : makeBasicTestOptions()) {
        LOGV2(5551106,
              "Running case",
              "test"_attr = _agent.getTestName(),
              "testOptions"_attr = testOptions);

        PauseDuringStateTransitions stateTransitionsGuard{controller(), recipientStates};
        auto doc = makeRecipientDocument(testOptions);
        auto instanceId =
            BSON(ReshardingRecipientDocument::kReshardingUUIDFieldName << doc.getReshardingUUID());
        auto opCtx = makeOperationContext();
        auto prevState = RecipientStateEnum::kUnused;

        for (const auto state : recipientStates) {

            auto recipient = [&] {
                if (prevState == RecipientStateEnum::kUnused) {
                    if (testOptions.isAlsoDonor) {
                        createSourceCollection(opCtx.get(), doc);
                    }

                    RecipientStateMachine::insertStateDocument(opCtx.get(), doc);
                    return RecipientStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());
                } else {
                    auto [maybeRecipient, isPausedOrShutdown] =
                        RecipientStateMachine::lookup(opCtx.get(), _service, instanceId);
                    ASSERT_TRUE(maybeRecipient);
                    ASSERT_FALSE(isPausedOrShutdown);

                    // Allow the transition to prevState to succeed on this primary-only service
                    // instance.
                    stateTransitionsGuard.unset(prevState);
                    return *maybeRecipient;
                }
            }();

            if (prevState != RecipientStateEnum::kUnused) {
                // Allow the transition to prevState to succeed on this primary-only service
                // instance.
                stateTransitionsGuard.unset(prevState);
            }

            // Signal the coordinator's earliest state that allows the recipient's transition
            // into 'state' to be valid. This mimics the real system where, upon step up, the
            // new RecipientStateMachine instance gets refreshed with the coordinator's most
            // recent state.
            switch (state) {
                case RecipientStateEnum::kCreatingCollection:
                case RecipientStateEnum::kCloning: {
                    notifyToStartCloning(opCtx.get(), *recipient, doc);
                    break;
                }
                case RecipientStateEnum::kDone: {
                    notifyReshardingCommitting(opCtx.get(), *recipient, doc);
                    break;
                }
                default:
                    break;
            }

            // Step down before the transition to state can complete.
            stateTransitionsGuard.wait(state);
            stepDown();

            ASSERT_EQ(recipient->getCompletionFuture().getNoThrow(), ErrorCodes::CallbackCanceled);

            prevState = state;

            recipient.reset();
            stepUp(opCtx.get());
        }

        // Finally complete the operation and ensure its success.
        auto [maybeRecipient, isPausedOrShutdown] =
            RecipientStateMachine::lookup(opCtx.get(), _service, instanceId);
        ASSERT_TRUE(maybeRecipient);
        ASSERT_FALSE(isPausedOrShutdown);

        auto recipient = *maybeRecipient;

        stateTransitionsGuard.unset(RecipientStateEnum::kDone);
        notifyReshardingCommitting(opCtx.get(), *recipient, doc);
        ASSERT_OK(recipient->getCompletionFuture().getNoThrow());
        checkRecipientDocumentRemoved(opCtx.get());
    }
}

TEST_F(ReshardingRecipientServiceTest, ReportForCurrentOpAfterCompletion) {
    for (const auto& testOptions : makeBasicTestOptions()) {
        LOGV2(9297801,
              "Running case",
              "test"_attr = _agent.getTestName(),
              "testOptions"_attr = testOptions);

        const auto recipientState = RecipientStateEnum::kCreatingCollection;

        PauseDuringStateTransitions stateTransitionsGuard{controller(), recipientState};
        auto doc = makeRecipientDocument(testOptions);
        auto instanceId =
            BSON(ReshardingRecipientDocument::kReshardingUUIDFieldName << doc.getReshardingUUID());
        auto opCtx = makeOperationContext();

        auto recipient = [&] {
            RecipientStateMachine::insertStateDocument(opCtx.get(), doc);
            return RecipientStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());
        }();

        notifyToStartCloning(opCtx.get(), *recipient, doc);

        // Step down before the transition to state can complete.
        stateTransitionsGuard.wait(recipientState);
        stepDown();
        stateTransitionsGuard.unset(recipientState);

        // At this point, the resharding metrics will have been unregistered from the cumulative
        // metrics
        ASSERT_EQ(recipient->getCompletionFuture().getNoThrow(), ErrorCodes::CallbackCanceled);

        // Now call step up. The old recipient object has not yet been destroyed because we
        // still hold a shared pointer to it ('recipient') - this can happen in production after
        // a failover if a state machine is slow to clean up.
        stepUp(opCtx.get());

        // Assert that the old recipient object will return a currentOp report, because the
        // resharding metrics still exist on the coordinator object itelf.
        ASSERT(recipient->reportForCurrentOp(
            MongoProcessInterface::CurrentOpConnectionsMode::kExcludeIdle,
            MongoProcessInterface::CurrentOpSessionsMode::kIncludeIdle));

        // Ensure the new recipient started up successfully (and thus, registered new resharding
        // metrics), despite the "zombie" state machine still existing.
        auto [maybeRecipient, isPausedOrShutdown] =
            RecipientStateMachine::lookup(opCtx.get(), _service, instanceId);
        ASSERT_TRUE(maybeRecipient);
        ASSERT_FALSE(isPausedOrShutdown);
        auto newRecipient = *maybeRecipient;
        ASSERT_NE(recipient, newRecipient);

        // No need to finish the resharding op, so we just cancel the op.
        newRecipient->abort(false);
        ASSERT_OK(newRecipient->getCompletionFuture().getNoThrow());
    }
}

TEST_F(ReshardingRecipientServiceTest, OpCtxKilledWhileRestoringMetrics) {
    for (const auto& testOptions : makeBasicTestOptions()) {
        LOGV2(5992701,
              "Running case",
              "test"_attr = _agent.getTestName(),
              "testOptions"_attr = testOptions);

        // Initialize recipient.
        auto doc = makeRecipientDocument(testOptions);
        auto instanceId =
            BSON(ReshardingRecipientDocument::kReshardingUUIDFieldName << doc.getReshardingUUID());
        auto opCtx = makeOperationContext();
        if (testOptions.isAlsoDonor) {
            createSourceCollection(opCtx.get(), doc);
        }
        RecipientStateMachine::insertStateDocument(opCtx.get(), doc);
        auto recipient = RecipientStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());

        // In order to restore metrics, metrics need to exist in the first place, so put the
        // recipient in the cloning state, then step down.
        PauseDuringStateTransitions stateTransitionsGuard{controller(),
                                                          RecipientStateEnum::kCloning};
        notifyToStartCloning(opCtx.get(), *recipient, doc);
        stateTransitionsGuard.wait(RecipientStateEnum::kCloning);
        stepDown();
        stateTransitionsGuard.unset(RecipientStateEnum::kCloning);
        recipient.reset();

        // Enable failpoint and step up.
        auto fp = globalFailPointRegistry().find("reshardingOpCtxKilledWhileRestoringMetrics");
        fp->setMode(FailPoint::nTimes, 1);
        stepUp(opCtx.get());

        // After the failpoint is disabled, the operation should succeed.
        auto [maybeRecipient, isPausedOrShutdown] =
            RecipientStateMachine::lookup(opCtx.get(), _service, instanceId);
        ASSERT_TRUE(maybeRecipient);
        ASSERT_FALSE(isPausedOrShutdown);
        recipient = *maybeRecipient;
        notifyReshardingCommitting(opCtx.get(), *recipient, doc);
        ASSERT_OK(recipient->getCompletionFuture().getNoThrow());
        checkRecipientDocumentRemoved(opCtx.get());
    }
}

DEATH_TEST_REGEX_F(ReshardingRecipientServiceTest, CommitFn, "4457001.*tripwire") {
    for (const auto& testOptions : makeBasicTestOptions()) {
        LOGV2(9297802,
              "Running case",
              "test"_attr = _agent.getTestName(),
              "testOptions"_attr = testOptions);
        auto doc = makeRecipientDocument(testOptions);
        auto opCtx = makeOperationContext();
        RecipientStateMachine::insertStateDocument(opCtx.get(), doc);
        auto recipient = RecipientStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());

        ASSERT_THROWS_CODE(
            recipient->commit(), DBException, ErrorCodes::ReshardCollectionInProgress);

        notifyToStartCloning(opCtx.get(), *recipient, doc);
        recipient->awaitInStrictConsistencyOrError().get();
        recipient->commit();

        ASSERT_OK(recipient->getCompletionFuture().getNoThrow());
    }
}

TEST_F(ReshardingRecipientServiceTest, DropsTemporaryReshardingCollectionOnAbort) {
    for (const auto& testOptions : makeBasicTestOptions()) {
        LOGV2(5551107,
              "Running case",
              "test"_attr = _agent.getTestName(),
              "testOptions"_attr = testOptions);

        boost::optional<PauseDuringStateTransitions> doneTransitionGuard;
        doneTransitionGuard.emplace(controller(), RecipientStateEnum::kDone);

        auto doc = makeRecipientDocument(testOptions);
        auto instanceId =
            BSON(ReshardingRecipientDocument::kReshardingUUIDFieldName << doc.getReshardingUUID());

        auto opCtx = makeOperationContext();

        if (testOptions.isAlsoDonor) {
            // If the recipient is also a donor, the original collection should already exist on
            // this shard.
            createSourceCollection(opCtx.get(), doc);
        }

        RecipientStateMachine::insertStateDocument(opCtx.get(), doc);
        auto recipient = RecipientStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());

        notifyToStartCloning(opCtx.get(), *recipient, doc);
        recipient->abort(false);

        doneTransitionGuard->wait(RecipientStateEnum::kDone);
        stepDown();

        ASSERT_EQ(recipient->getCompletionFuture().getNoThrow(), ErrorCodes::CallbackCanceled);

        recipient.reset();
        stepUp(opCtx.get());

        auto [maybeRecipient, isPausedOrShutdown] =
            RecipientStateMachine::lookup(opCtx.get(), _service, instanceId);
        ASSERT_TRUE(maybeRecipient);
        ASSERT_FALSE(isPausedOrShutdown);
        recipient = *maybeRecipient;

        doneTransitionGuard.reset();
        recipient->abort(false);

        ASSERT_OK(recipient->getCompletionFuture().getNoThrow());
        checkRecipientDocumentRemoved(opCtx.get());

        if (testOptions.isAlsoDonor) {
            // Verify original collection still exists after aborting.
            AutoGetCollection coll(opCtx.get(), doc.getSourceNss(), MODE_IS);
            ASSERT_TRUE(bool(coll));
            ASSERT_EQ(coll->uuid(), doc.getSourceUUID());
        }

        // Verify the temporary collection no longer exists.
        {
            AutoGetCollection coll(opCtx.get(), doc.getTempReshardingNss(), MODE_IS);
            ASSERT_FALSE(bool(coll));
        }
    }
}

TEST_F(ReshardingRecipientServiceTest, RenamesTemporaryReshardingCollectionWhenDone) {
    // The temporary collection is renamed by the donor service when the shard is also a donor. Only
    // on non-donor shards will the recipient service rename the temporary collection.
    bool isAlsoDonor = false;

    for (bool skipCloningAndApplying : {false, true}) {
        LOGV2(9297803,
              "Running case",
              "test"_attr = _agent.getTestName(),
              "skipCloningAndApplying"_attr = skipCloningAndApplying);
        boost::optional<PauseDuringStateTransitions> stateTransitionsGuard;
        stateTransitionsGuard.emplace(controller(), RecipientStateEnum::kApplying);

        auto doc = makeRecipientDocument({isAlsoDonor, skipCloningAndApplying});
        auto opCtx = makeOperationContext();
        RecipientStateMachine::insertStateDocument(opCtx.get(), doc);
        auto recipient = RecipientStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());

        notifyToStartCloning(opCtx.get(), *recipient, doc);

        // Wait to check the temporary collection has been created.
        stateTransitionsGuard->wait(RecipientStateEnum::kApplying);
        {
            // Check the temporary collection exists but is not yet renamed.
            AutoGetCollection coll(opCtx.get(), doc.getTempReshardingNss(), MODE_IS);
            ASSERT_TRUE(bool(coll));
            ASSERT_EQ(coll->uuid(), doc.getReshardingUUID());
        }
        stateTransitionsGuard.reset();

        notifyReshardingCommitting(opCtx.get(), *recipient, doc);

        ASSERT_OK(recipient->getCompletionFuture().getNoThrow());
        checkRecipientDocumentRemoved(opCtx.get());

        {
            // Ensure the temporary collection was renamed.
            AutoGetCollection coll(opCtx.get(), doc.getSourceNss(), MODE_IS);
            ASSERT_TRUE(bool(coll));
            ASSERT_EQ(coll->uuid(), doc.getReshardingUUID());
        }
    }
}

TEST_F(ReshardingRecipientServiceTest, WritesNoopOplogEntryOnReshardDoneCatchUp) {
    for (const auto& testOptions : makeBasicTestOptions()) {
        LOGV2(9297804,
              "Running case",
              "test"_attr = _agent.getTestName(),
              "testOptions"_attr = testOptions);

        boost::optional<PauseDuringStateTransitions> doneTransitionGuard;
        doneTransitionGuard.emplace(controller(), RecipientStateEnum::kDone);

        auto doc = makeRecipientDocument(testOptions);
        auto opCtx = makeOperationContext();
        auto rawOpCtx = opCtx.get();
        RecipientStateMachine::insertStateDocument(rawOpCtx, doc);
        auto recipient = RecipientStateMachine::getOrCreate(rawOpCtx, _service, doc.toBSON());

        notifyToStartCloning(rawOpCtx, *recipient, doc);
        notifyReshardingCommitting(opCtx.get(), *recipient, doc);

        doneTransitionGuard->wait(RecipientStateEnum::kDone);

        stepDown();
        doneTransitionGuard.reset();
        ASSERT_EQ(recipient->getCompletionFuture().getNoThrow(), ErrorCodes::CallbackCanceled);

        DBDirectClient client(opCtx.get());
        NamespaceString sourceNss =
            resharding::constructTemporaryReshardingNss(doc.getSourceNss(), doc.getSourceUUID());

        FindCommandRequest findRequest{NamespaceString::kRsOplogNamespace};
        findRequest.setFilter(BSON("ns" << sourceNss.toString_forTest() << "o2.reshardDoneCatchUp"
                                        << BSON("$exists" << true)));
        auto cursor = client.find(std::move(findRequest));

        ASSERT_TRUE(cursor->more()) << "Found no oplog entries for source collection";
        repl::OplogEntry op(cursor->next());
        ASSERT_FALSE(cursor->more())
            << "Found multiple oplog entries for source collection: " << op.getEntry() << " and "
            << cursor->nextSafe();

        ReshardDoneCatchUpChangeEventO2Field expectedChangeEvent{sourceNss,
                                                                 doc.getReshardingUUID()};
        auto receivedChangeEvent = ReshardDoneCatchUpChangeEventO2Field::parse(
            IDLParserContext("ReshardDoneCatchUpChangeEventO2Field"), *op.getObject2());

        ASSERT_EQ(OpType_serializer(op.getOpType()), OpType_serializer(repl::OpTypeEnum::kNoop))
            << op.getEntry();
        ASSERT_EQ(*op.getUuid(), doc.getReshardingUUID()) << op.getEntry();
        ASSERT_EQ(op.getObject()["msg"].type(), BSONType::String) << op.getEntry();
        ASSERT_TRUE(receivedChangeEvent == expectedChangeEvent);
        ASSERT_TRUE(op.getFromMigrate());
        ASSERT_FALSE(bool(op.getDestinedRecipient())) << op.getEntry();

        stepUp(opCtx.get());
    }
}

TEST_F(ReshardingRecipientServiceTest, WritesNoopOplogEntryForImplicitShardCollection) {
    for (const auto& testOptions : makeBasicTestOptions()) {
        LOGV2(9297805,
              "Running case",
              "test"_attr = _agent.getTestName(),
              "testOptions"_attr = testOptions);
        boost::optional<PauseDuringStateTransitions> doneTransitionGuard;
        doneTransitionGuard.emplace(controller(), RecipientStateEnum::kDone);

        auto doc = makeRecipientDocument(testOptions);
        auto opCtx = makeOperationContext();
        auto rawOpCtx = opCtx.get();
        RecipientStateMachine::insertStateDocument(rawOpCtx, doc);
        auto recipient = RecipientStateMachine::getOrCreate(rawOpCtx, _service, doc.toBSON());

        notifyToStartCloning(rawOpCtx, *recipient, doc);
        notifyReshardingCommitting(opCtx.get(), *recipient, doc);

        doneTransitionGuard->wait(RecipientStateEnum::kDone);

        stepDown();
        doneTransitionGuard.reset();
        ASSERT_EQ(recipient->getCompletionFuture().getNoThrow(), ErrorCodes::CallbackCanceled);

        DBDirectClient client(opCtx.get());
        NamespaceString sourceNss =
            resharding::constructTemporaryReshardingNss(doc.getSourceNss(), doc.getSourceUUID());

        FindCommandRequest findRequest{NamespaceString::kRsOplogNamespace};
        findRequest.setFilter(BSON("ns" << sourceNss.toString_forTest() << "o2.shardCollection"
                                        << BSON("$exists" << true)));
        auto cursor = client.find(std::move(findRequest));

        ASSERT_TRUE(cursor->more()) << "Found no oplog entries for source collection";
        repl::OplogEntry shardCollectionOp(cursor->next());

        ASSERT_EQ(OpType_serializer(shardCollectionOp.getOpType()),
                  OpType_serializer(repl::OpTypeEnum::kNoop))
            << shardCollectionOp.getEntry();
        ASSERT_EQ(*shardCollectionOp.getUuid(), doc.getReshardingUUID())
            << shardCollectionOp.getEntry();
        ASSERT_EQ(shardCollectionOp.getObject()["msg"].type(), BSONType::Object)
            << shardCollectionOp.getEntry();
        ASSERT_FALSE(shardCollectionOp.getFromMigrate());

        auto shardCollEventExpected = BSON("shardCollection" << sourceNss.toString_forTest()
                                                             << "shardKey" << newShardKeyPattern());
        ASSERT_BSONOBJ_EQ(*shardCollectionOp.getObject2(), shardCollEventExpected);

        stepUp(opCtx.get());
    }
}

TEST_F(ReshardingRecipientServiceTest, TruncatesXLErrorOnRecipientDocument) {
    for (const auto& testOptions : makeBasicTestOptions()) {
        LOGV2(5568600,
              "Running case",
              "test"_attr = _agent.getTestName(),
              "testOptions"_attr = testOptions);

        std::string xlErrMsg(6000, 'x');
        FailPointEnableBlock failpoint("reshardingRecipientFailsAfterTransitionToCloning",
                                       BSON("errmsg" << xlErrMsg));

        auto doc = makeRecipientDocument(testOptions);
        auto opCtx = makeOperationContext();
        RecipientStateMachine::insertStateDocument(opCtx.get(), doc);
        auto recipient = RecipientStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());

        notifyToStartCloning(opCtx.get(), *recipient, doc);

        auto localTransitionToErrorFuture = recipient->awaitInStrictConsistencyOrError();
        ASSERT_OK(localTransitionToErrorFuture.getNoThrow());

        // The recipient still waits for the abort decision from the coordinator despite it
        // having errored locally. It is therefore safe to check its local state document until
        // RecipientStateMachine::abort() is called.
        {
            auto persistedDoc = getPersistedRecipientDocument(opCtx.get(), doc.getReshardingUUID());
            auto abortReason = persistedDoc.getMutableState().getAbortReason();
            ASSERT(abortReason);
            // The actual abortReason will be slightly larger than kReshardErrorMaxBytes bytes
            // due to the primitive truncation algorithm - Check that the total size is less
            // than kReshardErrorMaxBytes + a couple additional bytes to provide a buffer for
            // the field name sizes.
            int maxReshardErrorBytesCeiling = resharding::kReshardErrorMaxBytes + 200;
            ASSERT_LT(abortReason->objsize(), maxReshardErrorBytesCeiling);
            ASSERT_EQ(abortReason->getIntField("code"),
                      ErrorCodes::ReshardCollectionTruncatedError);
        }

        recipient->abort(false);
        ASSERT_OK(recipient->getCompletionFuture().getNoThrow());
    }
}

TEST_F(ReshardingRecipientServiceTest, SkipCloningAndApplying) {
    // Set the failpoint to force the cloners and fetchers to fail to initialize so that the
    // recipient would fail if it does not skip cloning and applying.
    FailPointEnableBlock fp("failToCreateReshardingDataReplicationForTest");

    for (const auto& testOptions : makeBasicTestOptions()) {
        LOGV2(9110903,
              "Running case",
              "test"_attr = _agent.getTestName(),
              "testOptions"_attr = testOptions);
        auto doc = makeRecipientDocument(testOptions);
        auto opCtx = makeOperationContext();
        RecipientStateMachine::insertStateDocument(opCtx.get(), doc);
        auto recipient = RecipientStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());

        notifyToStartCloning(opCtx.get(), *recipient, doc);
        if (!testOptions.skipCloningAndApplying) {
            ASSERT_OK(recipient->awaitInStrictConsistencyOrError().getNoThrow());

            auto persistedDoc = getPersistedRecipientDocument(opCtx.get(), doc.getReshardingUUID());
            auto abortReason = persistedDoc.getMutableState().getAbortReason();
            ASSERT(abortReason);
            ASSERT_EQ(abortReason->getIntField("code"), ErrorCodes::InternalError);
            continue;
        }

        notifyReshardingCommitting(opCtx.get(), *recipient, doc);
        ASSERT_OK(recipient->getCompletionFuture().getNoThrow());
    }
}

TEST_F(ReshardingRecipientServiceTest, MetricsSuccessfullyShutDownOnUserCancelation) {
    for (const auto& testOptions : makeBasicTestOptions()) {
        LOGV2(9297806,
              "Running case",
              "test"_attr = _agent.getTestName(),
              "testOptions"_attr = testOptions);
        auto doc = makeRecipientDocument(testOptions);
        auto opCtx = makeOperationContext();
        RecipientStateMachine::insertStateDocument(opCtx.get(), doc);
        auto recipient = RecipientStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());

        notifyToStartCloning(opCtx.get(), *recipient, doc);

        auto localTransitionToErrorFuture = recipient->awaitInStrictConsistencyOrError();
        ASSERT_OK(localTransitionToErrorFuture.getNoThrow());

        recipient->abort(true);
        ASSERT_OK(recipient->getCompletionFuture().getNoThrow());
    }
}

TEST_F(ReshardingRecipientServiceTest, ReshardingMetricsBasic) {
    const std::vector<RecipientStateEnum> recipientStates{RecipientStateEnum::kCreatingCollection,
                                                          RecipientStateEnum::kCloning,
                                                          RecipientStateEnum::kBuildingIndex,
                                                          RecipientStateEnum::kApplying,
                                                          RecipientStateEnum::kStrictConsistency,
                                                          RecipientStateEnum::kDone};

    for (auto& testOptions : makeAllTestOptions()) {
        LOGV2(9297807,
              "Running case",
              "test"_attr = _agent.getTestName(),
              "testOptions"_attr = testOptions);
        setNoChunksToCopy(testOptions);

        PauseDuringStateTransitions stateTransitionsGuard{controller(), recipientStates};
        auto opCtx = makeOperationContext();

        auto recipientDoc = makeRecipientDocument(testOptions);
        auto instanceId = BSON(ReshardingRecipientDocument::kReshardingUUIDFieldName
                               << recipientDoc.getReshardingUUID());
        auto prevState = RecipientStateEnum::kUnused;

        auto coordinatorDoc = makeCoordinatorDocument(recipientDoc);
        insertDocuments(opCtx.get(),
                        NamespaceString::kConfigReshardingOperationsNamespace,
                        {coordinatorDoc.toBSON()});

        auto testRecipientMetrics =
            makeTestRecipientMetrics(testOptions, recipientDoc.getDonorShards());

        auto checkPersistedState = [&]() {
            auto persistedRecipientDoc =
                getPersistedRecipientDocument(opCtx.get(), recipientDoc.getReshardingUUID());
            auto persistedCoordinatorDoc =
                getPersistedCoordinatorDocument(opCtx.get(), recipientDoc.getReshardingUUID());
            auto persistedRecipientState = persistedRecipientDoc.getMutableState().getState();

            checkRecipientMetrics(
                testOptions, testRecipientMetrics, persistedRecipientDoc, boost::none /* currOp */);
            checkCoordinatorMetrics(testOptions,
                                    testRecipientMetrics,
                                    persistedCoordinatorDoc,
                                    persistedRecipientState);

            if (persistedRecipientState >= RecipientStateEnum::kCloning) {
                ASSERT(persistedRecipientDoc.getCloneTimestamp());
            }
        };

        auto removeRecipientDocFailpoint =
            globalFailPointRegistry().find("removeRecipientDocFailpoint");
        auto timesEnteredFailPoint = removeRecipientDocFailpoint->setMode(FailPoint::alwaysOn);

        for (const auto state : recipientStates) {
            auto recipient = [&] {
                if (prevState == RecipientStateEnum::kUnused) {
                    RecipientStateMachine::insertStateDocument(opCtx.get(), recipientDoc);
                    return RecipientStateMachine::getOrCreate(
                        opCtx.get(), _service, recipientDoc.toBSON());
                } else {
                    auto [maybeRecipient, isPausedOrShutdown] =
                        RecipientStateMachine::lookup(opCtx.get(), _service, instanceId);
                    ASSERT_TRUE(maybeRecipient);
                    ASSERT_FALSE(isPausedOrShutdown);

                    // Allow the transition to prevState to succeed on this primary-only service
                    // instance.
                    stateTransitionsGuard.unset(prevState);
                    return *maybeRecipient;
                }
            }();

            mockDataReplicationStateIfNeeded(opCtx.get(),
                                             testOptions,
                                             testRecipientMetrics,
                                             recipientDoc.getCommonReshardingMetadata(),
                                             prevState,
                                             recipient->getMetricsForTest());

            if (prevState != RecipientStateEnum::kUnused) {
                // Allow the transition to prevState to succeed on this primary-only service
                // instance.
                stateTransitionsGuard.unset(prevState);
            }

            // Signal the coordinator's earliest state that allows the recipient's transition into
            // 'state' to be valid.
            switch (state) {
                case RecipientStateEnum::kCreatingCollection:
                case RecipientStateEnum::kCloning: {
                    notifyToStartCloning(opCtx.get(), *recipient, recipientDoc);
                    break;
                }
                case RecipientStateEnum::kDone: {
                    notifyReshardingCommitting(opCtx.get(), *recipient, recipientDoc);
                    break;
                }
                default:
                    break;
            }

            stateTransitionsGuard.wait(state);
            checkPersistedState();

            prevState = state;
        }

        auto [maybeRecipient, isPausedOrShutdown] =
            RecipientStateMachine::lookup(opCtx.get(), _service, instanceId);
        ASSERT_TRUE(maybeRecipient);
        ASSERT_FALSE(isPausedOrShutdown);
        auto recipient = *maybeRecipient;

        stateTransitionsGuard.unset(RecipientStateEnum::kDone);
        notifyReshardingCommitting(opCtx.get(), *recipient, recipientDoc);

        removeRecipientDocFailpoint->waitForTimesEntered(timesEnteredFailPoint + 1);
        checkPersistedState();

        removeRecipientDocFailpoint->setMode(FailPoint::off);
        ASSERT_OK(recipient->getCompletionFuture().getNoThrow());
        checkRecipientDocumentRemoved(opCtx.get());
    }
}

TEST_F(ReshardingRecipientServiceTest, RestoreMetricsAfterStepUp) {
    const std::vector<RecipientStateEnum> recipientStates{RecipientStateEnum::kCreatingCollection,
                                                          RecipientStateEnum::kCloning,
                                                          RecipientStateEnum::kBuildingIndex,
                                                          RecipientStateEnum::kApplying,
                                                          RecipientStateEnum::kStrictConsistency,
                                                          RecipientStateEnum::kDone};

    for (const auto& testOptions : makeAllTestOptions()) {
        LOGV2(9297808,
              "Running case",
              "test"_attr = _agent.getTestName(),
              "testOptions"_attr = testOptions);

        setNoChunksToCopy(testOptions);

        PauseDuringStateTransitions stateTransitionsGuard{controller(), recipientStates};
        auto opCtx = makeOperationContext();

        auto doc = makeRecipientDocument(testOptions);
        auto instanceId =
            BSON(ReshardingRecipientDocument::kReshardingUUIDFieldName << doc.getReshardingUUID());
        auto prevState = RecipientStateEnum::kUnused;

        auto testRecipientMetrics = makeTestRecipientMetrics(testOptions, doc.getDonorShards());

        for (const auto state : recipientStates) {
            auto recipient = [&] {
                if (prevState == RecipientStateEnum::kUnused) {
                    RecipientStateMachine::insertStateDocument(opCtx.get(), doc);
                    return RecipientStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());
                } else {
                    auto [maybeRecipient, isPausedOrShutdown] =
                        RecipientStateMachine::lookup(opCtx.get(), _service, instanceId);
                    ASSERT_TRUE(maybeRecipient);
                    ASSERT_FALSE(isPausedOrShutdown);

                    // Allow the transition to prevState to succeed on this primary-only service
                    // instance.
                    stateTransitionsGuard.unset(prevState);
                    return *maybeRecipient;
                }
            }();

            mockDataReplicationStateIfNeeded(opCtx.get(),
                                             testOptions,
                                             testRecipientMetrics,
                                             doc.getCommonReshardingMetadata(),
                                             prevState,
                                             recipient->getMetricsForTest());

            if (prevState != RecipientStateEnum::kUnused) {
                // Allow the transition to prevState to succeed on this primary-only
                // service instance.
                stateTransitionsGuard.unset(prevState);
            }

            // Signal the coordinator's earliest state that allows the recipient's transition into
            // 'state' to be valid. This mimics the real system where, upon step up, the new
            // RecipientStateMachine instance gets refreshed with the coordinator's most recent
            // state.
            switch (state) {
                case RecipientStateEnum::kCreatingCollection:
                case RecipientStateEnum::kCloning: {
                    notifyToStartCloning(opCtx.get(), *recipient, doc);
                    break;
                }
                case RecipientStateEnum::kDone: {
                    notifyReshardingCommitting(opCtx.get(), *recipient, doc);
                    break;
                }
                default:
                    break;
            }
            // Step down before the transition to state can complete.
            stateTransitionsGuard.wait(state);

            auto persistedDoc = getPersistedRecipientDocument(opCtx.get(), doc.getReshardingUUID());
            auto currOp = recipient
                              ->reportForCurrentOp(
                                  MongoProcessInterface::CurrentOpConnectionsMode::kExcludeIdle,
                                  MongoProcessInterface::CurrentOpSessionsMode::kExcludeIdle)
                              .value();

            ASSERT_EQ(currOp.getStringField("recipientState"),
                      RecipientState_serializer(persistedDoc.getMutableState().getState()));
            checkRecipientMetrics(testOptions, testRecipientMetrics, persistedDoc, currOp);

            stepDown();

            ASSERT_EQ(recipient->getCompletionFuture().getNoThrow(), ErrorCodes::CallbackCanceled);

            prevState = state;
            if (state == RecipientStateEnum::kApplying ||
                state == RecipientStateEnum::kStrictConsistency) {
                // If metrics are being verified in the next pass, ensure a retry does not alter
                // values.
                auto fp =
                    globalFailPointRegistry().find("reshardingOpCtxKilledWhileRestoringMetrics");
                fp->setMode(FailPoint::nTimes, 1);
            }

            recipient.reset();
            if (state != RecipientStateEnum::kDone) {
                stepUp(opCtx.get());
            }
        }

        stepUp(opCtx.get());
    }
}

TEST_F(ReshardingRecipientServiceTest, RestoreMetricsAfterStepUpWithMissingProgressDoc) {
    for (const auto& testOptions : makeAllTestOptions()) {
        LOGV2(9297809,
              "Running case",
              "test"_attr = _agent.getTestName(),
              "testOptions"_attr = testOptions);
        auto doc = makeRecipientDocument(testOptions);
        auto instanceId =
            BSON(ReshardingRecipientDocument::kReshardingUUIDFieldName << doc.getReshardingUUID());
        auto opCtx = makeOperationContext();

        auto donorShards = doc.getDonorShards();
        for (unsigned i = 0; i < donorShards.size(); i++) {
            if (i == 0) {
                continue;
            }

            RecipientMetricsForDonor metricsForDonor;
            metricsForDonor.shardId = donorShards[i].getShardId();
            metricsForDonor.metrics.oplogFetched = i;
            metricsForDonor.metrics.oplogApplied = metricsForDonor.metrics.oplogFetched;

            mockOplogFetcherAndApplierStateForDonor(opCtx.get(),
                                                    testOptions,
                                                    std::move(metricsForDonor),
                                                    doc.getCommonReshardingMetadata());
        }

        auto mutableState = doc.getMutableState();
        mutableState.setState(RecipientStateEnum::kApplying);
        doc.setMutableState(mutableState);
        doc.setCloneTimestamp(Timestamp{10, 0});
        doc.setStartConfigTxnCloneTime(Date_t::now());

        auto metadata = doc.getCommonReshardingMetadata();
        metadata.setStartTime(Date_t::now());
        doc.setCommonReshardingMetadata(metadata);

        createTempReshardingCollection(opCtx.get(), doc);

        RecipientStateMachine::insertStateDocument(opCtx.get(), doc);
        auto recipient = RecipientStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());
        notifyReshardingCommitting(opCtx.get(), *recipient, doc);
        ASSERT_OK(recipient->getCompletionFuture().getNoThrow());
    }
}

TEST_F(ReshardingRecipientServiceTest, AbortAfterStepUpWithAbortReasonFromCoordinator) {
    repl::primaryOnlyServiceTestStepUpWaitForRebuildComplete.setMode(FailPoint::alwaysOn);
    const auto abortErrMsg = "Recieved abort from the resharding coordinator";

    for (const auto& testOptions : makeBasicTestOptions()) {
        LOGV2(8743301,
              "Running case",
              "test"_attr = _agent.getTestName(),
              "testOptions"_attr = testOptions);

        auto removeRecipientDocFailpoint =
            globalFailPointRegistry().find("removeRecipientDocFailpoint");
        auto timesEnteredFailPoint = removeRecipientDocFailpoint->setMode(FailPoint::alwaysOn);

        auto doc = makeRecipientDocument(testOptions);
        auto instanceId =
            BSON(ReshardingRecipientDocument::kReshardingUUIDFieldName << doc.getReshardingUUID());

        auto opCtx = makeOperationContext();
        if (testOptions.isAlsoDonor) {
            createSourceCollection(opCtx.get(), doc);
        }

        RecipientStateMachine::insertStateDocument(opCtx.get(), doc);
        auto recipient = RecipientStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());

        recipient->abort(false);
        removeRecipientDocFailpoint->waitForTimesEntered(timesEnteredFailPoint + 1);

        // Ensure the node is aborting with abortReason from coordinator.
        {
            auto persistedDoc = getPersistedRecipientDocument(opCtx.get(), doc.getReshardingUUID());
            auto state = persistedDoc.getMutableState().getState();
            ASSERT_EQ(state, RecipientStateEnum::kDone);

            auto abortReason = persistedDoc.getMutableState().getAbortReason();
            ASSERT(abortReason);
            ASSERT_EQ(abortReason->getIntField("code"), ErrorCodes::ReshardCollectionAborted);
            ASSERT_EQ(abortReason->getStringField("errmsg").toString(), abortErrMsg);
        }

        stepDown();
        ASSERT_EQ(recipient->getCompletionFuture().getNoThrow(), ErrorCodes::CallbackCanceled);
        recipient.reset();

        stepUp(opCtx.get());
        removeRecipientDocFailpoint->waitForTimesEntered(timesEnteredFailPoint + 2);

        auto [maybeRecipient, isPausedOrShutdown] =
            RecipientStateMachine::lookup(opCtx.get(), _service, instanceId);
        ASSERT_TRUE(maybeRecipient);
        ASSERT_FALSE(isPausedOrShutdown);
        recipient = *maybeRecipient;

        removeRecipientDocFailpoint->setMode(FailPoint::off);
        ASSERT_OK(recipient->getCompletionFuture().getNoThrow());
        checkRecipientDocumentRemoved(opCtx.get());
    }
}

TEST_F(ReshardingRecipientServiceTest, FailoverDuringErrorState) {
    for (const auto& testOptions : makeBasicTestOptions()) {
        LOGV2(8916100,
              "Running case",
              "test"_attr = _agent.getTestName(),
              "testOptions"_attr = testOptions);

        std::string errMsg("Simulating an unrecoverable error for testing");
        FailPointEnableBlock failpoint("reshardingRecipientFailsAfterTransitionToCloning",
                                       BSON("errmsg" << errMsg));

        auto doc = makeRecipientDocument(testOptions);
        auto instanceId =
            BSON(ReshardingRecipientDocument::kReshardingUUIDFieldName << doc.getReshardingUUID());

        auto opCtx = makeOperationContext();
        RecipientStateMachine::insertStateDocument(opCtx.get(), doc);
        auto recipient = RecipientStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());

        notifyToStartCloning(opCtx.get(), *recipient, doc);

        auto localTransitionToErrorFuture = recipient->awaitInStrictConsistencyOrError();
        ASSERT_OK(localTransitionToErrorFuture.getNoThrow());

        stepDown();
        ASSERT_EQ(recipient->getCompletionFuture().getNoThrow(), ErrorCodes::CallbackCanceled);
        recipient.reset();

        stepUp(opCtx.get());

        auto [maybeRecipient, isPausedOrShutdown] =
            RecipientStateMachine::lookup(opCtx.get(), _service, instanceId);
        ASSERT_TRUE(maybeRecipient);
        ASSERT_FALSE(isPausedOrShutdown);
        recipient = *maybeRecipient;

        {
            auto persistedDoc = getPersistedRecipientDocument(opCtx.get(), doc.getReshardingUUID());
            auto state = persistedDoc.getMutableState().getState();
            ASSERT_EQ(state, RecipientStateEnum::kError);
            ASSERT(persistedDoc.getMutableState().getAbortReason());
        }

        recipient->abort(false);
        ASSERT_OK(recipient->getCompletionFuture().getNoThrow());
    }
}

// The collection options for the source and temporary collections are both defaulted to BSONObj(),
// so this test will pass since both collection options are equal.
TEST_F(ReshardingRecipientServiceTest, TestVerifyCollectionOptionsHappyPath) {
    RAIIServerParameterControllerForTest featureFlagController("featureFlagReshardingVerification",
                                                               true);
    for (const auto& testOptions : makeBasicTestOptions()) {
        LOGV2(9799201,
              "Running case",
              "test"_attr = _agent.getTestName(),
              "testOptions"_attr = testOptions);

        auto doc = makeRecipientDocument(testOptions);
        auto instanceId =
            BSON(ReshardingRecipientDocument::kReshardingUUIDFieldName << doc.getReshardingUUID());

        auto opCtx = makeOperationContext();
        RecipientStateMachine::insertStateDocument(opCtx.get(), doc);
        auto recipient = RecipientStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());

        notifyToStartCloning(opCtx.get(), *recipient, doc);

        notifyReshardingCommitting(opCtx.get(), *recipient, doc);

        ASSERT_OK(recipient->getCompletionFuture().getNoThrow());
    }
}

TEST_F(ReshardingRecipientServiceTest,
       TestVerifyCollectionOptionsThrowsExceptionOnMismatchedOptions) {
    RAIIServerParameterControllerForTest featureFlagController("featureFlagReshardingVerification",
                                                               true);
    for (const auto& testOptions : makeBasicTestOptions()) {
        LOGV2(9799202,
              "Running case",
              "test"_attr = _agent.getTestName(),
              "testOptions"_attr = testOptions);

        auto doc = makeRecipientDocument(testOptions);
        auto instanceId =
            BSON(ReshardingRecipientDocument::kReshardingUUIDFieldName << doc.getReshardingUUID());

        auto opCtx = makeOperationContext();
        RecipientStateMachine::insertStateDocument(opCtx.get(), doc);
        auto recipient = RecipientStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());

        // Add dummy data to tempReshardingCollectionOptions to create mismatched collection
        // options.
        tempReshardingCollectionOptions = BSONObjBuilder().append("viewOn", "bar").obj();

        notifyToStartCloning(opCtx.get(), *recipient, doc);

        boost::optional<PauseDuringStateTransitions> stateTransitionsGuard;
        stateTransitionsGuard.emplace(controller(), RecipientStateEnum::kError);

        // Ensure we get to the errored state when we try to match options.
        // If we do not get to an errored state this test should hang here and time out.
        stateTransitionsGuard->wait(RecipientStateEnum::kError);
        stateTransitionsGuard->unset(RecipientStateEnum::kError);

        recipient->abort(false);

        ASSERT_OK(recipient->getCompletionFuture().getNoThrow());

        tempReshardingCollectionOptions = BSONObj();
    }
}

// Creates mismatched collection options with the feature flag turned off.
// If the feature was turned on we would catch the mismatched options and throw an exception.
TEST_F(ReshardingRecipientServiceTest,
       TestVerifyCollectionOptionsDoesNotPerformVerificationIfFeatureFlagIsNotSet) {
    RAIIServerParameterControllerForTest featureFlagController("featureFlagReshardingVerification",
                                                               false);
    for (const auto& testOptions : makeBasicTestOptions()) {
        LOGV2(9799203,
              "Running case",
              "test"_attr = _agent.getTestName(),
              "testOptions"_attr = testOptions);

        auto doc = makeRecipientDocument(testOptions);
        auto instanceId =
            BSON(ReshardingRecipientDocument::kReshardingUUIDFieldName << doc.getReshardingUUID());

        auto opCtx = makeOperationContext();
        RecipientStateMachine::insertStateDocument(opCtx.get(), doc);
        auto recipient = RecipientStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());

        // Add dummy data to tempReshardingCollectionOptions to create mismatched collection
        // options.
        tempReshardingCollectionOptions = BSONObjBuilder().append("viewOn", "bar").obj();

        notifyToStartCloning(opCtx.get(), *recipient, doc);

        notifyReshardingCommitting(opCtx.get(), *recipient, doc);

        ASSERT_OK(recipient->getCompletionFuture().getNoThrow());

        tempReshardingCollectionOptions = BSONObj();
    }
}

}  // namespace
}  // namespace mongo
