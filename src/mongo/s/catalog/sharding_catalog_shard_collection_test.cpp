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

#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/task_executor.h"
#include "mongo/rpc/metadata/tracking_metadata.h"
#include "mongo/s/catalog/sharding_catalog_manager.h"
#include "mongo/s/catalog/type_changelog.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog/type_locks.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/config_server_test_fixture.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/stdx/future.h"
#include "mongo/transport/mock_session.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"
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

const ShardId testPrimaryShard = ShardId("shard0");

class ShardCollectionTest : public ConfigServerTestFixture {
public:
    void expectCount(const HostAndPort& receivingHost,
                     const NamespaceString& expectedNs,
                     const BSONObj& expectedQuery,
                     const StatusWith<long long>& response) {
        onCommand([&](const RemoteCommandRequest& request) {
            ASSERT_EQUALS(receivingHost, request.target);
            string cmdName = request.cmdObj.firstElement().fieldName();

            ASSERT_EQUALS("count", cmdName);

            const NamespaceString nss(request.dbname, request.cmdObj.firstElement().String());
            ASSERT_EQUALS(expectedNs.toString(), nss.toString());

            if (expectedQuery.isEmpty()) {
                auto queryElem = request.cmdObj["query"];
                ASSERT_TRUE(queryElem.eoo() || queryElem.Obj().isEmpty());
            } else {
                ASSERT_BSONOBJ_EQ(expectedQuery, request.cmdObj["query"].Obj());
            }

            if (response.isOK()) {
                return BSON("ok" << 1 << "n" << response.getValue());
            }

            BSONObjBuilder responseBuilder;
            Command::appendCommandStatus(responseBuilder, response.getStatus());
            return responseBuilder.obj();
        });
    }

private:
    const HostAndPort configHost{"configHost1"};
    const ConnectionString configCS{ConnectionString::forReplicaSet("configReplSet", {configHost})};
    const HostAndPort clientHost{"clientHost1"};
};

