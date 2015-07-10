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
#include "mongo/db/query/lite_parsed_query.h"
#include "mongo/s/catalog/dist_lock_manager_mock.h"
#include "mongo/s/catalog/replset/catalog_manager_replica_set.h"
#include "mongo/s/catalog/replset/catalog_manager_replica_set_test_fixture.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/stdx/chrono.h"

namespace mongo {
namespace {

using std::string;
using std::vector;
using stdx::chrono::milliseconds;
using unittest::assertGet;

class DropColl2ShardTest : public CatalogManagerReplSetTestFixture {
public:
    void setUp() override {
        CatalogManagerReplSetTestFixture::setUp();

        getMessagingPort()->setRemote(_clientHost);
        configTargeter()->setFindHostReturnValue(_configHost);

        distLock()->expectLock(
            [this](StringData name, StringData whyMessage, milliseconds, milliseconds) {
                ASSERT_EQUALS(_dropNS.ns(), name);
                ASSERT_EQUALS("drop", whyMessage);
            },
            Status::OK());

        _shard1.setName("shard0001");
        _shard1.setHost("s:1");

        _shard2.setName("shard0002");
        _shard2.setHost("s:2");

        setupShards({_shard1, _shard2});

        RemoteCommandTargeterMock* shard1Targeter = RemoteCommandTargeterMock::get(
            shardRegistry()->getShard(_shard1.getName())->getTargeter());
        shard1Targeter->setFindHostReturnValue(HostAndPort(_shard1.getHost()));

        RemoteCommandTargeterMock* shard2Targeter = RemoteCommandTargeterMock::get(
            shardRegistry()->getShard(_shard2.getName())->getTargeter());
        shard2Targeter->setFindHostReturnValue(HostAndPort(_shard2.getHost()));
    }

    void expectDrop(const ShardType& shard) {
        onCommand([this, shard](const RemoteCommandRequest& request) {
            ASSERT_EQ(HostAndPort(shard.getHost()), request.target);
            ASSERT_EQ(_dropNS.db(), request.dbname);
            ASSERT_EQ(BSON("drop" << _dropNS.coll()), request.cmdObj);

            return BSON("ns" << _dropNS.ns() << "ok" << 1);
        });
    }

    void expectRemoveChunks() {
        onCommand([this](const RemoteCommandRequest& request) {
            ASSERT_EQ(_configHost, request.target);
            ASSERT_EQ("config", request.dbname);

            BSONObj expectedCmd(fromjson(R"({
                delete: "chunks",
                deletes: [{ q: { ns: "test.user" }, limit: 0 }],
                writeConcern: { w: "majority" }
            })"));

            ASSERT_EQ(expectedCmd, request.cmdObj);

            return BSON("n" << 1 << "ok" << 1);
        });
    }

    void expectSetShardVersionZero(const ShardType& shard) {
        onCommand([this, shard](const RemoteCommandRequest& request) {
            ASSERT_EQ(HostAndPort(shard.getHost()), request.target);
            ASSERT_EQ("admin", request.dbname);

            BSONObjBuilder builder;

            builder.append("setShardVersion", _dropNS.ns());
            builder.append("configdb", catalogManager()->connectionString().toString());
            builder.append("shard", shard.getName());
            builder.append("shardHost", shard.getHost());
            builder.appendTimestamp("version", 0);
            builder.append("versionEpoch", OID());
            builder.append("authoritative", true);

            ASSERT_EQ(builder.obj(), request.cmdObj);

            return BSON("n" << 1 << "ok" << 1);
        });
    }

