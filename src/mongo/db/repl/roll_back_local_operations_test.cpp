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


#include "mongo/db/repl/roll_back_local_operations.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/dbclient_base.h"
#include "mongo/client/dbclient_connection.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/client/dbclient_mockcursor.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_interface_mock.h"
#include "mongo/db/repl/oplog_interface_remote.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/storage/remove_saver.h"
#include "mongo/db/transaction/transaction_history_iterator.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/uuid.h"

#include <iterator>
#include <list>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace {

using namespace mongo;
using namespace mongo::repl;

BSONObj makeOp(long long seconds, long long term = 1LL, long wallClockMillis = 0) {
    auto uuid = unittest::assertGet(UUID::parse("b4c66a44-c1ca-4d86-8d25-12e82fa2de5b"));
    return BSON("ts" << Timestamp(seconds, seconds) << "t" << term << "op"
                     << "n"
                     << "o" << BSONObj() << "ns"
                     << "roll_back_local_operations.test"
                     << "ui" << uuid << "wall" << Date_t::fromMillisSinceEpoch(wallClockMillis));
}

int recordId = 0;
OplogInterfaceMock::Operation makeOpAndRecordId(long long seconds,
                                                long long term = 1LL,
                                                long long wallClockMillis = 0) {
    return std::make_pair(makeOp(seconds, term, wallClockMillis), RecordId(++recordId));
}

class RollBackLocalOperationsTest : public ServiceContextMongoDTest {
protected:
    const bool shouldCreateRollbackFiles = true;
    RemoveSaver removeSaver{"rollback", "local.oplog.rs", "removed"};

private:
    void setUp() override {
        ServiceContextMongoDTest::setUp();
    }

    void tearDown() override {
        ServiceContextMongoDTest::tearDown();
    }
};

TEST(SimpleRollBackLocalOperationsTest, InvalidLocalOplogIterator) {
    class InvalidOplogInterface : public OplogInterface {
    public:
        std::string toString() const override {
            return "";
        }
        std::unique_ptr<Iterator> makeIterator() const override {
            return std::unique_ptr<Iterator>();
        }
        std::unique_ptr<TransactionHistoryIteratorBase> makeTransactionHistoryIterator(
            const OpTime& startingOpTime, bool permitYield = false) const override {
            MONGO_UNREACHABLE;
        };
        HostAndPort hostAndPort() const override {
            return {};
        }
    } invalidOplog;
    ASSERT_THROWS_CODE(
        RollBackLocalOperations(invalidOplog, [](const BSONObj&) { return Status::OK(); }),
        AssertionException,
        ErrorCodes::BadValue);
}

TEST(SimpleRollBackLocalOperationsTest, InvalidRollbackOperationFunction) {
    ASSERT_THROWS_CODE(RollBackLocalOperations(OplogInterfaceMock({makeOpAndRecordId(1)}),
                                               RollBackLocalOperations::RollbackOperationFn()),
                       AssertionException,
                       ErrorCodes::BadValue);
}

TEST_F(RollBackLocalOperationsTest, EmptyLocalOplog) {
    OplogInterfaceMock localOplog;
    RollBackLocalOperations finder(localOplog, [](const BSONObj&) { return Status::OK(); });
    auto result = finder.onRemoteOperation(makeOp(1), removeSaver, shouldCreateRollbackFiles);
    ASSERT_EQUALS(ErrorCodes::OplogStartMissing, result.getStatus().code());
}

TEST_F(RollBackLocalOperationsTest, RollbackMultipleLocalOperations) {
    auto commonOperation = makeOpAndRecordId(1);
    OplogInterfaceMock::Operations localOperations({
        makeOpAndRecordId(5),
        makeOpAndRecordId(4),
        makeOpAndRecordId(3),
        makeOpAndRecordId(2),
        commonOperation,
    });
    OplogInterfaceMock localOplog(localOperations);
    auto i = localOperations.cbegin();
    auto rollbackOperation = [&](const BSONObj& operation) {
        ASSERT_BSONOBJ_EQ(i->first, operation);
        i++;
        return Status::OK();
    };
    RollBackLocalOperations finder(localOplog, rollbackOperation);
    auto result =
        finder.onRemoteOperation(commonOperation.first, removeSaver, shouldCreateRollbackFiles);
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(OpTime::parseFromOplogEntry(commonOperation.first),
                  result.getValue().getOpTime());
    ASSERT_EQUALS(commonOperation.second, result.getValue().getRecordId());
    ASSERT_FALSE(i == localOperations.cend());
    ASSERT_BSONOBJ_EQ(commonOperation.first, i->first);
    i++;
    ASSERT_TRUE(i == localOperations.cend());
}