TEST_F(ShardCollectionTest, anotherMongosSharding) {
    const auto nss = NamespaceString("db1.foo");

    ShardType shard;
    shard.setName("shard0");
    shard.setHost("shardHost");
    ASSERT_OK(setupShards(vector<ShardType>{shard}));

    setupDatabase(nss.db().toString(), shard.getName(), true);

    // Set up chunks in the collection, indicating that another mongos must have already started
    // sharding the collection.
    ChunkType chunk;
    chunk.setNS(nss.ns());
    chunk.setVersion(ChunkVersion(2, 0, OID::gen()));
    chunk.setShard(shard.getName());
    chunk.setMin(BSON("_id" << 1));
    chunk.setMax(BSON("_id" << 5));
    ASSERT_OK(setupChunks({chunk}));

    ShardKeyPattern shardKeyPattern(BSON("_id" << 1));
    BSONObj defaultCollation;

    ASSERT_THROWS_CODE(ShardingCatalogManager::get(operationContext())
                           ->shardCollection(operationContext(),
                                             nss.ns(),
                                             boost::none,  // UUID
                                             shardKeyPattern,
                                             defaultCollation,
                                             false,
                                             vector<BSONObj>{},
                                             false,
                                             testPrimaryShard),
                       AssertionException,
                       ErrorCodes::ManualInterventionRequired);
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

    ASSERT_OK(setupShards(vector<ShardType>{shard}));

    const auto nss = NamespaceString("db1.foo");

    setupDatabase(nss.db().toString(), shard.getName(), true);

    ShardKeyPattern shardKeyPattern(BSON("_id" << 1));
    BSONObj defaultCollation;

    // Now start actually sharding the collection.
    auto future = launchAsync([&] {
        ON_BLOCK_EXIT([&] { Client::destroy(); });
        Client::initThreadIfNotAlready("Test");
        auto opCtx = cc().makeOperationContext();
        ShardingCatalogManager::get(operationContext())
            ->shardCollection(opCtx.get(),
                              nss.ns(),
                              boost::none,  // UUID
                              shardKeyPattern,
                              defaultCollation,
                              false,
                              vector<BSONObj>{},
                              false,
                              testPrimaryShard);
    });

    // Report that no documents exist for the given collection on the primary shard
    expectCount(shardHost, nss, BSONObj(), 0);

    // Expect the set shard version for that namespace.
    // We do not check for a specific ChunkVersion, because we cannot easily know the OID that was
    // generated by shardCollection for the first chunk.
    // TODO SERVER-29451: add hooks to the mock storage engine to expect reads and writes.
    expectSetShardVersion(shardHost, shard, nss, boost::none /* expected ChunkVersion */);

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

    ASSERT_OK(setupShards(vector<ShardType>{shard0, shard1, shard2}));

    const auto nss = NamespaceString("db1.foo");
    string ns = "db1.foo";

    DatabaseType db;
    db.setName("db1");
    db.setPrimary(shard0.getName());
    db.setSharded(true);
    setupDatabase(nss.db().toString(), shard0.getName(), true);

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

    BSONObj defaultCollation;

    // Now start actually sharding the collection.
    auto future = launchAsync([&] {
        // TODO: can we mock the ShardRegistry to return these?
        set<ShardId> shards{shard0.getName(), shard1.getName(), shard2.getName()};

        ON_BLOCK_EXIT([&] { Client::destroy(); });
        Client::initThreadIfNotAlready("Test");
        auto opCtx = cc().makeOperationContext();
        ShardingCatalogManager::get(operationContext())
            ->shardCollection(opCtx.get(),
                              ns,
                              boost::none,  // UUID
                              keyPattern,
                              defaultCollation,
                              true,
                              vector<BSONObj>{splitPoint0, splitPoint1, splitPoint2, splitPoint3},
                              true,
                              testPrimaryShard);
    });

    // Expect the set shard version for that namespace
    // We do not check for a specific ChunkVersion, because we cannot easily know the OID that was
    // generated by shardCollection for the first chunk.
    // TODO SERVER-29451: add hooks to the mock storage engine to expect reads and writes.
    expectSetShardVersion(
        shard0Host, shard0, NamespaceString(ns), boost::none /* expected ChunkVersion */);

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

    ASSERT_OK(setupShards(vector<ShardType>{shard}));

    const auto nss = NamespaceString("db1.foo");
    string ns = "db1.foo";

    setupDatabase(nss.db().toString(), shard.getName(), true);

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

    BSONObj defaultCollation;

    // Now start actually sharding the collection.
    auto future = launchAsync([&] {
        ON_BLOCK_EXIT([&] { Client::destroy(); });
        Client::initThreadIfNotAlready("Test");
        auto opCtx = cc().makeOperationContext();
        ShardingCatalogManager::get(operationContext())
            ->shardCollection(opCtx.get(),
                              ns,
                              boost::none,  // UUID
                              keyPattern,
                              defaultCollation,
                              false,
                              vector<BSONObj>{},
                              false,
                              testPrimaryShard);
    });

    // Report that documents exist for the given collection on the primary shard, so that calling
    // splitVector is required for calculating the initial split points.
    expectCount(shardHost, NamespaceString(ns), BSONObj(), 1000);

    // Respond to the splitVector command sent to the shard to figure out initial split points
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(shardHost, request.target);
        string cmdName = request.cmdObj.firstElement().fieldName();
        ASSERT_EQUALS("splitVector", cmdName);
        ASSERT_EQUALS(ns, request.cmdObj["splitVector"].String());  // splitVector uses full ns

        ASSERT_BSONOBJ_EQ(keyPattern.toBSON(), request.cmdObj["keyPattern"].Obj());
        ASSERT_BSONOBJ_EQ(keyPattern.getKeyPattern().globalMin(), request.cmdObj["min"].Obj());
        ASSERT_BSONOBJ_EQ(keyPattern.getKeyPattern().globalMax(), request.cmdObj["max"].Obj());
        ASSERT_EQUALS(64 * 1024 * 1024ULL,
                      static_cast<uint64_t>(request.cmdObj["maxChunkSizeBytes"].numberLong()));
        ASSERT_EQUALS(0, request.cmdObj["maxSplitPoints"].numberLong());
        ASSERT_EQUALS(0, request.cmdObj["maxChunkObjects"].numberLong());

        ASSERT_BSONOBJ_EQ(
            ReadPreferenceSetting(ReadPreference::PrimaryPreferred).toContainingBSON(),
            rpc::TrackingMetadata::removeTrackingData(request.metadata));

        return BSON("ok" << 1 << "splitKeys"
                         << BSON_ARRAY(splitPoint0 << splitPoint1 << splitPoint2 << splitPoint3));
    });

    // Expect the set shard version for that namespace
    // We do not check for a specific ChunkVersion, because we cannot easily know the OID that was
    // generated by shardCollection for the first chunk.
    // TODO SERVER-29451: add hooks to the mock storage engine to expect reads and writes.
    expectSetShardVersion(shardHost, shard, NamespaceString(ns), boost::none);

    future.timed_get(kFutureTimeout);
}

}  // namespace
}  // namespace mongo
