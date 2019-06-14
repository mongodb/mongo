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

#include "mongo/platform/basic.h"

#include <memory>
#include <utility>

#include "mongo/bson/json.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/commands.h"
#include "mongo/db/query/find_and_modify_request.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/storage/duplicate_key_error_info.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/network_test_env.h"
#include "mongo/s/catalog/dist_lock_catalog_impl.h"
#include "mongo/s/catalog/dist_lock_manager_mock.h"
#include "mongo/s/catalog/sharding_catalog_client_mock.h"
#include "mongo/s/catalog/type_lockpings.h"
#include "mongo/s/catalog/type_locks.h"
#include "mongo/s/client/shard_factory.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_server_test_fixture.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {

using executor::NetworkInterfaceMock;
using executor::NetworkTestEnv;
using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;
using repl::ReadConcernArgs;

const HostAndPort dummyHost("dummy", 123);

/**
 * Sets up the mocked out objects for testing the replica-set backed catalog manager
 *
 * NOTE: Even though the dist lock manager only runs on the config server, this test is using the
 * ShardServerTestFixture and emulating the network due to legacy reasons.
 */
class DistLockCatalogTest : public ShardServerTestFixture {
protected:
    std::unique_ptr<DistLockCatalog> makeDistLockCatalog() override {
        return std::make_unique<DistLockCatalogImpl>();
    }

    std::unique_ptr<DistLockManager> makeDistLockManager(
        std::unique_ptr<DistLockCatalog> distLockCatalog) override {
        return std::make_unique<DistLockManagerMock>(std::move(distLockCatalog));
    }

    std::unique_ptr<ShardingCatalogClient> makeShardingCatalogClient(
        std::unique_ptr<DistLockManager> distLockManager) override {
        return std::make_unique<ShardingCatalogClientMock>(std::move(distLockManager));
    }

    std::shared_ptr<RemoteCommandTargeterMock> configTargeter() {
        return RemoteCommandTargeterMock::get(shardRegistry()->getConfigShard()->getTargeter());
    }

    auto launchOnSeparateThread(std::function<void(OperationContext*)> func) {
        auto const serviceContext = getServiceContext();
        return launchAsync([serviceContext, func] {
            ThreadClient tc("Test", getGlobalServiceContext());
            auto opCtx = Client::getCurrent()->makeOperationContext();
            func(opCtx.get());
        });
    }
};

void checkReadConcern(const BSONObj& findCmd) {
    ReadConcernArgs readConcernArgs;
    ASSERT_OK(readConcernArgs.initialize(findCmd[ReadConcernArgs::kReadConcernFieldName]));
    ASSERT(repl::ReadConcernLevel::kMajorityReadConcern == readConcernArgs.getLevel());
}

TEST_F(DistLockCatalogTest, BasicPing) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        Date_t ping(dateFromISOString("2014-03-11T09:17:18.098Z").getValue());
        auto status = distLockCatalog()->ping(opCtx, "abcd", ping);
        ASSERT_OK(status);
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        ASSERT_EQUALS(dummyHost, request.target);
        ASSERT_EQUALS("config", request.dbname);

        BSONObj expectedCmd(fromjson(R"({
                findAndModify: "lockpings",
                query: { _id: "abcd" },
                update: {
                    $set: {
                        ping: { $date: "2014-03-11T09:17:18.098Z" }
                    }
                },
                upsert: true,
                writeConcern: { w: "majority", wtimeout: 15000 },
                maxTimeMS: 30000
            })"));

        ASSERT_BSONOBJ_EQ(expectedCmd, request.cmdObj);

        return fromjson(R"({
                ok: 1,
                value: {
                    _id: "abcd",
                    ping: { $date: "2014-03-11T09:17:18.098Z" }
                }
            })");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogTest, PingTargetError) {
    configTargeter()->setFindHostReturnValue({ErrorCodes::InternalError, "can't target"});
    auto status = distLockCatalog()->ping(operationContext(), "abcd", Date_t::now());
    ASSERT_NOT_OK(status);
}

TEST_F(DistLockCatalogTest, PingRunCmdError) {
    shutdownExecutorPool();

    auto status = distLockCatalog()->ping(operationContext(), "abcd", Date_t::now());
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, status.code());
    ASSERT_FALSE(status.reason().empty());
}

