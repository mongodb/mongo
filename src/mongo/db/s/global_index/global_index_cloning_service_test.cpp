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

#include "mongo/db/commands/list_collections_filter.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/op_observer/op_observer_noop.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/read_write_concern_defaults_cache_lookup_mock.h"
#include "mongo/db/repl/database_cloner_gen.h"
#include "mongo/db/repl/primary_only_service_test_fixture.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/s/global_index/global_index_cloning_external_state.h"
#include "mongo/db/s/global_index/global_index_cloning_service.h"
#include "mongo/db/s/global_index/global_index_util.h"
#include "mongo/db/s/resharding/resharding_service_test_helpers.h"
#include "mongo/db/session/logical_session_cache_noop.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/transaction/session_catalog_mongod_transaction_interface_impl.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/logv2/log.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace global_index {
namespace {

using StateTransitionController =
    resharding_service_test_helpers::StateTransitionController<GlobalIndexClonerStateEnum>;
using OpObserverForTest =
    resharding_service_test_helpers::StateTransitionControllerOpObserver<GlobalIndexClonerStateEnum,
                                                                         GlobalIndexClonerDoc>;
using PauseDuringStateTransitions =
    resharding_service_test_helpers::PauseDuringStateTransitions<GlobalIndexClonerStateEnum>;

const ShardId kRecipientShardId{"myShardId"};
const NamespaceString kSourceNss =
    NamespaceString::createNamespaceString_forTest("sourcedb", "sourcecollection");
constexpr auto kSourceShardKey = "key"_sd;

class GlobalIndexExternalStateForTest : public GlobalIndexCloningService::CloningExternalState {
public:
    ShardId myShardId(ServiceContext* serviceContext) const override {
        return kRecipientShardId;
    }

    ChunkManager getShardedCollectionPlacementInfo(OperationContext* opCtx,
                                                   const NamespaceString& nss) const override {
        invariant(nss == kSourceNss);

        const OID epoch = OID::gen();
        std::vector<ChunkType> chunks = {
            ChunkType{_sourceUUID,
                      ChunkRange{BSON(kSourceShardKey << MINKEY), BSON(kSourceShardKey << MAXKEY)},
                      ChunkVersion({epoch, Timestamp(1, 1)}, {100, 0}),
                      _someDonorId}};

        auto rt = RoutingTableHistory::makeNew(kSourceNss,
                                               _sourceUUID,
                                               BSON(kSourceShardKey << 1),
                                               nullptr /* defaultCollator */,
                                               false /* unique */,
                                               std::move(epoch),
                                               Timestamp(1, 1),
                                               boost::none /* timeseriesFields */,
                                               boost::none /* reshardingFields */,
                                               true /* allowMigrations */,
                                               chunks);

        return ChunkManager(_someDonorId,
                            DatabaseVersion(UUID::gen(), Timestamp(1, 1)),
                            _makeStandaloneRoutingTableHistory(std::move(rt)),
                            boost::none /* clusterTime */);
    }

private:
    RoutingTableHistoryValueHandle _makeStandaloneRoutingTableHistory(
        RoutingTableHistory rt) const {
        const auto version = rt.getVersion();
        return RoutingTableHistoryValueHandle(
            std::make_shared<RoutingTableHistory>(std::move(rt)),
            ComparableChunkVersion::makeComparableChunkVersion(version));
    }

    const UUID _sourceUUID{UUID::gen()};
    const ShardId _someDonorId{"otherShardId"};
};

class Fault {
public:
    Fault(Status error, int triggerCount = 1)
        : _error(std::move(error)), _remainingTriggerCount(triggerCount) {}

    void throwIfEnabled() {
        if (_remainingTriggerCount == 0 || _remainingTriggerCount-- == 0) {
            return;
        }

        uassertStatusOK(_error);
    }

private:
    const Status _error;
    int _remainingTriggerCount{0};
};

template <typename T>
struct FaultOrData {
public:
    FaultOrData(T data) : _data(std::move(data)) {}
    FaultOrData(Fault fault) : _fault(std::move(fault)) {}

