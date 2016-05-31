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

#include "mongo/bson/json.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/namespace_string.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/rpc/metadata/server_selection_metadata.h"
#include "mongo/s/catalog/dist_lock_manager_mock.h"
#include "mongo/s/catalog/replset/sharding_catalog_client_impl.h"
#include "mongo/s/catalog/replset/sharding_catalog_test_fixture.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/write_ops/batched_update_request.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {

using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;
using std::string;
using std::vector;
using unittest::assertGet;

class DropColl2ShardTest : public ShardingCatalogTestFixture {
public:
    void setUp() override {
        ShardingCatalogTestFixture::setUp();
        setRemote(_clientHost);

        configTargeter()->setFindHostReturnValue(_configHost);
        configTargeter()->setConnectionStringReturnValue(_configCS);

        distLock()->expectLock(
            [this](StringData name, StringData whyMessage, Milliseconds, Milliseconds) {
                ASSERT_EQUALS(_dropNS.ns(), name);
                ASSERT_EQUALS("drop", whyMessage);
            },
            Status::OK());

        _shard1.setName("shard0001");
        _shard1.setHost("s:1");

        _shard2.setName("shard0002");
        _shard2.setHost("s:2");

        setupShards({_shard1, _shard2});

        auto shard1Targeter = RemoteCommandTargeterMock::get(
            shardRegistry()->getShard(operationContext(), _shard1.getName())->getTargeter());
        shard1Targeter->setFindHostReturnValue(HostAndPort(_shard1.getHost()));

        auto shard2Targeter = RemoteCommandTargeterMock::get(
            shardRegistry()->getShard(operationContext(), _shard2.getName())->getTargeter());
        shard2Targeter->setFindHostReturnValue(HostAndPort(_shard2.getHost()));
    }

    void expectDrop(const ShardType& shard) {
        onCommand([this, shard](const RemoteCommandRequest& request) {
            ASSERT_EQ(HostAndPort(shard.getHost()), request.target);
            ASSERT_EQ(_dropNS.db(), request.dbname);
            ASSERT_EQ(BSON("drop" << _dropNS.coll() << "writeConcern"
                                  << BSON("w" << 0 << "wtimeout" << 0)),
                      request.cmdObj);

            ASSERT_EQUALS(rpc::makeEmptyMetadata(), request.metadata);

            return BSON("ns" << _dropNS.ns() << "ok" << 1);
        });
    }

    void expectRemoveChunksAndMarkCollectionDropped() {
        onCommand([this](const RemoteCommandRequest& request) {
            ASSERT_EQUALS(BSON(rpc::kReplSetMetadataFieldName << 1), request.metadata);
            ASSERT_EQ(_configHost, request.target);
            ASSERT_EQ("config", request.dbname);

            BSONObj expectedCmd(fromjson(R"({
                delete: "chunks",
                deletes: [{ q: { ns: "test.user" }, limit: 0 }],
                writeConcern: { w: "majority", wtimeout: 15000 },
                maxTimeMS: 30000
            })"));

            ASSERT_EQ(expectedCmd, request.cmdObj);

            return BSON("n" << 1 << "ok" << 1);
        });

        CollectionType coll;
        coll.setNs(NamespaceString("test.user"));
        coll.setDropped(true);
        coll.setEpoch(ChunkVersion::DROPPED().epoch());
        coll.setUpdatedAt(network()->now());

        expectUpdateCollection(configHost(), coll);
    }

    void expectSetShardVersionZero(const ShardType& shard) {
        expectSetShardVersion(
            HostAndPort(shard.getHost()), shard, dropNS(), ChunkVersion::DROPPED());
    }

    void expectUnsetSharding(const ShardType& shard) {
        onCommand([shard](const RemoteCommandRequest& request) {
            ASSERT_EQ(HostAndPort(shard.getHost()), request.target);
            ASSERT_EQ("admin", request.dbname);
            ASSERT_EQ(BSON("unsetSharding" << 1), request.cmdObj);

            ASSERT_EQUALS(rpc::makeEmptyMetadata(), request.metadata);

            return BSON("n" << 1 << "ok" << 1);
        });
    }

    const NamespaceString& dropNS() const {
        return _dropNS;
    }

