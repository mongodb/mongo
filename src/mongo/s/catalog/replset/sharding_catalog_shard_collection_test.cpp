/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include <set>
#include <string>
#include <vector>

#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/task_executor.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/rpc/metadata/server_selection_metadata.h"
#include "mongo/s/catalog/dist_lock_manager_mock.h"
#include "mongo/s/catalog/replset/sharding_catalog_client_impl.h"
#include "mongo/s/catalog/replset/sharding_catalog_test_fixture.h"
#include "mongo/s/catalog/type_changelog.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/stdx/future.h"
#include "mongo/util/log.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {

using executor::NetworkInterfaceMock;
using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;
using executor::TaskExecutor;
using std::set;
using std::string;
using std::vector;
using unittest::assertGet;

const BSONObj kReplMetadata();
const BSONObj kSecondaryOkMetadata{rpc::ServerSelectionMetadata(true, boost::none).toBSON()};

const BSONObj kReplSecondaryOkMetadata{[] {
    BSONObjBuilder o;
    o.appendElements(rpc::ServerSelectionMetadata(true, boost::none).toBSON());
    o.append(rpc::kReplSetMetadataFieldName, 1);
    return o.obj();
}()};

class ShardCollectionTest : public ShardingCatalogTestFixture {
public:
    void setUp() override {
        ShardingCatalogTestFixture::setUp();
        configTargeter()->setFindHostReturnValue(configHost);
        configTargeter()->setConnectionStringReturnValue(configCS);
        setRemote(clientHost);
    }

    void expectGetDatabase(const DatabaseType& expectedDb) {
        onFindCommand([&](const RemoteCommandRequest& request) {
            ASSERT_EQUALS(configHost, request.target);
            ASSERT_EQUALS(kReplSecondaryOkMetadata, request.metadata);

            const NamespaceString nss(request.dbname, request.cmdObj.firstElement().String());
            ASSERT_EQ(DatabaseType::ConfigNS, nss.ns());

            auto query = assertGet(QueryRequest::makeFromFindCommand(nss, request.cmdObj, false));

            ASSERT_EQ(DatabaseType::ConfigNS, query->ns());
            ASSERT_EQ(BSON(DatabaseType::name(expectedDb.getName())), query->getFilter());
            ASSERT_EQ(BSONObj(), query->getSort());
            ASSERT_EQ(1, query->getLimit().get());

            checkReadConcern(request.cmdObj, Timestamp(0, 0), repl::OpTime::kUninitializedTerm);

            return vector<BSONObj>{expectedDb.toBSON()};
        });
    }

    // Intercepts network request to upsert a new chunk definition to the config.chunks collection.
    // Since the catalog manager cannot predict the epoch that will be assigned the new chunk,
    // returns the chunk version that is sent in the upsert.
    ChunkVersion expectCreateChunk(const ChunkType& expectedChunk) {
        ChunkVersion actualVersion;

        onCommand([&](const RemoteCommandRequest& request) {
            ASSERT_EQUALS(configHost, request.target);
            ASSERT_EQUALS("config", request.dbname);

            ASSERT_EQUALS(BSON(rpc::kReplSetMetadataFieldName << 1), request.metadata);

            BatchedInsertRequest actualBatchedInsert;
            std::string errmsg;
            ASSERT_TRUE(actualBatchedInsert.parseBSON(request.dbname, request.cmdObj, &errmsg));
            ASSERT_EQUALS(ChunkType::ConfigNS, actualBatchedInsert.getNS().ns());
            auto inserts = actualBatchedInsert.getDocuments();
            ASSERT_EQUALS(1U, inserts.size());

            BSONObj chunkObj = inserts.front();
            ASSERT_EQUALS(expectedChunk.getName(), chunkObj["_id"].String());
            ASSERT_EQUALS(Timestamp(expectedChunk.getVersion().toLong()),
                          chunkObj[ChunkType::DEPRECATED_lastmod()].timestamp());
            // Can't check the chunk version's epoch b/c they won't match since it's a randomly
            // generated OID so just check that the field exists and is *a* OID.
            ASSERT_EQUALS(jstOID, chunkObj[ChunkType::DEPRECATED_epoch()].type());
            ASSERT_EQUALS(expectedChunk.getNS(), chunkObj[ChunkType::ns()].String());
            ASSERT_EQUALS(expectedChunk.getMin(), chunkObj[ChunkType::min()].Obj());
            ASSERT_EQUALS(expectedChunk.getMax(), chunkObj[ChunkType::max()].Obj());
            ASSERT_EQUALS(expectedChunk.getShard(), chunkObj[ChunkType::shard()].String());

            actualVersion = ChunkVersion::fromBSON(chunkObj);

            BatchedCommandResponse response;
            response.setOk(true);
            response.setNModified(1);

            return response.toBSON();
        });

        return actualVersion;
    }

