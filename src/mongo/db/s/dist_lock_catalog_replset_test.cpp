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
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/s/dist_lock_catalog_replset.h"
#include "mongo/db/s/shard_server_test_fixture.h"
#include "mongo/db/s/type_lockpings.h"
#include "mongo/db/s/type_locks.h"
#include "mongo/db/storage/duplicate_key_error_info.h"
#include "mongo/executor/network_test_env.h"
#include "mongo/s/catalog/sharding_catalog_client_mock.h"
#include "mongo/s/client/shard_factory.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
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
class DistLockCatalogReplSetTest : public ShardServerTestFixture {
protected:
    std::unique_ptr<ShardingCatalogClient> makeShardingCatalogClient() override {
        return std::make_unique<ShardingCatalogClientMock>();
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

    DistLockCatalogImpl _distLockCatalog;
};

void checkReadConcern(const BSONObj& findCmd) {
    ReadConcernArgs readConcernArgs;
    ASSERT_OK(readConcernArgs.initialize(findCmd[ReadConcernArgs::kReadConcernFieldName]));
    ASSERT(repl::ReadConcernLevel::kMajorityReadConcern == readConcernArgs.getLevel());
}

TEST_F(DistLockCatalogReplSetTest, BasicPing) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        Date_t ping(dateFromISOString("2014-03-11T09:17:18.098Z").getValue());
        auto status = _distLockCatalog.ping(opCtx, "abcd", ping);
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
                lastErrorObject: {
                    n: 1,
                    updatedExisting: true
                },
                value: {
                    _id: "abcd",
                    ping: { $date: "2014-03-11T09:17:18.098Z" }
                }
            })");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogReplSetTest, PingTargetError) {
    configTargeter()->setFindHostReturnValue({ErrorCodes::InternalError, "can't target"});
    auto status = _distLockCatalog.ping(operationContext(), "abcd", Date_t::now());
    ASSERT_NOT_OK(status);
}

TEST_F(DistLockCatalogReplSetTest, PingRunCmdError) {
    shutdownExecutorPool();

    auto status = _distLockCatalog.ping(operationContext(), "abcd", Date_t::now());
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, status.code());
    ASSERT_FALSE(status.reason().empty());
}

TEST_F(DistLockCatalogReplSetTest, PingCommandError) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = _distLockCatalog.ping(opCtx, "abcd", Date_t::now());
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

TEST_F(DistLockCatalogReplSetTest, PingWriteError) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = _distLockCatalog.ping(opCtx, "abcd", Date_t::now());
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

TEST_F(DistLockCatalogReplSetTest, PingWriteConcernError) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = _distLockCatalog.ping(opCtx, "abcd", Date_t::now());
        ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return fromjson(R"({
                ok: 1,
                value: null,
                lastErrorObject: {
                    n: 1,
                    updatedExisting: true
                },
                writeConcernError: {
                    code: 64,
                    errmsg: "waiting for replication timed out"
                }
            })");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogReplSetTest, PingUnsupportedWriteConcernResponse) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = _distLockCatalog.ping(opCtx, "abcd", Date_t::now());
        ASSERT_EQUALS(ErrorCodes::UnsupportedFormat, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        // return non numeric code for writeConcernError.code
        return fromjson(R"({
                ok: 1,
                value: null,
                lastErrorObject: {
                    n: 1,
                    updatedExisting: true
                },
                writeConcernError: {
                    code: "bad format",
                    errmsg: "waiting for replication timed out"
                }
            })");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogReplSetTest, PingUnsupportedResponseFormat) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = _distLockCatalog.ping(opCtx, "abcd", Date_t::now());
        ASSERT_EQUALS(ErrorCodes::TypeMismatch, status.code());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return BSON("ok" << 1 << "value"
                         << "NaN");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogReplSetTest, GrabLockNoOp) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        OID myID("555f80be366c194b13fb0372");
        Date_t now(dateFromISOString("2015-05-22T19:17:18.098Z").getValue());
        auto resultStatus = _distLockCatalog
                                .grabLock(opCtx,
                                          "test",
                                          myID,
                                          0LL,
                                          "me",
                                          "mongos",
                                          now,
                                          "because",
                                          DistLockCatalog::kMajorityWriteConcern)
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
                        term: 0,
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
            ok: 1,
            value: null,
            lastErrorObject: {
                n: 1
            }
        })");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogReplSetTest, GrabLockWithNewDoc) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        OID myID("555f80be366c194b13fb0372");
        Date_t now(dateFromISOString("2015-05-22T19:17:18.098Z").getValue());
        auto resultStatus = _distLockCatalog.grabLock(opCtx,
                                                      "test",
                                                      myID,
                                                      0LL,
                                                      "me",
                                                      "mongos",
                                                      now,
                                                      "because",
                                                      DistLockCatalog::kMajorityWriteConcern);
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
                        term: 0,
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
                    n: 1,
                    updatedExisting: false,
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

