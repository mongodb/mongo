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

#include "mongo/db/repl/abstract_oplog_fetcher.h"
#include "mongo/db/repl/abstract_oplog_fetcher_test_fixture.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/task_executor_mock.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/task_executor_proxy.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/scopeguard.h"

namespace {

using namespace mongo;
using namespace mongo::repl;

using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;
using NetworkGuard = executor::NetworkInterfaceMock::InNetworkGuard;

HostAndPort source("localhost:12345");
NamespaceString nss("local.oplog.rs");

/**
 * This class is the minimal implementation of an oplog fetcher. It has the simplest `find` command
 * possible, no metadata, and the _onSuccessfulBatch function simply returns a `getMore` command
 * on the fetcher's cursor.
 */
class MockOplogFetcher : public AbstractOplogFetcher {
public:
    explicit MockOplogFetcher(executor::TaskExecutor* executor,
                              OpTimeWithHash lastFetched,
                              HostAndPort source,
                              NamespaceString nss,
                              std::size_t maxFetcherRestarts,
                              OnShutdownCallbackFn onShutdownCallbackFn);

private:
    BSONObj _makeFindCommandObject(const NamespaceString& nss,
                                   OpTime lastOpTimeFetched) const override;
    BSONObj _makeMetadataObject() const override;

    StatusWith<BSONObj> _onSuccessfulBatch(const Fetcher::QueryResponse& queryResponse) override;
};

MockOplogFetcher::MockOplogFetcher(executor::TaskExecutor* executor,
                                   OpTimeWithHash lastFetched,
                                   HostAndPort source,
                                   NamespaceString nss,
                                   std::size_t maxFetcherRestarts,
                                   OnShutdownCallbackFn onShutdownCallbackFn)
    : AbstractOplogFetcher(executor,
                           lastFetched,
                           source,
                           nss,
                           maxFetcherRestarts,
                           onShutdownCallbackFn,
                           "mock oplog fetcher") {}

BSONObj MockOplogFetcher::_makeFindCommandObject(const NamespaceString& nss,
                                                 OpTime lastOpTimeFetched) const {
    BSONObjBuilder cmdBob;
    cmdBob.append("find", nss.coll());
    cmdBob.append("filter", BSON("ts" << BSON("$gte" << lastOpTimeFetched.getTimestamp())));
    return cmdBob.obj();
}

BSONObj MockOplogFetcher::_makeMetadataObject() const {
    return BSONObj();
}

StatusWith<BSONObj> MockOplogFetcher::_onSuccessfulBatch(
    const Fetcher::QueryResponse& queryResponse) {
    BSONObjBuilder cmdBob;
    cmdBob.append("getMore", queryResponse.cursorId);
    cmdBob.append("collection", _getNamespace().coll());
    return cmdBob.obj();
}

TEST_F(AbstractOplogFetcherTest, ShuttingExecutorDownShouldPreventOplogFetcherFromStarting) {
    getExecutor().shutdown();

    MockOplogFetcher oplogFetcher(&getExecutor(), lastFetched, source, nss, 0, [](Status) {});

    // Last optime and hash fetched should match values passed to constructor.
    ASSERT_EQUALS(lastFetched, oplogFetcher.getLastOpTimeWithHashFetched_forTest());

    ASSERT_FALSE(oplogFetcher.isActive());
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, oplogFetcher.startup());
    ASSERT_FALSE(oplogFetcher.isActive());

    // Last optime and hash fetched should not change.
    ASSERT_EQUALS(lastFetched, oplogFetcher.getLastOpTimeWithHashFetched_forTest());
}

TEST_F(AbstractOplogFetcherTest, StartupReturnsOperationFailedIfExecutorFailsToScheduleFetcher) {
    ShutdownState shutdownState;

    TaskExecutorMock taskExecutorMock(&getExecutor());
    taskExecutorMock.shouldFailScheduleWorkRequest = []() { return true; };

    MockOplogFetcher oplogFetcher(
        &taskExecutorMock, lastFetched, source, nss, 0, stdx::ref(shutdownState));

    ASSERT_EQUALS(ErrorCodes::OperationFailed, oplogFetcher.startup());
}