    const ShardType& shard1() const {
        return _shard1;
    }

    const ShardType& shard2() const {
        return _shard2;
    }

    const HostAndPort& configHost() const {
        return _configHost;
    }

private:
    const HostAndPort _configHost{"TestHost1"};
    const ConnectionString _configCS{
        ConnectionString::forReplicaSet("configReplSet", {_configHost})};
    const HostAndPort _clientHost{"client:123"};
    const NamespaceString _dropNS{"test.user"};

    ShardType _shard1;
    ShardType _shard2;
};

TEST_F(DropColl2ShardTest, Basic) {
    auto future = launchAsync([this] {
        auto status = catalogClient()->dropCollection(operationContext(), dropNS());
        ASSERT_OK(status);
    });

    expectChangeLogCreate(configHost(), BSON("ok" << 1));
    expectChangeLogInsert(
        configHost(), network()->now(), "dropCollection.start", dropNS().ns(), BSONObj());

    expectGetShards({shard1(), shard2()});

    expectDrop(shard1());
    expectDrop(shard2());

    expectRemoveChunksAndMarkCollectionDropped();

    expectSetShardVersionZero(shard1());
    expectUnsetSharding(shard1());

    expectSetShardVersionZero(shard2());
    expectUnsetSharding(shard2());

    expectChangeLogInsert(
        configHost(), network()->now(), "dropCollection", dropNS().ns(), BSONObj());

    future.timed_get(kFutureTimeout);
}

TEST_F(DropColl2ShardTest, NSNotFound) {
    auto future = launchAsync([this] {
        auto status = catalogClient()->dropCollection(operationContext(), dropNS());
        ASSERT_OK(status);
    });

    expectChangeLogCreate(configHost(), BSON("ok" << 1));
    expectChangeLogInsert(
        configHost(), network()->now(), "dropCollection.start", dropNS().ns(), BSONObj());

    expectGetShards({shard1(), shard2()});

    onCommand([this](const RemoteCommandRequest& request) {
        ASSERT_EQ(HostAndPort(shard1().getHost()), request.target);
        ASSERT_EQ(dropNS().db(), request.dbname);
        ASSERT_EQ(
            BSON("drop" << dropNS().coll() << "writeConcern" << BSON("w" << 0 << "wtimeout" << 0)),
            request.cmdObj);

        ASSERT_EQUALS(rpc::makeEmptyMetadata(), request.metadata);

        return BSON("ok" << 0 << "code" << ErrorCodes::NamespaceNotFound);
    });

    onCommand([this](const RemoteCommandRequest& request) {
        ASSERT_EQ(HostAndPort(shard2().getHost()), request.target);
        ASSERT_EQ(dropNS().db(), request.dbname);
        ASSERT_EQ(
            BSON("drop" << dropNS().coll() << "writeConcern" << BSON("w" << 0 << "wtimeout" << 0)),
            request.cmdObj);

        ASSERT_EQUALS(rpc::makeEmptyMetadata(), request.metadata);

        return BSON("ok" << 0 << "code" << ErrorCodes::NamespaceNotFound);
    });

    expectRemoveChunksAndMarkCollectionDropped();

    expectSetShardVersionZero(shard1());
    expectUnsetSharding(shard1());

    expectSetShardVersionZero(shard2());
    expectUnsetSharding(shard2());

    expectChangeLogInsert(
        configHost(), network()->now(), "dropCollection", dropNS().ns(), BSONObj());

    future.timed_get(kFutureTimeout);
}

