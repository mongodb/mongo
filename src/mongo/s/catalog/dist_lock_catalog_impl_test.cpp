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

#include "mongo/platform/basic.h"

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/json.h"
#include "mongo/client/find_and_modify_request.h"
#include "mongo/client/remote_command_runner_mock.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/jsobj.h"
#include "mongo/s/catalog/dist_lock_catalog_impl.h"
#include "mongo/s/type_lockpings.h"
#include "mongo/s/type_locks.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {

    const HostAndPort dummyHost("dummy", 123);
    const Milliseconds kWTimeout(100);

    const auto noTest = [](const RemoteCommandRequest& request) {};

    TEST(DistLockCatalogImpl, BasicPing) {
        RemoteCommandTargeterMock targeter;
        RemoteCommandRunnerMock executor;

        targeter.setFindHostReturnValue(dummyHost);

        DistLockCatalogImpl catalog(&targeter, &executor, kWTimeout);

        executor.setNextExpectedCommand([](const RemoteCommandRequest& request) {
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
                writeConcern: { w: "majority", j: true, wtimeout: 100 }
            })"));

            ASSERT_EQUALS(expectedCmd, request.cmdObj);
        },
        RemoteCommandResponse(BSON("ok" << 1 << "nModified" << 1 << "n" << 1), Milliseconds(0)));

        Date_t ping(dateFromISOString("2014-03-11T09:17:18.098Z").getValue());
        auto status = catalog.ping("abcd", ping);
        ASSERT_OK(status);
    }

    TEST(DistLockCatalogImpl, PingTargetError) {
        RemoteCommandTargeterMock targeter;
        RemoteCommandRunnerMock executor;

        targeter.setFindHostReturnValue({ErrorCodes::InternalError, "can't target"});

        DistLockCatalogImpl catalog(&targeter, &executor, kWTimeout);

        auto status = catalog.ping("abcd", Date_t::now());
        ASSERT_NOT_OK(status);
    }

    TEST(DistLockCatalogImpl, PingRunnerError) {
        RemoteCommandTargeterMock targeter;
        RemoteCommandRunnerMock executor;

        targeter.setFindHostReturnValue(dummyHost);

        DistLockCatalogImpl catalog(&targeter, &executor, kWTimeout);

        executor.setNextExpectedCommand(noTest, {ErrorCodes::InternalError, "Bad"});

        auto status = catalog.ping("abcd", Date_t::now());
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::InternalError, status.code());
        ASSERT_FALSE(status.reason().empty());
    }

    TEST(DistLockCatalogImpl, PingCommandError) {
        RemoteCommandTargeterMock targeter;
        RemoteCommandRunnerMock executor;

        targeter.setFindHostReturnValue(dummyHost);

        DistLockCatalogImpl catalog(&targeter, &executor, kWTimeout);

        BSONObj returnObj(fromjson(R"({
            ok: 0,
            errmsg: "bad",
            code: 9
        })"));
        executor.setNextExpectedCommand(noTest, RemoteCommandResponse(returnObj, Milliseconds(0)));

        auto status = catalog.ping("abcd", Date_t::now());
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
        ASSERT_FALSE(status.reason().empty());
    }

    TEST(DistLockCatalogImpl, PingWriteError) {
        RemoteCommandTargeterMock targeter;
        RemoteCommandRunnerMock executor;

        targeter.setFindHostReturnValue(dummyHost);

        DistLockCatalogImpl catalog(&targeter, &executor, kWTimeout);

        BSONObj returnObj(fromjson(R"({
            ok: 0,
            code: 11000,
            errmsg: "E11000 duplicate key error"
        })"));
        executor.setNextExpectedCommand(noTest, RemoteCommandResponse(returnObj, Milliseconds(0)));

        auto status = catalog.ping("abcd", Date_t::now());
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::DuplicateKey, status.code());
        ASSERT_FALSE(status.reason().empty());
    }

    TEST(DistLockCatalogImpl, PingWriteConcernError) {
        RemoteCommandTargeterMock targeter;
        RemoteCommandRunnerMock executor;

        targeter.setFindHostReturnValue(dummyHost);

        DistLockCatalogImpl catalog(&targeter, &executor, kWTimeout);

        BSONObj returnObj(fromjson(R"({
            ok: 1,
            value: null,
            writeConcernError: {
                code: 64,
                errmsg: "waiting for replication timed out"
            }
        })"));
        executor.setNextExpectedCommand(noTest, RemoteCommandResponse(returnObj, Milliseconds(0)));

        auto status = catalog.ping("abcd", Date_t::now());
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, status.code());
        ASSERT_FALSE(status.reason().empty());
    }

    TEST(DistLockCatalogImpl, PingUnsupportedWriteConcernResponse) {
        RemoteCommandTargeterMock targeter;
        RemoteCommandRunnerMock executor;

        targeter.setFindHostReturnValue(dummyHost);

        DistLockCatalogImpl catalog(&targeter, &executor, kWTimeout);

        BSONObj returnObj(fromjson(R"({
            ok: 1,
            value: null,
            writeConcernError: {
                code: "bad format",
                errmsg: "waiting for replication timed out"
            }
        })"));
        executor.setNextExpectedCommand(noTest, RemoteCommandResponse(returnObj, Milliseconds(0)));

        auto status = catalog.ping("abcd", Date_t::now());
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::UnsupportedFormat, status.code());
        ASSERT_FALSE(status.reason().empty());
    }

    TEST(DistLockCatalogImpl, PingUnsupportedResponseFormat) {
        RemoteCommandTargeterMock targeter;
        RemoteCommandRunnerMock executor;

        targeter.setFindHostReturnValue(dummyHost);

        DistLockCatalogImpl catalog(&targeter, &executor, kWTimeout);

        executor.setNextExpectedCommand(noTest,
                RemoteCommandResponse(BSON("ok" << 1 << "value" << "NaN"), Milliseconds(0)));

        auto status = catalog.ping("abcd", Date_t::now());
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::UnsupportedFormat, status.code());
    }

    TEST(DistLockCatalogImpl, GrabLockNoOp) {
        RemoteCommandTargeterMock targeter;
        RemoteCommandRunnerMock executor;

        targeter.setFindHostReturnValue(dummyHost);

        DistLockCatalogImpl catalog(&targeter, &executor, kWTimeout);

        BSONObj responseObj(fromjson("{ ok: 1, value: null }"));

        executor.setNextExpectedCommand([](const RemoteCommandRequest& request) {
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
                writeConcern: { w: "majority", j: true, wtimeout: 100 }
            })"));

            ASSERT_EQUALS(expectedCmd, request.cmdObj);
        },
        RemoteCommandResponse(responseObj, Milliseconds(0)));

        OID myID("555f80be366c194b13fb0372");
        Date_t now(dateFromISOString("2015-05-22T19:17:18.098Z").getValue());
        auto resultStatus = catalog.grabLock("test", myID, "me", "mongos", now, "because");

        ASSERT_OK(resultStatus.getStatus());

        const auto& lockDoc = resultStatus.getValue();
        ASSERT_FALSE(lockDoc.isValid(nullptr));
    }

    TEST(DistLockCatalogImpl, GrabLockWithNewDoc) {
        RemoteCommandTargeterMock targeter;
        RemoteCommandRunnerMock executor;

        targeter.setFindHostReturnValue(dummyHost);

        DistLockCatalogImpl catalog(&targeter, &executor, kWTimeout);

        BSONObj responseObj(fromjson(R"({
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
        })"));

        executor.setNextExpectedCommand([](const RemoteCommandRequest& request) {
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
                writeConcern: { w: "majority", j: true, wtimeout: 100 }
            })"));

            ASSERT_EQUALS(expectedCmd, request.cmdObj);
        },
        RemoteCommandResponse(responseObj, Milliseconds(0)));

        OID myID("555f80be366c194b13fb0372");
        Date_t now(dateFromISOString("2015-05-22T19:17:18.098Z").getValue());
        auto resultStatus = catalog.grabLock("test", myID, "me", "mongos", now, "because");
        ASSERT_OK(resultStatus.getStatus());

        const auto& lockDoc = resultStatus.getValue();
        ASSERT_TRUE(lockDoc.isValid(nullptr));
        ASSERT_EQUALS("test", lockDoc.getName());
        ASSERT_EQUALS(myID, lockDoc.getLockID());
        ASSERT_EQUALS("me", lockDoc.getWho());
        ASSERT_EQUALS("mongos", lockDoc.getProcess());
        ASSERT_EQUALS("because", lockDoc.getWhy());
    }

    TEST(DistLockCatalogImpl, GrabLockTargetError) {
        RemoteCommandTargeterMock targeter;
        RemoteCommandRunnerMock executor;

        targeter.setFindHostReturnValue({ErrorCodes::InternalError, "can't target"});

        DistLockCatalogImpl catalog(&targeter, &executor, kWTimeout);

        auto status = catalog.grabLock("", OID::gen(), "", "", Date_t::now(), "").getStatus();
        ASSERT_NOT_OK(status);
    }

    TEST(DistLockCatalogImpl, GrabLockRunnerError) {
        RemoteCommandTargeterMock targeter;
        RemoteCommandRunnerMock executor;

        targeter.setFindHostReturnValue(dummyHost);

        DistLockCatalogImpl catalog(&targeter, &executor, kWTimeout);

        executor.setNextExpectedCommand(noTest, {ErrorCodes::InternalError, "Bad"});

        auto status = catalog.grabLock("", OID::gen(), "", "", Date_t::now(), "").getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::InternalError, status.code());
        ASSERT_FALSE(status.reason().empty());
    }

    TEST(DistLockCatalogImpl, GrabLockCommandError) {
        RemoteCommandTargeterMock targeter;
        RemoteCommandRunnerMock executor;

        targeter.setFindHostReturnValue(dummyHost);

        DistLockCatalogImpl catalog(&targeter, &executor, kWTimeout);

        BSONObj returnObj(fromjson(R"({
            ok: 0,
            errmsg: "bad",
            code: 9
        })"));
        executor.setNextExpectedCommand(noTest, RemoteCommandResponse(returnObj, Milliseconds(0)));

        auto status = catalog.grabLock("", OID::gen(), "", "", Date_t::now(), "").getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
        ASSERT_FALSE(status.reason().empty());
    }

    TEST(DistLockCatalogImpl, GrabLockWriteError) {
        RemoteCommandTargeterMock targeter;
        RemoteCommandRunnerMock executor;

        targeter.setFindHostReturnValue(dummyHost);

        DistLockCatalogImpl catalog(&targeter, &executor, kWTimeout);

        BSONObj returnObj(fromjson(R"({
            ok: 0,
            code: 11000,
            errmsg: "E11000 duplicate key error"
        })"));
        executor.setNextExpectedCommand(noTest, RemoteCommandResponse(returnObj, Milliseconds(0)));

        auto status = catalog.grabLock("", OID::gen(), "", "", Date_t::now(), "").getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::DuplicateKey, status.code());
        ASSERT_FALSE(status.reason().empty());
    }

    TEST(DistLockCatalogImpl, GrabLockWriteConcernError) {
        RemoteCommandTargeterMock targeter;
        RemoteCommandRunnerMock executor;

        targeter.setFindHostReturnValue(dummyHost);

        DistLockCatalogImpl catalog(&targeter, &executor, kWTimeout);

        BSONObj returnObj(fromjson(R"({
            ok: 1,
            value: null,
            writeConcernError: {
                code: 64,
                errmsg: "waiting for replication timed out"
            }
        })"));
        executor.setNextExpectedCommand(noTest, RemoteCommandResponse(returnObj, Milliseconds(0)));

        auto status = catalog.grabLock("", OID::gen(), "", "", Date_t::now(), "").getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, status.code());
        ASSERT_FALSE(status.reason().empty());
    }

    TEST(DistLockCatalogImpl, GrabLockUnsupportedWriteConcernResponse) {
        RemoteCommandTargeterMock targeter;
        RemoteCommandRunnerMock executor;

        targeter.setFindHostReturnValue(dummyHost);

        DistLockCatalogImpl catalog(&targeter, &executor, kWTimeout);

        BSONObj returnObj(fromjson(R"({
            ok: 1,
            value: null,
            writeConcernError: {
                code: "bad format",
                errmsg: "waiting for replication timed out"
            }
        })"));
        executor.setNextExpectedCommand(noTest,
                RemoteCommandResponse(returnObj, Milliseconds(0)));

        auto status = catalog.grabLock("", OID::gen(), "", "", Date_t::now(), "").getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::UnsupportedFormat, status.code());
        ASSERT_FALSE(status.reason().empty());
    }

    TEST(DistLockCatalogImpl, GrabLockUnsupportedResponseFormat) {
        RemoteCommandTargeterMock targeter;
        RemoteCommandRunnerMock executor;

        targeter.setFindHostReturnValue(dummyHost);

        DistLockCatalogImpl catalog(&targeter, &executor, kWTimeout);

        executor.setNextExpectedCommand(noTest,
                RemoteCommandResponse(BSON("ok" << 1 << "value" << "NaN"), Milliseconds(0)));

        auto status = catalog.grabLock("", OID::gen(), "", "", Date_t::now(), "").getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::UnsupportedFormat, status.code());
    }

    TEST(DistLockCatalogImpl, OvertakeLockNoOp) {
        RemoteCommandTargeterMock targeter;
        RemoteCommandRunnerMock executor;

        targeter.setFindHostReturnValue(dummyHost);

        DistLockCatalogImpl catalog(&targeter, &executor, kWTimeout);

        BSONObj responseObj(fromjson("{ ok: 1, value: null }"));

        executor.setNextExpectedCommand([](const RemoteCommandRequest& request) {
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
                writeConcern: { w: "majority", j: true, wtimeout: 100 }
            })"));

            ASSERT_EQUALS(expectedCmd, request.cmdObj);
        },
        RemoteCommandResponse(responseObj, Milliseconds(0)));

        OID myID("555f80be366c194b13fb0372");
        OID currentOwner("555f99712c99a78c5b083358");
        Date_t now(dateFromISOString("2015-05-22T19:17:18.098Z").getValue());
        auto resultStatus = catalog.overtakeLock("test", myID, currentOwner,
                "me", "mongos", now, "because");

        ASSERT_OK(resultStatus.getStatus());

        const auto& lockDoc = resultStatus.getValue();
        ASSERT_FALSE(lockDoc.isValid(nullptr));
    }

    TEST(DistLockCatalogImpl, OvertakeLockWithNewDoc) {
        RemoteCommandTargeterMock targeter;
        RemoteCommandRunnerMock executor;

        targeter.setFindHostReturnValue(dummyHost);

        DistLockCatalogImpl catalog(&targeter, &executor, kWTimeout);

        BSONObj responseObj(fromjson(R"({
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
        })"));

        executor.setNextExpectedCommand([](const RemoteCommandRequest& request) {
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
                writeConcern: { w: "majority", j: true, wtimeout: 100 }
            })"));

            ASSERT_EQUALS(expectedCmd, request.cmdObj);
        },
        RemoteCommandResponse(responseObj, Milliseconds(0)));

        OID myID("555f80be366c194b13fb0372");
        OID currentOwner("555f99712c99a78c5b083358");
        Date_t now(dateFromISOString("2015-05-22T19:17:18.098Z").getValue());
        auto resultStatus = catalog.overtakeLock("test", myID, currentOwner,
                "me", "mongos", now, "because");
        ASSERT_OK(resultStatus.getStatus());

        const auto& lockDoc = resultStatus.getValue();
        ASSERT_TRUE(lockDoc.isValid(nullptr));
        ASSERT_EQUALS("test", lockDoc.getName());
        ASSERT_EQUALS(myID, lockDoc.getLockID());
        ASSERT_EQUALS("me", lockDoc.getWho());
        ASSERT_EQUALS("mongos", lockDoc.getProcess());
        ASSERT_EQUALS("because", lockDoc.getWhy());
    }

    TEST(DistLockCatalogImpl, OvertakeLockTargetError) {
        RemoteCommandTargeterMock targeter;
        RemoteCommandRunnerMock executor;

        targeter.setFindHostReturnValue({ErrorCodes::InternalError, "can't target"});

        DistLockCatalogImpl catalog(&targeter, &executor, kWTimeout);

        auto status = catalog.overtakeLock(
                "", OID(), OID(), "", "", Date_t::now(), "").getStatus();
        ASSERT_NOT_OK(status);
    }

    TEST(DistLockCatalogImpl, OvertakeLockRunnerError) {
        RemoteCommandTargeterMock targeter;
        RemoteCommandRunnerMock executor;

        targeter.setFindHostReturnValue(dummyHost);

        DistLockCatalogImpl catalog(&targeter, &executor, kWTimeout);

        executor.setNextExpectedCommand(noTest, {ErrorCodes::InternalError, "Bad"});

        auto status = catalog.overtakeLock(
                "", OID(), OID(), "", "", Date_t::now(), "").getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::InternalError, status.code());
        ASSERT_FALSE(status.reason().empty());
    }

    TEST(DistLockCatalogImpl, OvertakeLockCommandError) {
        RemoteCommandTargeterMock targeter;
        RemoteCommandRunnerMock executor;

        targeter.setFindHostReturnValue(dummyHost);

        DistLockCatalogImpl catalog(&targeter, &executor, kWTimeout);

        BSONObj returnObj(fromjson(R"({
            ok: 0,
            errmsg: "bad",
            code: 9
        })"));
        executor.setNextExpectedCommand(noTest, RemoteCommandResponse(returnObj, Milliseconds(0)));

        auto status = catalog.overtakeLock(
                "", OID(), OID(), "", "", Date_t::now(), "").getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
        ASSERT_FALSE(status.reason().empty());
    }

    TEST(DistLockCatalogImpl, OvertakeLockWriteError) {
        RemoteCommandTargeterMock targeter;
        RemoteCommandRunnerMock executor;

        targeter.setFindHostReturnValue(dummyHost);

        DistLockCatalogImpl catalog(&targeter, &executor, kWTimeout);

        BSONObj returnObj(fromjson(R"({
            ok: 0,
            code: 11000,
            errmsg: "E11000 duplicate key error"
        })"));
        executor.setNextExpectedCommand(noTest, RemoteCommandResponse(returnObj, Milliseconds(0)));

        auto status = catalog.overtakeLock(
                "", OID(), OID(), "", "", Date_t::now(), "").getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::DuplicateKey, status.code());
        ASSERT_FALSE(status.reason().empty());
    }

    TEST(DistLockCatalogImpl, OvertakeLockWriteConcernError) {
        RemoteCommandTargeterMock targeter;
        RemoteCommandRunnerMock executor;

        targeter.setFindHostReturnValue(dummyHost);

        DistLockCatalogImpl catalog(&targeter, &executor, kWTimeout);

        BSONObj returnObj(fromjson(R"({
            ok: 1,
            value: null,
            writeConcernError: {
                code: 64,
                errmsg: "waiting for replication timed out"
            }
        })"));
        executor.setNextExpectedCommand(noTest, RemoteCommandResponse(returnObj, Milliseconds(0)));

        auto status = catalog.overtakeLock(
                "", OID(), OID(), "", "", Date_t::now(), "").getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, status.code());
        ASSERT_FALSE(status.reason().empty());
    }

    TEST(DistLockCatalogImpl, OvertakeLockUnsupportedWriteConcernResponse) {
        RemoteCommandTargeterMock targeter;
        RemoteCommandRunnerMock executor;

        targeter.setFindHostReturnValue(dummyHost);

        DistLockCatalogImpl catalog(&targeter, &executor, kWTimeout);

        BSONObj returnObj(fromjson(R"({
            ok: 1,
            value: null,
            writeConcernError: {
                code: "bad format",
                errmsg: "waiting for replication timed out"
            }
        })"));
        executor.setNextExpectedCommand(noTest,
                RemoteCommandResponse(returnObj, Milliseconds(0)));

        auto status = catalog.overtakeLock(
                "", OID(), OID(), "", "", Date_t::now(), "").getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::UnsupportedFormat, status.code());
        ASSERT_FALSE(status.reason().empty());
    }

    TEST(DistLockCatalogImpl, OvertakeLockUnsupportedResponseFormat) {
        RemoteCommandTargeterMock targeter;
        RemoteCommandRunnerMock executor;

        targeter.setFindHostReturnValue(dummyHost);

        DistLockCatalogImpl catalog(&targeter, &executor, kWTimeout);

        executor.setNextExpectedCommand(noTest,
                RemoteCommandResponse(BSON("ok" << 1 << "value" << "NaN"), Milliseconds(0)));

        auto status = catalog.overtakeLock(
                "", OID(), OID(), "", "", Date_t::now(), "").getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::UnsupportedFormat, status.code());
    }

    TEST(DistLockCatalogImpl, BasicUnlock) {
        RemoteCommandTargeterMock targeter;
        RemoteCommandRunnerMock executor;

        targeter.setFindHostReturnValue(dummyHost);

        DistLockCatalogImpl catalog(&targeter, &executor, kWTimeout);

        executor.setNextExpectedCommand([](const RemoteCommandRequest& request) {
            ASSERT_EQUALS(dummyHost, request.target);
            ASSERT_EQUALS("config", request.dbname);

            BSONObj expectedCmd(fromjson(R"({
                findAndModify: "locks",
                query: { ts: ObjectId("555f99712c99a78c5b083358") },
                update: { $set: { state: 0 }},
                writeConcern: { w: "majority", j: true, wtimeout: 100 }
            })"));

            ASSERT_EQUALS(expectedCmd, request.cmdObj);
        },
        RemoteCommandResponse(BSON("ok" << 1 << "nModified" << 1 << "n" << 1), Milliseconds(0)));

        auto status = catalog.unlock(OID("555f99712c99a78c5b083358"));
        ASSERT_OK(status);
    }

    TEST(DistLockCatalogImpl, UnlockTargetError) {
        RemoteCommandTargeterMock targeter;
        RemoteCommandRunnerMock executor;

        targeter.setFindHostReturnValue({ErrorCodes::InternalError, "can't target"});

        DistLockCatalogImpl catalog(&targeter, &executor, kWTimeout);

        auto status = catalog.unlock(OID());
        ASSERT_NOT_OK(status);
    }

    TEST(DistLockCatalogImpl, UnlockRunnerError) {
        RemoteCommandTargeterMock targeter;
        RemoteCommandRunnerMock executor;

        targeter.setFindHostReturnValue(dummyHost);

        DistLockCatalogImpl catalog(&targeter, &executor, kWTimeout);

        executor.setNextExpectedCommand(noTest, {ErrorCodes::InternalError, "Bad"});

        auto status = catalog.unlock(OID());
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::InternalError, status.code());
        ASSERT_FALSE(status.reason().empty());
    }

    TEST(DistLockCatalogImpl, UnlockCommandError) {
        RemoteCommandTargeterMock targeter;
        RemoteCommandRunnerMock executor;

        targeter.setFindHostReturnValue(dummyHost);

        DistLockCatalogImpl catalog(&targeter, &executor, kWTimeout);

        BSONObj returnObj(fromjson(R"({
            ok: 0,
            errmsg: "bad",
            code: 9
        })"));
        executor.setNextExpectedCommand(noTest, RemoteCommandResponse(returnObj, Milliseconds(0)));

        auto status = catalog.unlock(OID());
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
        ASSERT_FALSE(status.reason().empty());
    }

    TEST(DistLockCatalogImpl, UnlockWriteError) {
        RemoteCommandTargeterMock targeter;
        RemoteCommandRunnerMock executor;

        targeter.setFindHostReturnValue(dummyHost);

        DistLockCatalogImpl catalog(&targeter, &executor, kWTimeout);

        BSONObj returnObj(fromjson(R"({
            ok: 0,
            code: 11000,
            errmsg: "E11000 duplicate key error"
        })"));
        executor.setNextExpectedCommand(noTest, RemoteCommandResponse(returnObj, Milliseconds(0)));

        auto status = catalog.unlock(OID());
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::DuplicateKey, status.code());
        ASSERT_FALSE(status.reason().empty());
    }

    TEST(DistLockCatalogImpl, UnlockWriteConcernError) {
        RemoteCommandTargeterMock targeter;
        RemoteCommandRunnerMock executor;

        targeter.setFindHostReturnValue(dummyHost);

        DistLockCatalogImpl catalog(&targeter, &executor, kWTimeout);

        BSONObj returnObj(fromjson(R"({
            ok: 1,
            value: null,
            writeConcernError: {
                code: 64,
                errmsg: "waiting for replication timed out"
            }
        })"));
        executor.setNextExpectedCommand(noTest, RemoteCommandResponse(returnObj, Milliseconds(0)));

        auto status = catalog.unlock(OID());
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, status.code());
        ASSERT_FALSE(status.reason().empty());
    }

    TEST(DistLockCatalogImpl, UnlockUnsupportedWriteConcernResponse) {
        RemoteCommandTargeterMock targeter;
        RemoteCommandRunnerMock executor;

        targeter.setFindHostReturnValue(dummyHost);

        DistLockCatalogImpl catalog(&targeter, &executor, kWTimeout);

        BSONObj returnObj(fromjson(R"({
            ok: 1,
            value: null,
            writeConcernError: {
                code: "bad format",
                errmsg: "waiting for replication timed out"
            }
        })"));
        executor.setNextExpectedCommand(noTest, RemoteCommandResponse(returnObj, Milliseconds(0)));

        auto status = catalog.unlock(OID());
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::UnsupportedFormat, status.code());
        ASSERT_FALSE(status.reason().empty());
    }

    TEST(DistLockCatalogImpl, UnlockUnsupportedResponseFormat) {
        RemoteCommandTargeterMock targeter;
        RemoteCommandRunnerMock executor;

        targeter.setFindHostReturnValue(dummyHost);

        DistLockCatalogImpl catalog(&targeter, &executor, kWTimeout);

        executor.setNextExpectedCommand(noTest,
                RemoteCommandResponse(BSON("ok" << 1 << "value" << "NaN"), Milliseconds(0)));

        auto status = catalog.unlock(OID());
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::UnsupportedFormat, status.code());
    }

    TEST(DistLockCatalogImpl, BasicGetServerInfo) {
        RemoteCommandTargeterMock targeter;
        RemoteCommandRunnerMock executor;

        targeter.setFindHostReturnValue(dummyHost);

        DistLockCatalogImpl catalog(&targeter, &executor, kWTimeout);

        BSONObj responseObj(fromjson(R"({
            localTime: { $date: "2015-05-26T13:06:27.293Z" },
            $gleStats: {
                lastOpTime: { $timestamp: { t: 0, i: 0 }},
                electionId: ObjectId("555fa85d4d8640862a0fc79b")
            },
            ok: 1
        })"));

        executor.setNextExpectedCommand([](const RemoteCommandRequest& request) {
            ASSERT_EQUALS(dummyHost, request.target);
            ASSERT_EQUALS("admin", request.dbname);
            ASSERT_EQUALS(BSON("serverStatus" << 1), request.cmdObj);
        },
        RemoteCommandResponse(responseObj, Milliseconds(0)));

        Date_t localTime(dateFromISOString("2015-05-26T13:06:27.293Z").getValue());
        OID electionID("555fa85d4d8640862a0fc79b");
        auto resultStatus = catalog.getServerInfo();
        ASSERT_OK(resultStatus.getStatus());

        const auto& serverInfo = resultStatus.getValue();
        ASSERT_EQUALS(electionID, serverInfo.electionId);
        ASSERT_EQUALS(localTime, serverInfo.serverTime);
    }

    TEST(DistLockCatalogImpl, GetServerTargetError) {
        RemoteCommandTargeterMock targeter;
        RemoteCommandRunnerMock executor;

        targeter.setFindHostReturnValue({ErrorCodes::InternalError, "can't target"});

        DistLockCatalogImpl catalog(&targeter, &executor, kWTimeout);

        auto status = catalog.getServerInfo().getStatus();
        ASSERT_NOT_OK(status);
    }

    TEST(DistLockCatalogImpl, GetServerRunnerError) {
        RemoteCommandTargeterMock targeter;
        RemoteCommandRunnerMock executor;

        targeter.setFindHostReturnValue(dummyHost);

        DistLockCatalogImpl catalog(&targeter, &executor, kWTimeout);

        executor.setNextExpectedCommand(noTest, {ErrorCodes::InternalError, "Bad"});

        auto status = catalog.getServerInfo().getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::InternalError, status.code());
        ASSERT_FALSE(status.reason().empty());
    }

    TEST(DistLockCatalogImpl, GetServerCommandError) {
        RemoteCommandTargeterMock targeter;
        RemoteCommandRunnerMock executor;

        targeter.setFindHostReturnValue(dummyHost);

        DistLockCatalogImpl catalog(&targeter, &executor, kWTimeout);

        BSONObj returnObj(fromjson(R"({
            ok: 0,
            errmsg: "bad",
            code: 9
        })"));
        executor.setNextExpectedCommand(noTest, RemoteCommandResponse(returnObj, Milliseconds(0)));

        auto status = catalog.getServerInfo().getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
        ASSERT_FALSE(status.reason().empty());
    }

    TEST(DistLockCatalogImpl, GetServerBadElectionId) {
        RemoteCommandTargeterMock targeter;
        RemoteCommandRunnerMock executor;

        targeter.setFindHostReturnValue(dummyHost);

        DistLockCatalogImpl catalog(&targeter, &executor, kWTimeout);

        BSONObj returnObj(fromjson(R"({
            localTime: { $date: "2015-05-26T13:06:27.293Z" },
            $gleStats: {
                lastOpTime: { $timestamp: { t: 0, i: 0 }},
                electionId: 34
            },
            ok: 1
        })"));
        executor.setNextExpectedCommand(noTest, RemoteCommandResponse(returnObj, Milliseconds(0)));

        auto status = catalog.getServerInfo().getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::UnsupportedFormat, status.code());
        ASSERT_FALSE(status.reason().empty());
    }

    TEST(DistLockCatalogImpl, GetServerBadLocalTime) {
        RemoteCommandTargeterMock targeter;
        RemoteCommandRunnerMock executor;

        targeter.setFindHostReturnValue(dummyHost);

        DistLockCatalogImpl catalog(&targeter, &executor, kWTimeout);

        BSONObj returnObj(fromjson(R"({
            localTime: "2015-05-26T13:06:27.293Z",
            $gleStats: {
                lastOpTime: { $timestamp: { t: 0, i: 0 }},
                electionId: ObjectId("555fa85d4d8640862a0fc79b")
            },
            ok: 1
        })"));
        executor.setNextExpectedCommand(noTest, RemoteCommandResponse(returnObj, Milliseconds(0)));

        auto status = catalog.getServerInfo().getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::UnsupportedFormat, status.code());
        ASSERT_FALSE(status.reason().empty());
    }

    TEST(DistLockCatalogImpl, GetServerNoGLEStats) {
        RemoteCommandTargeterMock targeter;
        RemoteCommandRunnerMock executor;

        targeter.setFindHostReturnValue(dummyHost);

        DistLockCatalogImpl catalog(&targeter, &executor, kWTimeout);

        BSONObj returnObj(fromjson(R"({
            localTime: { $date: "2015-05-26T13:06:27.293Z" },
            ok: 1
        })"));
        executor.setNextExpectedCommand(noTest, RemoteCommandResponse(returnObj, Milliseconds(0)));

        auto status = catalog.getServerInfo().getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::UnsupportedFormat, status.code());
        ASSERT_FALSE(status.reason().empty());
    }

    TEST(DistLockCatalogImpl, GetServerNoElectionId) {
        RemoteCommandTargeterMock targeter;
        RemoteCommandRunnerMock executor;

        targeter.setFindHostReturnValue(dummyHost);

        DistLockCatalogImpl catalog(&targeter, &executor, kWTimeout);

        BSONObj returnObj(fromjson(R"({
            localTime: { $date: "2015-05-26T13:06:27.293Z" },
            $gleStats: {
                lastOpTime: { $timestamp: { t: 0, i: 0 }},
                termNumber: 64
            },
            ok: 1
        })"));
        executor.setNextExpectedCommand(noTest, RemoteCommandResponse(returnObj, Milliseconds(0)));

        auto status = catalog.getServerInfo().getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::UnsupportedFormat, status.code());
        ASSERT_FALSE(status.reason().empty());
    }

} // unnamed namespace
} // namespace mongo