TEST_F(AbstractOplogFetcherTest, OplogFetcherReturnsOperationFailedIfExecutorFailsToScheduleFind) {
    ShutdownState shutdownState;

    TaskExecutorMock taskExecutorMock(&getExecutor());
    taskExecutorMock.shouldFailScheduleRemoteCommandRequest =
        [](const executor::RemoteCommandRequest&) { return true; };

    MockOplogFetcher oplogFetcher(
        &taskExecutorMock, lastFetched, source, nss, 0, stdx::ref(shutdownState));

    ASSERT_FALSE(oplogFetcher.isActive());
    ASSERT_OK(oplogFetcher.startup());

    // It is racy to check OplogFetcher::isActive() immediately after calling startup() because
    // OplogFetcher schedules the remote command on a different thread from the caller of startup().

    oplogFetcher.join();

    ASSERT_EQUALS(ErrorCodes::OperationFailed, shutdownState.getStatus());
}

TEST_F(AbstractOplogFetcherTest, ShuttingExecutorDownAfterStartupStopsTheOplogFetcher) {
    ShutdownState shutdownState;

    TaskExecutorMock taskExecutorMock(&getExecutor());
    taskExecutorMock.shouldDeferScheduleWorkRequestByOneSecond = []() { return true; };

    MockOplogFetcher oplogFetcher(
        &taskExecutorMock, lastFetched, source, nss, 0, stdx::ref(shutdownState));

    ASSERT_FALSE(oplogFetcher.isActive());
    ASSERT_OK(oplogFetcher.startup());
    ASSERT_TRUE(oplogFetcher.isActive());

    getExecutor().shutdown();

    oplogFetcher.join();

    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, shutdownState.getStatus());
}

TEST_F(AbstractOplogFetcherTest, OplogFetcherReturnsCallbackCanceledIfShutdownAfterStartup) {
    ShutdownState shutdownState;

    TaskExecutorMock taskExecutorMock(&getExecutor());
    taskExecutorMock.shouldDeferScheduleWorkRequestByOneSecond = []() { return true; };

    MockOplogFetcher oplogFetcher(
        &taskExecutorMock, lastFetched, source, nss, 0, stdx::ref(shutdownState));

    ASSERT_FALSE(oplogFetcher.isActive());
    ASSERT_OK(oplogFetcher.startup());
    ASSERT_TRUE(oplogFetcher.isActive());

    oplogFetcher.shutdown();

    oplogFetcher.join();

    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, shutdownState.getStatus());
}

long long _getHash(const BSONObj& oplogEntry) {
    return oplogEntry["h"].numberLong();
}

Timestamp _getTimestamp(const BSONObj& oplogEntry) {
    return OplogEntry(oplogEntry).getOpTime().getTimestamp();
}

OpTimeWithHash _getOpTimeWithHash(const BSONObj& oplogEntry) {
    return {_getHash(oplogEntry), OplogEntry(oplogEntry).getOpTime()};
}

std::vector<BSONObj> _generateOplogEntries(std::size_t size) {
    std::vector<BSONObj> ops(size);
    for (std::size_t i = 0; i < size; ++i) {
        ops[i] = AbstractOplogFetcherTest::makeNoopOplogEntry(Seconds(100 + int(i)), 123LL);
    }
    return ops;
}

void _assertFindCommandTimestampEquals(const Timestamp& timestamp,
                                       const RemoteCommandRequest& request) {
    executor::TaskExecutorTest::assertRemoteCommandNameEquals("find", request);
    ASSERT_EQUALS(timestamp, request.cmdObj["filter"].Obj()["ts"].Obj()["$gte"].timestamp());
}

void _assertFindCommandTimestampEquals(const BSONObj& oplogEntry,
                                       const RemoteCommandRequest& request) {
    _assertFindCommandTimestampEquals(_getTimestamp(oplogEntry), request);
}

TEST_F(AbstractOplogFetcherTest,
       OplogFetcherCreatesNewFetcherOnCallbackErrorDuringGetMoreNumberOne) {
    auto ops = _generateOplogEntries(5U);
    std::size_t maxFetcherRestarts = 1U;
    auto shutdownState = stdx::make_unique<ShutdownState>();
    MockOplogFetcher oplogFetcher(&getExecutor(),
                                  _getOpTimeWithHash(ops[0]),
                                  source,
                                  nss,
                                  maxFetcherRestarts,
                                  stdx::ref(*shutdownState));

    ON_BLOCK_EXIT([this] { getExecutor().shutdown(); });

    ASSERT_OK(oplogFetcher.startup());

    // Send first batch from FIND.
    _assertFindCommandTimestampEquals(
        ops[0], processNetworkResponse({makeCursorResponse(1, {ops[0], ops[1], ops[2]})}, true));

    // Send error during GETMORE.
    processNetworkResponse({ErrorCodes::CursorNotFound, "cursor not found"}, true);

    // Send first batch from FIND, and Check that it started from the end of the last FIND response.
    // Check that the optimes match for the query and last oplog entry.
    _assertFindCommandTimestampEquals(
        ops[2], processNetworkResponse({makeCursorResponse(0, {ops[2], ops[3], ops[4]})}, false));

    // Done.
    oplogFetcher.join();
    ASSERT_OK(shutdownState->getStatus());
}