TEST_F(DropColl2ShardTest, ConfigTargeterError) {
    configTargeter()->setFindHostReturnValue({ErrorCodes::HostUnreachable, "bad test network"});

    auto future = launchAsync([this] {
        auto status = catalogClient()->dropCollection(operationContext(), dropNS());
        ASSERT_EQ(ErrorCodes::HostUnreachable, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(DropColl2ShardTest, DistLockBusy) {
    distLock()->expectLock([](StringData, StringData, Milliseconds, Milliseconds) {},
                           {ErrorCodes::LockBusy, "test lock taken"});

    auto future = launchAsync([this] {
        auto status = catalogClient()->dropCollection(operationContext(), dropNS());
        ASSERT_EQ(ErrorCodes::LockBusy, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    expectChangeLogCreate(configHost(), BSON("ok" << 1));
    expectChangeLogInsert(
        configHost(), network()->now(), "dropCollection.start", dropNS().ns(), BSONObj());

    expectGetShards({shard1(), shard2()});

    future.timed_get(kFutureTimeout);
}

TEST_F(DropColl2ShardTest, FirstShardTargeterError) {
    auto shard1Targeter = RemoteCommandTargeterMock::get(
        shardRegistry()->getShard(operationContext(), shard1().getName())->getTargeter());
    shard1Targeter->setFindHostReturnValue({ErrorCodes::HostUnreachable, "bad test network"});

    auto future = launchAsync([this] {
        auto status = catalogClient()->dropCollection(operationContext(), dropNS());
        ASSERT_EQ(ErrorCodes::HostUnreachable, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    expectChangeLogCreate(configHost(), BSON("ok" << 1));
    expectChangeLogInsert(
        configHost(), network()->now(), "dropCollection.start", dropNS().ns(), BSONObj());

    expectGetShards({shard1(), shard2()});

    future.timed_get(kFutureTimeout);
}

TEST_F(DropColl2ShardTest, FirstShardDropError) {
    auto future = launchAsync([this] {
        auto status = catalogClient()->dropCollection(operationContext(), dropNS());
        ASSERT_EQ(ErrorCodes::CallbackCanceled, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    expectChangeLogCreate(configHost(), BSON("ok" << 1));
    expectChangeLogInsert(
        configHost(), network()->now(), "dropCollection.start", dropNS().ns(), BSONObj());

    expectGetShards({shard1(), shard2()});

    onCommand([this](const RemoteCommandRequest& request) {
        shutdownExecutor();  // shutdown executor so drop command will fail.
        return BSON("ok" << 1);
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(DropColl2ShardTest, FirstShardDropCmdError) {
    auto future = launchAsync([this] {
        auto status = catalogClient()->dropCollection(operationContext(), dropNS());
        ASSERT_EQ(ErrorCodes::OperationFailed, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    expectChangeLogCreate(configHost(), BSON("ok" << 1));
    expectChangeLogInsert(
        configHost(), network()->now(), "dropCollection.start", dropNS().ns(), BSONObj());

    expectGetShards({shard1(), shard2()});

    // drop command will be sent to all shards even if we get a not ok response from one shard.
    onCommand([](const RemoteCommandRequest& request) {
        return BSON("ok" << 0 << "code" << ErrorCodes::Unauthorized);
    });

    expectDrop(shard2());

    future.timed_get(kFutureTimeout);
}

TEST_F(DropColl2ShardTest, SecondShardTargeterError) {
    auto shard2Targeter = RemoteCommandTargeterMock::get(
        shardRegistry()->getShard(operationContext(), shard2().getName())->getTargeter());
    shard2Targeter->setFindHostReturnValue({ErrorCodes::HostUnreachable, "bad test network"});

    auto future = launchAsync([this] {
        auto status = catalogClient()->dropCollection(operationContext(), dropNS());
        ASSERT_EQ(ErrorCodes::HostUnreachable, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    expectChangeLogCreate(configHost(), BSON("ok" << 1));
    expectChangeLogInsert(
        configHost(), network()->now(), "dropCollection.start", dropNS().ns(), BSONObj());

    expectGetShards({shard1(), shard2()});

    expectDrop(shard1());

    future.timed_get(kFutureTimeout);
}

TEST_F(DropColl2ShardTest, SecondShardDropError) {
    auto future = launchAsync([this] {
        auto status = catalogClient()->dropCollection(operationContext(), dropNS());
        ASSERT_EQ(ErrorCodes::CallbackCanceled, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    expectChangeLogCreate(configHost(), BSON("ok" << 1));
    expectChangeLogInsert(
        configHost(), network()->now(), "dropCollection.start", dropNS().ns(), BSONObj());

    expectGetShards({shard1(), shard2()});

    expectDrop(shard1());

    onCommand([this](const RemoteCommandRequest& request) {
        shutdownExecutor();  // shutdown executor so drop command will fail.
        return BSON("ok" << 1);
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(DropColl2ShardTest, SecondShardDropCmdError) {
    auto future = launchAsync([this] {
        auto status = catalogClient()->dropCollection(operationContext(), dropNS());
        ASSERT_EQ(ErrorCodes::OperationFailed, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    expectChangeLogCreate(configHost(), BSON("ok" << 1));
    expectChangeLogInsert(
        configHost(), network()->now(), "dropCollection.start", dropNS().ns(), BSONObj());

    expectGetShards({shard1(), shard2()});

    expectDrop(shard1());

    onCommand([](const RemoteCommandRequest& request) {
        return BSON("ok" << 0 << "code" << ErrorCodes::Unauthorized);
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(DropColl2ShardTest, CleanupChunkError) {
    auto future = launchAsync([this] {
        auto status = catalogClient()->dropCollection(operationContext(), dropNS());
        ASSERT_EQ(ErrorCodes::Unauthorized, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    expectChangeLogCreate(configHost(), BSON("ok" << 1));
    expectChangeLogInsert(
        configHost(), network()->now(), "dropCollection.start", dropNS().ns(), BSONObj());

    expectGetShards({shard1(), shard2()});

    expectDrop(shard1());
    expectDrop(shard2());

    onCommand([](const RemoteCommandRequest& request) {
        return BSON("ok" << 0 << "code" << ErrorCodes::Unauthorized << "errmsg"
                         << "bad delete");
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(DropColl2ShardTest, SSVCmdErrorOnShard1) {
    auto future = launchAsync([this] {
        auto status = catalogClient()->dropCollection(operationContext(), dropNS());
        ASSERT_EQ(ErrorCodes::Unauthorized, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    expectChangeLogCreate(configHost(), BSON("ok" << 1));
    expectChangeLogInsert(
        configHost(), network()->now(), "dropCollection.start", dropNS().ns(), BSONObj());

    expectGetShards({shard1(), shard2()});

    expectDrop(shard1());
    expectDrop(shard2());

    expectRemoveChunksAndMarkCollectionDropped();

    onCommand([](const RemoteCommandRequest& request) {
        return BSON("ok" << 0 << "code" << ErrorCodes::Unauthorized << "errmsg"
                         << "bad");
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(DropColl2ShardTest, SSVErrorOnShard1) {
    auto future = launchAsync([this] {
        auto status = catalogClient()->dropCollection(operationContext(), dropNS());
        ASSERT_EQ(ErrorCodes::CallbackCanceled, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    expectChangeLogCreate(configHost(), BSON("ok" << 1));
    expectChangeLogInsert(
        configHost(), network()->now(), "dropCollection.start", dropNS().ns(), BSONObj());

    expectGetShards({shard1(), shard2()});

    expectDrop(shard1());
    expectDrop(shard2());

    expectRemoveChunksAndMarkCollectionDropped();

    onCommand([this](const RemoteCommandRequest& request) {
        shutdownExecutor();  // shutdown executor so ssv command will fail.
        return BSON("ok" << 1);
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(DropColl2ShardTest, UnsetCmdErrorOnShard1) {
    auto future = launchAsync([this] {
        auto status = catalogClient()->dropCollection(operationContext(), dropNS());
        ASSERT_EQ(ErrorCodes::Unauthorized, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    expectChangeLogCreate(configHost(), BSON("ok" << 1));
    expectChangeLogInsert(
        configHost(), network()->now(), "dropCollection.start", dropNS().ns(), BSONObj());

    expectGetShards({shard1(), shard2()});

    expectDrop(shard1());
    expectDrop(shard2());

    expectRemoveChunksAndMarkCollectionDropped();

    expectSetShardVersionZero(shard1());

    onCommand([](const RemoteCommandRequest& request) {
        return BSON("ok" << 0 << "code" << ErrorCodes::Unauthorized << "errmsg"
                         << "bad");
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(DropColl2ShardTest, UnsetErrorOnShard1) {
    auto future = launchAsync([this] {
        auto status = catalogClient()->dropCollection(operationContext(), dropNS());
        ASSERT_EQ(ErrorCodes::CallbackCanceled, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    expectChangeLogCreate(configHost(), BSON("ok" << 1));
    expectChangeLogInsert(
        configHost(), network()->now(), "dropCollection.start", dropNS().ns(), BSONObj());

    expectGetShards({shard1(), shard2()});

    expectDrop(shard1());
    expectDrop(shard2());

    expectRemoveChunksAndMarkCollectionDropped();

    expectSetShardVersionZero(shard1());

    onCommand([this](const RemoteCommandRequest& request) {
        shutdownExecutor();  // shutdown executor so ssv command will fail.
        return BSON("ok" << 1);
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(DropColl2ShardTest, SSVCmdErrorOnShard2) {
    auto future = launchAsync([this] {
        auto status = catalogClient()->dropCollection(operationContext(), dropNS());
        ASSERT_EQ(ErrorCodes::Unauthorized, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    expectChangeLogCreate(configHost(), BSON("ok" << 1));
    expectChangeLogInsert(
        configHost(), network()->now(), "dropCollection.start", dropNS().ns(), BSONObj());

    expectGetShards({shard1(), shard2()});

    expectDrop(shard1());
    expectDrop(shard2());

    expectRemoveChunksAndMarkCollectionDropped();

    expectSetShardVersionZero(shard1());
    expectUnsetSharding(shard1());

    onCommand([](const RemoteCommandRequest& request) {
        return BSON("ok" << 0 << "code" << ErrorCodes::Unauthorized << "errmsg"
                         << "bad");
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(DropColl2ShardTest, SSVErrorOnShard2) {
    auto future = launchAsync([this] {
        auto status = catalogClient()->dropCollection(operationContext(), dropNS());
        ASSERT_EQ(ErrorCodes::CallbackCanceled, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    expectChangeLogCreate(configHost(), BSON("ok" << 1));
    expectChangeLogInsert(
        configHost(), network()->now(), "dropCollection.start", dropNS().ns(), BSONObj());

    expectGetShards({shard1(), shard2()});

    expectDrop(shard1());
    expectDrop(shard2());

    expectRemoveChunksAndMarkCollectionDropped();

    expectSetShardVersionZero(shard1());
    expectUnsetSharding(shard1());

    onCommand([this](const RemoteCommandRequest& request) {
        shutdownExecutor();  // shutdown executor so ssv command will fail.
        return BSON("ok" << 1);
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(DropColl2ShardTest, UnsetCmdErrorOnShard2) {
    auto future = launchAsync([this] {
        auto status = catalogClient()->dropCollection(operationContext(), dropNS());
        ASSERT_EQ(ErrorCodes::Unauthorized, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    expectChangeLogCreate(configHost(), BSON("ok" << 1));
    expectChangeLogInsert(
        configHost(), network()->now(), "dropCollection.start", dropNS().ns(), BSONObj());

    expectGetShards({shard1(), shard2()});

    expectDrop(shard1());
    expectDrop(shard2());

    expectRemoveChunksAndMarkCollectionDropped();

    expectSetShardVersionZero(shard1());
    expectUnsetSharding(shard1());

    expectSetShardVersionZero(shard2());

    onCommand([](const RemoteCommandRequest& request) {
        return BSON("ok" << 0 << "code" << ErrorCodes::Unauthorized << "errmsg"
                         << "bad");
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(DropColl2ShardTest, UnsetErrorOnShard2) {
    auto future = launchAsync([this] {
        auto status = catalogClient()->dropCollection(operationContext(), dropNS());
        ASSERT_EQ(ErrorCodes::CallbackCanceled, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    expectChangeLogCreate(configHost(), BSON("ok" << 1));
    expectChangeLogInsert(
        configHost(), network()->now(), "dropCollection.start", dropNS().ns(), BSONObj());

    expectGetShards({shard1(), shard2()});

    expectDrop(shard1());
    expectDrop(shard2());

    expectRemoveChunksAndMarkCollectionDropped();

    expectSetShardVersionZero(shard1());
    expectUnsetSharding(shard1());

    expectSetShardVersionZero(shard2());

    onCommand([this](const RemoteCommandRequest& request) {
        shutdownExecutor();  // shutdown executor so unset command will fail.
        return BSON("ok" << 1);
    });

    future.timed_get(kFutureTimeout);
}

}  // unnamed namespace
}  // namespace mongo