    void expectReloadChunks(const std::string& ns, const vector<ChunkType>& chunks) {
        onFindCommand([&](const RemoteCommandRequest& request) {
            ASSERT_EQUALS(configHost, request.target);
            ASSERT_EQUALS(kReplSecondaryOkMetadata, request.metadata);

            const NamespaceString nss(request.dbname, request.cmdObj.firstElement().String());
            ASSERT_EQ(nss.ns(), ChunkType::ConfigNS);

            auto query = assertGet(QueryRequest::makeFromFindCommand(nss, request.cmdObj, false));
            BSONObj expectedQuery =
                BSON(ChunkType::ns(ns) << ChunkType::DEPRECATED_lastmod << GTE << Timestamp());
            BSONObj expectedSort = BSON(ChunkType::DEPRECATED_lastmod() << 1);

            ASSERT_EQ(ChunkType::ConfigNS, query->ns());
            ASSERT_EQ(expectedQuery, query->getFilter());
            ASSERT_EQ(expectedSort, query->getSort());
            ASSERT_FALSE(query->getLimit().is_initialized());

            checkReadConcern(request.cmdObj, Timestamp(0, 0), repl::OpTime::kUninitializedTerm);

            vector<BSONObj> chunksToReturn;

            std::transform(chunks.begin(),
                           chunks.end(),
                           std::back_inserter(chunksToReturn),
                           [](const ChunkType& chunk) { return chunk.toBSON(); });
            return chunksToReturn;
        });
    }

    void expectUpdateCollection(const CollectionType& expectedCollection) {
        onCommand([&](const RemoteCommandRequest& request) {
            ASSERT_EQUALS(configHost, request.target);
            ASSERT_EQUALS("config", request.dbname);

            ASSERT_EQUALS(BSON(rpc::kReplSetMetadataFieldName << 1), request.metadata);

            BatchedUpdateRequest actualBatchedUpdate;
            std::string errmsg;
            ASSERT_TRUE(actualBatchedUpdate.parseBSON(request.dbname, request.cmdObj, &errmsg));
            ASSERT_EQUALS(CollectionType::ConfigNS, actualBatchedUpdate.getNS().ns());
            auto updates = actualBatchedUpdate.getUpdates();
            ASSERT_EQUALS(1U, updates.size());
            auto update = updates.front();

            ASSERT_TRUE(update->getUpsert());
            ASSERT_FALSE(update->getMulti());
            ASSERT_EQUALS(BSON(CollectionType::fullNs(expectedCollection.getNs().toString())),
                          update->getQuery());
            ASSERT_EQUALS(expectedCollection.toBSON(), update->getUpdateExpr());

            BatchedCommandResponse response;
            response.setOk(true);
            response.setNModified(1);

            return response.toBSON();
        });
    }

protected:
    const HostAndPort configHost{"configHost1"};
    const ConnectionString configCS{ConnectionString::forReplicaSet("configReplSet", {configHost})};
    const HostAndPort clientHost{"clientHost1"};
};

