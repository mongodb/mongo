/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/migration_util.h"
#include "mongo/db/s/persistent_task_store.h"
#include "mongo/db/s/shard_filtering_metadata_refresh.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/database_version_helpers.h"
#include "mongo/s/shard_server_test_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/future.h"

namespace mongo {
namespace {

using executor::RemoteCommandRequest;

class MigrationUtilsTest : public ShardServerTestFixture {
protected:
    const HostAndPort kConfigHostAndPort{"dummy", 123};
    const NamespaceString kNss{"test.foo"};

    void setUp() override {
        ShardServerTestFixture::setUp();

        CatalogCacheLoader::get(operationContext()).initializeReplicaSetRole(true);

        setupNShards(2);
    }

    void tearDown() override {
        ShardServerTestFixture::tearDown();

        CollectionShardingStateFactory::clear(getServiceContext());
    }

    executor::NetworkTestEnv::FutureHandle<boost::optional<CachedCollectionRoutingInfo>>
    scheduleRoutingInfoRefresh(const NamespaceString& nss) {
        return launchAsync([this, nss] {
            auto client = getServiceContext()->makeClient("Test");
            auto opCtx = client->makeOperationContext();
            auto const catalogCache = Grid::get(getServiceContext())->catalogCache();
            catalogCache->invalidateShardedCollection(nss);

            return boost::make_optional(
                uassertStatusOK(catalogCache->getCollectionRoutingInfo(opCtx.get(), nss)));
        });
    }

    void expectFindSendBSONObjVector(const HostAndPort& configHost, std::vector<BSONObj> obj) {
        onFindCommand([&, obj](const RemoteCommandRequest& request) {
            ASSERT_EQ(request.target, configHost);
            ASSERT_EQ(request.dbname, "config");
            return obj;
        });
    }

    void setupShards(const std::vector<ShardType>& shards) {
        auto future = launchAsync([this] { shardRegistry()->reload(operationContext()); });

        expectGetShards(shards);

        future.default_timed_get();
    }

    void expectGetShards(const std::vector<ShardType>& shards) {
        onFindCommand([this, &shards](const RemoteCommandRequest& request) {
            const NamespaceString nss(request.dbname, request.cmdObj.firstElement().String());
            ASSERT_EQ(nss, ShardType::ConfigNS);

            auto queryResult = QueryRequest::makeFromFindCommand(nss, request.cmdObj, false);
            ASSERT_OK(queryResult.getStatus());

            const auto& query = queryResult.getValue();
            ASSERT_EQ(query->nss(), ShardType::ConfigNS);

            ASSERT_BSONOBJ_EQ(query->getFilter(), BSONObj());
            ASSERT_BSONOBJ_EQ(query->getSort(), BSONObj());
            ASSERT_FALSE(query->getLimit().is_initialized());

            checkReadConcern(request.cmdObj, Timestamp(0, 0), repl::OpTime::kUninitializedTerm);

            std::vector<BSONObj> shardsToReturn;

            std::transform(shards.begin(),
                           shards.end(),
                           std::back_inserter(shardsToReturn),
                           [](const ShardType& shard) { return shard.toBSON(); });

            return shardsToReturn;
        });
    }

    void checkReadConcern(const BSONObj& cmdObj,
                          const Timestamp& expectedTS,
                          long long expectedTerm) const {
        auto readConcernElem = cmdObj[repl::ReadConcernArgs::kReadConcernFieldName];
        ASSERT_EQ(Object, readConcernElem.type());

        auto readConcernObj = readConcernElem.Obj();
        ASSERT_EQ("majority", readConcernObj[repl::ReadConcernArgs::kLevelFieldName].str());

        auto afterElem = readConcernObj[repl::ReadConcernArgs::kAfterOpTimeFieldName];
        ASSERT_EQ(Object, afterElem.type());

        auto afterObj = afterElem.Obj();

        ASSERT_TRUE(afterObj.hasField(repl::OpTime::kTimestampFieldName));
        ASSERT_EQ(expectedTS, afterObj[repl::OpTime::kTimestampFieldName].timestamp());
        ASSERT_TRUE(afterObj.hasField(repl::OpTime::kTermFieldName));
        ASSERT_EQ(expectedTerm, afterObj[repl::OpTime::kTermFieldName].numberLong());
    }

