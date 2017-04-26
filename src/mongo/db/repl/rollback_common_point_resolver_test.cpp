/**
 *    Copyright 2017 MongoDB Inc.
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

#include <algorithm>

#include "mongo/db/repl/abstract_oplog_fetcher_test_fixture.h"
#include "mongo/db/repl/oplog_interface_mock.h"
#include "mongo/db/repl/rollback_common_point_resolver.h"
#include "mongo/rpc/metadata.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/task_executor_proxy.h"
#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;
using namespace mongo::repl;
using namespace unittest;

using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;
using NetworkGuard = executor::NetworkInterfaceMock::InNetworkGuard;

class ListenerMock : public RollbackCommonPointResolver::Listener {

public:
    ListenerMock(std::vector<BSONObj>* localOplogEntriesProcessed,
                 std::vector<BSONObj>* remoteOplogEntriesProcessed,
                 RollbackCommonPointResolver::RollbackCommonPoint* commonPoint,
                 bool failLocalOplogEntry,
                 bool failRemoteOplogEntry,
                 bool failCommonPoint)
        : _localOplogEntriesProcessed(localOplogEntriesProcessed),
          _remoteOplogEntriesProcessed(remoteOplogEntriesProcessed),
          _commonPoint(commonPoint),
          _failLocalOplogEntry(failLocalOplogEntry),
          _failRemoteOplogEntry(failRemoteOplogEntry),
          _failCommonPoint(failCommonPoint) {}

    Status onLocalOplogEntry(const BSONObj& oplogEntry) {
        if (_failLocalOplogEntry) {
            return Status(ErrorCodes::OperationFailed, "Failed to process local oplog entry");
        }
        _localOplogEntriesProcessed->emplace_back(oplogEntry);
        return Status::OK();
    }

    Status onRemoteOplogEntry(const BSONObj& oplogEntry) {
        if (_failRemoteOplogEntry) {
            return Status(ErrorCodes::OperationFailed, "Failed to process remote oplog entry");
        }
        _remoteOplogEntriesProcessed->emplace_back(oplogEntry);
        return Status::OK();
    }

    Status onCommonPoint(const RollbackCommonPointResolver::RollbackCommonPoint& oplogEntry) {
        if (_failCommonPoint) {
            return Status(ErrorCodes::OperationFailed, "Failed to process common point");
        }
        *_commonPoint = oplogEntry;
        return Status::OK();
    }

private:
    std::vector<BSONObj>* _localOplogEntriesProcessed;
    std::vector<BSONObj>* _remoteOplogEntriesProcessed;
    RollbackCommonPointResolver::RollbackCommonPoint* _commonPoint;
    bool _failLocalOplogEntry;
    bool _failRemoteOplogEntry;
    bool _failCommonPoint;
};

class RollbackCommonPointResolverTest : public AbstractOplogFetcherTest {
protected:
    void setUp() override;
    void tearDown() override;

    /**
     * Starts a rollback common point resolver. Processes a single batch of results from
     * the oplog query and shuts down.
     * Returns shutdown state.
     */
    std::unique_ptr<ShutdownState> processSingleBatch(
        RemoteCommandResponse response,
        OplogInterfaceMock* localOplog,
        RollbackCommonPointResolver::Listener* listener);

    std::unique_ptr<ShutdownState> processSingleBatch(
        const BSONObj& obj,
        OplogInterfaceMock* localOplog,
        RollbackCommonPointResolver::Listener* listener);

    /**
     * Tests handling of three batches of operations returned from query.
     * Returns shutdown state.
     */
    std::unique_ptr<ShutdownState> processThreeBatches(RemoteCommandResponse response1,
                                                       RemoteCommandResponse response2,
                                                       RemoteCommandResponse response3,
                                                       OplogInterfaceMock* localOplog);

    std::unique_ptr<ShutdownState> processThreeBatches(const BSONObj& response1,
                                                       const BSONObj& response2,
                                                       const BSONObj& response3,
                                                       OplogInterfaceMock* localOplog);

    /**
     * Creates a cursor batch using the oplog entries from 'startIdx' to 'endIdx' (exclusive)
     * in 'oplog'.
     */
    BSONObj makeResponseBatch(std::vector<BSONObj> oplog,
                              bool isFirstBatch,
                              int startIdx,
                              int endIdx = -1);

    // A list of the local and remote oplog entries processed by onLocalOplogEntry and
    // onRemoteOplogEntry.
    std::vector<BSONObj> localOplogEntriesProcessed;
    std::vector<BSONObj> remoteOplogEntriesProcessed;

    // The common point found for this test.
    RollbackCommonPointResolver::RollbackCommonPoint commonPoint;

    std::unique_ptr<RollbackCommonPointResolver> resolver;
    std::unique_ptr<ShutdownState> shutdownState;
};