TEST_F(ShardCollectionTest, distLockFails) {
    distLock()->expectLock(
        [](StringData name,
           StringData whyMessage,
           Milliseconds waitFor,
           Milliseconds lockTryInterval) {
            ASSERT_EQUALS("test.foo", name);
            ASSERT_EQUALS("shardCollection", whyMessage);
        },
        Status(ErrorCodes::LockBusy, "lock already held"));

    ShardKeyPattern keyPattern(BSON("_id" << 1));
    ASSERT_EQUALS(
        ErrorCodes::LockBusy,
        catalogClient()->shardCollection(
            operationContext(), "test.foo", keyPattern, false, vector<BSONObj>{}, set<ShardId>{}));
}

TEST_F(ShardCollectionTest, anotherMongosSharding) {
    ShardType shard;
    shard.setName("shard0");
    shard.setHost("shardHost");

    setupShards(vector<ShardType>{shard});

    DatabaseType db;
    db.setName("db1");
    db.setPrimary(shard.getName());
    db.setSharded(true);

    string ns = "db1.foo";

    ShardKeyPattern keyPattern(BSON("_id" << 1));

    distLock()->expectLock(
        [&](StringData name,
            StringData whyMessage,
            Milliseconds waitFor,
            Milliseconds lockTryInterval) {
            ASSERT_EQUALS(ns, name);
            ASSERT_EQUALS("shardCollection", whyMessage);
        },
        Status::OK());

    auto future = launchAsync([&] {
        Client::initThreadIfNotAlready();
        ASSERT_EQUALS(
            ErrorCodes::AlreadyInitialized,
            catalogClient()->shardCollection(
                operationContext(), ns, keyPattern, false, vector<BSONObj>{}, set<ShardId>{}));
    });

    expectGetDatabase(db);

    // Report that chunks exist for the given collection, indicating that another mongos must have
    // already started sharding the collection.
    expectCount(configHost, NamespaceString(ChunkType::ConfigNS), BSON(ChunkType::ns(ns)), 1);

    future.timed_get(kFutureTimeout);
}

TEST_F(ShardCollectionTest, noInitialChunksOrData) {
    // Initial setup
    const HostAndPort shardHost{"shardHost"};
    ShardType shard;
    shard.setName("shard0");
    shard.setHost(shardHost.toString());

    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        stdx::make_unique<RemoteCommandTargeterMock>());
    targeter->setConnectionStringReturnValue(ConnectionString(shardHost));
    targeter->setFindHostReturnValue(shardHost);
    targeterFactory()->addTargeterToReturn(ConnectionString(shardHost), std::move(targeter));

    setupShards(vector<ShardType>{shard});

    string ns = "db1.foo";

    DatabaseType db;
    db.setName("db1");
    db.setPrimary(shard.getName());
    db.setSharded(true);

    ShardKeyPattern keyPattern(BSON("_id" << 1));

    ChunkType expectedChunk;
    expectedChunk.setNS(ns);
    expectedChunk.setMin(keyPattern.getKeyPattern().globalMin());
    expectedChunk.setMax(keyPattern.getKeyPattern().globalMax());
    expectedChunk.setShard(shard.getName());
    expectedChunk.setVersion(ChunkVersion(1, 0, OID::gen()));

    distLock()->expectLock(
        [&](StringData name,
            StringData whyMessage,
            Milliseconds waitFor,
            Milliseconds lockTryInterval) {
            ASSERT_EQUALS(ns, name);
            ASSERT_EQUALS("shardCollection", whyMessage);
        },
        Status::OK());

    // Now start actually sharding the collection.
    auto future = launchAsync([&] {
        Client::initThreadIfNotAlready();
        ASSERT_OK(catalogClient()->shardCollection(
            operationContext(), ns, keyPattern, false, vector<BSONObj>{}, set<ShardId>{}));
    });

    expectGetDatabase(db);

    // Report that no chunks exist for the given collection
    expectCount(configHost, NamespaceString(ChunkType::ConfigNS), BSON(ChunkType::ns(ns)), 0);

    // Respond to write to change log
    {
        BSONObj logChangeDetail =
            BSON("shardKey" << keyPattern.toBSON() << "collection" << ns << "primary"
                            << shard.getName() + ":" + shard.getHost()
                            << "initShards"
                            << BSONArray()
                            << "numChunks"
                            << 1);
        expectChangeLogCreate(configHost, BSON("ok" << 1));
        expectChangeLogInsert(
            configHost, network()->now(), "shardCollection.start", ns, logChangeDetail);
    }

    // Report that no documents exist for the given collection on the primary shard
    expectCount(shardHost, NamespaceString(ns), BSONObj(), 0);

    // Handle the write to create the initial chunk.
    ChunkVersion actualVersion = expectCreateChunk(expectedChunk);

    // Since the generated epoch OID will not match the one we initialized expectedChunk with,
    // update the stored version in expectedChunk so that it matches what was actually
    // written, to avoid problems relating to non-matching epochs down the road.
    expectedChunk.setVersion(actualVersion);

    // Handle the query to load the newly created chunk
    expectReloadChunks(ns, {expectedChunk});

    CollectionType expectedCollection;
    expectedCollection.setNs(NamespaceString(ns));
    expectedCollection.setEpoch(expectedChunk.getVersion().epoch());
    expectedCollection.setUpdatedAt(
        Date_t::fromMillisSinceEpoch(expectedChunk.getVersion().toLong()));
    expectedCollection.setKeyPattern(keyPattern.toBSON());
    expectedCollection.setUnique(false);

    // Handle the update to the collection entry in config.collectinos.
    expectUpdateCollection(expectedCollection);

    // Expect the set shard version for that namespace
    expectSetShardVersion(shardHost, shard, NamespaceString(ns), actualVersion);

    // Respond to request to write final changelog entry indicating success.
    expectChangeLogInsert(configHost,
                          network()->now(),
                          "shardCollection.end",
                          ns,
                          BSON("version" << actualVersion.toString()));

    future.timed_get(kFutureTimeout);
}