TEST_F(RollBackLocalOperationsTest, RollbackOperationFailed) {
    auto commonOperation = makeOpAndRecordId(1);
    OplogInterfaceMock::Operations localOperations({
        makeOpAndRecordId(2),
        commonOperation,
    });
    OplogInterfaceMock localOplog(localOperations);
    auto rollbackOperation = [&](const BSONObj& operation) {
        return Status(ErrorCodes::OperationFailed, "");
    };
    RollBackLocalOperations finder(localOplog, rollbackOperation);
    auto result =
        finder.onRemoteOperation(commonOperation.first, removeSaver, shouldCreateRollbackFiles);
    ASSERT_EQUALS(ErrorCodes::OperationFailed, result.getStatus().code());
}
TEST_F(RollBackLocalOperationsTest, EndOfLocalOplog) {
    auto commonOperation = makeOpAndRecordId(1);
    OplogInterfaceMock::Operations localOperations({
        makeOpAndRecordId(2),
    });
    OplogInterfaceMock localOplog(localOperations);
    RollBackLocalOperations finder(localOplog, [](const BSONObj&) { return Status::OK(); });
    auto result =
        finder.onRemoteOperation(commonOperation.first, removeSaver, shouldCreateRollbackFiles);
    ASSERT_EQUALS(ErrorCodes::NoMatchingDocument, result.getStatus().code());
}

TEST_F(RollBackLocalOperationsTest, SkipRemoteOperations) {
    auto commonOperation = makeOpAndRecordId(1);
    OplogInterfaceMock::Operations localOperations({
        makeOpAndRecordId(5),
        makeOpAndRecordId(4),
        makeOpAndRecordId(2),
        commonOperation,
    });
    OplogInterfaceMock localOplog(localOperations);
    auto i = localOperations.cbegin();
    auto rollbackOperation = [&](const BSONObj& operation) {
        ASSERT_BSONOBJ_EQ(i->first, operation);
        i++;
        return Status::OK();
    };
    RollBackLocalOperations finder(localOplog, rollbackOperation);
    {
        auto result = finder.onRemoteOperation(makeOp(6), removeSaver, shouldCreateRollbackFiles);
        ASSERT_EQUALS(ErrorCodes::NoSuchKey, result.getStatus().code());
        ASSERT_TRUE(i == localOperations.cbegin());
    }
    {
        auto result = finder.onRemoteOperation(makeOp(3), removeSaver, shouldCreateRollbackFiles);
        ASSERT_EQUALS(ErrorCodes::NoSuchKey, result.getStatus().code());
        ASSERT_TRUE(std::distance(localOperations.cbegin(), i) == 2);
    }
    auto result =
        finder.onRemoteOperation(commonOperation.first, removeSaver, shouldCreateRollbackFiles);
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(OpTime::parseFromOplogEntry(commonOperation.first),
                  result.getValue().getOpTime());
    ASSERT_EQUALS(commonOperation.second, result.getValue().getRecordId());
    ASSERT_FALSE(i == localOperations.cend());
    ASSERT_BSONOBJ_EQ(commonOperation.first, i->first);
    i++;
    ASSERT_TRUE(i == localOperations.cend());
}

TEST_F(RollBackLocalOperationsTest, SameTimestampDifferentTermsRollbackNoSuchKey) {
    auto commonOperation = makeOpAndRecordId(1, 1);
    OplogInterfaceMock::Operations localOperations({
        makeOpAndRecordId(2, 3),
        commonOperation,
    });
    OplogInterfaceMock localOplog(localOperations);
    auto rollbackOperation = [&](const BSONObj& operation) {
        return Status(ErrorCodes::OperationFailed, "");
    };
    RollBackLocalOperations finder(localOplog, rollbackOperation);
    auto result = finder.onRemoteOperation(makeOp(2, 2), removeSaver, shouldCreateRollbackFiles);
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, result.getStatus().code());
}

