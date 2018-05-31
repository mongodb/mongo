/**
 *    Copyright 2015 MongoDB Inc.
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

#include <iterator>

#include "mongo/client/connection_pool.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/client/dbclientmockcursor.h"
#include "mongo/db/client.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/oplog_interface_mock.h"
#include "mongo/db/repl/oplog_interface_remote.h"
#include "mongo/db/repl/roll_back_local_operations.h"
#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;
using namespace mongo::repl;

BSONObj makeOp(long long seconds, long long hash) {
    auto uuid = unittest::assertGet(UUID::parse("b4c66a44-c1ca-4d86-8d25-12e82fa2de5b"));
    return BSON("ts" << Timestamp(seconds, seconds) << "h" << hash << "t" << seconds << "op"
                     << "n"
                     << "o"
                     << BSONObj()
                     << "ns"
                     << "roll_back_local_operations.test"
                     << "ui"
                     << uuid);
}

int recordId = 0;
OplogInterfaceMock::Operation makeOpAndRecordId(long long seconds, long long hash) {
    return std::make_pair(makeOp(seconds, hash), RecordId(++recordId));
}

TEST(RollBackLocalOperationsTest, InvalidLocalOplogIterator) {
    class InvalidOplogInterface : public OplogInterface {
    public:
        std::string toString() const override {
            return "";
        }
        std::unique_ptr<Iterator> makeIterator() const override {
            return std::unique_ptr<Iterator>();
        }
        HostAndPort hostAndPort() const override {
            return {};
        }
    } invalidOplog;
    ASSERT_THROWS_CODE(
        RollBackLocalOperations(invalidOplog, [](const BSONObj&) { return Status::OK(); }),
        AssertionException,
        ErrorCodes::BadValue);
}

TEST(RollBackLocalOperationsTest, InvalidRollbackOperationFunction) {
    ASSERT_THROWS_CODE(RollBackLocalOperations(OplogInterfaceMock({makeOpAndRecordId(1, 0)}),
                                               RollBackLocalOperations::RollbackOperationFn()),
                       AssertionException,
                       ErrorCodes::BadValue);
}

TEST(RollBackLocalOperationsTest, EmptyLocalOplog) {
    OplogInterfaceMock localOplog;
    RollBackLocalOperations finder(localOplog, [](const BSONObj&) { return Status::OK(); });
    auto result = finder.onRemoteOperation(makeOp(1, 0));
    ASSERT_EQUALS(ErrorCodes::OplogStartMissing, result.getStatus().code());
}

TEST(RollBackLocalOperationsTest, RollbackMultipleLocalOperations) {
    auto commonOperation = makeOpAndRecordId(1, 1);
    OplogInterfaceMock::Operations localOperations({
        makeOpAndRecordId(5, 1),
        makeOpAndRecordId(4, 1),
        makeOpAndRecordId(3, 1),
        makeOpAndRecordId(2, 1),
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
    auto result = finder.onRemoteOperation(commonOperation.first);
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(OpTime::parseFromOplogEntry(commonOperation.first),
                  result.getValue().getOpTime());
    ASSERT_EQUALS(commonOperation.second, result.getValue().getRecordId());
    ASSERT_FALSE(i == localOperations.cend());
    ASSERT_BSONOBJ_EQ(commonOperation.first, i->first);
    i++;
    ASSERT_TRUE(i == localOperations.cend());
}

TEST(RollBackLocalOperationsTest, RollbackOperationFailed) {
    auto commonOperation = makeOpAndRecordId(1, 1);
    OplogInterfaceMock::Operations localOperations({
        makeOpAndRecordId(2, 1), commonOperation,
    });
    OplogInterfaceMock localOplog(localOperations);
    auto rollbackOperation = [&](const BSONObj& operation) {
        return Status(ErrorCodes::OperationFailed, "");
    };
    RollBackLocalOperations finder(localOplog, rollbackOperation);
    auto result = finder.onRemoteOperation(commonOperation.first);
    ASSERT_EQUALS(ErrorCodes::OperationFailed, result.getStatus().code());
}

TEST(RollBackLocalOperationsTest, EndOfLocalOplog) {
    auto commonOperation = makeOpAndRecordId(1, 1);
    OplogInterfaceMock::Operations localOperations({
        makeOpAndRecordId(2, 1),
    });
    OplogInterfaceMock localOplog(localOperations);
    RollBackLocalOperations finder(localOplog, [](const BSONObj&) { return Status::OK(); });
    auto result = finder.onRemoteOperation(commonOperation.first);
    ASSERT_EQUALS(ErrorCodes::NoMatchingDocument, result.getStatus().code());
}

TEST(RollBackLocalOperationsTest, SkipRemoteOperations) {
    auto commonOperation = makeOpAndRecordId(1, 1);
    OplogInterfaceMock::Operations localOperations({
        makeOpAndRecordId(5, 1), makeOpAndRecordId(4, 1), makeOpAndRecordId(2, 1), commonOperation,
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
        auto result = finder.onRemoteOperation(makeOp(6, 1));
        ASSERT_EQUALS(ErrorCodes::NoSuchKey, result.getStatus().code());
        ASSERT_TRUE(i == localOperations.cbegin());
    }
    {
        auto result = finder.onRemoteOperation(makeOp(3, 1));
        ASSERT_EQUALS(ErrorCodes::NoSuchKey, result.getStatus().code());
        ASSERT_TRUE(std::distance(localOperations.cbegin(), i) == 2);
    }
    auto result = finder.onRemoteOperation(commonOperation.first);
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(OpTime::parseFromOplogEntry(commonOperation.first),
                  result.getValue().getOpTime());
    ASSERT_EQUALS(commonOperation.second, result.getValue().getRecordId());
    ASSERT_FALSE(i == localOperations.cend());
    ASSERT_BSONOBJ_EQ(commonOperation.first, i->first);
    i++;
    ASSERT_TRUE(i == localOperations.cend());
}

TEST(RollBackLocalOperationsTest, SameTimestampDifferentHashess) {
    auto commonOperation = makeOpAndRecordId(1, 1);
    OplogInterfaceMock::Operations localOperations({
        makeOpAndRecordId(1, 5), makeOpAndRecordId(1, 3), commonOperation,
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
        auto result = finder.onRemoteOperation(makeOp(1, 4));
        ASSERT_EQUALS(ErrorCodes::NoSuchKey, result.getStatus().code());
        ASSERT_TRUE(std::distance(localOperations.cbegin(), i) == 1);
    }
    {
        auto result = finder.onRemoteOperation(makeOp(1, 2));
        ASSERT_EQUALS(ErrorCodes::NoSuchKey, result.getStatus().code());
        ASSERT_TRUE(std::distance(localOperations.cbegin(), i) == 2);
    }
    auto result = finder.onRemoteOperation(commonOperation.first);
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(OpTime::parseFromOplogEntry(commonOperation.first),
                  result.getValue().getOpTime());
    ASSERT_EQUALS(commonOperation.second, result.getValue().getRecordId());
    ASSERT_FALSE(i == localOperations.cend());
    ASSERT_BSONOBJ_EQ(commonOperation.first, i->first);
    i++;
    ASSERT_TRUE(i == localOperations.cend());
}

TEST(RollBackLocalOperationsTest, SameTimestampDifferentHashesRollbackOperationFailed) {
    auto commonOperation = makeOpAndRecordId(1, 1);
    OplogInterfaceMock::Operations localOperations({
        makeOpAndRecordId(1, 3), commonOperation,
    });
    OplogInterfaceMock localOplog(localOperations);
    auto rollbackOperation = [&](const BSONObj& operation) {
        return Status(ErrorCodes::OperationFailed, "");
    };
    RollBackLocalOperations finder(localOplog, rollbackOperation);
    auto result = finder.onRemoteOperation(makeOp(1, 2));
    ASSERT_EQUALS(ErrorCodes::OperationFailed, result.getStatus().code());
}

TEST(RollBackLocalOperationsTest, SameTimestampDifferentHashesEndOfLocalOplog) {
    OplogInterfaceMock::Operations localOperations({
        makeOpAndRecordId(1, 3),
    });
    OplogInterfaceMock localOplog(localOperations);
    RollBackLocalOperations finder(localOplog, [](const BSONObj&) { return Status::OK(); });
    auto result = finder.onRemoteOperation(makeOp(1, 2));
    ASSERT_EQUALS(ErrorCodes::NoMatchingDocument, result.getStatus().code());
}

TEST(SyncRollBackLocalOperationsTest, OplogStartMissing) {
    ASSERT_EQUALS(ErrorCodes::OplogStartMissing,
                  syncRollBackLocalOperations(OplogInterfaceMock(),
                                              OplogInterfaceMock({makeOpAndRecordId(1, 0)}),
                                              [](const BSONObj&) { return Status::OK(); })
                      .getStatus()
                      .code());
}

TEST(SyncRollBackLocalOperationsTest, RemoteOplogMissing) {
    ASSERT_EQUALS(ErrorCodes::InvalidSyncSource,
                  syncRollBackLocalOperations(OplogInterfaceMock({makeOpAndRecordId(1, 0)}),
                                              OplogInterfaceMock(),
                                              [](const BSONObj&) { return Status::OK(); })
                      .getStatus()
                      .code());
}

TEST(SyncRollBackLocalOperationsTest, RollbackTwoOperations) {
    auto commonOperation = makeOpAndRecordId(1, 1);
    OplogInterfaceMock::Operations localOperations({
        makeOpAndRecordId(3, 1), makeOpAndRecordId(2, 1), commonOperation,
    });
    auto i = localOperations.cbegin();
    auto result = syncRollBackLocalOperations(OplogInterfaceMock(localOperations),
                                              OplogInterfaceMock({commonOperation}),
                                              [&](const BSONObj& operation) {
                                                  ASSERT_BSONOBJ_EQ(i->first, operation);
                                                  i++;
                                                  return Status::OK();
                                              });
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(OpTime::parseFromOplogEntry(commonOperation.first),
                  result.getValue().getOpTime());
    ASSERT_EQUALS(commonOperation.second, result.getValue().getRecordId());
    ASSERT_FALSE(i == localOperations.cend());
    ASSERT_BSONOBJ_EQ(commonOperation.first, i->first);
    i++;
    ASSERT_TRUE(i == localOperations.cend());
}

TEST(SyncRollBackLocalOperationsTest, SkipOneRemoteOperation) {
    auto commonOperation = makeOpAndRecordId(1, 1);
    auto remoteOperation = makeOpAndRecordId(2, 1);
    auto result =
        syncRollBackLocalOperations(OplogInterfaceMock({commonOperation}),
                                    OplogInterfaceMock({remoteOperation, commonOperation}),
                                    [&](const BSONObj& operation) {
                                        FAIL("should not reach here");
                                        return Status::OK();
                                    });
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(OpTime::parseFromOplogEntry(commonOperation.first),
                  result.getValue().getOpTime());
    ASSERT_EQUALS(commonOperation.second, result.getValue().getRecordId());
}

TEST(SyncRollBackLocalOperationsTest, SameTimestampDifferentHashes) {
    auto commonOperation = makeOpAndRecordId(1, 1);
    auto localOperation = makeOpAndRecordId(1, 2);
    auto remoteOperation = makeOpAndRecordId(1, 3);
    bool called = false;
    auto result =
        syncRollBackLocalOperations(OplogInterfaceMock({localOperation, commonOperation}),
                                    OplogInterfaceMock({remoteOperation, commonOperation}),
                                    [&](const BSONObj& operation) {
                                        ASSERT_BSONOBJ_EQ(localOperation.first, operation);
                                        called = true;
                                        return Status::OK();
                                    });
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(OpTime::parseFromOplogEntry(commonOperation.first),
                  result.getValue().getOpTime());
    ASSERT_EQUALS(commonOperation.second, result.getValue().getRecordId());
    ASSERT_TRUE(called);
}

TEST(SyncRollBackLocalOperationsTest, SameTimestampEndOfLocalOplog) {
    auto commonOperation = makeOpAndRecordId(1, 1);
    auto localOperation = makeOpAndRecordId(1, 2);
    auto remoteOperation = makeOpAndRecordId(1, 3);
    bool called = false;
    auto result =
        syncRollBackLocalOperations(OplogInterfaceMock({localOperation}),
                                    OplogInterfaceMock({remoteOperation, commonOperation}),
                                    [&](const BSONObj& operation) {
                                        ASSERT_BSONOBJ_EQ(localOperation.first, operation);
                                        called = true;
                                        return Status::OK();
                                    });
    ASSERT_EQUALS(ErrorCodes::NoMatchingDocument, result.getStatus().code());
    ASSERT_STRING_CONTAINS(result.getStatus().reason(), "reached beginning of local oplog");
    ASSERT_TRUE(called);
}

TEST(SyncRollBackLocalOperationsTest, SameTimestampRollbackOperationFailed) {
    auto commonOperation = makeOpAndRecordId(1, 1);
    auto localOperation = makeOpAndRecordId(1, 2);
    auto remoteOperation = makeOpAndRecordId(1, 3);
    auto result = syncRollBackLocalOperations(
        OplogInterfaceMock({localOperation, commonOperation}),
        OplogInterfaceMock({remoteOperation, commonOperation}),
        [&](const BSONObj& operation) { return Status(ErrorCodes::OperationFailed, ""); });
    ASSERT_EQUALS(ErrorCodes::OperationFailed, result.getStatus().code());
}

TEST(SyncRollBackLocalOperationsTest, SameTimestampEndOfRemoteOplog) {
    auto commonOperation = makeOpAndRecordId(1, 1);
    auto localOperation = makeOpAndRecordId(1, 2);
    auto remoteOperation = makeOpAndRecordId(1, 3);
    bool called = false;
    auto result =
        syncRollBackLocalOperations(OplogInterfaceMock({localOperation, commonOperation}),
                                    OplogInterfaceMock({remoteOperation}),
                                    [&](const BSONObj& operation) {
                                        ASSERT_BSONOBJ_EQ(localOperation.first, operation);
                                        called = true;
                                        return Status::OK();
                                    });
    ASSERT_EQUALS(ErrorCodes::NoMatchingDocument, result.getStatus().code());
    ASSERT_STRING_CONTAINS(result.getStatus().reason(), "reached beginning of remote oplog");
    ASSERT_TRUE(called);
}

TEST(SyncRollBackLocalOperationsTest, DifferentTimestampEndOfLocalOplog) {
    auto commonOperation = makeOpAndRecordId(1, 1);
    auto localOperation = makeOpAndRecordId(3, 1);
    auto remoteOperation = makeOpAndRecordId(2, 1);
    bool called = false;
    auto result =
        syncRollBackLocalOperations(OplogInterfaceMock({localOperation}),
                                    OplogInterfaceMock({remoteOperation, commonOperation}),
                                    [&](const BSONObj& operation) {
                                        ASSERT_BSONOBJ_EQ(localOperation.first, operation);
                                        called = true;
                                        return Status::OK();
                                    });
    ASSERT_EQUALS(ErrorCodes::NoMatchingDocument, result.getStatus().code());
    ASSERT_STRING_CONTAINS(result.getStatus().reason(), "reached beginning of local oplog");
    ASSERT_TRUE(called);
}

TEST(SyncRollBackLocalOperationsTest, DifferentTimestampRollbackOperationFailed) {
    auto localOperation = makeOpAndRecordId(3, 1);
    auto remoteOperation = makeOpAndRecordId(2, 1);
    auto result = syncRollBackLocalOperations(
        OplogInterfaceMock({localOperation}),
        OplogInterfaceMock({remoteOperation}),
        [&](const BSONObj& operation) { return Status(ErrorCodes::OperationFailed, ""); });
    ASSERT_EQUALS(ErrorCodes::OperationFailed, result.getStatus().code());
}

TEST(SyncRollBackLocalOperationsTest, DifferentTimestampEndOfRemoteOplog) {
    auto commonOperation = makeOpAndRecordId(1, 1);
    auto localOperation = makeOpAndRecordId(2, 1);
    auto remoteOperation = makeOpAndRecordId(3, 1);
    auto result = syncRollBackLocalOperations(OplogInterfaceMock({localOperation, commonOperation}),
                                              OplogInterfaceMock({remoteOperation}),
                                              [&](const BSONObj& operation) {
                                                  FAIL("Should not reach here");
                                                  return Status::OK();
                                              });
    ASSERT_EQUALS(ErrorCodes::NoMatchingDocument, result.getStatus().code());
    ASSERT_STRING_CONTAINS(result.getStatus().reason(), "reached beginning of remote oplog");
}

class DBClientConnectionForTest : public DBClientConnection {
public:
    DBClientConnectionForTest(int numInitFailures) : _initFailuresLeft(numInitFailures) {}

    using DBClientConnection::query;

    std::unique_ptr<DBClientCursor> query(const std::string& ns,
                                          Query query,
                                          int nToReturn,
                                          int nToSkip,
                                          const BSONObj* fieldsToReturn,
                                          int queryOptions,
                                          int batchSize) override {
        if (_initFailuresLeft > 0) {
            _initFailuresLeft--;
            unittest::log()
                << "Throwing DBException on DBClientCursorForTest::query(). Failures left: "
                << _initFailuresLeft;
            uasserted(50852, "Simulated network error");
            MONGO_UNREACHABLE;
        }

        unittest::log() << "Returning success on DBClientCursorForTest::query()";

        BSONArrayBuilder builder;
        builder.append(makeOp(1, 1));
        builder.append(makeOp(2, 2));
        return std::make_unique<DBClientMockCursor>(this, builder.arr());
    }

private:
    int _initFailuresLeft;
};

void checkRemoteIterator(int numNetworkFailures, bool expectedToSucceed) {

    DBClientConnectionForTest conn(numNetworkFailures);
    auto getConnection = [&]() -> DBClientBase* { return &conn; };

    auto localOperation = makeOpAndRecordId(1, 1);
    OplogInterfaceRemote remoteOplogMock(
        HostAndPort("229w43rd", 10036), getConnection, "somecollection", 0);

    auto result = Status::OK();

    try {
        result = syncRollBackLocalOperations(OplogInterfaceMock({localOperation}),
                                             remoteOplogMock,
                                             [&](const BSONObj&) { return Status::OK(); })
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

TEST(SyncRollBackLocalOperationsTest, RemoteOplogMakeIteratorSucceedsWithNoNetworkFailures) {
    checkRemoteIterator(0, true);
}

TEST(SyncRollBackLocalOperationsTest, RemoteOplogMakeIteratorSucceedsWithOneNetworkFailure) {
    checkRemoteIterator(1, true);
}

TEST(SyncRollBackLocalOperationsTest, RemoteOplogMakeIteratorSucceedsWithTwoNetworkFailures) {
    checkRemoteIterator(2, true);
}

TEST(SyncRollBackLocalOperationsTest, RemoteOplogMakeIteratorFailsWithTooManyNetworkFailures) {
    checkRemoteIterator(3, false);
}

}  // namespace