void RollbackCommonPointResolverTest::setUp() {
    AbstractOplogFetcherTest::setUp();
}

void RollbackCommonPointResolverTest::tearDown() {
    AbstractOplogFetcherTest::tearDown();
    resolver.reset();
    shutdownState.reset();
}

HostAndPort source("localhost:12345");
NamespaceString nss("local.oplog.rs");

std::unique_ptr<ShutdownState> RollbackCommonPointResolverTest::processSingleBatch(
    RemoteCommandResponse response,
    OplogInterfaceMock* oplog,
    RollbackCommonPointResolver::Listener* listener) {
    auto shutdownState = stdx::make_unique<ShutdownState>();
    RollbackCommonPointResolver rollbackCommonPointResolver(
        &getExecutor(), source, nss, 0, oplog, listener, stdx::ref(*shutdownState));

    ASSERT_FALSE(rollbackCommonPointResolver.isActive());
    ASSERT_EQUALS(AbstractOplogFetcher::State::kPreStart,
                  rollbackCommonPointResolver.getState_forTest());
    ASSERT_OK(rollbackCommonPointResolver.startup());
    ASSERT_TRUE(rollbackCommonPointResolver.isActive());
    ASSERT_EQUALS(AbstractOplogFetcher::State::kRunning,
                  rollbackCommonPointResolver.getState_forTest());

    auto request = processNetworkResponse(response);
    ASSERT_BSONOBJ_EQ(rollbackCommonPointResolver.getCommandObject_forTest(), request.cmdObj);

    rollbackCommonPointResolver.shutdown();
    rollbackCommonPointResolver.join();
    ASSERT_EQUALS(AbstractOplogFetcher::State::kComplete,
                  rollbackCommonPointResolver.getState_forTest());

    return shutdownState;
}

std::unique_ptr<ShutdownState> RollbackCommonPointResolverTest::processSingleBatch(
    const BSONObj& obj,
    OplogInterfaceMock* localOplog,
    RollbackCommonPointResolver::Listener* listener) {
    return processSingleBatch(
        {obj, rpc::makeEmptyMetadata(), Milliseconds(0)}, localOplog, listener);
}

/**
 * Helper functions to make simple oplog entries with timestamps, terms, and hashes.
 */
BSONObj makeOp(OpTime time, long long hash) {
    return BSON("ts" << time.getTimestamp() << "h" << hash << "t" << time.getTerm());
}

BSONObj makeOp(int count) {
    return makeOp(OpTime(Timestamp(count, count), count), count);
}

/**
 * Helper functions to make pairs of oplog entries and recordIds for the OplogInterfaceMock used
 * to mock out the local oplog.
 */
int recordId = 0;
OplogInterfaceMock::Operation makeOpAndRecordId(const BSONObj& op) {
    return std::make_pair(op, RecordId(++recordId));
}

OplogInterfaceMock::Operation makeOpAndRecordId(OpTime time, long long hash) {
    return makeOpAndRecordId(makeOp(time, hash));
}

OplogInterfaceMock::Operation makeOpAndRecordId(int count) {
    return makeOpAndRecordId(makeOp(count));
}

/**
 * Helper function to assert that an OplogInterfaceMock has the same oplog entries as a vector
 * of BSONObj oplog entries.
 *
 * If a limit is provided, then it only asserts that many oplog entries are the same.
 */
