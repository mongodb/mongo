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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/rpc/metadata/tracking_metadata.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/config_server_test_fixture.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace {

using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;
using std::string;
using std::vector;
using unittest::assertGet;

class DropColl2ShardTest : public ConfigServerTestFixture {
public:
    void setUp() override {
        ConfigServerTestFixture::setUp();

        _shard1.setName("shard0001");
        _shard1.setHost("s:1");

        _shard2.setName("shard0002");
        _shard2.setHost("s:2");

        _zoneName = "zoneName";
        _shardKey = "x";
        _min = BSON(_shardKey << 0);
        _max = BSON(_shardKey << 10);

        setupShards({_shard1, _shard2});

        auto shard1Targeter = RemoteCommandTargeterMock::get(
            uassertStatusOK(shardRegistry()->getShard(operationContext(), _shard1.getName()))
                ->getTargeter());
        shard1Targeter->setFindHostReturnValue(HostAndPort(_shard1.getHost()));

        auto shard2Targeter = RemoteCommandTargeterMock::get(
            uassertStatusOK(shardRegistry()->getShard(operationContext(), _shard2.getName()))
                ->getTargeter());
        shard2Targeter->setFindHostReturnValue(HostAndPort(_shard2.getHost()));

        // insert documents into the config database
        CollectionType shardedCollection;
        shardedCollection.setNs(dropNS());
        shardedCollection.setEpoch(OID::gen());
        shardedCollection.setKeyPattern(BSON(_shardKey << 1));
        ASSERT_OK(insertToConfigCollection(
            operationContext(), CollectionType::ConfigNS, shardedCollection.toBSON()));

        BSONObjBuilder tagDocBuilder;
        tagDocBuilder.append("_id", BSON(TagsType::ns(dropNS().ns()) << TagsType::min(_min)));
        tagDocBuilder.append(TagsType::ns(), dropNS().ns());
        tagDocBuilder.append(TagsType::min(), _min);
        tagDocBuilder.append(TagsType::max(), _max);
        tagDocBuilder.append(TagsType::tag(), _zoneName);
        ASSERT_OK(
            insertToConfigCollection(operationContext(), TagsType::ConfigNS, tagDocBuilder.obj()));

        BSONObjBuilder chunkDocBuilder;
        chunkDocBuilder.append("ns", dropNS().ns());
        chunkDocBuilder.append("min", _min);
        chunkDocBuilder.append("max", _max);
        chunkDocBuilder.append("shard", _shard1.getName());
        ASSERT_OK(insertToConfigCollection(
            operationContext(), ChunkType::ConfigNS, chunkDocBuilder.obj()));
    }

    void expectDrop(const ShardType& shard) {
        onCommand([this, shard](const RemoteCommandRequest& request) {
            ASSERT_EQ(HostAndPort(shard.getHost()), request.target);
            ASSERT_EQ(_dropNS.db(), request.dbname);
            ASSERT_BSONOBJ_EQ(BSON("drop" << _dropNS.coll()), request.cmdObj);

            ASSERT_BSONOBJ_EQ(rpc::makeEmptyMetadata(),
                              rpc::TrackingMetadata::removeTrackingData(request.metadata));

            return BSON("ns" << _dropNS.ns() << "ok" << 1);
        });
    }

    void expectSetShardVersionZero(const ShardType& shard) {
        expectSetShardVersion(
            HostAndPort(shard.getHost()), shard, dropNS(), ChunkVersion::DROPPED());
    }

    void expectUnsetSharding(const ShardType& shard) {
        onCommand([shard](const RemoteCommandRequest& request) {
            ASSERT_EQ(HostAndPort(shard.getHost()), request.target);
            ASSERT_EQ("admin", request.dbname);
            ASSERT_BSONOBJ_EQ(BSON("unsetSharding" << 1), request.cmdObj);

            ASSERT_BSONOBJ_EQ(rpc::makeEmptyMetadata(),
                              rpc::TrackingMetadata::removeTrackingData(request.metadata));

            return BSON("n" << 1 << "ok" << 1);
        });
    }

    void expectCollectionDocMarkedAsDropped() {
        auto findStatus =
            findOneOnConfigCollection(operationContext(), CollectionType::ConfigNS, BSONObj());
        ASSERT_OK(findStatus.getStatus());
        ASSERT_TRUE(findStatus.getValue().getField("dropped"));
    }

    void expectNoChunkDocs() {
        auto findStatus =
            findOneOnConfigCollection(operationContext(), ChunkType::ConfigNS, BSONObj());
        ASSERT_EQ(ErrorCodes::NoMatchingDocument, findStatus);
    }

    void expectNoTagDocs() {
        auto findStatus =
            findOneOnConfigCollection(operationContext(), TagsType::ConfigNS, BSONObj());
        ASSERT_EQ(ErrorCodes::NoMatchingDocument, findStatus);
    }

    void shutdownExecutor() {
        ConfigServerTestFixture::executor()->shutdown();
    }

    Status doDrop() {
        ThreadClient tc("Test", getGlobalServiceContext());
        auto opCtx = cc().makeOperationContext();
        return ShardingCatalogManager::get(opCtx.get())->dropCollection(opCtx.get(), dropNS());
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

private:
    const NamespaceString _dropNS{"test.user"};
    ShardType _shard1;
    ShardType _shard2;
    string _zoneName;
    string _shardKey;
    BSONObj _min;
    BSONObj _max;
};

TEST_F(DropColl2ShardTest, Basic) {
    auto future = launchAsync([this] {
        auto status = doDrop();
        ASSERT_OK(status);
    });

    expectDrop(shard1());
    expectDrop(shard2());

    expectSetShardVersionZero(shard1());
    expectUnsetSharding(shard1());

    expectSetShardVersionZero(shard2());
    expectUnsetSharding(shard2());

    future.default_timed_get();

    expectCollectionDocMarkedAsDropped();
    expectNoChunkDocs();
    expectNoTagDocs();
}

TEST_F(DropColl2ShardTest, NSNotFound) {
    auto future = launchAsync([this] {
        auto status = doDrop();
        ASSERT_OK(status);
    });

    onCommand([this](const RemoteCommandRequest& request) {
        ASSERT_EQ(HostAndPort(shard1().getHost()), request.target);
        ASSERT_EQ(dropNS().db(), request.dbname);
        ASSERT_BSONOBJ_EQ(BSON("drop" << dropNS().coll()), request.cmdObj);

        ASSERT_BSONOBJ_EQ(rpc::makeEmptyMetadata(),
                          rpc::TrackingMetadata::removeTrackingData(request.metadata));

        return BSON("ok" << 0 << "code" << ErrorCodes::NamespaceNotFound);
    });

    onCommand([this](const RemoteCommandRequest& request) {
        ASSERT_EQ(HostAndPort(shard2().getHost()), request.target);
        ASSERT_EQ(dropNS().db(), request.dbname);
        ASSERT_BSONOBJ_EQ(BSON("drop" << dropNS().coll()), request.cmdObj);

        ASSERT_BSONOBJ_EQ(rpc::makeEmptyMetadata(),
                          rpc::TrackingMetadata::removeTrackingData(request.metadata));

        return BSON("ok" << 0 << "code" << ErrorCodes::NamespaceNotFound);
    });

    expectSetShardVersionZero(shard1());
    expectUnsetSharding(shard1());

    expectSetShardVersionZero(shard2());
    expectUnsetSharding(shard2());

    future.default_timed_get();

    expectCollectionDocMarkedAsDropped();
    expectNoChunkDocs();
    expectNoTagDocs();
}

TEST_F(DropColl2ShardTest, FirstShardTargeterError) {
    auto shard1Targeter = RemoteCommandTargeterMock::get(
        uassertStatusOK(shardRegistry()->getShard(operationContext(), shard1().getName()))
            ->getTargeter());
    shard1Targeter->setFindHostReturnValue({ErrorCodes::HostUnreachable, "bad test network"});

    auto future = launchAsync([this] {
        auto status = doDrop();
        ASSERT_EQ(ErrorCodes::HostUnreachable, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    future.default_timed_get();
}

TEST_F(DropColl2ShardTest, FirstShardDropError) {
    auto future = launchAsync([this] {
        auto status = doDrop();
        ASSERT_EQ(ErrorCodes::CallbackCanceled, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([this](const RemoteCommandRequest& request) {
        shutdownExecutor();  // shutdown executor so drop command will fail.
        return BSON("ok" << 1);
    });

    future.default_timed_get();
}

TEST_F(DropColl2ShardTest, FirstShardDropCmdError) {
    auto future = launchAsync([this] {
        auto status = doDrop();
        ASSERT_EQ(ErrorCodes::OperationFailed, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    // drop command will be sent to all shards even if we get a not ok response from one shard.
    onCommand([](const RemoteCommandRequest& request) {
        return BSON("ok" << 0 << "code" << ErrorCodes::Unauthorized);
    });

    expectDrop(shard2());

    future.default_timed_get();
}

TEST_F(DropColl2ShardTest, SecondShardTargeterError) {
    auto shard2Targeter = RemoteCommandTargeterMock::get(
        uassertStatusOK(shardRegistry()->getShard(operationContext(), shard2().getName()))
            ->getTargeter());
    shard2Targeter->setFindHostReturnValue({ErrorCodes::HostUnreachable, "bad test network"});

    auto future = launchAsync([this] {
        auto status = doDrop();
        ASSERT_EQ(ErrorCodes::HostUnreachable, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    expectDrop(shard1());

    future.default_timed_get();
}

TEST_F(DropColl2ShardTest, SecondShardDropError) {
    auto future = launchAsync([this] {
        auto status = doDrop();
        ASSERT_EQ(ErrorCodes::CallbackCanceled, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    expectDrop(shard1());

    onCommand([this](const RemoteCommandRequest& request) {
        shutdownExecutor();  // shutdown executor so drop command will fail.
        return BSON("ok" << 1);
    });

    future.default_timed_get();
}

TEST_F(DropColl2ShardTest, SecondShardDropCmdError) {
    auto future = launchAsync([this] {
        auto status = doDrop();
        ASSERT_EQ(ErrorCodes::OperationFailed, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    expectDrop(shard1());

    onCommand([](const RemoteCommandRequest& request) {
        return BSON("ok" << 0 << "code" << ErrorCodes::Unauthorized);
    });

    future.default_timed_get();
}

TEST_F(DropColl2ShardTest, CleanupChunkError) {
    auto future = launchAsync([this] {
        auto status = doDrop();
        ASSERT_EQ(ErrorCodes::Unauthorized, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    expectDrop(shard1());
    expectDrop(shard2());

    onCommand([](const RemoteCommandRequest& request) {
        return BSON("ok" << 0 << "code" << ErrorCodes::Unauthorized << "errmsg"
                         << "bad delete");
    });

    future.default_timed_get();

    expectCollectionDocMarkedAsDropped();
    expectNoChunkDocs();
    expectNoTagDocs();
}

TEST_F(DropColl2ShardTest, SSVCmdErrorOnShard1) {
    auto future = launchAsync([this] {
        auto status = doDrop();
        ASSERT_EQ(ErrorCodes::Unauthorized, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    expectDrop(shard1());
    expectDrop(shard2());

    onCommand([](const RemoteCommandRequest& request) {
        return BSON("ok" << 0 << "code" << ErrorCodes::Unauthorized << "errmsg"
                         << "bad");
    });

    future.default_timed_get();

    expectCollectionDocMarkedAsDropped();
    expectNoChunkDocs();
    expectNoTagDocs();
}

TEST_F(DropColl2ShardTest, SSVErrorOnShard1) {
    auto future = launchAsync([this] {
        auto status = doDrop();
        ASSERT_EQ(ErrorCodes::CallbackCanceled, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    expectDrop(shard1());
    expectDrop(shard2());

    onCommand([this](const RemoteCommandRequest& request) {
        shutdownExecutor();  // shutdown executor so ssv command will fail.
        return BSON("ok" << 1);
    });

    future.default_timed_get();

    expectCollectionDocMarkedAsDropped();
    expectNoChunkDocs();
    expectNoTagDocs();
}

TEST_F(DropColl2ShardTest, UnsetCmdErrorOnShard1) {
    auto future = launchAsync([this] {
        auto status = doDrop();
        ASSERT_EQ(ErrorCodes::Unauthorized, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    expectDrop(shard1());
    expectDrop(shard2());

    expectSetShardVersionZero(shard1());

    onCommand([](const RemoteCommandRequest& request) {
        return BSON("ok" << 0 << "code" << ErrorCodes::Unauthorized << "errmsg"
                         << "bad");
    });

    future.default_timed_get();

    expectCollectionDocMarkedAsDropped();
    expectNoChunkDocs();
    expectNoTagDocs();
}

TEST_F(DropColl2ShardTest, UnsetErrorOnShard1) {
    auto future = launchAsync([this] {
        auto status = doDrop();
        ASSERT_EQ(ErrorCodes::CallbackCanceled, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    expectDrop(shard1());
    expectDrop(shard2());

    expectSetShardVersionZero(shard1());

    onCommand([this](const RemoteCommandRequest& request) {
        shutdownExecutor();  // shutdown executor so unsetSharding command will fail.
        return BSON("ok" << 1);
    });

    future.default_timed_get();

    expectCollectionDocMarkedAsDropped();
    expectNoChunkDocs();
    expectNoTagDocs();
}

TEST_F(DropColl2ShardTest, SSVCmdErrorOnShard2) {
    auto future = launchAsync([this] {
        auto status = doDrop();
        ASSERT_EQ(ErrorCodes::Unauthorized, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    expectDrop(shard1());
    expectDrop(shard2());

    expectSetShardVersionZero(shard1());
    expectUnsetSharding(shard1());

    onCommand([](const RemoteCommandRequest& request) {
        return BSON("ok" << 0 << "code" << ErrorCodes::Unauthorized << "errmsg"
                         << "bad");
    });

    future.default_timed_get();

    expectCollectionDocMarkedAsDropped();
    expectNoChunkDocs();
    expectNoTagDocs();
}

TEST_F(DropColl2ShardTest, SSVErrorOnShard2) {
    auto future = launchAsync([this] {
        auto status = doDrop();
        ASSERT_EQ(ErrorCodes::CallbackCanceled, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    expectDrop(shard1());
    expectDrop(shard2());

    expectSetShardVersionZero(shard1());
    expectUnsetSharding(shard1());

    onCommand([this](const RemoteCommandRequest& request) {
        shutdownExecutor();  // shutdown executor so ssv command will fail.
        return BSON("ok" << 1);
    });

    future.default_timed_get();

    expectCollectionDocMarkedAsDropped();
    expectNoChunkDocs();
    expectNoTagDocs();
}

TEST_F(DropColl2ShardTest, UnsetCmdErrorOnShard2) {
    auto future = launchAsync([this] {
        auto status = doDrop();
        ASSERT_EQ(ErrorCodes::Unauthorized, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    expectDrop(shard1());
    expectDrop(shard2());

    expectSetShardVersionZero(shard1());
    expectUnsetSharding(shard1());

    expectSetShardVersionZero(shard2());

    onCommand([](const RemoteCommandRequest& request) {
        return BSON("ok" << 0 << "code" << ErrorCodes::Unauthorized << "errmsg"
                         << "bad");
    });

    future.default_timed_get();

    expectCollectionDocMarkedAsDropped();
    expectNoChunkDocs();
    expectNoTagDocs();
}

TEST_F(DropColl2ShardTest, UnsetErrorOnShard2) {
    auto future = launchAsync([this] {
        auto status = doDrop();
        ASSERT_EQ(ErrorCodes::CallbackCanceled, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    expectDrop(shard1());
    expectDrop(shard2());

    expectSetShardVersionZero(shard1());
    expectUnsetSharding(shard1());

    expectSetShardVersionZero(shard2());

    onCommand([this](const RemoteCommandRequest& request) {
        shutdownExecutor();  // shutdown executor so unset command will fail.
        return BSON("ok" << 1);
    });

    future.default_timed_get();

    expectCollectionDocMarkedAsDropped();
    expectNoChunkDocs();
    expectNoTagDocs();
}

}  // unnamed namespace
}  // namespace mongo