TEST_F(DistLockCatalogReplSetTest, GrabLockWithBadLockDoc) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        Date_t now(dateFromISOString("2015-05-22T19:17:18.098Z").getValue());
        auto resultStatus = _distLockCatalog
                                .grabLock(opCtx,
                                          "test",
                                          OID(),
                                          0LL,
                                          "",
                                          "",
                                          now,
                                          "",
                                          DistLockCatalog::kMajorityWriteConcern)
                                .getStatus();
        ASSERT_EQUALS(ErrorCodes::FailedToParse, resultStatus.code());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        // Return an invalid lock document on value. This is theoretically impossible because
        // the vital parts of the resulting doc are derived from the update request.
        return fromjson(R"({
                lastErrorObject: {
                    n: 1,
                    updatedExisting: false,
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

TEST_F(DistLockCatalogReplSetTest, GrabLockTargetError) {
    configTargeter()->setFindHostReturnValue({ErrorCodes::InternalError, "can't target"});

    auto status = _distLockCatalog
                      .grabLock(operationContext(),
                                "",
                                OID::gen(),
                                0LL,
                                "",
                                "",
                                Date_t::now(),
                                "",
                                DistLockCatalog::kMajorityWriteConcern)
                      .getStatus();
    ASSERT_NOT_OK(status);
}

TEST_F(DistLockCatalogReplSetTest, GrabLockRunCmdError) {
    shutdownExecutorPool();

    auto status = _distLockCatalog
                      .grabLock(operationContext(),
                                "",
                                OID::gen(),
                                0LL,
                                "",
                                "",
                                Date_t::now(),
                                "",
                                DistLockCatalog::kMajorityWriteConcern)
                      .getStatus();
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, status.code());
    ASSERT_FALSE(status.reason().empty());
}

TEST_F(DistLockCatalogReplSetTest, GrabLockCommandError) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = _distLockCatalog
                          .grabLock(opCtx,
                                    "",
                                    OID::gen(),
                                    0LL,
                                    "",
                                    "",
                                    Date_t::now(),
                                    "",
                                    DistLockCatalog::kMajorityWriteConcern)
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

TEST_F(DistLockCatalogReplSetTest, GrabLockDupKeyError) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = _distLockCatalog
                          .grabLock(opCtx,
                                    "",
                                    OID::gen(),
                                    0LL,
                                    "",
                                    "",
                                    Date_t::now(),
                                    "",
                                    DistLockCatalog::kMajorityWriteConcern)
                          .getStatus();
        ASSERT_EQUALS(ErrorCodes::LockStateChangeFailed, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return Status({DuplicateKeyErrorInfo(BSON("x" << 1), BSON("" << 1), BSONObj{}),
                       "Mock duplicate key error"});
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogReplSetTest, GrabLockWriteError) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = _distLockCatalog
                          .grabLock(opCtx,
                                    "",
                                    OID::gen(),
                                    0LL,
                                    "",
                                    "",
                                    Date_t::now(),
                                    "",
                                    DistLockCatalog::kMajorityWriteConcern)
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

TEST_F(DistLockCatalogReplSetTest, GrabLockWriteConcernError) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = _distLockCatalog
                          .grabLock(operationContext(),
                                    "",
                                    OID::gen(),
                                    0LL,
                                    "",
                                    "",
                                    Date_t::now(),
                                    "",
                                    DistLockCatalog::kMajorityWriteConcern)
                          .getStatus();
        ASSERT_EQUALS(ErrorCodes::NotWritablePrimary, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return fromjson(R"({
                ok: 1,
                value: null,
                writeConcernError: {
                    code: 10107,
                    errmsg: "Not primary while waiting for write concern"
                }
            })");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogReplSetTest, GrabLockWriteConcernErrorBadType) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = _distLockCatalog
                          .grabLock(operationContext(),
                                    "",
                                    OID::gen(),
                                    0LL,
                                    "",
                                    "",
                                    Date_t::now(),
                                    "",
                                    DistLockCatalog::kMajorityWriteConcern)
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

TEST_F(DistLockCatalogReplSetTest, GrabLockResponseMissingValueField) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        ASSERT_NOT_OK(_distLockCatalog
                          .grabLock(operationContext(),
                                    "",
                                    OID::gen(),
                                    0LL,
                                    "",
                                    "",
                                    Date_t::now(),
                                    "",
                                    DistLockCatalog::kMajorityWriteConcern)
                          .getStatus());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return fromjson(R"({
                ok: 1
            })");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogReplSetTest, GrabLockUnsupportedWriteConcernResponse) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        ASSERT_NOT_OK(_distLockCatalog
                          .grabLock(operationContext(),
                                    "",
                                    OID::gen(),
                                    0LL,
                                    "",
                                    "",
                                    Date_t::now(),
                                    "",
                                    DistLockCatalog::kMajorityWriteConcern)
                          .getStatus());
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