bool assertOplogInterfaceMockEqualsOpList(const OplogInterfaceMock& oplog1,
                                          std::vector<BSONObj> oplog2,
                                          size_t limit = 0) {
    auto oplog1Iterator = oplog1.makeIterator();
    for (size_t i = 0; i < oplog2.size(); i++) {
        ASSERT_BSONOBJ_EQ(oplog2[i], oplog1Iterator->next().getValue().first);
        if (limit > 0 && i == limit) {
            return true;
        }
    }

    // Assert that we've hit the end, and thus that the oplogs are the same size.
    // If there is a limit, it is higher than the length of oplog2 and is not being used.
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, oplog1Iterator->next().getStatus());
    return true;
}

/**
 * Helper function to assert that two vectors of BSONObj oplog entries are identical.
 *
 * If a limit is provided, then it only asserts that many oplog entries are the same.
 */
bool assertOpListsAreEqual(std::vector<BSONObj> oplog1,
                           std::vector<BSONObj> oplog2,
                           size_t limit = 0) {
    // If there is no limit, we want to make sure the oplogs are the same size.
    if (limit == 0) {
        ASSERT_EQUALS(oplog1.size(), oplog2.size());
    }

    for (size_t i = 0; i < oplog1.size(); i++) {
        ASSERT_BSONOBJ_EQ(oplog1[i], oplog2[i]);
        if (limit > 0 && i == limit) {
            return true;
        }
    }

    return true;
}

TEST_F(RollbackCommonPointResolverTest, FindQueryContainsCorrectFields) {
    OplogInterfaceMock oplog;
    ListenerMock listener(&localOplogEntriesProcessed,
                          &remoteOplogEntriesProcessed,
                          &commonPoint,
                          false,
                          false,
                          false);

    RollbackCommonPointResolver rollbackCommonPointResolver(
        &getExecutor(), source, nss, 0, &oplog, &listener, [](Status) {});

    auto cmdObj = rollbackCommonPointResolver.getFindQuery_forTest();
    ASSERT_EQUALS(mongo::BSONType::Object, cmdObj["filter"].type());
    ASSERT_EQUALS(mongo::BSONType::Object, cmdObj["sort"].type());
    ASSERT_EQUALS(std::string("find"), cmdObj.firstElementFieldName());
    ASSERT_EQUALS(60000, cmdObj.getIntField("maxTimeMS"));
}

TEST_F(RollbackCommonPointResolverTest,
       CommonPointResolverReturnsOplogStartMissingWithEmptyLocalOplog) {
    std::vector<BSONObj> remoteOplog = {makeOp(1)};
    OplogInterfaceMock localOplog;
    ListenerMock listener(&localOplogEntriesProcessed,
                          &remoteOplogEntriesProcessed,
                          &commonPoint,
                          false,
                          false,
                          false);

    ASSERT_EQUALS(ErrorCodes::OplogStartMissing,
                  processSingleBatch({makeCursorResponse(0, remoteOplog)}, &localOplog, &listener)
                      ->getStatus());

    ASSERT(remoteOplogEntriesProcessed.empty());
    ASSERT(localOplogEntriesProcessed.empty());
}

TEST_F(RollbackCommonPointResolverTest,
       CommonPointResolverReturnsNoMatchingDocumentWithEmptyRemoteOplog) {
    std::vector<BSONObj> remoteOplog;
    OplogInterfaceMock localOplog({makeOpAndRecordId(1)});
    ListenerMock listener(&localOplogEntriesProcessed,
                          &remoteOplogEntriesProcessed,
                          &commonPoint,
                          false,
                          false,
                          false);

    ASSERT_EQUALS(ErrorCodes::NoMatchingDocument,
                  processSingleBatch({makeCursorResponse(0, remoteOplog)}, &localOplog, &listener)
                      ->getStatus());

    ASSERT(remoteOplogEntriesProcessed.empty());
    ASSERT(localOplogEntriesProcessed.empty());
}