    void expectUnsetSharding(const ShardType& shard) {
        onCommand([shard](const RemoteCommandRequest& request) {
            ASSERT_EQ(HostAndPort(shard.getHost()), request.target);
            ASSERT_EQ("admin", request.dbname);
            ASSERT_EQ(BSON("unsetSharding" << 1), request.cmdObj);

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

    string testClient() const {
        return _clientHost.toString();
    }

    const HostAndPort& configHost() const {
        return _configHost;
    }

private:
    const HostAndPort _configHost{"TestHost1"};
    const HostAndPort _clientHost{"client:123"};
    const NamespaceString _dropNS{"test.user"};

    ShardType _shard1;
    ShardType _shard2;
};

TEST_F(DropColl2ShardTest, Basic) {
    auto future = launchAsync([this] {
        auto status = catalogManager()->dropCollection(operationContext(), dropNS());
        ASSERT_OK(status);
    });

    expectChangeLogCreate(configHost(), BSON("ok" << 1));
    expectChangeLogInsert(configHost(),
                          testClient(),
                          network()->now(),
                          "dropCollection.start",
                          dropNS().ns(),
                          BSONObj());

    expectGetShards({shard1(), shard2()});

    expectDrop(shard1());
    expectDrop(shard2());

    expectRemoveChunks();

    expectSetShardVersionZero(shard1());
    expectUnsetSharding(shard1());

    expectSetShardVersionZero(shard2());
    expectUnsetSharding(shard2());

    expectChangeLogInsert(
        configHost(), testClient(), network()->now(), "dropCollection", dropNS().ns(), BSONObj());
}

TEST_F(DropColl2ShardTest, NSNotFound) {
    auto future = launchAsync([this] {
        auto status = catalogManager()->dropCollection(operationContext(), dropNS());
        ASSERT_OK(status);
    });

    expectChangeLogCreate(configHost(), BSON("ok" << 1));
    expectChangeLogInsert(configHost(),
                          testClient(),
                          network()->now(),
                          "dropCollection.start",
                          dropNS().ns(),
                          BSONObj());

    expectGetShards({shard1(), shard2()});

    onCommand([this](const RemoteCommandRequest& request) {
        ASSERT_EQ(HostAndPort(shard1().getHost()), request.target);
        ASSERT_EQ(dropNS().db(), request.dbname);
        ASSERT_EQ(BSON("drop" << dropNS().coll()), request.cmdObj);

        return BSON("ok" << 0 << "code" << ErrorCodes::NamespaceNotFound);
    });

    onCommand([this](const RemoteCommandRequest& request) {
        ASSERT_EQ(HostAndPort(shard2().getHost()), request.target);
        ASSERT_EQ(dropNS().db(), request.dbname);
        ASSERT_EQ(BSON("drop" << dropNS().coll()), request.cmdObj);

        return BSON("ok" << 0 << "code" << ErrorCodes::NamespaceNotFound);
    });

    expectRemoveChunks();

    expectSetShardVersionZero(shard1());
    expectUnsetSharding(shard1());

    expectSetShardVersionZero(shard2());
    expectUnsetSharding(shard2());

    expectChangeLogInsert(
        configHost(), testClient(), network()->now(), "dropCollection", dropNS().ns(), BSONObj());
}

TEST_F(DropColl2ShardTest, ConfigTargeterError) {
    configTargeter()->setFindHostReturnValue({ErrorCodes::HostUnreachable, "bad test network"});

    auto future = launchAsync([this] {
        auto status = catalogManager()->dropCollection(operationContext(), dropNS());
        ASSERT_EQ(ErrorCodes::HostUnreachable, status.code());
        ASSERT_FALSE(status.reason().empty());
    });
}

TEST_F(DropColl2ShardTest, DistLockBusy) {
    distLock()->expectLock([](StringData, StringData, milliseconds, milliseconds) {},
                           {ErrorCodes::LockBusy, "test lock taken"});

    auto future = launchAsync([this] {
        auto status = catalogManager()->dropCollection(operationContext(), dropNS());
        ASSERT_EQ(ErrorCodes::LockBusy, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    expectChangeLogCreate(configHost(), BSON("ok" << 1));
    expectChangeLogInsert(configHost(),
                          testClient(),
                          network()->now(),
                          "dropCollection.start",
                          dropNS().ns(),
                          BSONObj());
}

TEST_F(DropColl2ShardTest, FirstShardTargeterError) {
    RemoteCommandTargeterMock* shard1Targeter = RemoteCommandTargeterMock::get(
        shardRegistry()->getShard(shard1().getName())->getTargeter());
    shard1Targeter->setFindHostReturnValue({ErrorCodes::HostUnreachable, "bad test network"});

    auto future = launchAsync([this] {
        auto status = catalogManager()->dropCollection(operationContext(), dropNS());
        ASSERT_EQ(ErrorCodes::HostUnreachable, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    expectChangeLogCreate(configHost(), BSON("ok" << 1));
    expectChangeLogInsert(configHost(),
                          testClient(),
                          network()->now(),
                          "dropCollection.start",
                          dropNS().ns(),
                          BSONObj());

    expectGetShards({shard1(), shard2()});
}

TEST_F(DropColl2ShardTest, FirstShardDropError) {
    auto future = launchAsync([this] {
        auto status = catalogManager()->dropCollection(operationContext(), dropNS());
        ASSERT_EQ(ErrorCodes::HostUnreachable, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    expectChangeLogCreate(configHost(), BSON("ok" << 1));
    expectChangeLogInsert(configHost(),
                          testClient(),
                          network()->now(),
                          "dropCollection.start",
                          dropNS().ns(),
                          BSONObj());

    expectGetShards({shard1(), shard2()});

    onCommand([](const RemoteCommandRequest& request) {
        return Status{ErrorCodes::HostUnreachable, "drop bad network"};
    });
}

TEST_F(DropColl2ShardTest, FirstShardDropCmdError) {
    auto future = launchAsync([this] {
        auto status = catalogManager()->dropCollection(operationContext(), dropNS());
        ASSERT_EQ(ErrorCodes::OperationFailed, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    expectChangeLogCreate(configHost(), BSON("ok" << 1));
    expectChangeLogInsert(configHost(),
                          testClient(),
                          network()->now(),
                          "dropCollection.start",
                          dropNS().ns(),
                          BSONObj());

    expectGetShards({shard1(), shard2()});

    onCommand([](const RemoteCommandRequest& request) {
        return BSON("ok" << 0 << "code" << ErrorCodes::Unauthorized);
    });
}

TEST_F(DropColl2ShardTest, SecondShardTargeterError) {
    RemoteCommandTargeterMock* shard2Targeter = RemoteCommandTargeterMock::get(
        shardRegistry()->getShard(shard2().getHost())->getTargeter());
    shard2Targeter->setFindHostReturnValue({ErrorCodes::HostUnreachable, "bad test network"});

    auto future = launchAsync([this] {
        auto status = catalogManager()->dropCollection(operationContext(), dropNS());
        ASSERT_EQ(ErrorCodes::HostUnreachable, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    expectChangeLogCreate(configHost(), BSON("ok" << 1));
    expectChangeLogInsert(configHost(),
                          testClient(),
                          network()->now(),
                          "dropCollection.start",
                          dropNS().ns(),
                          BSONObj());

    expectGetShards({shard1(), shard2()});

    expectDrop(shard1());
}

TEST_F(DropColl2ShardTest, SecondShardDropError) {
    auto future = launchAsync([this] {
        auto status = catalogManager()->dropCollection(operationContext(), dropNS());
        ASSERT_EQ(ErrorCodes::HostUnreachable, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    expectChangeLogCreate(configHost(), BSON("ok" << 1));
    expectChangeLogInsert(configHost(),
                          testClient(),
                          network()->now(),
                          "dropCollection.start",
                          dropNS().ns(),
                          BSONObj());

    expectGetShards({shard1(), shard2()});

    expectDrop(shard1());

    onCommand([](const RemoteCommandRequest& request) {
        return Status{ErrorCodes::HostUnreachable, "drop bad network"};
    });
}

TEST_F(DropColl2ShardTest, SecondShardDropCmdError) {
    auto future = launchAsync([this] {
        auto status = catalogManager()->dropCollection(operationContext(), dropNS());
        ASSERT_EQ(ErrorCodes::OperationFailed, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    expectChangeLogCreate(configHost(), BSON("ok" << 1));
    expectChangeLogInsert(configHost(),
                          testClient(),
                          network()->now(),
                          "dropCollection.start",
                          dropNS().ns(),
                          BSONObj());

    expectGetShards({shard1(), shard2()});

    expectDrop(shard1());

    onCommand([](const RemoteCommandRequest& request) {
        return BSON("ok" << 0 << "code" << ErrorCodes::Unauthorized);
    });
}

TEST_F(DropColl2ShardTest, CleanupChunkError) {
    auto future = launchAsync([this] {
        auto status = catalogManager()->dropCollection(operationContext(), dropNS());
        ASSERT_EQ(ErrorCodes::Unauthorized, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    expectChangeLogCreate(configHost(), BSON("ok" << 1));
    expectChangeLogInsert(configHost(),
                          testClient(),
                          network()->now(),
                          "dropCollection.start",
                          dropNS().ns(),
                          BSONObj());

    expectGetShards({shard1(), shard2()});

    expectDrop(shard1());
    expectDrop(shard2());

    onCommand([](const RemoteCommandRequest& request) {
        return BSON("ok" << 0 << "code" << ErrorCodes::Unauthorized << "errmsg"
                         << "bad delete");
    });
}

TEST_F(DropColl2ShardTest, SSVErrorOnShard1) {
    auto future = launchAsync([this] {
        auto status = catalogManager()->dropCollection(operationContext(), dropNS());
        ASSERT_EQ(ErrorCodes::HostUnreachable, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    expectChangeLogCreate(configHost(), BSON("ok" << 1));
    expectChangeLogInsert(configHost(),
                          testClient(),
                          network()->now(),
                          "dropCollection.start",
                          dropNS().ns(),
                          BSONObj());

    expectGetShards({shard1(), shard2()});

    expectDrop(shard1());
    expectDrop(shard2());

    expectRemoveChunks();

    onCommand([](const RemoteCommandRequest& request) {
        return Status{ErrorCodes::HostUnreachable, "bad test network"};
    });
}

TEST_F(DropColl2ShardTest, UnsetErrorOnShard1) {
    auto future = launchAsync([this] {
        auto status = catalogManager()->dropCollection(operationContext(), dropNS());
        ASSERT_EQ(ErrorCodes::HostUnreachable, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    expectChangeLogCreate(configHost(), BSON("ok" << 1));
    expectChangeLogInsert(configHost(),
                          testClient(),
                          network()->now(),
                          "dropCollection.start",
                          dropNS().ns(),
                          BSONObj());

    expectGetShards({shard1(), shard2()});

    expectDrop(shard1());
    expectDrop(shard2());

    expectRemoveChunks();

    expectSetShardVersionZero(shard1());

    onCommand([](const RemoteCommandRequest& request) {
        return Status{ErrorCodes::HostUnreachable, "bad test network"};
    });
}

TEST_F(DropColl2ShardTest, SSVErrorOnShard2) {
    auto future = launchAsync([this] {
        auto status = catalogManager()->dropCollection(operationContext(), dropNS());
        ASSERT_EQ(ErrorCodes::HostUnreachable, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    expectChangeLogCreate(configHost(), BSON("ok" << 1));
    expectChangeLogInsert(configHost(),
                          testClient(),
                          network()->now(),
                          "dropCollection.start",
                          dropNS().ns(),
                          BSONObj());

    expectGetShards({shard1(), shard2()});

    expectDrop(shard1());
    expectDrop(shard2());

    expectRemoveChunks();

    expectSetShardVersionZero(shard1());
    expectUnsetSharding(shard1());

    onCommand([](const RemoteCommandRequest& request) {
        return Status{ErrorCodes::HostUnreachable, "bad test network"};
    });
}

TEST_F(DropColl2ShardTest, UnsetErrorOnShard2) {
    auto future = launchAsync([this] {
        auto status = catalogManager()->dropCollection(operationContext(), dropNS());
        ASSERT_EQ(ErrorCodes::HostUnreachable, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    expectChangeLogCreate(configHost(), BSON("ok" << 1));
    expectChangeLogInsert(configHost(),
                          testClient(),
                          network()->now(),
                          "dropCollection.start",
                          dropNS().ns(),
                          BSONObj());

    expectGetShards({shard1(), shard2()});

    expectDrop(shard1());
    expectDrop(shard2());

    expectRemoveChunks();

    expectSetShardVersionZero(shard1());
    expectUnsetSharding(shard1());

    expectSetShardVersionZero(shard2());

    onCommand([](const RemoteCommandRequest& request) {
        return Status{ErrorCodes::HostUnreachable, "bad test network"};
    });
}

}  // unnamed namespace
}  // namespace mongo