TEST_F(DistLockCatalogTest, PingCommandError) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = distLockCatalog()->ping(opCtx, "abcd", Date_t::now());
        ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return fromjson(R"({
                ok: 0,
                errmsg: "bad",
                code: 9
            })");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogTest, PingWriteError) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = distLockCatalog()->ping(opCtx, "abcd", Date_t::now());
        ASSERT_EQUALS(ErrorCodes::Unauthorized, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return fromjson(R"({
                ok: 0,
                code: 13,
                errmsg: "not authorized"
            })");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogTest, PingWriteConcernError) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = distLockCatalog()->ping(opCtx, "abcd", Date_t::now());
        ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return fromjson(R"({
                ok: 1,
                value: null,
                writeConcernError: {
                    code: 64,
                    errmsg: "waiting for replication timed out"
                }
            })");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogTest, PingUnsupportedWriteConcernResponse) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = distLockCatalog()->ping(opCtx, "abcd", Date_t::now());
        ASSERT_EQUALS(ErrorCodes::UnsupportedFormat, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        // return non numeric code for writeConcernError.code
        return fromjson(R"({
                ok: 1,
                value: null,
                writeConcernError: {
                    code: "bad format",
                    errmsg: "waiting for replication timed out"
                }
            })");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogTest, PingUnsupportedResponseFormat) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = distLockCatalog()->ping(opCtx, "abcd", Date_t::now());
        ASSERT_EQUALS(ErrorCodes::UnsupportedFormat, status.code());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return BSON("ok" << 1 << "value"
                         << "NaN");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogTest, GrabLockNoOp) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        OID myID("555f80be366c194b13fb0372");
        Date_t now(dateFromISOString("2015-05-22T19:17:18.098Z").getValue());
        auto resultStatus = distLockCatalog()
                                ->grabLock(opCtx, "test", myID, "me", "mongos", now, "because")
                                .getStatus();

        ASSERT_EQUALS(ErrorCodes::LockStateChangeFailed, resultStatus.code());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        ASSERT_EQUALS(dummyHost, request.target);
        ASSERT_EQUALS("config", request.dbname);

        BSONObj expectedCmd(fromjson(R"({
                findAndModify: "locks",
                query: { _id: "test", state: 0 },
                update: {
                    $set: {
                        ts: ObjectId("555f80be366c194b13fb0372"),
                        state: 2,
                        who: "me",
                        process: "mongos",
                        when: { $date: "2015-05-22T19:17:18.098Z" },
                        why: "because"
                    }
                },
                upsert: true,
                new: true,
                writeConcern: { w: "majority", wtimeout: 15000 },
                maxTimeMS: 30000
            })"));

        ASSERT_BSONOBJ_EQ(expectedCmd, request.cmdObj);

        return fromjson("{ ok: 1, value: null }");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogTest, GrabLockWithNewDoc) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        OID myID("555f80be366c194b13fb0372");
        Date_t now(dateFromISOString("2015-05-22T19:17:18.098Z").getValue());
        auto resultStatus =
            distLockCatalog()->grabLock(opCtx, "test", myID, "me", "mongos", now, "because");
        ASSERT_OK(resultStatus.getStatus());

        const auto& lockDoc = resultStatus.getValue();
        ASSERT_OK(lockDoc.validate());
        ASSERT_EQUALS("test", lockDoc.getName());
        ASSERT_EQUALS(myID, lockDoc.getLockID());
        ASSERT_EQUALS("me", lockDoc.getWho());
        ASSERT_EQUALS("mongos", lockDoc.getProcess());
        ASSERT_EQUALS("because", lockDoc.getWhy());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        ASSERT_EQUALS(dummyHost, request.target);
        ASSERT_EQUALS("config", request.dbname);

        BSONObj expectedCmd(fromjson(R"({
                findAndModify: "locks",
                query: { _id: "test", state: 0 },
                update: {
                    $set: {
                        ts: ObjectId("555f80be366c194b13fb0372"),
                        state: 2,
                        who: "me",
                        process: "mongos",
                        when: { $date: "2015-05-22T19:17:18.098Z" },
                        why: "because"
                    }
                },
                upsert: true,
                new: true,
                writeConcern: { w: "majority", wtimeout: 15000 },
                maxTimeMS: 30000
            })"));

        ASSERT_BSONOBJ_EQ(expectedCmd, request.cmdObj);

        return fromjson(R"({
                lastErrorObject: {
                    updatedExisting: false,
                    n: 1,
                    upserted: 1
                },
                value: {
                    _id: "test",
                    ts: ObjectId("555f80be366c194b13fb0372"),
                    state: 2,
                    who: "me",
                    process: "mongos",
                    when: { $date: "2015-05-22T19:17:18.098Z" },
                    why: "because"
                },
                ok: 1
            })");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogTest, GrabLockWithBadLockDoc) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        Date_t now(dateFromISOString("2015-05-22T19:17:18.098Z").getValue());
        auto resultStatus =
            distLockCatalog()->grabLock(opCtx, "test", OID(), "", "", now, "").getStatus();
        ASSERT_EQUALS(ErrorCodes::FailedToParse, resultStatus.code());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        // Return an invalid lock document on value. This is theoretically impossible because
        // the vital parts of the resulting doc are derived from the update request.
        return fromjson(R"({
                lastErrorObject: {
                    updatedExisting: false,
                    n: 1,
                    upserted: 1
                },
                value: {
                    _id: "test",
                    ts: ObjectId("555f80be366c194b13fb0372"),
                    state: "x",
                    who: "me",
                    process: "mongos",
                    when: { $date: "2015-05-22T19:17:18.098Z" },
                    why: "because"
                },
                ok: 1
            })");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogTest, GrabLockTargetError) {
    configTargeter()->setFindHostReturnValue({ErrorCodes::InternalError, "can't target"});
    auto status = distLockCatalog()
                      ->grabLock(operationContext(), "", OID::gen(), "", "", Date_t::now(), "")
                      .getStatus();
    ASSERT_NOT_OK(status);
}

TEST_F(DistLockCatalogTest, GrabLockRunCmdError) {
    shutdownExecutorPool();

    auto status = distLockCatalog()
                      ->grabLock(operationContext(), "", OID::gen(), "", "", Date_t::now(), "")
                      .getStatus();
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, status.code());
    ASSERT_FALSE(status.reason().empty());
}