TEST_F(AbstractOplogFetcherTest, OplogFetcherStopsRestartingFetcherIfRestartLimitIsReached) {
    auto ops = _generateOplogEntries(3U);
    std::size_t maxFetcherRestarts = 2U;
    auto shutdownState = stdx::make_unique<ShutdownState>();
    MockOplogFetcher oplogFetcher(&getExecutor(),
                                  _getOpTimeWithHash(ops[0]),
                                  source,
                                  nss,
                                  maxFetcherRestarts,
                                  stdx::ref(*shutdownState));

    ON_BLOCK_EXIT([this] { getExecutor().shutdown(); });

    ASSERT_OK(oplogFetcher.startup());

    unittest::log() << "processing find request from first fetcher";

    _assertFindCommandTimestampEquals(
        ops[0], processNetworkResponse({makeCursorResponse(1, {ops[0], ops[1], ops[2]})}, true));

    unittest::log() << "sending error response to getMore request from first fetcher";
    assertRemoteCommandNameEquals(
        "getMore", processNetworkResponse({ErrorCodes::CappedPositionLost, "fail 1"}, true));

    unittest::log() << "sending error response to find request from second fetcher";
    _assertFindCommandTimestampEquals(
        ops[2], processNetworkResponse({ErrorCodes::IllegalOperation, "fail 2"}, true));

    unittest::log() << "sending error response to find request from third fetcher";
    _assertFindCommandTimestampEquals(
        ops[2], processNetworkResponse({ErrorCodes::OperationFailed, "fail 3"}, false));

    oplogFetcher.join();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, shutdownState->getStatus());
}

TEST_F(AbstractOplogFetcherTest, OplogFetcherResetsRestartCounterOnSuccessfulFetcherResponse) {
    auto ops = _generateOplogEntries(5U);
    std::size_t maxFetcherRestarts = 2U;
    auto shutdownState = stdx::make_unique<ShutdownState>();
    MockOplogFetcher oplogFetcher(&getExecutor(),
                                  _getOpTimeWithHash(ops[0]),
                                  source,
                                  nss,
                                  maxFetcherRestarts,
                                  stdx::ref(*shutdownState));
    ON_BLOCK_EXIT([this] { getExecutor().shutdown(); });

    ASSERT_OK(oplogFetcher.startup());

    unittest::log() << "processing find request from first fetcher";

    _assertFindCommandTimestampEquals(
        ops[0], processNetworkResponse({makeCursorResponse(1, {ops[0], ops[1], ops[2]})}, true));

    unittest::log() << "sending error response to getMore request from first fetcher";
    assertRemoteCommandNameEquals(
        "getMore", processNetworkResponse({ErrorCodes::CappedPositionLost, "fail 1"}, true));

    unittest::log() << "processing find request from second fetcher";
    _assertFindCommandTimestampEquals(
        ops[2], processNetworkResponse({makeCursorResponse(1, {ops[2], ops[3], ops[4]})}, true));

    unittest::log() << "sending error response to getMore request from second fetcher";
    assertRemoteCommandNameEquals(
        "getMore", processNetworkResponse({ErrorCodes::IllegalOperation, "fail 2"}, true));

    unittest::log() << "sending error response to find request from third fetcher";
    _assertFindCommandTimestampEquals(
        ops[4], processNetworkResponse({ErrorCodes::InternalError, "fail 3"}, true));

    unittest::log() << "sending error response to find request from fourth fetcher";
    _assertFindCommandTimestampEquals(
        ops[4], processNetworkResponse({ErrorCodes::OperationFailed, "fail 4"}, false));

    oplogFetcher.join();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, shutdownState->getStatus());
}

