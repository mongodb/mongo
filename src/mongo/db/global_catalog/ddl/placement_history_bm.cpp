// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_database_gen.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/read_write_concern_defaults_cache_lookup_mock.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/transaction_coordinator_service.h"
#include "mongo/db/server_options.h"
#include "mongo/db/session/logical_session_cache.h"
#include "mongo/db/session/logical_session_cache_noop.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/sharding_environment/config_server_op_observer.h"
#include "mongo/db/sharding_environment/config_server_test_fixture.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/topology/vector_clock/vector_clock.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <benchmark/benchmark.h>

namespace mongo {

namespace {

// Reusing the ConfigServerTestFixture for benchmarking.
class BenchmarkConfigServerTestFixture : public ConfigServerTestFixture {
public:
    BenchmarkConfigServerTestFixture() : ConfigServerTestFixture() {
        ConfigServerTestFixture::setUp();
        DBDirectClient client(operationContext());
        client.createCollection(NamespaceString::kSessionTransactionsTableNamespace);
        client.createIndexes(NamespaceString::kSessionTransactionsTableNamespace,
                             {MongoDSessionCatalog::getConfigTxnPartialIndexSpec()});

        ReadWriteConcernDefaults::create(getService(), _lookupMock.getFetchDefaultsFn());

        LogicalSessionCache::set(getServiceContext(), std::make_unique<LogicalSessionCacheNoop>());
        TransactionCoordinatorService::get(operationContext())
            ->initializeIfNeeded(operationContext(), /* term */ 1);

        WaitForMajorityService::get(getServiceContext()).startup(getServiceContext());

        ShardingCatalogManager::get(operationContext())->startup();
        ASSERT_OK(ShardingCatalogManager::get(operationContext())
                      ->initializeConfigDatabaseIfNeeded(operationContext()));
    }

    ~BenchmarkConfigServerTestFixture() override {
        TransactionCoordinatorService::get(operationContext())->interruptForStepDown();
        WaitForMajorityService::get(getServiceContext()).shutDown();
        ShardingCatalogManager::get(operationContext())->shutDown();
        ConfigServerTestFixture::tearDown();
    }

    OperationContext* opCxt() {
        return operationContext();
    }

    /* Generate and insert the entries into the config.shards collection
     * Given the desired number of shards n, generates a vector of n ShardType objects (in BSON
     * format) according to the following template,  Given the i-th element :
     *  - shard_id : shard<i>
     *  - host : localhost:3000<i>
     *  - state : always 1 (kShardAware)
     */
    void setupConfigShards(int nShards) {
        _shardIds.reserve(nShards);
        for (auto& doc : _generateConfigShardSampleData(nShards)) {
            _shardIds.push_back(doc["_id"].String());  // store it locally for later use
            ASSERT_OK(insertToConfigCollection(
                operationContext(), NamespaceString::kConfigsvrShardsNamespace, doc));
        }
    }

    /* Generate and insert the entries into the config.database collection
     * Given the desired number of db n, generates a vector of n DatabaseType objects (in BSON
     * format) according to the following template,  Given the i-th element :
     *  - dbName : db<i>
     *  - primaryShard :  shard<i>
     *  - databaseVersion : always DatabaseVersion::makeFixed()
     */
    void setupConfigDatabase(int nDbs) {
        for (int i = 1; i <= nDbs; i++) {
            const std::string dbName = "db" + std::to_string(i);
            const std::string shardName = "shard" + std::to_string(i);
            const DatabaseType dbEntry(
                DatabaseName::createDatabaseName_forTest(boost::none, dbName),
                ShardId(shardName),
                DatabaseVersion::makeFixed());
            ASSERT_OK(insertToConfigCollection(
                operationContext(), NamespaceString::kConfigDatabasesNamespace, dbEntry.toBSON()));
        }
    }

    /*
     * Generates N chunks for a collection on a given shard
     */
    void setupCollectionWithChunks(const mongo::NamespaceString& nss, uint32_t numChunks) {
        std::vector<ChunkType> chunks;
        chunks.reserve(numChunks);
        auto collUuid = UUID::gen();
        for (uint32_t i = 1; i <= numChunks; i++) {
            uint32_t lb = _lastKey + i;
            uint32_t ub = _lastKey + i + 1;
            uint32_t epoch = _lastKey + i + 2;
            ChunkType c1(collUuid,
                         ChunkRange(BSON("x" << (int)lb), BSON("x" << (int)ub)),
                         ChunkVersion({OID::gen(), {epoch, epoch}}, {epoch, epoch}),
                         _shardIds[i % _shardIds.size()]);

            chunks.push_back(c1);
        }
        _lastKey += numChunks + 2;
        setupCollection(nss, BSON("x" << 1), chunks);

        // Ensure that the vector clock is able to return an up-to-date config time to both the
        // ShardingCatalogManager and this test.
        mongo::ConfigServerOpObserver opObserver;
        auto initTime = VectorClock::get(operationContext())->getTime();
        repl::OpTime majorityCommitPoint(initTime.clusterTime().asTimestamp(), 1);
        opObserver.onMajorityCommitPointUpdate(getServiceContext(), majorityCommitPoint);
    }

private:
    void TestBody() override {}

    std::vector<BSONObj> _generateConfigShardSampleData(int nShards) const {
        std::vector<BSONObj> configShardData;
        for (int i = 1; i <= nShards; i++) {
            const std::string shardName = "shard" + std::to_string(i);
            const std::string shardHost = "localhost:" + std::to_string(30000 + i);
            const auto& doc = BSON("_id" << shardName << "host" << shardHost);
            configShardData.push_back(doc);
        }

        return configShardData;
    }

    std::vector<ShardId> _shardIds;
    uint32_t _lastKey = 0;

    ReadWriteConcernDefaultsLookupMock _lookupMock;
};

}  // namespace

void BM_initPlacementHistory(benchmark::State& state) {
    serverGlobalParams.clusterRole = {ClusterRole::ShardServer, ClusterRole::ConfigServer};

    BenchmarkConfigServerTestFixture fixture;


    const int nChunks = state.range(0);
    const int nCollections = state.range(1);
    const int nShards = state.range(2);

    fixture.setupConfigShards(nShards);
    fixture.setupConfigDatabase(10);  // 10 databases

    for (int i = 1; i <= nCollections; i++) {
        const std::string collName = "coll" + std::to_string(i);
        fixture.setupCollectionWithChunks(
            NamespaceString::createNamespaceString_forTest("db1." + collName), nChunks);
    }

    for (auto _ : state) {
        auto now = VectorClock::get(fixture.opCxt())->getTime();
        const auto initTime = now.configTime().asTimestamp();

        ShardingCatalogManager::get(fixture.opCxt())
            ->initializePlacementHistory(fixture.opCxt(), initTime);
    }
};

// nChunksPerCollection, nCollections, nShards
BENCHMARK(BM_initPlacementHistory)
    ->Unit(benchmark::kMillisecond)
    ->Args({100, 1, 10})
    ->Args({100, 10, 10})
    ->Args({100, 100, 10})
    ->Args({100, 100, 100})
    ->Args({1000, 10, 10})
    ->Args({1000, 100, 10})
    ->Args({1000, 100, 100})
    ->Args({10000, 10, 10})
    ->Args({10000, 100, 10})
    ->Args({10000, 100, 100});
}  // namespace mongo