    boost::optional<T> getData() {
        if (_fault) {
            _fault->throwIfEnabled();
        }

        return _data;
    }

private:
    boost::optional<T> _data;
    boost::optional<Fault> _fault;
};

std::shared_ptr<FaultOrData<GlobalIndexClonerFetcherInterface::FetchedEntry>> mockFetchedEntry(
    const BSONObj& docKey, const BSONObj& indexKey) {
    return std::make_shared<FaultOrData<GlobalIndexClonerFetcherInterface::FetchedEntry>>(
        GlobalIndexClonerFetcherInterface::FetchedEntry(docKey, indexKey));
}

std::shared_ptr<FaultOrData<GlobalIndexClonerFetcherInterface::FetchedEntry>> mockError(
    Status error) {
    return std::make_shared<FaultOrData<GlobalIndexClonerFetcherInterface::FetchedEntry>>(error);
}

using MockedResults =
    std::list<std::shared_ptr<FaultOrData<GlobalIndexClonerFetcherInterface::FetchedEntry>>>;

class MockGlobalIndexClonerFetcher : public GlobalIndexClonerFetcherInterface {
public:
    explicit MockGlobalIndexClonerFetcher(std::shared_ptr<Value> resumeId) : _resumeId(resumeId) {}

    void setResultList(MockedResults newResults) {
        _mockedResults = std::move(newResults);
    }

    boost::optional<FetchedEntry> getNext(OperationContext* opCtx) override {
        boost::optional<FetchedEntry> ret;

        while (!_mockedResults.empty() && !ret) {
            auto next = _mockedResults.front();

            if (auto actualData = next->getData()) {
                Value idValue(actualData->documentKey["_id"]);
                ValueComparator comparator;
                if (comparator.evaluate(idValue >= *_resumeId)) {
                    ret = actualData;
                }
            }

            _mockedResults.pop_front();
        }

        return ret;
    }

    void setResumeId(Value resumeId) override {
        *_resumeId = std::move(resumeId);
    }

private:
    MockedResults _mockedResults;
    std::shared_ptr<Value> _resumeId;
};

class GlobalIndexCloningFetcherFactoryForTest : public GlobalIndexClonerFetcherFactoryInterface {
public:
    explicit GlobalIndexCloningFetcherFactoryForTest(MockGlobalIndexClonerFetcher* mockFetcher)
        : _mockFetcher(mockFetcher) {}

    std::unique_ptr<GlobalIndexClonerFetcherInterface> make(
        NamespaceString nss,
        UUID collUUId,
        UUID indexUUID,
        ShardId myShardId,
        Timestamp minFetchTimestamp,
        KeyPattern sourceShardKeyPattern,
        KeyPattern globalIndexPattern) override {
        return std::make_unique<MockGlobalIndexClonerFetcher>(*_mockFetcher);
    }

private:
    MockGlobalIndexClonerFetcher* _mockFetcher;
};

class GlobalIndexCloningServiceForTest : public GlobalIndexCloningService {
public:
    explicit GlobalIndexCloningServiceForTest(ServiceContext* serviceContext,
                                              MockGlobalIndexClonerFetcher* mockFetcher)
        : GlobalIndexCloningService(serviceContext),
          _serviceContext(serviceContext),
          _mockFetcher(mockFetcher) {}

    std::shared_ptr<repl::PrimaryOnlyService::Instance> constructInstance(
        BSONObj initialState) override {
        return std::make_shared<GlobalIndexCloningService::CloningStateMachine>(
            _serviceContext,
            this,
            std::make_unique<GlobalIndexExternalStateForTest>(),
            std::make_unique<GlobalIndexCloningFetcherFactoryForTest>(_mockFetcher),
            GlobalIndexClonerDoc::parse(IDLParserContext{"GlobalIndexCloningServiceForTest"},
                                        initialState));
    }

private:
    ServiceContext* _serviceContext;
    MockGlobalIndexClonerFetcher* _mockFetcher;
};

GlobalIndexCloningService::InstanceID extractInstanceId(const GlobalIndexClonerDoc& doc) {
    return BSON("_id" << doc.getIndexCollectionUUID());
}

using GlobalIndexStateMachine = GlobalIndexCloningServiceForTest::CloningStateMachine;

class GlobalIndexClonerServiceTest : public repl::PrimaryOnlyServiceMongoDTest {
public:
    const int kDefaultMockId = 10;

    GlobalIndexClonerServiceTest() {
        _lastSetResumeId = std::make_shared<Value>();
        _mockFetcher = std::make_unique<MockGlobalIndexClonerFetcher>(_lastSetResumeId);
        _fetcherCopyForVerification = std::make_unique<MockGlobalIndexClonerFetcher>(*_mockFetcher);
    }