    void setupNShards(int numShards) {
        setupShards([&]() {
            std::vector<ShardType> shards;
            for (int i = 0; i < numShards; i++) {
                ShardId name(str::stream() << i);
                HostAndPort host(str::stream() << "Host" << i << ":12345");

                ShardType shard;
                shard.setName(name.toString());
                shard.setHost(host.toString());
                shards.emplace_back(std::move(shard));

                std::unique_ptr<RemoteCommandTargeterMock> targeter(
                    std::make_unique<RemoteCommandTargeterMock>());
                targeter->setConnectionStringReturnValue(ConnectionString(host));
                targeter->setFindHostReturnValue(host);
                targeterFactory()->addTargeterToReturn(ConnectionString(host), std::move(targeter));
            }

            return shards;
        }());
    }

    void respondToMetadataRefreshRequests() {
        const OID epoch = OID::gen();
        const ShardKeyPattern shardKeyPattern(BSON("_id" << 1));

        const BSONObj databaseBSON = [&]() {
            DatabaseType db(kNss.db().toString(), {"0"}, true, databaseVersion::makeNew());
            return db.toBSON();
        }();

        const BSONObj collectionBSON = [&]() {
            CollectionType coll;
            coll.setNs(kNss);
            coll.setEpoch(epoch);
            coll.setKeyPattern(shardKeyPattern.getKeyPattern());
            coll.setUnique(true);
            coll.setUUID(UUID::gen());

            return coll.toBSON();
        }();

        expectFindSendBSONObjVector(kConfigHostAndPort, {databaseBSON});
        expectFindSendBSONObjVector(kConfigHostAndPort, {collectionBSON});
        expectFindSendBSONObjVector(kConfigHostAndPort, {collectionBSON});

        expectFindSendBSONObjVector(kConfigHostAndPort, [&]() {
            ChunkVersion version(1, 0, epoch);

            ChunkType chunk1(kNss,
                             {shardKeyPattern.getKeyPattern().globalMin(), BSON("_id" << -100)},
                             version,
                             {"0"});
            chunk1.setName(OID::gen());
            version.incMinor();

            ChunkType chunk2(kNss, {BSON("_id" << -100), BSON("_id" << 0)}, version, {"1"});
            chunk2.setName(OID::gen());
            version.incMinor();

            ChunkType chunk3(kNss, {BSON("_id" << 0), BSON("_id" << 100)}, version, {"0"});
            chunk3.setName(OID::gen());
            version.incMinor();

            ChunkType chunk4(kNss,
                             {BSON("_id" << 100), shardKeyPattern.getKeyPattern().globalMax()},
                             version,
                             {"1"});
            chunk4.setName(OID::gen());
            version.incMinor();

            return std::vector<BSONObj>{chunk1.toConfigBSON(),
                                        chunk2.toConfigBSON(),
                                        chunk3.toConfigBSON(),
                                        chunk4.toConfigBSON()};
        }());
    }

    void respondToMetadataRefreshRequestsWithError() {
        // Return an empty database (need to return it twice because for missing databases, the
        // CatalogClient tries twice)
        expectFindSendBSONObjVector(kConfigHostAndPort, {});
        expectFindSendBSONObjVector(kConfigHostAndPort, {});

        // getCollectionRoutingInfoWithRefresh calls _getCollectionRoutingInfo twice
        expectFindSendBSONObjVector(kConfigHostAndPort, {});
        expectFindSendBSONObjVector(kConfigHostAndPort, {});
    }