TEST_F(DistLockCatalogTest, GrabLockCommandError) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = distLockCatalog()
                          ->grabLock(opCtx, "", OID::gen(), "", "", Date_t::now(), "")
                          .getStatus();
        ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return fromjson(R"({
                ok: 0,
                errmsg: "bad",
                code: 9
            })");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogTest, GrabLockDupKeyError) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = distLockCatalog()
                          ->grabLock(opCtx, "", OID::gen(), "", "", Date_t::now(), "")
                          .getStatus();
        ASSERT_EQUALS(ErrorCodes::LockStateChangeFailed, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return Status(
            {DuplicateKeyErrorInfo(BSON("x" << 1), BSON("" << 1)), "Mock duplicate key error"});
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogTest, GrabLockWriteError) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = distLockCatalog()
                          ->grabLock(opCtx, "", OID::gen(), "", "", Date_t::now(), "")
                          .getStatus();
        ASSERT_EQUALS(ErrorCodes::Unauthorized, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return fromjson(R"({
                ok: 0,
                code: 13,
                errmsg: "not authorized"
            })");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogTest, GrabLockWriteConcernError) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = distLockCatalog()
                          ->grabLock(operationContext(), "", OID::gen(), "", "", Date_t::now(), "")
                          .getStatus();
        ASSERT_EQUALS(ErrorCodes::NotMaster, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return fromjson(R"({
                ok: 1,
                value: null,
                writeConcernError: {
                    code: 10107,
                    errmsg: "Not master while waiting for write concern"
                }
            })");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogTest, GrabLockWriteConcernErrorBadType) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = distLockCatalog()
                          ->grabLock(operationContext(), "", OID::gen(), "", "", Date_t::now(), "")
                          .getStatus();
        ASSERT_EQUALS(ErrorCodes::TypeMismatch, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        // return invalid non-object type for writeConcernError.
        return fromjson(R"({
                ok: 1,
                value: null,
                writeConcernError: "unexpected"
            })");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogTest, GrabLockResponseMissingValueField) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = distLockCatalog()
                          ->grabLock(operationContext(), "", OID::gen(), "", "", Date_t::now(), "")
                          .getStatus();
        ASSERT_EQUALS(ErrorCodes::UnsupportedFormat, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return fromjson(R"({
                ok: 1
            })");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogTest, GrabLockUnsupportedWriteConcernResponse) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = distLockCatalog()
                          ->grabLock(operationContext(), "", OID::gen(), "", "", Date_t::now(), "")
                          .getStatus();
        ASSERT_EQUALS(ErrorCodes::UnsupportedFormat, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        // return non numeric code for writeConcernError.code
        return fromjson(R"({
                ok: 1,
                value: null,
                writeConcernError: {
                    code: "bad format",
                    errmsg: "waiting for replication timed out"
                }
            })");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogTest, GrabLockUnsupportedResponseFormat) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = distLockCatalog()
                          ->grabLock(operationContext(), "", OID::gen(), "", "", Date_t::now(), "")
                          .getStatus();
        ASSERT_EQUALS(ErrorCodes::UnsupportedFormat, status.code());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return BSON("ok" << 1 << "value"
                         << "NaN");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogTest, OvertakeLockNoOp) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        OID myID("555f80be366c194b13fb0372");
        OID currentOwner("555f99712c99a78c5b083358");
        Date_t now(dateFromISOString("2015-05-22T19:17:18.098Z").getValue());
        auto resultStatus =
            distLockCatalog()
                ->overtakeLock(
                    operationContext(), "test", myID, currentOwner, "me", "mongos", now, "because")
                .getStatus();

        ASSERT_EQUALS(ErrorCodes::LockStateChangeFailed, resultStatus.code());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        ASSERT_EQUALS(dummyHost, request.target);
        ASSERT_EQUALS("config", request.dbname);

        BSONObj expectedCmd(fromjson(R"({
                findAndModify: "locks",
                query: {
                    $or: [
                        { _id: "test", state: 0 },
                        { _id: "test", ts: ObjectId("555f99712c99a78c5b083358") }
                    ]
                },
                update: {
                    $set: {
                        ts: ObjectId("555f80be366c194b13fb0372"),
                        state: 2,
                        who: "me",
                        process: "mongos",
                        when: { $date: "2015-05-22T19:17:18.098Z" },
                        why: "because"
                    }
                },
                new: true,
                writeConcern: { w: "majority", wtimeout: 15000 },
                maxTimeMS: 30000
            })"));

        ASSERT_BSONOBJ_EQ(expectedCmd, request.cmdObj);

        return fromjson("{ ok: 1, value: null }");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogTest, OvertakeLockWithNewDoc) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        OID myID("555f80be366c194b13fb0372");
        OID currentOwner("555f99712c99a78c5b083358");
        Date_t now(dateFromISOString("2015-05-22T19:17:18.098Z").getValue());
        auto resultStatus = distLockCatalog()->overtakeLock(
            operationContext(), "test", myID, currentOwner, "me", "mongos", now, "because");
        ASSERT_OK(resultStatus.getStatus());

        const auto& lockDoc = resultStatus.getValue();
        ASSERT_OK(lockDoc.validate());
        ASSERT_EQUALS("test", lockDoc.getName());
        ASSERT_EQUALS(myID, lockDoc.getLockID());
        ASSERT_EQUALS("me", lockDoc.getWho());
        ASSERT_EQUALS("mongos", lockDoc.getProcess());
        ASSERT_EQUALS("because", lockDoc.getWhy());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        ASSERT_EQUALS(dummyHost, request.target);
        ASSERT_EQUALS("config", request.dbname);

        BSONObj expectedCmd(fromjson(R"({
                findAndModify: "locks",
                query: {
                    $or: [
                        { _id: "test", state: 0 },
                        { _id: "test", ts: ObjectId("555f99712c99a78c5b083358") }
                    ]
                },
                update: {
                    $set: {
                        ts: ObjectId("555f80be366c194b13fb0372"),
                        state: 2,
                        who: "me",
                        process: "mongos",
                        when: { $date: "2015-05-22T19:17:18.098Z" },
                        why: "because"
                    }
                },
                new: true,
                writeConcern: { w: "majority", wtimeout: 15000 },
                maxTimeMS: 30000
            })"));

        ASSERT_BSONOBJ_EQ(expectedCmd, request.cmdObj);

        return fromjson(R"({
                lastErrorObject: {
                    updatedExisting: false,
                    n: 1,
                    upserted: 1
                },
                value: {
                    _id: "test",
                    ts: ObjectId("555f80be366c194b13fb0372"),
                    state: 2,
                    who: "me",
                    process: "mongos",
                    when: { $date: "2015-05-22T19:17:18.098Z" },
                    why: "because"
                },
                ok: 1
            })");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogTest, OvertakeLockWithBadLockDoc) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        Date_t now(dateFromISOString("2015-05-22T19:17:18.098Z").getValue());
        auto resultStatus =
            distLockCatalog()
                ->overtakeLock(operationContext(), "test", OID(), OID(), "", "", now, "")
                .getStatus();
        ASSERT_EQUALS(ErrorCodes::FailedToParse, resultStatus.code());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        // Return an invalid lock document on value. This is theoretically impossible because
        // the vital parts of the resulting doc are derived from the update request.
        return fromjson(R"({
                lastErrorObject: {
                    updatedExisting: false,
                    n: 1,
                    upserted: 1
                },
                value: {
                    _id: "test",
                    ts: ObjectId("555f80be366c194b13fb0372"),
                    state: "x",
                    who: "me",
                    process: "mongos",
                    when: { $date: "2015-05-22T19:17:18.098Z" },
                    why: "because"
                },
                ok: 1
            })");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogTest, OvertakeLockTargetError) {
    configTargeter()->setFindHostReturnValue({ErrorCodes::InternalError, "can't target"});
    auto status =
        distLockCatalog()
            ->overtakeLock(operationContext(), "", OID(), OID(), "", "", Date_t::now(), "")
            .getStatus();
    ASSERT_NOT_OK(status);
}