TEST_F(DistLockCatalogReplSetTest, GrabLockUnsupportedResponseFormat) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        ASSERT_NOT_OK(_distLockCatalog
                          .grabLock(operationContext(),
                                    "",
                                    OID::gen(),
                                    0LL,
                                    "",
                                    "",
                                    Date_t::now(),
                                    "",
                                    DistLockCatalog::kMajorityWriteConcern)
                          .getStatus());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return BSON("ok" << 1 << "value"
                         << "NaN");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogReplSetTest, OvertakeLockNoOp) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        OID myID("555f80be366c194b13fb0372");
        OID currentOwner("555f99712c99a78c5b083358");
        Date_t now(dateFromISOString("2015-05-22T19:17:18.098Z").getValue());
        auto resultStatus = _distLockCatalog
                                .overtakeLock(operationContext(),
                                              "test",
                                              myID,
                                              0LL,
                                              currentOwner,
                                              "me",
                                              "mongos",
                                              now,
                                              "because")
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
                        term: 0,
                        why: "because"
                    }
                },
                new: true,
                writeConcern: { w: "majority", wtimeout: 15000 },
                maxTimeMS: 30000
            })"));

        ASSERT_BSONOBJ_EQ(expectedCmd, request.cmdObj);

        return fromjson(R"({
            ok: 1,
            value: null,
            lastErrorObject: {
                n: 0
            }
        })");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogReplSetTest, OvertakeLockWithNewDoc) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        OID myID("555f80be366c194b13fb0372");
        OID currentOwner("555f99712c99a78c5b083358");
        Date_t now(dateFromISOString("2015-05-22T19:17:18.098Z").getValue());
        auto resultStatus = _distLockCatalog.overtakeLock(
            operationContext(), "test", myID, 0LL, currentOwner, "me", "mongos", now, "because");
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
                        term: 0,
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
                    n: 1,
                    updatedExisting: false,
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