TEST_F(RollbackCommonPointResolverTest, CommonPointResolverFindsCommonPointWithIdenticalOplogs) {
    OpTime time(Timestamp(1, 1), 1);
    long long hash = 2;
    OplogInterfaceMock::Operation commonPointOp = makeOpAndRecordId(time, hash);

    std::vector<BSONObj> remoteOplog = {commonPointOp.first};
    OplogInterfaceMock localOplog({commonPointOp});
    ListenerMock listener(&localOplogEntriesProcessed,
                          &remoteOplogEntriesProcessed,
                          &commonPoint,
                          false,
                          false,
                          false);

    ASSERT_OK(processSingleBatch({makeCursorResponse(0, remoteOplog)}, &localOplog, &listener)
                  ->getStatus());
    ASSERT_EQUALS(commonPoint.first, time);
    ASSERT_EQUALS(commonPoint.second, commonPointOp.second);

    ASSERT_TRUE(remoteOplogEntriesProcessed.empty());
    ASSERT_TRUE(localOplogEntriesProcessed.empty());
}

TEST_F(RollbackCommonPointResolverTest,
       CommonPointResolverReturnsNoMatchingDocumentWithNoCommonPoint) {
    std::vector<BSONObj> remoteOplog = {makeOp(1)};
    OplogInterfaceMock localOplog({makeOpAndRecordId(2)});
    ListenerMock listener(&localOplogEntriesProcessed,
                          &remoteOplogEntriesProcessed,
                          &commonPoint,
                          false,
                          false,
                          false);

    ASSERT_EQUALS(ErrorCodes::NoMatchingDocument,
                  processSingleBatch({makeCursorResponse(0, remoteOplog)}, &localOplog, &listener)
                      ->getStatus());
}

TEST_F(RollbackCommonPointResolverTest,
       CommonPointResolverPassesThroughErrorFromLocalOplogEntryProcessor) {
    std::vector<BSONObj> remoteOplog = {makeOp(3), makeOp(1)};
    OplogInterfaceMock localOplog({makeOpAndRecordId(4), makeOpAndRecordId(2)});
    ListenerMock listener(&localOplogEntriesProcessed,
                          &remoteOplogEntriesProcessed,
                          &commonPoint,
                          true,
                          false,
                          false);

    ASSERT_EQUALS(ErrorCodes::OperationFailed,
                  processSingleBatch({makeCursorResponse(0, remoteOplog)}, &localOplog, &listener)
                      ->getStatus());
}

TEST_F(RollbackCommonPointResolverTest,
       CommonPointResolverPassesThroughErrorFromRemoteOplogEntryProcessor) {
    std::vector<BSONObj> remoteOplog = {makeOp(3), makeOp(1)};
    OplogInterfaceMock localOplog({makeOpAndRecordId(4), makeOpAndRecordId(2)});
    ListenerMock listener(&localOplogEntriesProcessed,
                          &remoteOplogEntriesProcessed,
                          &commonPoint,
                          false,
                          true,
                          false);

    ASSERT_EQUALS(ErrorCodes::OperationFailed,
                  processSingleBatch({makeCursorResponse(0, remoteOplog)}, &localOplog, &listener)
                      ->getStatus());
}

TEST_F(RollbackCommonPointResolverTest,
       CommonPointResolverPassesThroughErrorFromCommonPointProcessor) {
    OpTime time(Timestamp(3, 3), 3);
    long long hash = 3;
    OplogInterfaceMock::Operation commonPointOp = makeOpAndRecordId(time, hash);

    std::vector<BSONObj> remoteOplog = {commonPointOp.first};
    OplogInterfaceMock localOplog({commonPointOp});
    ListenerMock listener(&localOplogEntriesProcessed,
                          &remoteOplogEntriesProcessed,
                          &commonPoint,
                          false,
                          false,
                          true);

    ASSERT_EQUALS(ErrorCodes::OperationFailed,
                  processSingleBatch({makeCursorResponse(0, remoteOplog)}, &localOplog, &listener)
                      ->getStatus());
}