    std::unique_ptr<repl::PrimaryOnlyService> makeService(ServiceContext* serviceContext) override {
        return std::make_unique<GlobalIndexCloningServiceForTest>(serviceContext,
                                                                  _mockFetcher.get());
    }

    void setUp() override {
        repl::PrimaryOnlyServiceMongoDTest::setUp();

        auto serviceContext = getServiceContext();
        auto storageImpl = std::make_unique<repl::StorageInterfaceImpl>();
        repl::StorageInterface::set(serviceContext, std::move(storageImpl));

        // The ReadWriteConcernDefaults decoration on the service context won't always be created,
        // so we should manually instantiate it to ensure it exists in our tests.
        ReadWriteConcernDefaults::create(getServiceContext(), _lookupMock.getFetchDefaultsFn());

        _stateTransitionController = std::make_shared<StateTransitionController>();
        _opObserverRegistry->addObserver(
            std::make_unique<OpObserverForTest>(_stateTransitionController,
                                                NamespaceString::kGlobalIndexClonerNamespace,
                                                [](const GlobalIndexClonerDoc& stateDoc) {
                                                    return stateDoc.getMutableState().getState();
                                                }));

        // Create config.transactions collection
        auto opCtx = serviceContext->makeOperationContext(Client::getCurrent());
        DBDirectClient client(opCtx.get());
        client.createCollection(NamespaceString::kSessionTransactionsTableNamespace);
        client.createIndexes(NamespaceString::kSessionTransactionsTableNamespace,
                             {MongoDSessionCatalog::getConfigTxnPartialIndexSpec()});

        MongoDSessionCatalog::set(
            getServiceContext(),
            std::make_unique<MongoDSessionCatalog>(
                std::make_unique<MongoDSessionCatalogTransactionInterfaceImpl>()));

        // Session cache is needed otherwise client session info will ignored.
        LogicalSessionCache::set(getServiceContext(), std::make_unique<LogicalSessionCacheNoop>());

        MockedResults fetcherResults;
        fetcherResults.push_front(mockFetchedEntry(
            BSON("_id" << kDefaultMockId << kSourceShardKey << 20), BSON(_indexKey << 30)));
        replaceFetcherResultList(std::move(fetcherResults));

        CreateGlobalIndex createGlobalIndex(_indexCollectionUUID);
        createGlobalIndex.setDbName(DatabaseName::kAdmin);
        BSONObj cmdResult;
        auto success =
            client.runCommand({boost::none, "admin"}, createGlobalIndex.toBSON({}), cmdResult);
        ASSERT(success) << "createGlobalIndex cmd failed with result: " << cmdResult;
    }

    /**
     * Checks that the contents of the global index output collection matches with the results
     * stored in the mocked results.
     *
     * Note: this can trigger the fault in the mock structure if it is still enabled.
     */
    void checkIndexCollection(OperationContext* opCtx) {
        DBDirectClient client(opCtx);

        MockGlobalIndexClonerFetcher fetcherCopy(*_fetcherCopyForVerification);
        while (auto next = fetcherCopy.getNext(opCtx)) {
            FindCommandRequest query(NamespaceString::makeGlobalIndexNSS(_indexCollectionUUID));
            query.setFilter(BSON("_id" << next->documentKey));

            auto doc = client.findOne(query);
            ASSERT_TRUE(!doc.isEmpty())
                << "doc with document key: " << next->documentKey
                << " missing in global index output collection: " << dumpOutputColl(opCtx);
        }
    }

    GlobalIndexClonerDoc makeStateDocument() {
        NewIndexSpec indexSpec(_indexSpec, _indexName);
        CommonGlobalIndexMetadata metadata(
            _indexCollectionUUID, kSourceNss, _collectionUUID, indexSpec);
        GlobalIndexClonerDoc clonerDoc({}, {GlobalIndexClonerStateEnum::kUnused});
        clonerDoc.setCommonGlobalIndexMetadata(metadata);
        return clonerDoc;
    }

    bool doesCollectionExist(OperationContext* opCtx, const NamespaceString& nss) {
        DBDirectClient client(opCtx);
        auto collectionInfos = client.getCollectionInfos(
            nss.dbName(), ListCollectionsFilter::makeTypeCollectionFilter());

        for (auto&& info : collectionInfos) {
            auto coll =
                repl::ListCollectionResult::parse(IDLParserContext("doesCollectionExist"), info);

            if (coll.getName() == nss.coll()) {
                return true;
            }
        }

        return false;
    }

    StateTransitionController* stateTransitionController() {
        return _stateTransitionController.get();
    }