TEST_F(DistLockCatalogTest, OvertakeLockRunCmdError) {
    shutdownExecutorPool();

    auto status =
        distLockCatalog()
            ->overtakeLock(operationContext(), "", OID(), OID(), "", "", Date_t::now(), "")
            .getStatus();
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, status.code());
    ASSERT_FALSE(status.reason().empty());
}

TEST_F(DistLockCatalogTest, OvertakeLockCommandError) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status =
            distLockCatalog()
                ->overtakeLock(operationContext(), "", OID(), OID(), "", "", Date_t::now(), "")
                .getStatus();
        ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return fromjson(R"({
                ok: 0,
                errmsg: "bad",
                code: 9
            })");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogTest, OvertakeLockWriteError) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status =
            distLockCatalog()
                ->overtakeLock(operationContext(), "", OID(), OID(), "", "", Date_t::now(), "")
                .getStatus();
        ASSERT_EQUALS(ErrorCodes::Unauthorized, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return fromjson(R"({
                ok: 0,
                code: 13,
                errmsg: "not authorized"
            })");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogTest, OvertakeLockWriteConcernError) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status =
            distLockCatalog()
                ->overtakeLock(operationContext(), "", OID(), OID(), "", "", Date_t::now(), "")
                .getStatus();
        ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return fromjson(R"({
                ok: 1,
                value: null,
                writeConcernError: {
                    code: 64,
                    errmsg: "waiting for replication timed out"
                }
            })");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogTest, OvertakeLockUnsupportedWriteConcernResponse) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status =
            distLockCatalog()
                ->overtakeLock(operationContext(), "", OID(), OID(), "", "", Date_t::now(), "")
                .getStatus();
        ASSERT_EQUALS(ErrorCodes::UnsupportedFormat, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        // return non numeric code for writeConcernError.code
        return fromjson(R"({
                ok: 1,
                value: null,
                writeConcernError: {
                    code: "bad format",
                    errmsg: "waiting for replication timed out"
                }
            })");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogTest, OvertakeLockUnsupportedResponseFormat) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status =
            distLockCatalog()
                ->overtakeLock(operationContext(), "", OID(), OID(), "", "", Date_t::now(), "")
                .getStatus();
        ASSERT_EQUALS(ErrorCodes::UnsupportedFormat, status.code());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return BSON("ok" << 1 << "value"
                         << "NaN");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogTest, BasicUnlock) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status =
            distLockCatalog()->unlock(operationContext(), OID("555f99712c99a78c5b083358"));
        ASSERT_OK(status);
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        ASSERT_EQUALS(dummyHost, request.target);
        ASSERT_EQUALS("config", request.dbname);

        BSONObj expectedCmd(fromjson(R"({
                findAndModify: "locks",
                query: { ts: ObjectId("555f99712c99a78c5b083358") },
                update: { $set: { state: 0 }},
                writeConcern: { w: "majority", wtimeout: 15000 },
                maxTimeMS: 30000
            })"));

        ASSERT_BSONOBJ_EQ(expectedCmd, request.cmdObj);

        return fromjson(R"({
                ok: 1,
                value: {
                    _id: "",
                    ts: ObjectId("555f99712c99a78c5b083358"),
                    state: 0
                }
            })");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogTest, BasicUnlockWithName) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = distLockCatalog()->unlock(
            operationContext(), OID("555f99712c99a78c5b083358"), "TestDB.TestColl");
        ASSERT_OK(status);
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        ASSERT_EQUALS(dummyHost, request.target);
        ASSERT_EQUALS("config", request.dbname);

        BSONObj expectedCmd(fromjson(R"({
                findAndModify: "locks",
                query: { ts: ObjectId("555f99712c99a78c5b083358"), _id: "TestDB.TestColl" },
                update: { $set: { state: 0 }},
                writeConcern: { w: "majority", wtimeout: 15000 },
                maxTimeMS: 30000
            })"));

        ASSERT_BSONOBJ_EQ(expectedCmd, request.cmdObj);

        return fromjson(R"({
                ok: 1,
                value: {
                    _id: "TestDB.TestColl",
                    ts: ObjectId("555f99712c99a78c5b083358"),
                    state: 0
                }
            })");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogTest, UnlockWithNoNewDoc) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status =
            distLockCatalog()->unlock(operationContext(), OID("555f99712c99a78c5b083358"));
        ASSERT_OK(status);
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        ASSERT_EQUALS(dummyHost, request.target);
        ASSERT_EQUALS("config", request.dbname);

        BSONObj expectedCmd(fromjson(R"({
                findAndModify: "locks",
                query: { ts: ObjectId("555f99712c99a78c5b083358") },
                update: { $set: { state: 0 }},
                writeConcern: { w: "majority", wtimeout: 15000 },
                maxTimeMS: 30000
            })"));

        ASSERT_BSONOBJ_EQ(expectedCmd, request.cmdObj);

        return fromjson(R"({
                ok: 1,
                value: null
            })");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogTest, UnlockWithNameWithNoNewDoc) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = distLockCatalog()->unlock(
            operationContext(), OID("555f99712c99a78c5b083358"), "TestDB.TestColl");
        ASSERT_OK(status);
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        ASSERT_EQUALS(dummyHost, request.target);
        ASSERT_EQUALS("config", request.dbname);

        BSONObj expectedCmd(fromjson(R"({
                findAndModify: "locks",
                query: { ts: ObjectId("555f99712c99a78c5b083358"), _id: "TestDB.TestColl" },
                update: { $set: { state: 0 }},
                writeConcern: { w: "majority", wtimeout: 15000 },
                maxTimeMS: 30000
            })"));

        ASSERT_BSONOBJ_EQ(expectedCmd, request.cmdObj);

        return fromjson(R"({
                ok: 1,
                value: null
            })");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogTest, UnlockTargetError) {
    configTargeter()->setFindHostReturnValue({ErrorCodes::InternalError, "can't target"});
    auto status = distLockCatalog()->unlock(operationContext(), OID());
    ASSERT_NOT_OK(status);
}