TEST_F(RollbackCommonPointResolverTest, CommonPointResolverFindsCommonPointWhenOpsAlternate) {
    OpTime time(Timestamp(3, 3), 3);
    long long hash = 3;
    OplogInterfaceMock::Operation commonPointOp = makeOpAndRecordId(time, hash);

    std::vector<BSONObj> remoteOplog = {
        makeOp(10), makeOp(8), makeOp(5), commonPointOp.first, makeOp(1)};
    OplogInterfaceMock localOplog({makeOpAndRecordId(9),
                                   makeOpAndRecordId(7),
                                   makeOpAndRecordId(6),
                                   commonPointOp,
                                   makeOpAndRecordId(2)});
    ListenerMock listener(&localOplogEntriesProcessed,
                          &remoteOplogEntriesProcessed,
                          &commonPoint,
                          false,
                          false,
                          false);

    ASSERT_OK(processSingleBatch({makeCursorResponse(0, remoteOplog)}, &localOplog, &listener)
                  ->getStatus());
    ASSERT_EQUALS(commonPoint.first, time);
    ASSERT_EQUALS(commonPoint.second, commonPointOp.second);

    assertOpListsAreEqual(remoteOplog, remoteOplogEntriesProcessed, 2);
    assertOplogInterfaceMockEqualsOpList(localOplog, localOplogEntriesProcessed, 2);
}

TEST_F(
    RollbackCommonPointResolverTest,
    CommonPointResolverReturnsNoMatchingDocumentWhenLocalOplogEndsAfterEqualTimestampButDifferentHash) {
    OpTime time(Timestamp(3, 3), 3);
    long long hash = 3;

    std::vector<BSONObj> remoteOplog = {makeOp(10), makeOp(time, hash), makeOp(1)};
    OplogInterfaceMock localOplog({makeOpAndRecordId(9), makeOpAndRecordId(time, hash + 1)});
    ListenerMock listener(&localOplogEntriesProcessed,
                          &remoteOplogEntriesProcessed,
                          &commonPoint,
                          false,
                          false,
                          false);

    ASSERT_EQUALS(ErrorCodes::NoMatchingDocument,
                  processSingleBatch({makeCursorResponse(0, remoteOplog)}, &localOplog, &listener)
                      ->getStatus());
}

TEST_F(
    RollbackCommonPointResolverTest,
    CommonPointResolverReturnsCommonPointWhenCommonPointIsAfterEqualTimestampButDifferentHashOnLocalOplog) {
    OpTime diffTime(Timestamp(7, 7), 7);
    long long diffHash = 7;
    OpTime commonTime(Timestamp(1, 1), 1);
    long long commonHash = 1;
    OplogInterfaceMock::Operation commonPointOp = makeOpAndRecordId(commonTime, commonHash);

    std::vector<BSONObj> remoteOplog = {
        makeOp(10), makeOp(diffTime, diffHash), makeOp(6), commonPointOp.first};
    OplogInterfaceMock localOplog(
        {makeOpAndRecordId(9), makeOpAndRecordId(diffTime, diffHash + 1), commonPointOp});
    ListenerMock listener(&localOplogEntriesProcessed,
                          &remoteOplogEntriesProcessed,
                          &commonPoint,
                          false,
                          false,
                          false);

    ASSERT_OK(processSingleBatch({makeCursorResponse(0, remoteOplog)}, &localOplog, &listener)
                  ->getStatus());
    ASSERT_EQUALS(commonPoint.first, commonTime);
    ASSERT_EQUALS(commonPoint.second, commonPointOp.second);

    assertOpListsAreEqual(remoteOplog, remoteOplogEntriesProcessed, 2);
    assertOplogInterfaceMockEqualsOpList(localOplog, localOplogEntriesProcessed, 1);
}