    boost::optional<CachedCollectionRoutingInfo> getRoutingInfo() {
        auto future = scheduleRoutingInfoRefresh(kNss);

        respondToMetadataRefreshRequests();

        auto routingInfo = future.default_timed_get();

        return routingInfo;
    }
};

UUID getCollectionUuid(OperationContext* opCtx, const NamespaceString& nss) {
    AutoGetCollection autoColl(opCtx, nss, MODE_IS);

    ASSERT(autoColl.getCollection());

    return autoColl.getCollection()->uuid();
}

void addRangeToReceivingChunks(OperationContext* opCtx,
                               const NamespaceString& nss,
                               const ChunkRange& range) {
    AutoGetCollection autoColl(opCtx, nss, MODE_IS);

    auto notification = CollectionShardingRuntime::get(opCtx, nss)->beginReceive(range);
    notification.abandon();
}

RangeDeletionTask createDeletionTask(
    NamespaceString nss, const UUID& uuid, int min, int max, bool pending = true) {
    auto task = RangeDeletionTask(UUID::gen(),
                                  nss,
                                  uuid,
                                  ShardId("donorShard"),
                                  ChunkRange{BSON("_id" << min), BSON("_id" << max)},
                                  CleanWhenEnum::kNow);

    if (pending)
        task.setPending(true);

    return task;
}

// Test that overlappingRangeQuery() can handle the cases that we expect to encounter.
//           1    1    2    2    3    3    4    4    5
// 0----5----0----5----0----5----0----5----0----5----0
//                          |---------O                Range 1 [25, 35)
//      |---------O                                    Range 2 [5, 15)
//           |---------O                               Range 4 [10, 20)
// |----O                                              Range 5 [0, 5)
//             |-----O                                 Range 7 [12, 18)
//                               |---------O           Range 8 [30, 40)
// Ranges in store
// |---------O                                         [0, 10)
//           |---------O                               [10, 20)
//                                         |---------O [40 50)
//           1    1    2    2    3    3    4    4    5
// 0----5----0----5----0----5----0----5----0----5----0
TEST_F(MigrationUtilsTest, TestOverlappingRangeQuery) {
    auto opCtx = operationContext();
    const auto uuid = UUID::gen();

    PersistentTaskStore<RangeDeletionTask> store(opCtx, NamespaceString::kRangeDeletionNamespace);

    store.add(opCtx, createDeletionTask(NamespaceString{"one"}, uuid, 0, 10));
    store.add(opCtx, createDeletionTask(NamespaceString{"two"}, uuid, 10, 20));
    store.add(opCtx, createDeletionTask(NamespaceString{"three"}, uuid, 40, 50));

    ASSERT_EQ(store.count(opCtx), 3);

    // 1. Non-overlapping range
    auto range1 = ChunkRange{BSON("_id" << 25), BSON("_id" << 35)};
    auto results = store.count(opCtx, migrationutil::overlappingRangeQuery(range1, uuid));
    ASSERT_EQ(results, 0);
    ASSERT_FALSE(migrationutil::checkForConflictingDeletions(opCtx, range1, uuid));

    // 2, 3. Find overlapping ranges, either direction.
    auto range2 = ChunkRange{BSON("_id" << 5), BSON("_id" << 15)};
    results = store.count(opCtx, migrationutil::overlappingRangeQuery(range2, uuid));
    ASSERT_EQ(results, 2);
    ASSERT(migrationutil::checkForConflictingDeletions(opCtx, range2, uuid));

    // 4. Identical range
    auto range4 = ChunkRange{BSON("_id" << 10), BSON("_id" << 20)};
    results = store.count(opCtx, migrationutil::overlappingRangeQuery(range4, uuid));
    ASSERT_EQ(results, 1);
    ASSERT(migrationutil::checkForConflictingDeletions(opCtx, range4, uuid));

    // 5, 6. Find overlapping edge, either direction.
    auto range5 = ChunkRange{BSON("_id" << 0), BSON("_id" << 5)};
    results = store.count(opCtx, migrationutil::overlappingRangeQuery(range5, uuid));
    ASSERT_EQ(results, 1);
    ASSERT(migrationutil::checkForConflictingDeletions(opCtx, range5, uuid));
    auto range6 = ChunkRange{BSON("_id" << 5), BSON("_id" << 10)};
    results = store.count(opCtx, migrationutil::overlappingRangeQuery(range6, uuid));
    ASSERT_EQ(results, 1);
    ASSERT(migrationutil::checkForConflictingDeletions(opCtx, range6, uuid));

    // 7. Find fully enclosed range
    auto range7 = ChunkRange{BSON("_id" << 12), BSON("_id" << 18)};
    results = store.count(opCtx, migrationutil::overlappingRangeQuery(range7, uuid));
    ASSERT_EQ(results, 1);
    ASSERT(migrationutil::checkForConflictingDeletions(opCtx, range7, uuid));

    // 8, 9. Open max doesn't overlap closed min, either direction.
    auto range8 = ChunkRange{BSON("_id" << 30), BSON("_id" << 40)};
    results = store.count(opCtx, migrationutil::overlappingRangeQuery(range8, uuid));
    ASSERT_EQ(results, 0);
    ASSERT_FALSE(migrationutil::checkForConflictingDeletions(opCtx, range8, uuid));
    auto range9 = ChunkRange{BSON("_id" << 20), BSON("_id" << 30)};
    results = store.count(opCtx, migrationutil::overlappingRangeQuery(range9, uuid));
    ASSERT_EQ(results, 0);
    ASSERT_FALSE(migrationutil::checkForConflictingDeletions(opCtx, range9, uuid));
}

TEST_F(MigrationUtilsTest, TestInvalidUUID) {
    auto opCtx = operationContext();
    const auto uuid = UUID::gen();

    PersistentTaskStore<RangeDeletionTask> store(opCtx, NamespaceString::kRangeDeletionNamespace);

    store.add(opCtx, createDeletionTask(NamespaceString{"one"}, uuid, 0, 10));
    store.add(opCtx, createDeletionTask(NamespaceString{"two"}, uuid, 10, 20));
    store.add(opCtx, createDeletionTask(NamespaceString{"three"}, uuid, 40, 50));

    ASSERT_EQ(store.count(opCtx), 3);

    const auto wrongUuid = UUID::gen();
    auto range = ChunkRange{BSON("_id" << 5), BSON("_id" << 15)};
    auto results = store.count(opCtx, migrationutil::overlappingRangeQuery(range, wrongUuid));
    ASSERT_EQ(results, 0);
    ASSERT_FALSE(migrationutil::checkForConflictingDeletions(opCtx, range, wrongUuid));
}

TEST_F(MigrationUtilsTest, TestSubmitRangeDeletionTask) {
    auto opCtx = operationContext();

    DBDirectClient client(opCtx);
    client.insert(kNss.toString(), BSON("_id" << 5));

    auto uuid = getCollectionUuid(opCtx, kNss);
    auto deletionTask = createDeletionTask(kNss, uuid, 0, 10);

    auto result = stdx::async(stdx::launch::async, [this] { respondToMetadataRefreshRequests(); });
    forceShardFilteringMetadataRefresh(opCtx, kNss, true);

    auto taskValid = migrationutil::submitRangeDeletionTask(opCtx, deletionTask);
    ASSERT(taskValid);
}

TEST_F(MigrationUtilsTest, TestSubmitRangeNoCollection) {
    auto opCtx = operationContext();

    DBDirectClient client(opCtx);
    client.insert(kNss.toString(), BSON("_id" << 5));

    auto uuid = getCollectionUuid(opCtx, kNss);
    auto deletionTask = createDeletionTask(kNss, uuid, 0, 10);

    ASSERT(client.dropCollection(kNss.toString()));

    auto result = stdx::async(stdx::launch::async, [this] { respondToMetadataRefreshRequests(); });
    forceShardFilteringMetadataRefresh(opCtx, kNss, true);

    auto taskValid = migrationutil::submitRangeDeletionTask(opCtx, deletionTask);
    ASSERT_FALSE(taskValid);
}

TEST_F(MigrationUtilsTest, TestSubmitRangeNoMetadata) {
    auto opCtx = operationContext();

    DBDirectClient client(opCtx);
    client.insert(kNss.toString(), BSON("_id" << 5));

    auto uuid = getCollectionUuid(opCtx, kNss);
    auto deletionTask = createDeletionTask(kNss, uuid, 0, 10);

    auto taskValid = migrationutil::submitRangeDeletionTask(opCtx, deletionTask);
    ASSERT_FALSE(taskValid);
}

TEST_F(MigrationUtilsTest, TestSubmitRangeWrongUUID) {
    auto opCtx = operationContext();

    DBDirectClient client(opCtx);

    client.insert(kNss.toString(), BSON("_id" << 5));

    const auto wrongUuid = UUID::gen();
    auto deletionTask = createDeletionTask(kNss, wrongUuid, 0, 10);

    auto taskValid = migrationutil::submitRangeDeletionTask(opCtx, deletionTask);
    ASSERT_FALSE(taskValid);
}

TEST_F(MigrationUtilsTest, TestInvalidRangeIsDeleted) {
    auto opCtx = operationContext();
    const auto uuid = UUID::gen();

    PersistentTaskStore<RangeDeletionTask> store(opCtx, NamespaceString::kRangeDeletionNamespace);

    store.add(opCtx, createDeletionTask(kNss, uuid, 10, 20, false));

    ASSERT_EQ(store.count(opCtx), 1);

    auto result = stdx::async(stdx::launch::async, [this] { respondToMetadataRefreshRequests(); });
    migrationutil::submitPendingDeletions(opCtx);

    ASSERT_EQ(store.count(opCtx), 0);
}

}  // namespace
}  // namespace mongo