TEST_F(RollBackLocalOperationsTest, OplogStartMissing) {
    ASSERT_EQUALS(ErrorCodes::OplogStartMissing,
                  syncRollBackLocalOperations(
                      OplogInterfaceMock(),
                      OplogInterfaceMock({makeOpAndRecordId(1)}),
                      [](const BSONObj&) { return Status::OK(); },
                      shouldCreateRollbackFiles)
                      .getStatus()
                      .code());
}

TEST_F(RollBackLocalOperationsTest, RemoteOplogMissing) {
    ASSERT_EQUALS(ErrorCodes::InvalidSyncSource,
                  syncRollBackLocalOperations(
                      OplogInterfaceMock({makeOpAndRecordId(1)}),
                      OplogInterfaceMock(),
                      [](const BSONObj&) { return Status::OK(); },
                      shouldCreateRollbackFiles)
                      .getStatus()
                      .code());
}

TEST_F(RollBackLocalOperationsTest, RollbackTwoOperations) {
    auto commonOperation = makeOpAndRecordId(1, 1LL, 1 * 5000);
    auto firstOpAfterCommonPoint = makeOpAndRecordId(2, 1LL, 2 * 60 * 60 * 24 * 1000);
    OplogInterfaceMock::Operations localOperations({
        makeOpAndRecordId(3),
        firstOpAfterCommonPoint,
        commonOperation,
    });
    auto i = localOperations.cbegin();
    auto result = syncRollBackLocalOperations(
        OplogInterfaceMock(localOperations),
        OplogInterfaceMock({commonOperation}),
        [&](const BSONObj& operation) {
            ASSERT_BSONOBJ_EQ(i->first, operation);
            i++;
            return Status::OK();
        },
        shouldCreateRollbackFiles);
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(OpTime::parseFromOplogEntry(commonOperation.first),
                  result.getValue().getOpTime());
    ASSERT_EQUALS(commonOperation.second, result.getValue().getRecordId());
    ASSERT_FALSE(i == localOperations.cend());
    ASSERT_BSONOBJ_EQ(commonOperation.first, i->first);
    auto firstOplogEntryAfterCommonPoint =
        uassertStatusOK(OplogEntry::parse(firstOpAfterCommonPoint.first));
    ASSERT_EQUALS(result.getValue().getFirstOpWallClockTimeAfterCommonPoint(),
                  firstOplogEntryAfterCommonPoint.getWallClockTime());
    i++;
    ASSERT_TRUE(i == localOperations.cend());
}

TEST_F(RollBackLocalOperationsTest, SkipOneRemoteOperation) {
    auto commonOperation = makeOpAndRecordId(1);
    auto remoteOperation = makeOpAndRecordId(2);
    auto result = syncRollBackLocalOperations(
        OplogInterfaceMock({commonOperation}),
        OplogInterfaceMock({remoteOperation, commonOperation}),
        [&](const BSONObj& operation) {
            FAIL("should not reach here");
            return Status::OK();
        },
        shouldCreateRollbackFiles);
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(OpTime::parseFromOplogEntry(commonOperation.first),
                  result.getValue().getOpTime());
    ASSERT_EQUALS(commonOperation.second, result.getValue().getRecordId());
}

TEST_F(RollBackLocalOperationsTest, DifferentTimestampEndOfLocalOplog) {
    auto commonOperation = makeOpAndRecordId(1);
    auto localOperation = makeOpAndRecordId(3);
    auto remoteOperation = makeOpAndRecordId(2);
    bool called = false;
    auto result = syncRollBackLocalOperations(
        OplogInterfaceMock({localOperation}),
        OplogInterfaceMock({remoteOperation, commonOperation}),
        [&](const BSONObj& operation) {
            ASSERT_BSONOBJ_EQ(localOperation.first, operation);
            called = true;
            return Status::OK();
        },
        shouldCreateRollbackFiles);
    ASSERT_EQUALS(ErrorCodes::NoMatchingDocument, result.getStatus().code());
    ASSERT_STRING_CONTAINS(result.getStatus().reason(), "reached beginning of local oplog");
    ASSERT_TRUE(called);
}