TEST_F(DistLockCatalogReplSetTest, OvertakeLockWithBadLockDoc) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        Date_t now(dateFromISOString("2015-05-22T19:17:18.098Z").getValue());
        auto resultStatus =
            _distLockCatalog
                .overtakeLock(operationContext(), "test", OID(), 0LL, OID(), "", "", now, "")
                .getStatus();
        ASSERT_EQUALS(ErrorCodes::FailedToParse, resultStatus.code());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        // Return an invalid lock document on value. This is theoretically impossible because
        // the vital parts of the resulting doc are derived from the update request.
        return fromjson(R"({
                lastErrorObject: {
                    n: 1,
                    updatedExisting: false,
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

TEST_F(DistLockCatalogReplSetTest, OvertakeLockTargetError) {
    configTargeter()->setFindHostReturnValue({ErrorCodes::InternalError, "can't target"});
    auto status =
        _distLockCatalog
            .overtakeLock(operationContext(), "", OID(), 0LL, OID(), "", "", Date_t::now(), "")
            .getStatus();
    ASSERT_NOT_OK(status);
}

TEST_F(DistLockCatalogReplSetTest, OvertakeLockRunCmdError) {
    shutdownExecutorPool();

    auto status =
        _distLockCatalog
            .overtakeLock(operationContext(), "", OID(), 0LL, OID(), "", "", Date_t::now(), "")
            .getStatus();
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, status.code());
    ASSERT_FALSE(status.reason().empty());
}

TEST_F(DistLockCatalogReplSetTest, OvertakeLockCommandError) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status =
            _distLockCatalog
                .overtakeLock(operationContext(), "", OID(), 0LL, OID(), "", "", Date_t::now(), "")
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

TEST_F(DistLockCatalogReplSetTest, OvertakeLockWriteError) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status =
            _distLockCatalog
                .overtakeLock(operationContext(), "", OID(), 0LL, OID(), "", "", Date_t::now(), "")
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

TEST_F(DistLockCatalogReplSetTest, OvertakeLockWriteConcernError) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status =
            _distLockCatalog
                .overtakeLock(operationContext(), "", OID(), 0LL, OID(), "", "", Date_t::now(), "")
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

TEST_F(DistLockCatalogReplSetTest, OvertakeLockUnsupportedWriteConcernResponse) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status =
            _distLockCatalog
                .overtakeLock(operationContext(), "", OID(), 0LL, OID(), "", "", Date_t::now(), "")
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

TEST_F(DistLockCatalogReplSetTest, OvertakeLockUnsupportedResponseFormat) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        ASSERT_NOT_OK(
            _distLockCatalog
                .overtakeLock(operationContext(), "", OID(), 0LL, OID(), "", "", Date_t::now(), "")
                .getStatus());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return BSON("ok" << 1 << "value"
                         << "NaN");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogReplSetTest, BasicUnlock) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = _distLockCatalog.unlock(
            operationContext(), OID("555f99712c99a78c5b083358"), "TestName");
        ASSERT_OK(status);
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        ASSERT_EQUALS(dummyHost, request.target);
        ASSERT_EQUALS("config", request.dbname);

        BSONObj expectedCmd(fromjson(R"({
                findAndModify: "locks",
                query: { ts: ObjectId("555f99712c99a78c5b083358"), _id: "TestName" },
                update: { $set: { state: 0 }},
                writeConcern: { w: "majority", wtimeout: 15000 },
                maxTimeMS: 30000
            })"));

        ASSERT_BSONOBJ_EQ(expectedCmd, request.cmdObj);

        return fromjson(R"({
                ok: 1,
                lastErrorObject: {
                    n: 1,
                    updatedExisting: true
                },
                value: {
                    _id: "",
                    ts: ObjectId("555f99712c99a78c5b083358"),
                    state: 0
                }
            })");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogReplSetTest, BasicUnlockWithName) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        ASSERT_OK(_distLockCatalog.unlock(
            operationContext(), OID("555f99712c99a78c5b083358"), "TestDB.TestColl"));
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
                lastErrorObject: {
                    n: 1,
                    updatedExisting: true
                },
                value: {
                    _id: "TestDB.TestColl",
                    ts: ObjectId("555f99712c99a78c5b083358"),
                    state: 0
                }
            })");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogReplSetTest, UnlockWithNoNewDoc) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = _distLockCatalog.unlock(
            operationContext(), OID("555f99712c99a78c5b083358"), "TestName");
        ASSERT_OK(status);
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        ASSERT_EQUALS(dummyHost, request.target);
        ASSERT_EQUALS("config", request.dbname);

        BSONObj expectedCmd(fromjson(R"({
                findAndModify: "locks",
                query: { ts: ObjectId("555f99712c99a78c5b083358"), _id: "TestName" },
                update: { $set: { state: 0 }},
                writeConcern: { w: "majority", wtimeout: 15000 },
                maxTimeMS: 30000
            })"));

        ASSERT_BSONOBJ_EQ(expectedCmd, request.cmdObj);

        return fromjson(R"({
                ok: 1,
                lastErrorObject: {
                    n: 0
                },
                value: null
            })");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogReplSetTest, UnlockWithNameWithNoNewDoc) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = _distLockCatalog.unlock(
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
                lastErrorObject: {
                    n: 0
                },
                value: null
            })");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogReplSetTest, UnlockTargetError) {
    configTargeter()->setFindHostReturnValue({ErrorCodes::InternalError, "can't target"});
    auto status = _distLockCatalog.unlock(operationContext(), OID(), "TestName");
    ASSERT_NOT_OK(status);
}

TEST_F(DistLockCatalogReplSetTest, UnlockRunCmdError) {
    shutdownExecutorPool();

    auto status = _distLockCatalog.unlock(operationContext(), OID(), "TestName");
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, status.code());
    ASSERT_FALSE(status.reason().empty());
}