TEST_F(DistLockCatalogTest, UnlockRunCmdError) {
    shutdownExecutorPool();

    auto status = distLockCatalog()->unlock(operationContext(), OID());
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, status.code());
    ASSERT_FALSE(status.reason().empty());
}

TEST_F(DistLockCatalogTest, UnlockCommandError) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = distLockCatalog()->unlock(operationContext(), OID());
        ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return fromjson(R"({
                ok: 0,
                errmsg: "bad",
                code: 9
            })");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogTest, UnlockWriteError) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = distLockCatalog()->unlock(operationContext(), OID());
        ASSERT_EQUALS(ErrorCodes::Unauthorized, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return fromjson(R"({
                ok: 0,
                code: 13,
                errmsg: "not authorized"
            })");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogTest, UnlockWriteConcernError) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = distLockCatalog()->unlock(operationContext(), OID());
        ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    BSONObj writeConcernFailedResponse = fromjson(R"({
                ok: 1,
                value: null,
                writeConcernError: {
                    code: 64,
                    errmsg: "waiting for replication timed out"
                }
            })");

    // The dist lock catalog calls into the ShardRegistry, which will retry 3 times for
    // WriteConcernFailed errors
    onCommand([&](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return writeConcernFailedResponse;
    });

    onCommand([&](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return writeConcernFailedResponse;
    });

    onCommand([&](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return writeConcernFailedResponse;
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogTest, UnlockUnsupportedWriteConcernResponse) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = distLockCatalog()->unlock(operationContext(), OID());
        ASSERT_EQUALS(ErrorCodes::UnsupportedFormat, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        // return non numeric code for writeConcernError.code
        return fromjson(R"({
                ok: 1,
                value: null,
                writeConcernError: {
                    code: "bad format",
                    errmsg: "waiting for replication timed out"
                }
            })");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogTest, UnlockUnsupportedResponseFormat) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = distLockCatalog()->unlock(operationContext(), OID());
        ASSERT_EQUALS(ErrorCodes::UnsupportedFormat, status.code());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return BSON("ok" << 1 << "value"
                         << "NaN");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogTest, BasicUnlockAll) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = distLockCatalog()->unlockAll(operationContext(), "processID");
        ASSERT_OK(status);
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        ASSERT_EQUALS(dummyHost, request.target);
        ASSERT_EQUALS("config", request.dbname);

        const auto opMsgRequest(OpMsgRequest::fromDBAndBody(request.dbname, request.cmdObj));
        const auto commandRequest(BatchedCommandRequest::parseUpdate(opMsgRequest));

        ASSERT_BSONOBJ_EQ(BSON("w" << 1 << "wtimeout" << 0), commandRequest.getWriteConcern());

        const auto& updateOp = commandRequest.getUpdateRequest();
        ASSERT_EQUALS(LocksType::ConfigNS, updateOp.getNamespace());

        const auto& updates = updateOp.getUpdates();
        ASSERT_EQUALS(1U, updates.size());

        const auto& update = updates.front();
        ASSERT(!update.getUpsert());
        ASSERT(update.getMulti());
        ASSERT_BSONOBJ_EQ(BSON(LocksType::process("processID")), update.getQ());
        ASSERT_BSONOBJ_EQ(BSON("$set" << BSON(LocksType::state(LocksType::UNLOCKED))),
                          update.getU().getUpdateClassic());

        return BSON("ok" << 1);
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogTest, UnlockAllWriteFailed) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = distLockCatalog()->unlockAll(operationContext(), "processID");
        ASSERT_EQUALS(ErrorCodes::IllegalOperation, status);
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return BSON("ok" << 0 << "code" << ErrorCodes::IllegalOperation << "errmsg"
                         << "something went wrong");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogTest, UnlockAllNetworkError) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = distLockCatalog()->unlockAll(operationContext(), "processID");
        ASSERT_EQUALS(ErrorCodes::NetworkTimeout, status);
    });

    for (int i = 0; i < 3; i++) {  // ShardRegistry will retry 3 times on network errors
        onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
            return Status(ErrorCodes::NetworkTimeout, "network error");
        });
    }

    future.default_timed_get();
}