    void replaceFetcherResultList(MockedResults newResults) {
        _mockFetcher->setResultList(std::move(newResults));
        _fetcherCopyForVerification = std::make_unique<MockGlobalIndexClonerFetcher>(*_mockFetcher);
    }

    StringData indexKey() const {
        return _indexKey;
    }

    Value getLastSetResumeId() {
        return *_lastSetResumeId;
    }

    std::string dumpOutputColl(OperationContext* opCtx) {
        DBDirectClient client(opCtx);
        FindCommandRequest query(NamespaceString::makeGlobalIndexNSS(_indexCollectionUUID));

        std::ostringstream outputStr;
        auto res = client.find(query);

        if (!res || !res->more()) {
            return "<empty>";
        }

        outputStr << "docs: " << std::endl;
        while (res->more()) {
            auto doc = res->next();
            outputStr << doc.toString() << std::endl;
        }

        return outputStr.str();
    }

    void checkExpectedEntries(OperationContext* opCtx, int count) {
        DBDirectClient client(opCtx);
        ASSERT_EQ(count, client.count(NamespaceString::makeGlobalIndexNSS(_indexCollectionUUID)))
            << dumpOutputColl(opCtx);
    }

private:
    const UUID _indexCollectionUUID{UUID::gen()};
    const UUID _collectionUUID{UUID::gen()};
    const std::string _indexName{"global_x_1"};
    const StringData _indexKey{"x"};
    const BSONObj _indexSpec{BSON("key" << BSON(_indexKey << 1) << "unique" << true)};

    ReadWriteConcernDefaultsLookupMock _lookupMock;
    std::shared_ptr<StateTransitionController> _stateTransitionController;

    // This is a shared_ptr to make sure that this will be available when the primary only service
    // instance outlives this test fixture (usually happens when assertion occurs).
    std::shared_ptr<Value> _lastSetResumeId;