TEST_F(ShardCollectionTest, withInitialChunks) {
    // Initial setup
    const HostAndPort shard0Host{"shardHost0"};
    const HostAndPort shard1Host{"shardHost1"};
    const HostAndPort shard2Host{"shardHost2"};

    ShardType shard0;
    shard0.setName("shard0");
    shard0.setHost(shard0Host.toString());

    ShardType shard1;
    shard1.setName("shard1");
    shard1.setHost(shard1Host.toString());

    ShardType shard2;
    shard2.setName("shard2");
    shard2.setHost(shard2Host.toString());

    std::unique_ptr<RemoteCommandTargeterMock> targeter0(
        stdx::make_unique<RemoteCommandTargeterMock>());
    std::unique_ptr<RemoteCommandTargeterMock> targeter1(
        stdx::make_unique<RemoteCommandTargeterMock>());
    std::unique_ptr<RemoteCommandTargeterMock> targeter2(
        stdx::make_unique<RemoteCommandTargeterMock>());
    targeter0->setConnectionStringReturnValue(ConnectionString(shard0Host));
    targeter0->setFindHostReturnValue(shard0Host);
    targeterFactory()->addTargeterToReturn(ConnectionString(shard0Host), std::move(targeter0));
    targeter1->setConnectionStringReturnValue(ConnectionString(shard1Host));
    targeter1->setFindHostReturnValue(shard1Host);
    targeterFactory()->addTargeterToReturn(ConnectionString(shard1Host), std::move(targeter1));
    targeter2->setConnectionStringReturnValue(ConnectionString(shard2Host));
    targeter2->setFindHostReturnValue(shard2Host);
    targeterFactory()->addTargeterToReturn(ConnectionString(shard2Host), std::move(targeter2));

    setupShards(vector<ShardType>{shard0, shard1, shard2});

    string ns = "db1.foo";

    DatabaseType db;
    db.setName("db1");
    db.setPrimary(shard0.getName());
    db.setSharded(true);

    ShardKeyPattern keyPattern(BSON("_id" << 1));

    BSONObj splitPoint0 = BSON("_id" << 1);
    BSONObj splitPoint1 = BSON("_id" << 100);
    BSONObj splitPoint2 = BSON("_id" << 200);
    BSONObj splitPoint3 = BSON("_id" << 300);

    ChunkVersion expectedVersion(1, 0, OID::gen());

    ChunkType expectedChunk0;
    expectedChunk0.setNS(ns);
    expectedChunk0.setShard(shard0.getName());
    expectedChunk0.setMin(keyPattern.getKeyPattern().globalMin());
    expectedChunk0.setMax(splitPoint0);
    expectedChunk0.setVersion(expectedVersion);
    expectedVersion.incMinor();

    ChunkType expectedChunk1;
    expectedChunk1.setNS(ns);
    expectedChunk1.setShard(shard1.getName());
    expectedChunk1.setMin(splitPoint0);
    expectedChunk1.setMax(splitPoint1);
    expectedChunk1.setVersion(expectedVersion);
    expectedVersion.incMinor();

    ChunkType expectedChunk2;
    expectedChunk2.setNS(ns);
    expectedChunk2.setShard(shard2.getName());
    expectedChunk2.setMin(splitPoint1);
    expectedChunk2.setMax(splitPoint2);
    expectedChunk2.setVersion(expectedVersion);
    expectedVersion.incMinor();

    ChunkType expectedChunk3;
    expectedChunk3.setNS(ns);
    expectedChunk3.setShard(shard0.getName());
    expectedChunk3.setMin(splitPoint2);
    expectedChunk3.setMax(splitPoint3);
    expectedChunk3.setVersion(expectedVersion);
    expectedVersion.incMinor();

    ChunkType expectedChunk4;
    expectedChunk4.setNS(ns);
    expectedChunk4.setShard(shard1.getName());
    expectedChunk4.setMin(splitPoint3);
    expectedChunk4.setMax(keyPattern.getKeyPattern().globalMax());
    expectedChunk4.setVersion(expectedVersion);

    vector<ChunkType> expectedChunks{
        expectedChunk0, expectedChunk1, expectedChunk2, expectedChunk3, expectedChunk4};

    distLock()->expectLock(
        [&](StringData name,
            StringData whyMessage,
            Milliseconds waitFor,
            Milliseconds lockTryInterval) {
            ASSERT_EQUALS(ns, name);
            ASSERT_EQUALS("shardCollection", whyMessage);
        },
        Status::OK());

    // Now start actually sharding the collection.
    auto future = launchAsync([&] {
        Client::initThreadIfNotAlready();
        set<ShardId> shards{shard0.getName(), shard1.getName(), shard2.getName()};
        ASSERT_OK(catalogClient()->shardCollection(
            operationContext(),
            ns,
            keyPattern,
            true,
            vector<BSONObj>{splitPoint0, splitPoint1, splitPoint2, splitPoint3},
            shards));
    });

    expectGetDatabase(db);

    // Report that no chunks exist for the given collection
    expectCount(configHost, NamespaceString(ChunkType::ConfigNS), BSON(ChunkType::ns(ns)), 0);

    // Respond to write to change log
    {
        BSONObj logChangeDetail =
            BSON("shardKey" << keyPattern.toBSON() << "collection" << ns << "primary"
                            << shard0.getName() + ":" + shard0.getHost()
                            << "initShards"
                            << BSON_ARRAY(shard0.getName() << shard1.getName() << shard2.getName())
                            << "numChunks"
                            << (int)expectedChunks.size());
        expectChangeLogCreate(configHost, BSON("ok" << 1));
        expectChangeLogInsert(
            configHost, network()->now(), "shardCollection.start", ns, logChangeDetail);
    }

    for (auto& expectedChunk : expectedChunks) {
        // Handle the write to create the initial chunk.
        ChunkVersion actualVersion = expectCreateChunk(expectedChunk);

        // Since the generated epoch OID will not match the one we initialized expectedChunk with
        // update the stored version in expectedChunk so that it matches what was actually
        // written, to avoid problems relating to non-matching epochs down the road.
        expectedChunk.setVersion(actualVersion);
    }

    // Handle the query to load the newly created chunk
    expectReloadChunks(ns, expectedChunks);

    CollectionType expectedCollection;
    expectedCollection.setNs(NamespaceString(ns));
    expectedCollection.setEpoch(expectedChunks[4].getVersion().epoch());
    expectedCollection.setUpdatedAt(
        Date_t::fromMillisSinceEpoch(expectedChunks[4].getVersion().toLong()));
    expectedCollection.setKeyPattern(keyPattern.toBSON());
    expectedCollection.setUnique(true);

    // Handle the update to the collection entry in config.collectinos.
    expectUpdateCollection(expectedCollection);

    // Expect the set shard version for that namespace
    expectSetShardVersion(shard0Host, shard0, NamespaceString(ns), expectedChunks[4].getVersion());

    // Respond to request to write final changelog entry indicating success.
    expectChangeLogInsert(configHost,
                          network()->now(),
                          "shardCollection.end",
                          ns,
                          BSON("version" << expectedChunks[4].getVersion().toString()));

    future.timed_get(kFutureTimeout);
}