TEST_F(DistLockCatalogTest, BasicGetServerInfo) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        Date_t localTime(dateFromISOString("2015-05-26T13:06:27.293Z").getValue());
        OID electionID("555fa85d4d8640862a0fc79b");
        auto resultStatus = distLockCatalog()->getServerInfo(operationContext());
        ASSERT_OK(resultStatus.getStatus());

        const auto& serverInfo = resultStatus.getValue();
        ASSERT_EQUALS(electionID, serverInfo.electionId);
        ASSERT_EQUALS(localTime, serverInfo.serverTime);
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        ASSERT_EQUALS(dummyHost, request.target);
        ASSERT_EQUALS("admin", request.dbname);
        ASSERT_BSONOBJ_EQ(BSON("serverStatus" << 1 << "maxTimeMS" << 30000), request.cmdObj);

        return fromjson(R"({
                localTime: { $date: "2015-05-26T13:06:27.293Z" },
                repl: {
                    electionId: ObjectId("555fa85d4d8640862a0fc79b")
                },
                ok: 1
            })");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogTest, GetServerTargetError) {
    configTargeter()->setFindHostReturnValue({ErrorCodes::InternalError, "can't target"});
    auto status = distLockCatalog()->getServerInfo(operationContext()).getStatus();
    ASSERT_NOT_OK(status);
}

TEST_F(DistLockCatalogTest, GetServerRunCmdError) {
    shutdownExecutorPool();

    auto status = distLockCatalog()->getServerInfo(operationContext()).getStatus();
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, status.code());
    ASSERT_FALSE(status.reason().empty());
}

TEST_F(DistLockCatalogTest, GetServerCommandError) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = distLockCatalog()->getServerInfo(operationContext()).getStatus();
        ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return fromjson(R"({
                ok: 0,
                errmsg: "bad",
                code: 9
            })");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogTest, GetServerBadElectionId) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = distLockCatalog()->getServerInfo(operationContext()).getStatus();
        ASSERT_EQUALS(ErrorCodes::UnsupportedFormat, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        // return invalid non-oid electionId
        return fromjson(R"({
                localTime: { $date: "2015-05-26T13:06:27.293Z" },
                repl: {
                    electionId: 34
                },
                ok: 1
            })");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogTest, GetServerBadLocalTime) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = distLockCatalog()->getServerInfo(operationContext()).getStatus();
        ASSERT_EQUALS(ErrorCodes::UnsupportedFormat, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        // return invalid non date type for localTime field.
        return fromjson(R"({
                localTime: "2015-05-26T13:06:27.293Z",
                repl: {
                    electionId: ObjectId("555fa85d4d8640862a0fc79b")
                },
                ok: 1
            })");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogTest, GetServerNoGLEStats) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = distLockCatalog()->getServerInfo(operationContext()).getStatus();
        ASSERT_EQUALS(ErrorCodes::UnsupportedFormat, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return fromjson(R"({
                localTime: { $date: "2015-05-26T13:06:27.293Z" },
                ok: 1
            })");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogTest, GetServerNoElectionId) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = distLockCatalog()->getServerInfo(operationContext()).getStatus();
        ASSERT_EQUALS(ErrorCodes::NotMaster, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return fromjson(R"({
                localTime: { $date: "2015-05-26T13:06:27.293Z" },
                repl: {
                    ismaster: false,
                    me: "me:1234"
                },
                ok: 1
            })");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogTest, GetServerInvalidReplSubsectionShouldFail) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = distLockCatalog()->getServerInfo(operationContext()).getStatus();
        ASSERT_EQUALS(ErrorCodes::UnsupportedFormat, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return fromjson(R"({
                localTime: { $date: "2015-05-26T13:06:27.293Z" },
                repl: {
                    invalid: true
                },
                ok: 1
            })");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogTest, GetServerNoElectionIdButMasterShouldFail) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = distLockCatalog()->getServerInfo(operationContext()).getStatus();
        ASSERT_EQUALS(ErrorCodes::UnsupportedFormat, status.code());
        ASSERT_NOT_EQUALS(std::string::npos, status.reason().find("me:1234"));
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return fromjson(R"({
                localTime: { $date: "2015-05-26T13:06:27.293Z" },
                repl: {
                    ismaster: true,
                    me: "me:1234"
                },
                ok: 1
            })");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogTest, BasicStopPing) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = distLockCatalog()->stopPing(operationContext(), "test");
        ASSERT_OK(status);
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        ASSERT_EQUALS(dummyHost, request.target);
        ASSERT_EQUALS("config", request.dbname);

        BSONObj expectedCmd(fromjson(R"({
                findAndModify: "lockpings",
                query: { _id: "test" },
                remove: true,
                writeConcern: { w: "majority", wtimeout: 15000 },
                maxTimeMS: 30000
            })"));

        ASSERT_BSONOBJ_EQ(expectedCmd, request.cmdObj);

        return fromjson(R"({
                ok: 1,
                value: {
                  _id: "test",
                  ping: { $date: "2014-03-11T09:17:18.098Z" }
                }
            })");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogTest, StopPingTargetError) {
    configTargeter()->setFindHostReturnValue({ErrorCodes::InternalError, "can't target"});
    auto status = distLockCatalog()->stopPing(operationContext(), "");
    ASSERT_NOT_OK(status);
}