    std::unique_ptr<MockGlobalIndexClonerFetcher> _mockFetcher;
    std::unique_ptr<MockGlobalIndexClonerFetcher> _fetcherCopyForVerification;
};

MONGO_INITIALIZER_GENERAL(EnableFeatureFlagGlobalIndexes,
                          ("EndServerParameterRegistration"),
                          ("default"))
(InitializerContext*) {
    auto* param = ServerParameterSet::getNodeParameterSet()->get("featureFlagGlobalIndexes");
    uassertStatusOK(
        param->set(BSON("featureFlagGlobalIndexes" << true).firstElement(), boost::none));
}

TEST_F(GlobalIndexClonerServiceTest, CloneInsertsToGlobalIndexCollection) {
    auto doc = makeStateDocument();
    auto opCtx = makeOperationContext();
    auto rawOpCtx = opCtx.get();

    auto cloner = GlobalIndexStateMachine::getOrCreate(rawOpCtx, _service, doc.toBSON());
    auto future = cloner->getCompletionFuture();
    cloner->getReadyToCommitFuture().get();
    cloner->cleanup();
    future.get();

    auto resumeId = getLastSetResumeId();
    ASSERT_EQ(kDefaultMockId, resumeId.getInt());

    ASSERT_TRUE(doesCollectionExist(
        rawOpCtx,
        skipIdNss(doc.getNss(), doc.getCommonGlobalIndexMetadata().getIndexSpec().getName())));
    checkIndexCollection(rawOpCtx);
}

TEST_F(GlobalIndexClonerServiceTest, ShouldBeSafeToRetryOnStepDown) {
    auto doc = makeStateDocument();
    auto opCtx = makeOperationContext();
    auto rawOpCtx = opCtx.get();

    const std::vector<GlobalIndexClonerStateEnum> states{GlobalIndexClonerStateEnum::kCloning,
                                                         GlobalIndexClonerStateEnum::kReadyToCommit,
                                                         GlobalIndexClonerStateEnum::kDone};
    PauseDuringStateTransitions stateTransitionsGuard{stateTransitionController(), states};

    auto prevState = GlobalIndexClonerStateEnum::kUnused;
    for (const auto& nextState : states) {
        LOGV2(6870601,
              "Testing step down prior to state",
              "state"_attr = GlobalIndexClonerState_serializer(nextState));

        auto cloner = ([&] {
            if (nextState == GlobalIndexClonerStateEnum::kCloning ||
                nextState == GlobalIndexClonerStateEnum::kReadyToCommit) {
                return GlobalIndexStateMachine::getOrCreate(rawOpCtx, _service, doc.toBSON());
            }

            return *GlobalIndexStateMachine::lookup(rawOpCtx, _service, extractInstanceId(doc));
        })();

        if (prevState != GlobalIndexClonerStateEnum::kUnused) {
            stateTransitionsGuard.unset(prevState);
        }

        auto readyToCommitFuture = cloner->getReadyToCommitFuture();

        if (nextState == GlobalIndexClonerStateEnum::kDone) {
            readyToCommitFuture.get();
            cloner->cleanup();
        }

        stateTransitionsGuard.wait(nextState);
        stepDown();

        if (nextState != GlobalIndexClonerStateEnum::kDone) {
            ASSERT_THROWS(readyToCommitFuture.get(), DBException);
        }

        // Note: can either throw InterruptDueToRepl or ShutdownInProgress (from executor).
        ASSERT_THROWS(cloner->getCompletionFuture().get(), DBException);

        stepUp(rawOpCtx);

        prevState = nextState;
    }

    auto cloner = *GlobalIndexStateMachine::lookup(rawOpCtx, _service, extractInstanceId(doc));
    stateTransitionsGuard.unset(GlobalIndexClonerStateEnum::kDone);
    cloner->cleanup();
    cloner->getCompletionFuture().get();

    checkIndexCollection(rawOpCtx);
}

TEST_F(GlobalIndexClonerServiceTest, ShouldBeAbleToConsumeMultipleBatchesWorthofDocs) {
    MockedResults fetcherResults;

    RAIIServerParameterControllerForTest batchSizeForTest(
        "globalIndexClonerServiceFetchBatchMaxSizeBytes", 50);
    std::string padding(50, 'x');

    // Populate enough to have more than one batch worth of documents.
    for (int x = 0; x < 4; x++) {
        fetcherResults.push_front(
            mockFetchedEntry(BSON("_id" << x << kSourceShardKey << x),
                             BSON(indexKey() << (std::to_string(x) + padding))));
    }
    replaceFetcherResultList(std::move(fetcherResults));

    auto doc = makeStateDocument();
    auto opCtx = makeOperationContext();
    auto rawOpCtx = opCtx.get();

    auto cloner = GlobalIndexStateMachine::getOrCreate(rawOpCtx, _service, doc.toBSON());
    auto future = cloner->getCompletionFuture();
    cloner->getReadyToCommitFuture().get();
    cloner->cleanup();
    future.get();

    ASSERT_TRUE(doesCollectionExist(
        rawOpCtx,
        skipIdNss(doc.getNss(), doc.getCommonGlobalIndexMetadata().getIndexSpec().getName())));
    checkIndexCollection(rawOpCtx);
}

TEST_F(GlobalIndexClonerServiceTest, ShouldWorkWithEmptyCollection) {
    replaceFetcherResultList({});

    auto doc = makeStateDocument();
    auto opCtx = makeOperationContext();
    auto rawOpCtx = opCtx.get();

    auto cloner = GlobalIndexStateMachine::getOrCreate(rawOpCtx, _service, doc.toBSON());
    auto future = cloner->getCompletionFuture();
    cloner->getReadyToCommitFuture().get();
    cloner->cleanup();
    future.get();

    ASSERT_TRUE(doesCollectionExist(
        rawOpCtx,
        skipIdNss(doc.getNss(), doc.getCommonGlobalIndexMetadata().getIndexSpec().getName())));
    checkIndexCollection(rawOpCtx);
}

TEST_F(GlobalIndexClonerServiceTest, CleanupBeforeReadyResultsInAbort) {
    auto doc = makeStateDocument();
    auto opCtx = makeOperationContext();
    auto rawOpCtx = opCtx.get();

    const std::vector<GlobalIndexClonerStateEnum> states{
        GlobalIndexClonerStateEnum::kCloning, GlobalIndexClonerStateEnum::kReadyToCommit};
    PauseDuringStateTransitions stateTransitionsGuard{stateTransitionController(), states};

    for (const auto& nextState : states) {
        LOGV2(6756300,
              "Testing cleanup abort",
              "state"_attr = GlobalIndexClonerState_serializer(nextState));

        auto cloner = GlobalIndexStateMachine::getOrCreate(rawOpCtx, _service, doc.toBSON());
        auto readyToCommitFuture = cloner->getReadyToCommitFuture();
        auto completionFuture = cloner->getCompletionFuture();

        stateTransitionsGuard.wait(nextState);

        cloner->cleanup();

        ASSERT_THROWS(readyToCommitFuture.get(), DBException);
        completionFuture.get();

        stateTransitionsGuard.unset(nextState);
    }
}

TEST_F(GlobalIndexClonerServiceTest, ResumeIdShouldBeRestoredOnStepUp) {
    auto opCtx = makeOperationContext();
    auto rawOpCtx = opCtx.get();

    auto doc = makeStateDocument();
    auto mutableState = doc.getMutableState();
    mutableState.setState(GlobalIndexClonerStateEnum::kCloning);
    mutableState.setLastProcessedId(Value(3));
    doc.setMutableState(mutableState);

    DBDirectClient client(rawOpCtx);
    write_ops::InsertCommandRequest stateDocInsert(NamespaceString::kGlobalIndexClonerNamespace);
    stateDocInsert.setDocuments({doc.toBSON()});
    auto insertResult = client.insert(stateDocInsert);
    ASSERT_FALSE(insertResult.getWriteErrors()) << insertResult.toBSON();

    MockedResults fetcherResults;
    for (int x = 0; x < 4; x++) {
        fetcherResults.push_front(mockFetchedEntry(BSON("_id" << x << kSourceShardKey << x),
                                                   BSON(indexKey() << std::to_string(x))));
    }
    replaceFetcherResultList(std::move(fetcherResults));

    stepDown();
    stepUp(rawOpCtx);

    auto cloner = *GlobalIndexStateMachine::lookup(rawOpCtx, _service, extractInstanceId(doc));
    ASSERT_TRUE(cloner);

    cloner->getReadyToCommitFuture().get();
    cloner->cleanup();
    cloner->getCompletionFuture().get();

    checkExpectedEntries(rawOpCtx, 1);
}

TEST_F(GlobalIndexClonerServiceTest, ClonerShouldAutoRetryOnNetworkError) {
    const int kTotalResponses = 3;
    const int kFaultPosition = 1;

    replaceFetcherResultList([&] {
        MockedResults fetcherResults;
        for (int x = 0; x < kTotalResponses; x++) {
            if (x == kFaultPosition) {
                fetcherResults.push_front(
                    mockError(Status(ErrorCodes::SocketException, "simulated network error")));
            } else {
                fetcherResults.push_front(mockFetchedEntry(BSON("_id" << x << kSourceShardKey << x),
                                                           BSON(indexKey() << std::to_string(x))));
            }
        }

        return fetcherResults;
    }());

    auto doc = makeStateDocument();
    auto opCtx = makeOperationContext();
    auto rawOpCtx = opCtx.get();

    auto cloner = GlobalIndexStateMachine::getOrCreate(rawOpCtx, _service, doc.toBSON());
    auto future = cloner->getCompletionFuture();
    cloner->getReadyToCommitFuture().get();
    cloner->cleanup();
    future.get();

    ASSERT_TRUE(doesCollectionExist(
        rawOpCtx,
        skipIdNss(doc.getNss(), doc.getCommonGlobalIndexMetadata().getIndexSpec().getName())));
    checkIndexCollection(rawOpCtx);
}

TEST_F(GlobalIndexClonerServiceTest, MetricsGetsUpdatedWhileRunning) {
    auto doc = makeStateDocument();
    auto opCtx = makeOperationContext();
    auto rawOpCtx = opCtx.get();

    auto cloner = GlobalIndexStateMachine::getOrCreate(rawOpCtx, _service, doc.toBSON());
    auto readyToCommitFuture = cloner->getReadyToCommitFuture();
    readyToCommitFuture.get();

    auto currentOpBSONOpt =
        cloner->reportForCurrentOp(MongoProcessInterface::CurrentOpConnectionsMode::kIncludeIdle,
                                   MongoProcessInterface::CurrentOpSessionsMode::kIncludeIdle);
    ASSERT_TRUE(currentOpBSONOpt);
    const auto currentOpBSON = *currentOpBSONOpt;

    ASSERT_EQ("ready-to-commit", currentOpBSON["recipientState"].str()) << currentOpBSON;
    ASSERT_EQ(1, currentOpBSON["keysWrittenFromScan"].safeNumberInt()) << currentOpBSON;
    ASSERT_EQ(35, currentOpBSON["bytesWritten"].safeNumberInt()) << currentOpBSON;

    cloner->cleanup();
    cloner->getCompletionFuture().get();
}

}  // namespace
}  // namespace global_index
}  // namespace mongo