TEST_F(DistLockCatalogReplSetTest, UnlockCommandError) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = _distLockCatalog.unlock(operationContext(), OID(), "TestName");
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

TEST_F(DistLockCatalogReplSetTest, UnlockWriteError) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = _distLockCatalog.unlock(operationContext(), OID(), "TestName");
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

TEST_F(DistLockCatalogReplSetTest, UnlockWriteConcernError) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = _distLockCatalog.unlock(operationContext(), OID(), "TestName");
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

    future.default_timed_get();
}

TEST_F(DistLockCatalogReplSetTest, UnlockUnsupportedWriteConcernResponse) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = _distLockCatalog.unlock(operationContext(), OID(), "TestName");
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

TEST_F(DistLockCatalogReplSetTest, UnlockUnsupportedResponseFormat) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        ASSERT_NOT_OK(_distLockCatalog.unlock(operationContext(), OID(), "TestName"));
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return BSON("ok" << 1 << "value"
                         << "NaN");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogReplSetTest, BasicUnlockAll) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = _distLockCatalog.unlockAll(operationContext(), "processID", boost::none);
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
                          update.getU().getUpdateModifier());

        return BSON("ok" << 1);
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogReplSetTest, UnlockAllWriteFailed) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = _distLockCatalog.unlockAll(operationContext(), "processID", boost::none);
        ASSERT_EQUALS(ErrorCodes::IllegalOperation, status);
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return BSON("ok" << 0 << "code" << ErrorCodes::IllegalOperation << "errmsg"
                         << "something went wrong");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogReplSetTest, UnlockAllNetworkError) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = _distLockCatalog.unlockAll(operationContext(), "processID", boost::none);
        ASSERT_EQUALS(ErrorCodes::NetworkTimeout, status);
    });

    for (int i = 0; i < 3; i++) {  // ShardRegistry will retry 3 times on network errors
        onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
            return Status(ErrorCodes::NetworkTimeout, "network error");
        });
    }

    future.default_timed_get();
}

TEST_F(DistLockCatalogReplSetTest, BasicGetServerInfo) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        Date_t localTime(dateFromISOString("2015-05-26T13:06:27.293Z").getValue());
        OID electionID("555fa85d4d8640862a0fc79b");
        auto resultStatus = _distLockCatalog.getServerInfo(operationContext());
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

TEST_F(DistLockCatalogReplSetTest, GetServerTargetError) {
    configTargeter()->setFindHostReturnValue({ErrorCodes::InternalError, "can't target"});
    auto status = _distLockCatalog.getServerInfo(operationContext()).getStatus();
    ASSERT_NOT_OK(status);
}

TEST_F(DistLockCatalogReplSetTest, GetServerRunCmdError) {
    shutdownExecutorPool();

    auto status = _distLockCatalog.getServerInfo(operationContext()).getStatus();
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, status.code());
    ASSERT_FALSE(status.reason().empty());
}

TEST_F(DistLockCatalogReplSetTest, GetServerCommandError) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = _distLockCatalog.getServerInfo(operationContext()).getStatus();
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

TEST_F(DistLockCatalogReplSetTest, GetServerBadElectionId) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = _distLockCatalog.getServerInfo(operationContext()).getStatus();
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

TEST_F(DistLockCatalogReplSetTest, GetServerBadLocalTime) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = _distLockCatalog.getServerInfo(operationContext()).getStatus();
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

TEST_F(DistLockCatalogReplSetTest, GetServerNoGLEStats) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = _distLockCatalog.getServerInfo(operationContext()).getStatus();
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

TEST_F(DistLockCatalogReplSetTest, GetServerNoElectionId) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = _distLockCatalog.getServerInfo(operationContext()).getStatus();
        ASSERT_EQUALS(ErrorCodes::NotWritablePrimary, status.code());
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

TEST_F(DistLockCatalogReplSetTest, GetServerInvalidReplSubsectionShouldFail) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = _distLockCatalog.getServerInfo(operationContext()).getStatus();
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

TEST_F(DistLockCatalogReplSetTest, GetServerNoElectionIdButMasterShouldFail) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = _distLockCatalog.getServerInfo(operationContext()).getStatus();
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