TEST_F(DistLockCatalogTest, StopPingRunCmdError) {
    shutdownExecutorPool();

    auto status = distLockCatalog()->stopPing(operationContext(), "");
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, status.code());
    ASSERT_FALSE(status.reason().empty());
}

TEST_F(DistLockCatalogTest, StopPingCommandError) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = distLockCatalog()->stopPing(operationContext(), "");
        ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return fromjson(R"({
                ok: 0,
                errmsg: "bad",
                code: 9
            })");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogTest, StopPingWriteError) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = distLockCatalog()->stopPing(operationContext(), "");
        ASSERT_EQUALS(ErrorCodes::Unauthorized, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return fromjson(R"({
                ok: 0,
                code: 13,
                errmsg: "Unauthorized"
            })");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogTest, StopPingWriteConcernError) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = distLockCatalog()->stopPing(operationContext(), "");
        ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return fromjson(R"({
                ok: 1,
                value: null,
                writeConcernError: {
                    code: 64,
                    errmsg: "waiting for replication timed out"
                }
            })");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogTest, StopPingUnsupportedWriteConcernResponse) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = distLockCatalog()->stopPing(operationContext(), "");
        ASSERT_EQUALS(ErrorCodes::UnsupportedFormat, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        // return non numeric code for writeConcernError.code
        return fromjson(R"({
                ok: 1,
                value: null,
                writeConcernError: {
                    code: "bad format",
                    errmsg: "waiting for replication timed out"
                }
            })");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogTest, StopPingUnsupportedResponseFormat) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = distLockCatalog()->stopPing(operationContext(), "");
        ASSERT_EQUALS(ErrorCodes::UnsupportedFormat, status.code());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return BSON("ok" << 1 << "value"
                         << "NaN");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogTest, BasicGetPing) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        Date_t ping(dateFromISOString("2015-05-26T13:06:27.293Z").getValue());
        auto resultStatus = distLockCatalog()->getPing(operationContext(), "test");
        ASSERT_OK(resultStatus.getStatus());

        const auto& pingDoc = resultStatus.getValue();
        ASSERT_EQUALS("test", pingDoc.getProcess());
        ASSERT_EQUALS(ping, pingDoc.getPing());
    });

    onFindCommand([](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(dummyHost, request.target);
        ASSERT_EQUALS("config", request.dbname);

        const auto& findCmd = request.cmdObj;
        ASSERT_EQUALS("lockpings", findCmd["find"].str());
        ASSERT_BSONOBJ_EQ(BSON("_id"
                               << "test"),
                          findCmd["filter"].Obj());
        ASSERT_EQUALS(1, findCmd["limit"].numberLong());
        checkReadConcern(findCmd);

        BSONObj pingDoc(fromjson(R"({
                _id: "test",
                ping: { $date: "2015-05-26T13:06:27.293Z" }
            })"));

        std::vector<BSONObj> result;
        result.push_back(pingDoc);

        return result;
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogTest, GetPingTargetError) {
    configTargeter()->setFindHostReturnValue({ErrorCodes::InternalError, "can't target"});
    auto status = distLockCatalog()->getPing(operationContext(), "").getStatus();
    ASSERT_EQUALS(ErrorCodes::InternalError, status.code());
}

TEST_F(DistLockCatalogTest, GetPingRunCmdError) {
    shutdownExecutorPool();

    auto status = distLockCatalog()->getPing(operationContext(), "").getStatus();
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, status.code());
    ASSERT_FALSE(status.reason().empty());
}