TEST_F(ShardCollectionTest, withInitialData) {
    // Initial setup
    const HostAndPort shardHost{"shardHost"};
    ShardType shard;
    shard.setName("shard0");
    shard.setHost(shardHost.toString());

    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        stdx::make_unique<RemoteCommandTargeterMock>());
    targeter->setConnectionStringReturnValue(ConnectionString(shardHost));
    targeter->setFindHostReturnValue(shardHost);
    targeterFactory()->addTargeterToReturn(ConnectionString(shardHost), std::move(targeter));

    setupShards(vector<ShardType>{shard});

    string ns = "db1.foo";

    DatabaseType db;
    db.setName("db1");
    db.setPrimary(shard.getName());
    db.setSharded(true);

    ShardKeyPattern keyPattern(BSON("_id" << 1));

    BSONObj splitPoint0 = BSON("_id" << 1);
    BSONObj splitPoint1 = BSON("_id" << 100);
    BSONObj splitPoint2 = BSON("_id" << 200);
    BSONObj splitPoint3 = BSON("_id" << 300);

    ChunkVersion expectedVersion(1, 0, OID::gen());

    ChunkType expectedChunk0;
    expectedChunk0.setNS(ns);
    expectedChunk0.setShard(shard.getName());
    expectedChunk0.setMin(keyPattern.getKeyPattern().globalMin());
    expectedChunk0.setMax(splitPoint0);
    expectedChunk0.setVersion(expectedVersion);
    expectedVersion.incMinor();

    ChunkType expectedChunk1;
    expectedChunk1.setNS(ns);
    expectedChunk1.setShard(shard.getName());
    expectedChunk1.setMin(splitPoint0);
    expectedChunk1.setMax(splitPoint1);
    expectedChunk1.setVersion(expectedVersion);
    expectedVersion.incMinor();

    ChunkType expectedChunk2;
    expectedChunk2.setNS(ns);
    expectedChunk2.setShard(shard.getName());
    expectedChunk2.setMin(splitPoint1);
    expectedChunk2.setMax(splitPoint2);
    expectedChunk2.setVersion(expectedVersion);
    expectedVersion.incMinor();

    ChunkType expectedChunk3;
    expectedChunk3.setNS(ns);
    expectedChunk3.setShard(shard.getName());
    expectedChunk3.setMin(splitPoint2);
    expectedChunk3.setMax(splitPoint3);
    expectedChunk3.setVersion(expectedVersion);
    expectedVersion.incMinor();

    ChunkType expectedChunk4;
    expectedChunk4.setNS(ns);
    expectedChunk4.setShard(shard.getName());
    expectedChunk4.setMin(splitPoint3);
    expectedChunk4.setMax(keyPattern.getKeyPattern().globalMax());
    expectedChunk4.setVersion(expectedVersion);

    vector<ChunkType> expectedChunks{
        expectedChunk0, expectedChunk1, expectedChunk2, expectedChunk3, expectedChunk4};

    distLock()->expectLock(
        [&](StringData name,
            StringData whyMessage,
            Milliseconds waitFor,
            Milliseconds lockTryInterval) {
            ASSERT_EQUALS(ns, name);
            ASSERT_EQUALS("shardCollection", whyMessage);
        },
        Status::OK());

    // Now start actually sharding the collection.
    auto future = launchAsync([&] {
        Client::initThreadIfNotAlready();
        ASSERT_OK(catalogClient()->shardCollection(
            operationContext(), ns, keyPattern, false, vector<BSONObj>{}, set<ShardId>{}));
    });

    expectGetDatabase(db);

    // Report that no chunks exist for the given collection
    expectCount(configHost, NamespaceString(ChunkType::ConfigNS), BSON(ChunkType::ns(ns)), 0);

    // Respond to write to change log
    {
        BSONObj logChangeDetail =
            BSON("shardKey" << keyPattern.toBSON() << "collection" << ns << "primary"
                            << shard.getName() + ":" + shard.getHost()
                            << "initShards"
                            << BSONArray()
                            << "numChunks"
                            << 1);
        expectChangeLogCreate(configHost, BSON("ok" << 1));
        expectChangeLogInsert(
            configHost, network()->now(), "shardCollection.start", ns, logChangeDetail);
    }

    // Report that documents exist for the given collection on the primary shard, so that calling
    // splitVector is required for calculating the initial split points.
    expectCount(shardHost, NamespaceString(ns), BSONObj(), 1000);

    // Respond to the splitVector command sent to the shard to figure out initial split points
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(shardHost, request.target);
        string cmdName = request.cmdObj.firstElement().fieldName();
        ASSERT_EQUALS("splitVector", cmdName);
        ASSERT_EQUALS(ns, request.cmdObj["splitVector"].String());  // splitVector uses full ns

        ASSERT_EQUALS(keyPattern.toBSON(), request.cmdObj["keyPattern"].Obj());
        ASSERT_EQUALS(keyPattern.getKeyPattern().globalMin(), request.cmdObj["min"].Obj());
        ASSERT_EQUALS(keyPattern.getKeyPattern().globalMax(), request.cmdObj["max"].Obj());
        ASSERT_EQUALS(64 * 1024 * 1024ULL,
                      static_cast<uint64_t>(request.cmdObj["maxChunkSizeBytes"].numberLong()));
        ASSERT_EQUALS(0, request.cmdObj["maxSplitPoints"].numberLong());
        ASSERT_EQUALS(0, request.cmdObj["maxChunkObjects"].numberLong());

        ASSERT_EQUALS(rpc::ServerSelectionMetadata(true, boost::none).toBSON(), request.metadata);

        return BSON("ok" << 1 << "splitKeys"
                         << BSON_ARRAY(splitPoint0 << splitPoint1 << splitPoint2 << splitPoint3));
    });

    for (auto& expectedChunk : expectedChunks) {
        // Handle the write to create the initial chunk.
        ChunkVersion actualVersion = expectCreateChunk(expectedChunk);

        // Since the generated epoch OID will not match the one we initialized expectedChunk with
        // update the stored version in expectedChunk so that it matches what was actually
        // written, to avoid problems relating to non-matching epochs down the road.
        expectedChunk.setVersion(actualVersion);
    }

    // Handle the query to load the newly created chunk
    expectReloadChunks(ns, expectedChunks);

    CollectionType expectedCollection;
    expectedCollection.setNs(NamespaceString(ns));
    expectedCollection.setEpoch(expectedChunks[4].getVersion().epoch());
    expectedCollection.setUpdatedAt(
        Date_t::fromMillisSinceEpoch(expectedChunks[4].getVersion().toLong()));
    expectedCollection.setKeyPattern(keyPattern.toBSON());
    expectedCollection.setUnique(false);

    // Handle the update to the collection entry in config.collectinos.
    expectUpdateCollection(expectedCollection);

    // Expect the set shard version for that namespace
    expectSetShardVersion(shardHost, shard, NamespaceString(ns), expectedChunks[4].getVersion());

    // Respond to request to write final changelog entry indicating success.
    expectChangeLogInsert(configHost,
                          network()->now(),
                          "shardCollection.end",
                          ns,
                          BSON("version" << expectedChunks[4].getVersion().toString()));

    future.timed_get(kFutureTimeout);
}

}  // namespace
}  // namespace mongo