TEST_F(
    RollbackCommonPointResolverTest,
    CommonPointResolverReturnsCommonPointWhenCommonPointIsAfterEqualTimestampButDifferentHashOnRemoteOplog) {
    OpTime diffTime(Timestamp(7, 7), 7);
    long long diffHash = 7;
    OpTime commonTime(Timestamp(1, 1), 1);
    long long commonHash = 1;
    OplogInterfaceMock::Operation commonPointOp = makeOpAndRecordId(commonTime, commonHash);

    std::vector<BSONObj> remoteOplog = {
        makeOp(10), makeOp(diffTime, diffHash), commonPointOp.first};
    OplogInterfaceMock localOplog({makeOpAndRecordId(9),
                                   makeOpAndRecordId(diffTime, diffHash + 1),
                                   makeOpAndRecordId(5),
                                   commonPointOp});
    ListenerMock listener(&localOplogEntriesProcessed,
                          &remoteOplogEntriesProcessed,
                          &commonPoint,
                          false,
                          false,
                          false);

    ASSERT_OK(processSingleBatch({makeCursorResponse(0, remoteOplog)}, &localOplog, &listener)
                  ->getStatus());
    ASSERT_EQUALS(commonPoint.first, commonTime);
    ASSERT_EQUALS(commonPoint.second, commonPointOp.second);

    assertOpListsAreEqual(remoteOplog, remoteOplogEntriesProcessed, 1);
    assertOplogInterfaceMockEqualsOpList(localOplog, localOplogEntriesProcessed, 2);
}

std::unique_ptr<ShutdownState> RollbackCommonPointResolverTest::processThreeBatches(
    RemoteCommandResponse response1,
    RemoteCommandResponse response2,
    RemoteCommandResponse response3,
    OplogInterfaceMock* oplog) {
    auto shutdownState = stdx::make_unique<ShutdownState>();
    ListenerMock listener(&localOplogEntriesProcessed,
                          &remoteOplogEntriesProcessed,
                          &commonPoint,
                          false,
                          false,
                          false);

    RollbackCommonPointResolver rollbackCommonPointResolver(
        &getExecutor(), source, nss, 0, oplog, &listener, stdx::ref(*shutdownState));

    ASSERT_FALSE(rollbackCommonPointResolver.isActive());
    ASSERT_EQUALS(AbstractOplogFetcher::State::kPreStart,
                  rollbackCommonPointResolver.getState_forTest());
    ASSERT_OK(rollbackCommonPointResolver.startup());
    ASSERT_TRUE(rollbackCommonPointResolver.isActive());
    ASSERT_EQUALS(AbstractOplogFetcher::State::kRunning,
                  rollbackCommonPointResolver.getState_forTest());

    unittest::log() << "Sending in find response";

    auto findRequest = processNetworkResponse(response1, true);
    ASSERT_BSONOBJ_EQ(rollbackCommonPointResolver.getCommandObject_forTest(), findRequest.cmdObj);

    unittest::log() << "Sending in first getMore response";

    auto getMoreRequest = processNetworkResponse(response2, true);
    ASSERT_EQUALS(std::string("getMore"), getMoreRequest.cmdObj.firstElementFieldName());
    ASSERT_EQUALS(nss.coll(), getMoreRequest.cmdObj["collection"].String());
    ASSERT_EQUALS(5000, getMoreRequest.cmdObj.getIntField("maxTimeMS"));

    unittest::log() << "Sending in second getMore response";

    getMoreRequest = processNetworkResponse(response3, false);
    ASSERT_EQUALS(std::string("getMore"), getMoreRequest.cmdObj.firstElementFieldName());
    ASSERT_EQUALS(nss.coll(), getMoreRequest.cmdObj["collection"].String());
    ASSERT_EQUALS(5000, getMoreRequest.cmdObj.getIntField("maxTimeMS"));

    rollbackCommonPointResolver.shutdown();
    rollbackCommonPointResolver.join();
    ASSERT_EQUALS(AbstractOplogFetcher::State::kComplete,
                  rollbackCommonPointResolver.getState_forTest());

    return shutdownState;
}

std::unique_ptr<ShutdownState> RollbackCommonPointResolverTest::processThreeBatches(
    const BSONObj& response1,
    const BSONObj& response2,
    const BSONObj& response3,
    OplogInterfaceMock* localOplog) {
    return processThreeBatches({response1, rpc::makeEmptyMetadata(), Milliseconds(0)},
                               {response2, rpc::makeEmptyMetadata(), Milliseconds(0)},
                               {response3, rpc::makeEmptyMetadata(), Milliseconds(0)},
                               localOplog);
}