TEST_F(DistLockCatalogTest, GetPingNotFound) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = distLockCatalog()->getPing(operationContext(), "").getStatus();
        ASSERT_EQUALS(ErrorCodes::NoMatchingDocument, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onFindCommand([](const RemoteCommandRequest& request) -> StatusWith<std::vector<BSONObj>> {
        return std::vector<BSONObj>();
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogTest, GetPingUnsupportedFormat) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = distLockCatalog()->getPing(operationContext(), "test").getStatus();
        ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onFindCommand([](const RemoteCommandRequest& request) -> StatusWith<std::vector<BSONObj>> {
        // return non-date type for ping.
        BSONObj pingDoc(fromjson(R"({
            _id: "test",
            ping: "bad"
        })"));

        std::vector<BSONObj> result;
        result.push_back(pingDoc);

        return result;
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogTest, BasicGetLockByTS) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        OID ts("555f99712c99a78c5b083358");
        auto resultStatus = distLockCatalog()->getLockByTS(operationContext(), ts);
        ASSERT_OK(resultStatus.getStatus());

        const auto& lockDoc = resultStatus.getValue();
        ASSERT_EQUALS("test", lockDoc.getName());
        ASSERT_EQUALS(ts, lockDoc.getLockID());
    });

    onFindCommand([](const RemoteCommandRequest& request) -> StatusWith<std::vector<BSONObj>> {
        ASSERT_EQUALS(dummyHost, request.target);
        ASSERT_EQUALS("config", request.dbname);

        const auto& findCmd = request.cmdObj;
        ASSERT_EQUALS("locks", findCmd["find"].str());
        ASSERT_BSONOBJ_EQ(BSON("ts" << OID("555f99712c99a78c5b083358")), findCmd["filter"].Obj());
        ASSERT_EQUALS(1, findCmd["limit"].numberLong());
        checkReadConcern(findCmd);

        BSONObj lockDoc(fromjson(R"({
            _id: "test",
            state: 2,
            ts: ObjectId("555f99712c99a78c5b083358")
        })"));

        std::vector<BSONObj> result;
        result.push_back(lockDoc);
        return result;
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogTest, GetLockByTSTargetError) {
    configTargeter()->setFindHostReturnValue({ErrorCodes::InternalError, "can't target"});
    auto status = distLockCatalog()->getLockByTS(operationContext(), OID()).getStatus();
    ASSERT_EQUALS(ErrorCodes::InternalError, status.code());
}

TEST_F(DistLockCatalogTest, GetLockByTSRunCmdError) {
    shutdownExecutorPool();
    auto status = distLockCatalog()->getLockByTS(operationContext(), OID()).getStatus();
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, status.code());
    ASSERT_FALSE(status.reason().empty());
}

TEST_F(DistLockCatalogTest, GetLockByTSNotFound) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = distLockCatalog()->getLockByTS(operationContext(), OID()).getStatus();
        ASSERT_EQUALS(ErrorCodes::LockNotFound, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onFindCommand([](const RemoteCommandRequest& request) -> StatusWith<std::vector<BSONObj>> {
        return std::vector<BSONObj>();
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogTest, GetLockByTSUnsupportedFormat) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = distLockCatalog()->getLockByTS(operationContext(), OID()).getStatus();
        ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onFindCommand([](const RemoteCommandRequest& request) -> StatusWith<std::vector<BSONObj>> {
        // return invalid non-numeric type for state.
        BSONObj lockDoc(fromjson(R"({
            _id: "test",
            state: "bad"
        })"));

        std::vector<BSONObj> result;
        result.push_back(lockDoc);

        return result;
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogTest, BasicGetLockByName) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        OID ts("555f99712c99a78c5b083358");
        auto resultStatus = distLockCatalog()->getLockByName(operationContext(), "abc");
        ASSERT_OK(resultStatus.getStatus());

        const auto& lockDoc = resultStatus.getValue();
        ASSERT_EQUALS("abc", lockDoc.getName());
        ASSERT_EQUALS(ts, lockDoc.getLockID());
    });

    onFindCommand([](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(dummyHost, request.target);
        ASSERT_EQUALS("config", request.dbname);

        const auto& findCmd = request.cmdObj;
        ASSERT_EQUALS("locks", findCmd["find"].str());
        ASSERT_BSONOBJ_EQ(BSON("_id"
                               << "abc"),
                          findCmd["filter"].Obj());
        ASSERT_EQUALS(1, findCmd["limit"].numberLong());
        checkReadConcern(findCmd);

        BSONObj lockDoc(fromjson(R"({
                _id: "abc",
                state: 2,
                ts: ObjectId("555f99712c99a78c5b083358")
            })"));

        std::vector<BSONObj> result;
        result.push_back(lockDoc);
        return result;
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogTest, GetLockByNameTargetError) {
    configTargeter()->setFindHostReturnValue({ErrorCodes::InternalError, "can't target"});
    auto status = distLockCatalog()->getLockByName(operationContext(), "x").getStatus();
    ASSERT_EQUALS(ErrorCodes::InternalError, status.code());
}

TEST_F(DistLockCatalogTest, GetLockByNameRunCmdError) {
    shutdownExecutorPool();

    auto status = distLockCatalog()->getLockByName(operationContext(), "x").getStatus();
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, status.code());
    ASSERT_FALSE(status.reason().empty());
}

TEST_F(DistLockCatalogTest, GetLockByNameNotFound) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = distLockCatalog()->getLockByName(operationContext(), "x").getStatus();
        ASSERT_EQUALS(ErrorCodes::LockNotFound, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onFindCommand([](const RemoteCommandRequest& request) -> StatusWith<std::vector<BSONObj>> {
        return std::vector<BSONObj>();
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogTest, GetLockByNameUnsupportedFormat) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = distLockCatalog()->getLockByName(operationContext(), "x").getStatus();
        ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onFindCommand([](const RemoteCommandRequest& request) -> StatusWith<std::vector<BSONObj>> {
        // Return non-numeric type for state.
        BSONObj lockDoc(fromjson(R"({
            _id: "x",
            state: "bad"
        })"));

        std::vector<BSONObj> result;
        result.push_back(lockDoc);

        return result;
    });

    future.default_timed_get();
}

}  // namespace
}  // namespace mongo
