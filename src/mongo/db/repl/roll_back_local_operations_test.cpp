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

#include "mongo/db/jsobj.h"
#include "mongo/db/repl/oplog_interface_mock.h"
#include "mongo/db/repl/roll_back_local_operations.h"
#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;
using namespace mongo::repl;

const OplogInterfaceMock::Operations kEmptyMockOperations;

BSONObj makeOp(int seconds, long long hash) {
    return BSON("ts" << Timestamp(Seconds(seconds), 0) << "h" << hash);
}

int recordId = 0;
OplogInterfaceMock::Operation makeOpAndRecordId(int seconds, long long hash) {
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
    } invalidOplog;
    ASSERT_THROWS_CODE(
        RollBackLocalOperations(invalidOplog, [](const BSONObj&) { return Status::OK(); }),
        UserException,
        ErrorCodes::BadValue);
}

TEST(RollBackLocalOperationsTest, InvalidRollbackOperationFunction) {
    ASSERT_THROWS_CODE(RollBackLocalOperations(OplogInterfaceMock({makeOpAndRecordId(1, 0)}),
                                               RollBackLocalOperations::RollbackOperationFn()),
                       UserException,
                       ErrorCodes::BadValue);
}

TEST(RollBackLocalOperationsTest, EmptyLocalOplog) {
    OplogInterfaceMock localOplog(kEmptyMockOperations);
    RollBackLocalOperations finder(localOplog, [](const BSONObj&) { return Status::OK(); });
    auto result = finder.onRemoteOperation(makeOp(1, 0));
    ASSERT_EQUALS(ErrorCodes::OplogStartMissing, result.getStatus().code());
}

TEST(RollBackLocalOperationsTest, RollbackPeriodTooLong) {
    OplogInterfaceMock localOplog({makeOpAndRecordId(1802, 0)});
    RollBackLocalOperations finder(localOplog, [](const BSONObj&) { return Status::OK(); });
    auto result = finder.onRemoteOperation(makeOp(1, 0));
    ASSERT_EQUALS(ErrorCodes::ExceededTimeLimit, result.getStatus().code());
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
        ASSERT_EQUALS(i->first, operation);
        i++;
        return Status::OK();
    };
    RollBackLocalOperations finder(localOplog, rollbackOperation);
    auto result = finder.onRemoteOperation(commonOperation.first);
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(commonOperation.first["ts"].timestamp(), result.getValue().first);
    ASSERT_EQUALS(commonOperation.second, result.getValue().second);
    ASSERT_FALSE(i == localOperations.cend());
    ASSERT_EQUALS(commonOperation.first, i->first);
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
        ASSERT_EQUALS(i->first, operation);
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
    ASSERT_EQUALS(commonOperation.first["ts"].timestamp(), result.getValue().first);
    ASSERT_EQUALS(commonOperation.second, result.getValue().second);
    ASSERT_FALSE(i == localOperations.cend());
    ASSERT_EQUALS(commonOperation.first, i->first);
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
        ASSERT_EQUALS(i->first, operation);
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
    ASSERT_EQUALS(commonOperation.first["ts"].timestamp(), result.getValue().first);
    ASSERT_EQUALS(commonOperation.second, result.getValue().second);
    ASSERT_FALSE(i == localOperations.cend());
    ASSERT_EQUALS(commonOperation.first, i->first);
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
                  syncRollBackLocalOperations(OplogInterfaceMock(kEmptyMockOperations),
                                              OplogInterfaceMock({makeOpAndRecordId(1, 0)}),
                                              [](const BSONObj&) { return Status::OK(); })
                      .getStatus()
                      .code());
}

TEST(SyncRollBackLocalOperationsTest, RemoteOplogMissing) {
    ASSERT_EQUALS(ErrorCodes::InvalidSyncSource,
                  syncRollBackLocalOperations(OplogInterfaceMock({makeOpAndRecordId(1, 0)}),
                                              OplogInterfaceMock(kEmptyMockOperations),
                                              [](const BSONObj&) { return Status::OK(); })
                      .getStatus()
                      .code());
}

TEST(SyncRollBackLocalOperationsTest, RollbackPeriodTooLong) {
    ASSERT_EQUALS(ErrorCodes::ExceededTimeLimit,
                  syncRollBackLocalOperations(OplogInterfaceMock({makeOpAndRecordId(1802, 0)}),
                                              OplogInterfaceMock({makeOpAndRecordId(1, 0)}),
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
                                                  ASSERT_EQUALS(i->first, operation);
                                                  i++;
                                                  return Status::OK();
                                              });
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(commonOperation.first["ts"].timestamp(), result.getValue().first);
    ASSERT_EQUALS(commonOperation.second, result.getValue().second);
    ASSERT_FALSE(i == localOperations.cend());
    ASSERT_EQUALS(commonOperation.first, i->first);
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
    ASSERT_EQUALS(commonOperation.first["ts"].timestamp(), result.getValue().first);
    ASSERT_EQUALS(commonOperation.second, result.getValue().second);
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
                                        ASSERT_EQUALS(localOperation.first, operation);
                                        called = true;
                                        return Status::OK();
                                    });
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(commonOperation.first["ts"].timestamp(), result.getValue().first);
    ASSERT_EQUALS(commonOperation.second, result.getValue().second);
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
                                        ASSERT_EQUALS(localOperation.first, operation);
                                        called = true;
                                        return Status::OK();
                                    });
    ASSERT_EQUALS(ErrorCodes::NoMatchingDocument, result.getStatus().code());
    ASSERT_STRING_CONTAINS(result.getStatus().reason(),
                           "RS101 reached beginning of local oplog [1]");
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
    auto result = syncRollBackLocalOperations(OplogInterfaceMock({localOperation, commonOperation}),
                                              OplogInterfaceMock({remoteOperation}),
                                              [&](const BSONObj& operation) {
                                                  ASSERT_EQUALS(localOperation.first, operation);
                                                  called = true;
                                                  return Status::OK();
                                              });
    ASSERT_EQUALS(ErrorCodes::NoMatchingDocument, result.getStatus().code());
    ASSERT_STRING_CONTAINS(result.getStatus().reason(), "RS100 reached beginning of remote oplog");
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
                                        ASSERT_EQUALS(localOperation.first, operation);
                                        called = true;
                                        return Status::OK();
                                    });
    ASSERT_EQUALS(ErrorCodes::NoMatchingDocument, result.getStatus().code());
    ASSERT_STRING_CONTAINS(result.getStatus().reason(),
                           "RS101 reached beginning of local oplog [2]");
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
    ASSERT_STRING_CONTAINS(result.getStatus().reason(),
                           "RS100 reached beginning of remote oplog [1]");
}

}  // namespace