TEST_F(DistLockCatalogReplSetTest, BasicStopPing) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        ASSERT_OK(_distLockCatalog.stopPing(operationContext(), "test"));
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
                lastErrorObject: {
                    n: 1,
                    updatedExisting: true
                },
                value: {
                  _id: "test",
                  ping: { $date: "2014-03-11T09:17:18.098Z" }
                }
            })");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogReplSetTest, StopPingTargetError) {
    configTargeter()->setFindHostReturnValue({ErrorCodes::InternalError, "can't target"});
    auto status = _distLockCatalog.stopPing(operationContext(), "");
    ASSERT_NOT_OK(status);
}

TEST_F(DistLockCatalogReplSetTest, StopPingRunCmdError) {
    shutdownExecutorPool();

    auto status = _distLockCatalog.stopPing(operationContext(), "");
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, status.code());
    ASSERT_FALSE(status.reason().empty());
}

TEST_F(DistLockCatalogReplSetTest, StopPingCommandError) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = _distLockCatalog.stopPing(operationContext(), "");
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

TEST_F(DistLockCatalogReplSetTest, StopPingWriteError) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = _distLockCatalog.stopPing(operationContext(), "");
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

TEST_F(DistLockCatalogReplSetTest, StopPingWriteConcernError) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = _distLockCatalog.stopPing(operationContext(), "");
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

TEST_F(DistLockCatalogReplSetTest, StopPingUnsupportedWriteConcernResponse) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = _distLockCatalog.stopPing(operationContext(), "");
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

TEST_F(DistLockCatalogReplSetTest, StopPingUnsupportedResponseFormat) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        ASSERT_NOT_OK(_distLockCatalog.stopPing(operationContext(), ""));
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return BSON("ok" << 1 << "value"
                         << "NaN");
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogReplSetTest, BasicGetPing) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        Date_t ping(dateFromISOString("2015-05-26T13:06:27.293Z").getValue());
        auto resultStatus = _distLockCatalog.getPing(operationContext(), "test");
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

TEST_F(DistLockCatalogReplSetTest, GetPingTargetError) {
    configTargeter()->setFindHostReturnValue({ErrorCodes::InternalError, "can't target"});
    auto status = _distLockCatalog.getPing(operationContext(), "").getStatus();
    ASSERT_EQUALS(ErrorCodes::InternalError, status.code());
}

TEST_F(DistLockCatalogReplSetTest, GetPingRunCmdError) {
    shutdownExecutorPool();

    auto status = _distLockCatalog.getPing(operationContext(), "").getStatus();
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, status.code());
    ASSERT_FALSE(status.reason().empty());
}

TEST_F(DistLockCatalogReplSetTest, GetPingNotFound) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = _distLockCatalog.getPing(operationContext(), "").getStatus();
        ASSERT_EQUALS(ErrorCodes::NoMatchingDocument, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onFindCommand([](const RemoteCommandRequest& request) -> StatusWith<std::vector<BSONObj>> {
        return std::vector<BSONObj>();
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogReplSetTest, GetPingUnsupportedFormat) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = _distLockCatalog.getPing(operationContext(), "test").getStatus();
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

TEST_F(DistLockCatalogReplSetTest, BasicGetLockByName) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        OID ts("555f99712c99a78c5b083358");
        auto resultStatus = _distLockCatalog.getLockByName(operationContext(), "abc");
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

TEST_F(DistLockCatalogReplSetTest, GetLockByNameTargetError) {
    configTargeter()->setFindHostReturnValue({ErrorCodes::InternalError, "can't target"});
    auto status = _distLockCatalog.getLockByName(operationContext(), "x").getStatus();
    ASSERT_EQUALS(ErrorCodes::InternalError, status.code());
}

TEST_F(DistLockCatalogReplSetTest, GetLockByNameRunCmdError) {
    shutdownExecutorPool();

    auto status = _distLockCatalog.getLockByName(operationContext(), "x").getStatus();
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, status.code());
    ASSERT_FALSE(status.reason().empty());
}

TEST_F(DistLockCatalogReplSetTest, GetLockByNameNotFound) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = _distLockCatalog.getLockByName(operationContext(), "x").getStatus();
        ASSERT_EQUALS(ErrorCodes::LockNotFound, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onFindCommand([](const RemoteCommandRequest& request) -> StatusWith<std::vector<BSONObj>> {
        return std::vector<BSONObj>();
    });

    future.default_timed_get();
}

TEST_F(DistLockCatalogReplSetTest, GetLockByNameUnsupportedFormat) {
    auto future = launchOnSeparateThread([this](OperationContext* opCtx) {
        auto status = _distLockCatalog.getLockByName(operationContext(), "x").getStatus();
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