TEST_F(RollBackLocalOperationsTest, DifferentTimestampRollbackOperationFailed) {
    auto localOperation = makeOpAndRecordId(3);
    auto remoteOperation = makeOpAndRecordId(2);
    auto result = syncRollBackLocalOperations(
        OplogInterfaceMock({localOperation}),
        OplogInterfaceMock({remoteOperation}),
        [&](const BSONObj& operation) { return Status(ErrorCodes::OperationFailed, ""); },
        shouldCreateRollbackFiles);
    ASSERT_EQUALS(ErrorCodes::OperationFailed, result.getStatus().code());
}

TEST_F(RollBackLocalOperationsTest, DifferentTimestampEndOfRemoteOplog) {
    auto commonOperation = makeOpAndRecordId(1);
    auto localOperation = makeOpAndRecordId(2);
    auto remoteOperation = makeOpAndRecordId(3);
    auto result = syncRollBackLocalOperations(
        OplogInterfaceMock({localOperation, commonOperation}),
        OplogInterfaceMock({remoteOperation}),
        [&](const BSONObj& operation) {
            FAIL("Should not reach here");
            return Status::OK();
        },
        shouldCreateRollbackFiles);
    ASSERT_EQUALS(ErrorCodes::NoMatchingDocument, result.getStatus().code());
    ASSERT_STRING_CONTAINS(result.getStatus().reason(), "reached beginning of remote oplog");
}

class DBClientConnectionForTest : public DBClientConnection {
public:
    DBClientConnectionForTest(int numInitFailures) : _initFailuresLeft(numInitFailures) {}

    std::unique_ptr<DBClientCursor> find(FindCommandRequest findRequest,
                                         const ReadPreferenceSetting& readPref,
                                         ExhaustMode exhaustMode) override {
        if (_initFailuresLeft > 0) {
            _initFailuresLeft--;
            LOGV2(21657,
                  "Throwing DBException on DBClientCursorForTest::find()",
                  "initFailuresLeft"_attr = _initFailuresLeft);
            uasserted(50852, "Simulated network error");
            MONGO_UNREACHABLE;
        }

        LOGV2(21658, "Returning success on DBClientCursorForTest::find()");

        BSONArrayBuilder builder;
        builder.append(makeOp(1));
        builder.append(makeOp(2));
        return std::make_unique<DBClientMockCursor>(this, builder.arr());
    }

private:
    int _initFailuresLeft;
};

void checkRemoteIterator(int numNetworkFailures,
                         bool expectedToSucceed,
                         bool shouldCreateRollbackFiles) {

    DBClientConnectionForTest conn(numNetworkFailures);
    auto getConnection = [&]() -> DBClientBase* {
        return &conn;
    };

    auto localOperation = makeOpAndRecordId(1);
    OplogInterfaceRemote remoteOplogMock(HostAndPort("229w43rd", 10036), getConnection, 0);

    auto result = Status::OK();

    try {
        result = syncRollBackLocalOperations(
                     OplogInterfaceMock({localOperation}),
                     remoteOplogMock,
                     [&](const BSONObj&) { return Status::OK(); },
                     shouldCreateRollbackFiles)
                     .getStatus();
    } catch (...) {
        // For the failure scenario.
        ASSERT_FALSE(expectedToSucceed);
        return;
    }
    // For the success scenario.
    ASSERT_TRUE(expectedToSucceed);
    ASSERT_OK(result);
}

TEST_F(RollBackLocalOperationsTest, RemoteOplogMakeIteratorSucceedsWithNoNetworkFailures) {
    checkRemoteIterator(0, true, shouldCreateRollbackFiles);
}

TEST_F(RollBackLocalOperationsTest, RemoteOplogMakeIteratorSucceedsWithOneNetworkFailure) {
    checkRemoteIterator(1, true, shouldCreateRollbackFiles);
}

TEST_F(RollBackLocalOperationsTest, RemoteOplogMakeIteratorSucceedsWithTwoNetworkFailures) {
    checkRemoteIterator(2, true, shouldCreateRollbackFiles);
}

TEST_F(RollBackLocalOperationsTest, RemoteOplogMakeIteratorFailsWithTooManyNetworkFailures) {
    checkRemoteIterator(3, false, shouldCreateRollbackFiles);
}

}  // namespace