BSONObj RollbackCommonPointResolverTest::makeResponseBatch(std::vector<BSONObj> oplog,
                                                           bool isFirstBatch,
                                                           int startIdx,
                                                           int endIdx) {
    if (endIdx == -1) {
        // If it's the last batch we should provide a 0 as the cursorId.
        return {makeCursorResponse(0, {oplog.begin() + startIdx, oplog.end()}, isFirstBatch)};
    }
    int cursorId = 4;
    return {makeCursorResponse(
        cursorId, {oplog.begin() + startIdx, oplog.begin() + endIdx}, isFirstBatch)};
}

TEST_F(RollbackCommonPointResolverTest, CommonPointResolverFindsCommonPointInThirdBatch) {
    OpTime time(Timestamp(7, 7), 7);
    long long hash = 7;
    OplogInterfaceMock::Operation commonPointOp = makeOpAndRecordId(time, hash);

    std::vector<BSONObj> remoteOplog = {
        makeOp(20), makeOp(18), makeOp(15), makeOp(14), commonPointOp.first, makeOp(1)};
    OplogInterfaceMock localOplog({makeOpAndRecordId(19),
                                   makeOpAndRecordId(17),
                                   makeOpAndRecordId(16),
                                   makeOpAndRecordId(13),
                                   makeOpAndRecordId(12),
                                   makeOpAndRecordId(11),
                                   makeOpAndRecordId(10),
                                   commonPointOp,
                                   makeOpAndRecordId(2)});

    auto batch1 = makeResponseBatch(remoteOplog, true, 0, 1);
    auto batch2 = makeResponseBatch(remoteOplog, false, 1, 3);
    auto batch3 = makeResponseBatch(remoteOplog, false, 3);

    ASSERT_OK(processThreeBatches(batch1, batch2, batch3, &localOplog)->getStatus());
    ASSERT_EQUALS(commonPoint.first, time);
    ASSERT_EQUALS(commonPoint.second, commonPointOp.second);

    assertOpListsAreEqual(remoteOplog, remoteOplogEntriesProcessed, 3);
    assertOplogInterfaceMockEqualsOpList(localOplog, localOplogEntriesProcessed, 6);
}

TEST_F(RollbackCommonPointResolverTest, CommonPointResolverFinishesLocalOplogInThirdBatch) {
    std::vector<BSONObj> remoteOplog = {makeOp(20), makeOp(18), makeOp(15), makeOp(14), makeOp(1)};
    OplogInterfaceMock localOplog({makeOpAndRecordId(19),
                                   makeOpAndRecordId(17),
                                   makeOpAndRecordId(10),
                                   makeOpAndRecordId(2)});

    auto batch1 = makeResponseBatch(remoteOplog, true, 0, 1);
    auto batch2 = makeResponseBatch(remoteOplog, false, 1, 3);
    auto batch3 = makeResponseBatch(remoteOplog, false, 3);

    ASSERT_EQUALS(ErrorCodes::NoMatchingDocument,
                  processThreeBatches(batch1, batch2, batch3, &localOplog)->getStatus());
}

TEST_F(RollbackCommonPointResolverTest, CommonPointResolverFinishesRemoteOplogInThirdBatch) {
    std::vector<BSONObj> remoteOplog = {makeOp(20), makeOp(18), makeOp(15), makeOp(14), makeOp(4)};
    OplogInterfaceMock localOplog({makeOpAndRecordId(19),
                                   makeOpAndRecordId(17),
                                   makeOpAndRecordId(10),
                                   makeOpAndRecordId(2)});

    auto batch1 = makeResponseBatch(remoteOplog, true, 0, 1);
    // We go all the way to the end in the second batch so the third batch is empty.
    auto batch2 = makeResponseBatch(remoteOplog, false, 1, 5);
    auto batch3 = makeResponseBatch(remoteOplog, false, 5);

    ASSERT_EQUALS(ErrorCodes::NoMatchingDocument,
                  processThreeBatches(batch1, batch2, batch3, &localOplog)->getStatus());
}

