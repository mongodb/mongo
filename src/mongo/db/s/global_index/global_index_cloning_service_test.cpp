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
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/s/global_index/global_index_cloning_external_state.h"
#include "mongo/db/s/global_index/global_index_cloning_service.h"
#include "mongo/db/s/global_index/global_index_util.h"
#include "mongo/db/session/logical_session_cache_noop.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/transaction/session_catalog_mongod_transaction_interface_impl.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace global_index {
namespace {

const ShardId kRecipientShardId{"myShardId"};
const NamespaceString kSourceNss{"sourcedb", "sourcecollection"};
constexpr auto kSourceShardKey = "key"_sd;

class GlobalIndexExternalStateForTest : public GlobalIndexCloningService::CloningExternalState {
public:
    ShardId myShardId(ServiceContext* serviceContext) const override {
        return kRecipientShardId;
    }

    ChunkManager getShardedCollectionRoutingInfo(OperationContext* opCtx,
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
                                               boost::none /* chunkSizeBytes */,
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

class MockGlobalIndexClonerFetcher : public GlobalIndexClonerFetcherInterface {
public:
    void setResultList(std::list<FetchedEntry> newResults) {
        _docs = std::move(newResults);
    }

    boost::optional<FetchedEntry> getNext(OperationContext* opCtx) override {
        if (_docs.empty()) {
            return boost::none;
        }

        auto ret = _docs.front();
        _docs.pop_front();
        return ret;
    }

private:
    std::list<FetchedEntry> _docs;
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

class Blocker {
public:
    ~Blocker() {
        stdx::unique_lock lk(_mutex);
        _shouldBlock = false;
        _cvBlocked.notify_all();
    }

    void blockIfActivated(OperationContext* opCtx) {
        stdx::unique_lock lk(_mutex);
        _blockedOnce = true;
        opCtx->waitForConditionOrInterrupt(_cvBlocked, lk, [this] { return !_shouldBlock; });
    }

    void waitUntilBlockedOccurred(OperationContext* opCtx) {
        stdx::unique_lock lk(_mutex);
        opCtx->waitForConditionOrInterrupt(_cvBlocked, lk, [this] { return _blockedOnce; });
    }

    void block() {
        stdx::unique_lock lk(_mutex);
        _shouldBlock = true;
    }

    void unblock() {
        stdx::unique_lock lk(_mutex);
        _shouldBlock = false;
    }

private:
    Mutex _mutex = MONGO_MAKE_LATCH("GlobalIndexCloningServiceTestBlocker::_mutex");
    stdx::condition_variable _cvBlocked;
    bool _shouldBlock{false};
    bool _blockedOnce{false};
};

class OpObserverForTest : public OpObserverNoop {
public:
    OpObserverForTest(Blocker* insertBlocker, Blocker* deleteBlocker)
        : _insertBlocker(insertBlocker), _deleteBlocker(deleteBlocker) {}

    void onInserts(OperationContext* opCtx,
                   const CollectionPtr& coll,
                   std::vector<InsertStatement>::const_iterator begin,
                   std::vector<InsertStatement>::const_iterator end,
                   bool fromMigrate) override {
        if (NamespaceString::kGlobalIndexClonerNamespace == coll->ns()) {
            _insertBlocker->blockIfActivated(opCtx);
        }
    }

    void onDelete(OperationContext* opCtx,
                  const NamespaceString& nss,
                  const UUID& uuid,
                  StmtId stmtId,
                  const OplogDeleteEntryArgs& args) override {
        if (NamespaceString::kGlobalIndexClonerNamespace == nss) {
            _deleteBlocker->blockIfActivated(opCtx);
        }
    }

private:
    Blocker* _insertBlocker;
    Blocker* _deleteBlocker;
};

GlobalIndexCloningService::InstanceID extractInstanceId(const GlobalIndexClonerDoc& doc) {
    return BSON("_id" << doc.getIndexCollectionUUID());
}

using GlobalIndexStateMachine = GlobalIndexCloningServiceForTest::CloningStateMachine;

class GlobalIndexClonerServiceTest : public repl::PrimaryOnlyServiceMongoDTest {
public:
    std::unique_ptr<repl::PrimaryOnlyService> makeService(ServiceContext* serviceContext) override {
        return std::make_unique<GlobalIndexCloningServiceForTest>(serviceContext, &_mockFetcher);
    }

    void setUp() override {
        repl::PrimaryOnlyServiceMongoDTest::setUp();

        auto serviceContext = getServiceContext();
        auto storageMock = std::make_unique<repl::StorageInterfaceMock>();
        repl::StorageInterface::set(serviceContext, std::move(storageMock));

        // The ReadWriteConcernDefaults decoration on the service context won't always be created,
        // so we should manually instantiate it to ensure it exists in our tests.
        ReadWriteConcernDefaults::create(getServiceContext(), _lookupMock.getFetchDefaultsFn());

        _opObserverRegistry->addObserver(
            std::make_unique<OpObserverForTest>(&_stateDocInsertBlocker, &_stateDocDeleteBlocker));

        // Create config.transactions collection
        auto opCtx = serviceContext->makeOperationContext(Client::getCurrent());
        DBDirectClient client(opCtx.get());
        client.createCollection(NamespaceString::kSessionTransactionsTableNamespace.ns());
        client.createIndexes(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                             {MongoDSessionCatalog::getConfigTxnPartialIndexSpec()});

        MongoDSessionCatalog::set(
            getServiceContext(),
            std::make_unique<MongoDSessionCatalog>(
                std::make_unique<MongoDSessionCatalogTransactionInterfaceImpl>()));

        // Session cache is needed otherwise client session info will ignored.
        LogicalSessionCache::set(getServiceContext(), std::make_unique<LogicalSessionCacheNoop>());

        std::list<GlobalIndexClonerFetcherInterface::FetchedEntry> fetcherResults;
        fetcherResults.push_front(
            {BSON("_id" << 10 << kSourceShardKey << 20), BSON(_indexKey << 30)});
        replaceFetcherResultList(std::move(fetcherResults));

        const auto& indexNs = globalIndexNss(kSourceNss, _indexName);
        client.createCollection(indexNs.ns());
        auto all = client.getCollectionInfos(indexNs.db().toString(),
                                             BSON("name" << indexNs.coll().toString()));

        ASSERT_EQ(1, all.size());
        _indexCollectionUUID.emplace(uassertStatusOK(UUID::parse(all.front()["info"]["uuid"])));
    }

    void checkIndexCollection(OperationContext* opCtx) {
        DBDirectClient client(opCtx);

        MockGlobalIndexClonerFetcher fetcherCopy(_fetcherCopyForVerification);
        while (auto next = fetcherCopy.getNext(opCtx)) {
            FindCommandRequest query(globalIndexNss(kSourceNss, _indexName));
            query.setFilter(BSON("_id" << next->indexKeyValues));

            auto doc = client.findOne(query);
            ASSERT_TRUE(!doc.isEmpty())
                << "doc with index key: " << next->indexKeyValues
                << " missing in global index output collection: " << dumpOutputColl(opCtx);
        }
    }

    GlobalIndexClonerDoc makeStateDocument() {
        return GlobalIndexClonerDoc(*_indexCollectionUUID,
                                    kSourceNss,
                                    _collectionUUID,
                                    _indexName,
                                    _indexSpec,
                                    {},
                                    GlobalIndexClonerStateEnum::kUnused);
    }

    bool doesCollectionExist(OperationContext* opCtx, const NamespaceString& nss) {
        DBDirectClient client(opCtx);
        auto collectionInfos = client.getCollectionInfos(
            nss.db().toString(), ListCollectionsFilter::makeTypeCollectionFilter());

        for (auto&& info : collectionInfos) {
            auto coll =
                repl::ListCollectionResult::parse(IDLParserContext("doesCollectionExist"), info);

            if (coll.getName() == nss.coll()) {
                return true;
            }
        }

        return false;
    }

    Blocker* getStateDocInsertBlocker() {
        return &_stateDocInsertBlocker;
    }

    Blocker* getStateDocDeleteBlocker() {
        return &_stateDocDeleteBlocker;
    }

    void replaceFetcherResultList(
        std::list<GlobalIndexClonerFetcherInterface::FetchedEntry> newResults) {
        _mockFetcher.setResultList(std::move(newResults));
        _fetcherCopyForVerification = _mockFetcher;
    }

    StringData indexKey() const {
        return _indexKey;
    }

private:
    std::string dumpOutputColl(OperationContext* opCtx) {
        DBDirectClient client(opCtx);
        FindCommandRequest query(globalIndexNss(kSourceNss, _indexName));

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

    boost::optional<UUID> _indexCollectionUUID;
    const UUID _collectionUUID{UUID::gen()};
    const std::string _indexName{"global_x_1"};
    const StringData _indexKey{"x"};
    const BSONObj _indexSpec{BSON("key" << BSON(_indexKey << 1) << "unique" << true)};

    ReadWriteConcernDefaultsLookupMock _lookupMock;
    Blocker _stateDocInsertBlocker;
    Blocker _stateDocDeleteBlocker;

    MockGlobalIndexClonerFetcher _mockFetcher;
    MockGlobalIndexClonerFetcher _fetcherCopyForVerification;
};

TEST_F(GlobalIndexClonerServiceTest, CloneInsertsToGlobalIndexCollection) {
    auto doc = makeStateDocument();
    auto opCtx = makeOperationContext();
    auto rawOpCtx = opCtx.get();

    auto cloner = GlobalIndexStateMachine::getOrCreate(rawOpCtx, _service, doc.toBSON());
    auto future = cloner->getCompletionFuture();
    future.get();

    ASSERT_TRUE(doesCollectionExist(rawOpCtx, skipIdNss(doc.getNss(), doc.getIndexName())));
    checkIndexCollection(rawOpCtx);
}

TEST_F(GlobalIndexClonerServiceTest, ShouldBeSafeToRetryOnStepDown) {
    auto doc = makeStateDocument();
    auto opCtx = makeOperationContext();
    auto rawOpCtx = opCtx.get();

    auto stateDocInsertBlocker = getStateDocInsertBlocker();
    stateDocInsertBlocker->block();
    auto stateDocDeleteBlocker = getStateDocDeleteBlocker();
    stateDocDeleteBlocker->block();

    {
        auto cloner = GlobalIndexStateMachine::getOrCreate(rawOpCtx, _service, doc.toBSON());
        stateDocInsertBlocker->waitUntilBlockedOccurred(rawOpCtx);
        stepDown();

        ASSERT_THROWS(cloner->getCompletionFuture().get(), DBException);
    }

    stepUp(rawOpCtx);

    {
        auto cloner = GlobalIndexStateMachine::getOrCreate(rawOpCtx, _service, doc.toBSON());
        stateDocInsertBlocker->unblock();
        stateDocDeleteBlocker->waitUntilBlockedOccurred(rawOpCtx);
        stepDown();

        ASSERT_THROWS(cloner->getCompletionFuture().get(), DBException);
    }

    stepUp(rawOpCtx);

    // It is possible for the primary only service to run to completion and no longer exists.
    {
        auto cloner = GlobalIndexStateMachine::lookup(rawOpCtx, _service, extractInstanceId(doc));
        stateDocDeleteBlocker->unblock();
        (*cloner)->getCompletionFuture().get();
    }

    checkIndexCollection(rawOpCtx);
}

TEST_F(GlobalIndexClonerServiceTest, ShouldBeAbleToConsumeMultipleBatchesWorthofDocs) {
    std::list<GlobalIndexClonerFetcherInterface::FetchedEntry> fetcherResults;

    RAIIServerParameterControllerForTest batchSizeForTest(
        "globalIndexClonerServiceFetchBatchMaxSizeBytes", 50);
    std::string padding(50, 'x');

    // Populate enough to have more than one batch worth of documents.
    for (int x = 0; x < 4; x++) {
        fetcherResults.push_front({BSON("_id" << x << kSourceShardKey << x),
                                   BSON(indexKey() << (std::to_string(x) + padding))});
    }
    replaceFetcherResultList(std::move(fetcherResults));

    auto doc = makeStateDocument();
    auto opCtx = makeOperationContext();
    auto rawOpCtx = opCtx.get();

    auto cloner = GlobalIndexStateMachine::getOrCreate(rawOpCtx, _service, doc.toBSON());
    auto future = cloner->getCompletionFuture();
    future.get();

    ASSERT_TRUE(doesCollectionExist(rawOpCtx, skipIdNss(doc.getNss(), doc.getIndexName())));
    checkIndexCollection(rawOpCtx);
}

TEST_F(GlobalIndexClonerServiceTest, ShouldWorkWithEmptyCollection) {
    replaceFetcherResultList({});

    auto doc = makeStateDocument();
    auto opCtx = makeOperationContext();
    auto rawOpCtx = opCtx.get();

    auto cloner = GlobalIndexStateMachine::getOrCreate(rawOpCtx, _service, doc.toBSON());
    auto future = cloner->getCompletionFuture();
    future.get();

    ASSERT_TRUE(doesCollectionExist(rawOpCtx, skipIdNss(doc.getNss(), doc.getIndexName())));
    checkIndexCollection(rawOpCtx);
}

}  // namespace
}  // namespace global_index
}  // namespace mongo