class TaskExecutorWithFailureInScheduleRemoteCommand : public unittest::TaskExecutorProxy {
public:
    using ShouldFailRequestFn = stdx::function<bool(const executor::RemoteCommandRequest&)>;

    TaskExecutorWithFailureInScheduleRemoteCommand(executor::TaskExecutor* executor,
                                                   ShouldFailRequestFn shouldFailRequest)
        : unittest::TaskExecutorProxy(executor), _shouldFailRequest(shouldFailRequest) {}

    StatusWith<CallbackHandle> scheduleRemoteCommand(const executor::RemoteCommandRequest& request,
                                                     const RemoteCommandCallbackFn& cb) override {
        if (_shouldFailRequest(request)) {
            return Status(ErrorCodes::OperationFailed, "failed to schedule remote command");
        }
        return getExecutor()->scheduleRemoteCommand(request, cb);
    }

private:
    ShouldFailRequestFn _shouldFailRequest;
};

TEST_F(AbstractOplogFetcherTest,
       OplogFetcherAbortsWithOriginalResponseErrorOnFailureToScheduleNewFetcher) {
    auto ops = _generateOplogEntries(3U);
    std::size_t maxFetcherRestarts = 2U;
    auto shutdownState = stdx::make_unique<ShutdownState>();
    bool shouldFailSchedule = false;
    TaskExecutorWithFailureInScheduleRemoteCommand _executorProxy(
        &getExecutor(), [&shouldFailSchedule](const executor::RemoteCommandRequest& request) {
            return shouldFailSchedule;
        });
    MockOplogFetcher oplogFetcher(&_executorProxy,
                                  _getOpTimeWithHash(ops[0]),
                                  source,
                                  nss,
                                  maxFetcherRestarts,
                                  stdx::ref(*shutdownState));
    ON_BLOCK_EXIT([this] { getExecutor().shutdown(); });

    ASSERT_OK(oplogFetcher.startup());
    ASSERT_TRUE(oplogFetcher.isActive());

    unittest::log() << "processing find request from first fetcher";

    _assertFindCommandTimestampEquals(
        ops[0], processNetworkResponse({makeCursorResponse(1, {ops[0], ops[1], ops[2]})}, true));

    unittest::log() << "sending error response to getMore request from first fetcher";
    shouldFailSchedule = true;
    assertRemoteCommandNameEquals(
        "getMore", processNetworkResponse({ErrorCodes::CappedPositionLost, "dead cursor"}, false));

    oplogFetcher.join();
    // Status in shutdown callback should match error for dead cursor instead of error from failed
    // schedule request.
    ASSERT_EQUALS(ErrorCodes::CappedPositionLost, shutdownState->getStatus());
}

bool sharedCallbackStateDestroyed = false;
class SharedCallbackState {
    MONGO_DISALLOW_COPYING(SharedCallbackState);

public:
    SharedCallbackState() {}
    ~SharedCallbackState() {
        sharedCallbackStateDestroyed = true;
    }
};

TEST_F(AbstractOplogFetcherTest, OplogFetcherResetsOnShutdownCallbackFunctionOnCompletion) {
    auto sharedCallbackData = std::make_shared<SharedCallbackState>();
    auto callbackInvoked = false;
    auto status = getDetectableErrorStatus();

    MockOplogFetcher oplogFetcher(
        &getExecutor(),
        lastFetched,
        source,
        nss,
        0,
        [&callbackInvoked, sharedCallbackData, &status](const Status& shutdownStatus) {
            status = shutdownStatus, callbackInvoked = true;
        });
    ON_BLOCK_EXIT([this] { getExecutor().shutdown(); });

    ASSERT_FALSE(oplogFetcher.isActive());
    ASSERT_OK(oplogFetcher.startup());
    ASSERT_TRUE(oplogFetcher.isActive());

    sharedCallbackData.reset();
    ASSERT_FALSE(sharedCallbackStateDestroyed);

    processNetworkResponse({ErrorCodes::OperationFailed, "oplog tailing query failed"}, false);

    oplogFetcher.join();

    ASSERT_EQUALS(ErrorCodes::OperationFailed, status);

    // Oplog fetcher should reset 'OplogFetcher::_onShutdownCallbackFn' after running callback
    // function before becoming inactive.
    // This ensures that we release resources associated with
    // 'OplogFetcher::_onShutdownCallbackFn'.
    ASSERT_TRUE(callbackInvoked);
    ASSERT_TRUE(sharedCallbackStateDestroyed);
}

}  // namespace