TEST_F(RollbackCommonPointResolverTest, CommonPointResolverFindsCommonPointAfterCursorFailure) {
    OpTime time(Timestamp(7, 7), 7);
    long long hash = 7;
    OplogInterfaceMock::Operation commonPointOp = makeOpAndRecordId(time, hash);

    std::vector<BSONObj> remoteOplog = {
        makeOp(20), makeOp(18), makeOp(15), makeOp(14), commonPointOp.first, makeOp(1)};
    OplogInterfaceMock localOplog({makeOpAndRecordId(19),
                                   makeOpAndRecordId(17),
                                   makeOpAndRecordId(16),
                                   makeOpAndRecordId(13),
                                   makeOpAndRecordId(12),
                                   makeOpAndRecordId(11),
                                   makeOpAndRecordId(10),
                                   commonPointOp,
                                   makeOpAndRecordId(2)});

    auto batch1 = makeResponseBatch(remoteOplog, true, 0, 1);
    auto batch2 = makeResponseBatch(remoteOplog, false, 1, 3);
    RemoteCommandResponse batchError{ErrorCodes::CursorNotFound, "cursor not found"};
    auto batch3 = makeResponseBatch(remoteOplog, true, 3);

    ListenerMock listener(&localOplogEntriesProcessed,
                          &remoteOplogEntriesProcessed,
                          &commonPoint,
                          false,
                          false,
                          false);

    shutdownState.reset(new ShutdownState());
    resolver.reset(new RollbackCommonPointResolver(
        &getExecutor(), source, nss, 1, &localOplog, &listener, stdx::ref(*shutdownState)));

    ASSERT_FALSE(resolver->isActive());
    ASSERT_EQUALS(AbstractOplogFetcher::State::kPreStart, resolver->getState_forTest());
    ASSERT_OK(resolver->startup());
    ASSERT_TRUE(resolver->isActive());
    ASSERT_EQUALS(AbstractOplogFetcher::State::kRunning, resolver->getState_forTest());

    unittest::log() << "Sending in find response";

    auto findRequest = processNetworkResponse(batch1, true);
    ASSERT_BSONOBJ_EQ(resolver->getCommandObject_forTest(), findRequest.cmdObj);

    unittest::log() << "Sending in first getMore response";

    auto getMoreRequest = processNetworkResponse(batch2, true);
    ASSERT_EQUALS(std::string("getMore"), getMoreRequest.cmdObj.firstElementFieldName());
    ASSERT_EQUALS(nss.coll(), getMoreRequest.cmdObj["collection"].String());
    ASSERT_EQUALS(5000, getMoreRequest.cmdObj.getIntField("maxTimeMS"));

    unittest::log() << "Sending in second getMore response with failure";

    getMoreRequest = processNetworkResponse(batchError, true);
    ASSERT_EQUALS(std::string("getMore"), getMoreRequest.cmdObj.firstElementFieldName());
    ASSERT_EQUALS(nss.coll(), getMoreRequest.cmdObj["collection"].String());
    ASSERT_EQUALS(5000, getMoreRequest.cmdObj.getIntField("maxTimeMS"));

    unittest::log() << "Sending in second find response";

    findRequest = processNetworkResponse(batch3, false);
    ASSERT_BSONOBJ_EQ(resolver->getCommandObject_forTest(), findRequest.cmdObj);

    resolver->shutdown();
    resolver->join();
    ASSERT_EQUALS(AbstractOplogFetcher::State::kComplete, resolver->getState_forTest());

    ASSERT_OK(shutdownState->getStatus());
    ASSERT_EQUALS(commonPoint.first, time);
    ASSERT_EQUALS(commonPoint.second, commonPointOp.second);

    assertOpListsAreEqual(remoteOplog, remoteOplogEntriesProcessed, 3);
    assertOplogInterfaceMockEqualsOpList(localOplog, localOplogEntriesProcessed, 6);
}

}  // namespace
