/**
 *    Copyright 2016 MongoDB Inc.
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

#include <memory>

#include "mongo/db/cursor_id.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/sync_source_resolver.h"
#include "mongo/db/repl/sync_source_selector.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/task_executor_proxy.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace {

using namespace mongo;
using namespace mongo::repl;

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

class SyncSourceSelectorMock : public SyncSourceSelector {
public:
    void clearSyncSourceBlacklist() override {}
    HostAndPort chooseNewSyncSource(const Timestamp& ts) override {
        chooseNewSyncSourceHook();
        lastTimestampFetched = ts;
        return syncSource;
    }
    void blacklistSyncSource(const HostAndPort& host, Date_t until) override {
        blacklistHost = host;
        blacklistUntil = until;
    }
    bool shouldChangeSyncSource(const HostAndPort&, const rpc::ReplSetMetadata&) {
        return false;
    }

    HostAndPort syncSource = HostAndPort("host1", 1234);
    Timestamp lastTimestampFetched;
    stdx::function<void()> chooseNewSyncSourceHook = []() {};

    HostAndPort blacklistHost;
    Date_t blacklistUntil;
};

class SyncSourceResolverTest : public executor::ThreadPoolExecutorTest {
private:
    void setUp() override;
    void tearDown() override;

protected:
    std::unique_ptr<SyncSourceResolver> _makeResolver(const OpTime& lastOpTimeFetched,
                                                      const OpTime& requiredOpTime);
    TaskExecutorWithFailureInScheduleRemoteCommand::ShouldFailRequestFn _shouldFailRequest;
    std::unique_ptr<TaskExecutorWithFailureInScheduleRemoteCommand> _executorProxy;

    SyncSourceResolverResponse _response;
    SyncSourceResolver::OnCompletionFn _onCompletion;
    std::unique_ptr<SyncSourceSelectorMock> _selector;

    std::unique_ptr<SyncSourceResolver> _resolver;
};

const OpTime lastOpTimeFetched(Timestamp(Seconds(100), 1U), 1LL);

void SyncSourceResolverTest::setUp() {
    executor::ThreadPoolExecutorTest::setUp();

    _shouldFailRequest = [](const executor::RemoteCommandRequest&) { return false; };
    _executorProxy = stdx::make_unique<TaskExecutorWithFailureInScheduleRemoteCommand>(
        &getExecutor(), [this](const executor::RemoteCommandRequest& request) {
            return _shouldFailRequest(request);
        });

    _response.syncSourceStatus = getDetectableErrorStatus();
    _onCompletion = [this](const SyncSourceResolverResponse& response) { _response = response; };

    _selector = stdx::make_unique<SyncSourceSelectorMock>();
    _resolver = _makeResolver(lastOpTimeFetched, OpTime());

    launchExecutorThread();
}

void SyncSourceResolverTest::tearDown() {
    executor::ThreadPoolExecutorTest::shutdownExecutorThread();
    executor::ThreadPoolExecutorTest::joinExecutorThread();

    _resolver.reset();
    _selector.reset();
    _executorProxy.reset();

    executor::ThreadPoolExecutorTest::tearDown();
}

std::unique_ptr<SyncSourceResolver> SyncSourceResolverTest::_makeResolver(
    const OpTime& lastOpTimeFetched, const OpTime& requiredOpTime) {
    return stdx::make_unique<SyncSourceResolver>(
        _executorProxy.get(),
        _selector.get(),
        lastOpTimeFetched,
        requiredOpTime,
        [this](const SyncSourceResolverResponse& response) { _onCompletion(response); });
}

const NamespaceString nss("local.oplog.rs");

BSONObj makeCursorResponse(CursorId cursorId,
                           const NamespaceString& nss,
                           std::vector<BSONObj> docs,
                           bool isFirstBatch = true) {
    BSONObjBuilder bob;
    {
        BSONObjBuilder cursorBob(bob.subobjStart("cursor"));
        cursorBob.append("id", cursorId);
        cursorBob.append("ns", nss.toString());
        {
            BSONArrayBuilder batchBob(
                cursorBob.subarrayStart(isFirstBatch ? "firstBatch" : "nextBatch"));
            for (const auto& doc : docs) {
                batchBob.append(doc);
            }
        }
    }
    bob.append("ok", 1);
    return bob.obj();
}

TEST_F(SyncSourceResolverTest, InvalidConstruction) {
    SyncSourceSelectorMock selector;
    const OpTime lastOpTimeFetched(Timestamp(Seconds(100), 1U), 1LL);
    const OpTime requiredOpTime;
    auto onCompletion = [](const SyncSourceResolverResponse&) {};

    // Null task executor.
    ASSERT_THROWS_CODE_AND_WHAT(
        SyncSourceResolver(nullptr, &selector, lastOpTimeFetched, requiredOpTime, onCompletion),
        UserException,
        ErrorCodes::BadValue,
        "task executor cannot be null");

    // Null sync source selector.
    ASSERT_THROWS_CODE_AND_WHAT(
        SyncSourceResolver(
            &getExecutor(), nullptr, lastOpTimeFetched, requiredOpTime, onCompletion),
        UserException,
        ErrorCodes::BadValue,
        "sync source selector cannot be null");

    // Null last fetched optime.
    ASSERT_THROWS_CODE_AND_WHAT(
        SyncSourceResolver(&getExecutor(), &selector, OpTime(), requiredOpTime, onCompletion),
        UserException,
        ErrorCodes::BadValue,
        "last fetched optime cannot be null");

    // If provided, required optime must be more recent than last fetched optime.
    ASSERT_THROWS_CODE_AND_WHAT(SyncSourceResolver(&getExecutor(),
                                                   &selector,
                                                   lastOpTimeFetched,
                                                   OpTime(Timestamp(Seconds(50), 1U), 1LL),
                                                   onCompletion),
                                UserException,
                                ErrorCodes::BadValue,
                                "required optime (if provided) must be more recent than last "
                                "fetched optime. requiredOpTime: { ts: Timestamp 50000|1, t: 1 }, "
                                "lastOpTimeFetched: { ts: Timestamp 100000|1, t: 1 }");
    ASSERT_THROWS_CODE_AND_WHAT(
        SyncSourceResolver(
            &getExecutor(), &selector, lastOpTimeFetched, lastOpTimeFetched, onCompletion),
        UserException,
        ErrorCodes::BadValue,
        "required optime (if provided) must be more recent than last fetched optime. "
        "requiredOpTime: { ts: Timestamp 100000|1, t: 1 }, lastOpTimeFetched: { ts: Timestamp "
        "100000|1, t: 1 }");

    // Null callback function.
    ASSERT_THROWS_CODE_AND_WHAT(SyncSourceResolver(&getExecutor(),
                                                   &selector,
                                                   lastOpTimeFetched,
                                                   requiredOpTime,
                                                   SyncSourceResolver::OnCompletionFn()),
                                UserException,
                                ErrorCodes::BadValue,
                                "callback function cannot be null");
}

TEST_F(SyncSourceResolverTest, StartupReturnsIllegalOperationIfAlreadyActive) {
    ASSERT_FALSE(_resolver->isActive());
    ASSERT_OK(_resolver->startup());
    ASSERT_TRUE(_resolver->isActive());
    ASSERT_EQUALS(ErrorCodes::IllegalOperation, _resolver->startup());
    ASSERT_TRUE(_resolver->isActive());
}

TEST_F(SyncSourceResolverTest, StartupReturnsShutdownInProgressIfResolverIsShuttingDown) {
    _selector->syncSource = HostAndPort("node1", 12345);
    ASSERT_OK(_resolver->startup());
    ASSERT_TRUE(executor::NetworkInterfaceMock::InNetworkGuard(getNet())->hasReadyRequests());
    _resolver->shutdown();
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, _resolver->startup());
    ASSERT_TRUE(_resolver->isActive());
}

TEST_F(SyncSourceResolverTest, StartupReturnsShutdownInProgressIfExecutorIsShutdown) {
    ASSERT_FALSE(_resolver->isActive());
    getExecutor().shutdown();
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, _resolver->startup());
    ASSERT_FALSE(_resolver->isActive());
}

TEST_F(SyncSourceResolverTest,
       SyncSourceResolverReturnsStatusOkAndAnEmptyHostWhenNoViableHostExists) {
    _selector->syncSource = HostAndPort();
    ASSERT_OK(_resolver->startup());

    // Resolver invokes callback with empty host and becomes inactive immediately.
    ASSERT_FALSE(_resolver->isActive());
    ASSERT_EQUALS(HostAndPort(), unittest::assertGet(_response.syncSourceStatus));
    ASSERT_EQUALS(lastOpTimeFetched.getTimestamp(), _selector->lastTimestampFetched);

    // Cannot restart a completed resolver.
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, _resolver->startup());
}

TEST_F(
    SyncSourceResolverTest,
    SyncSourceResolverReturnsCallbackCanceledIfResolverIsShutdownBeforeReturningEmptySyncSource) {
    _selector->syncSource = HostAndPort();
    _selector->chooseNewSyncSourceHook = [this]() { _resolver->shutdown(); };
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, _resolver->startup());
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, _response.syncSourceStatus);
}

TEST_F(SyncSourceResolverTest,
       SyncSourceResolverConvertsExceptionToStatusIfChoosingViableSyncSourceThrowsException) {
    _selector->chooseNewSyncSourceHook = [this]() {
        uassert(ErrorCodes::InternalError, "", false);
    };
    ASSERT_EQUALS(ErrorCodes::InternalError, _resolver->startup());
    ASSERT_EQUALS(ErrorCodes::InternalError, _response.syncSourceStatus);
}

TEST_F(SyncSourceResolverTest,
       SyncSourceResolverReturnsScheduleErrorIfTaskExecutorFailsToScheduleRemoteCommand) {
    _shouldFailRequest = [](const executor::RemoteCommandRequest&) { return true; };
    ASSERT_EQUALS(ErrorCodes::OperationFailed, _resolver->startup());
    ASSERT_EQUALS(ErrorCodes::OperationFailed, _response.syncSourceStatus);
}

void _scheduleFirstOplogEntryFetcherResponse(executor::NetworkInterfaceMock* net,
                                             SyncSourceSelectorMock* selector,
                                             HostAndPort currentSyncSource,
                                             HostAndPort nextSyncSource,
                                             std::vector<BSONObj> docs) {
    executor::NetworkInterfaceMock::InNetworkGuard networkGuard(net);
    ASSERT_TRUE(net->hasReadyRequests());
    auto request = net->scheduleSuccessfulResponse(makeCursorResponse(0, nss, docs));
    ASSERT_EQUALS(currentSyncSource, request.target);
    ASSERT_EQUALS(SyncSourceResolver::kLocalOplogNss.db(), request.dbname);
    ASSERT_EQUALS(SyncSourceResolver::kFetcherTimeout, request.timeout);
    auto firstElement = request.cmdObj.firstElement();
    ASSERT_EQUALS("find"_sd, firstElement.fieldNameStringData());
    ASSERT_EQUALS(SyncSourceResolver::kLocalOplogNss.coll(), firstElement.String());
    ASSERT_EQUALS(1, request.cmdObj.getIntField("limit"));
    ASSERT_BSONOBJ_EQ(BSON("$natural" << 1), request.cmdObj.getObjectField("sort"));

    // Change next sync source candidate before delivering scheduled response.
    selector->syncSource = nextSyncSource;

    net->runReadyNetworkOperations();
}

void _scheduleFirstOplogEntryFetcherResponse(executor::NetworkInterfaceMock* net,
                                             SyncSourceSelectorMock* selector,
                                             HostAndPort currentSyncSource,
                                             HostAndPort nextSyncSource,
                                             Timestamp ts) {
    _scheduleFirstOplogEntryFetcherResponse(
        net, selector, currentSyncSource, nextSyncSource, {BSON("ts" << ts << "t" << 0)});
}

TEST_F(SyncSourceResolverTest,
       SyncSourceResolverReturnsStatusOkAndTheFoundHostWhenAnEligibleSyncSourceExists) {
    HostAndPort candidate1("node1", 12345);
    _selector->syncSource = candidate1;
    ASSERT_OK(_resolver->startup());
    ASSERT_TRUE(_resolver->isActive());

    _scheduleFirstOplogEntryFetcherResponse(
        getNet(), _selector.get(), candidate1, HostAndPort(), Timestamp(10, 2));

    _resolver->join();
    ASSERT_FALSE(_resolver->isActive());
    ASSERT_EQUALS(candidate1, unittest::assertGet(_response.syncSourceStatus));
}

TEST_F(SyncSourceResolverTest,
       SyncSourceResolverTransitionsToCompleteWhenFinishCallbackThrowsException) {
    HostAndPort candidate1("node1", 12345);
    _selector->syncSource = candidate1;
    _onCompletion = [this](const SyncSourceResolverResponse& response) {
        _response = response;
        uassert(ErrorCodes::InternalError, "", false);
    };

    ASSERT_OK(_resolver->startup());
    ASSERT_TRUE(_resolver->isActive());

    _scheduleFirstOplogEntryFetcherResponse(
        getNet(), _selector.get(), candidate1, HostAndPort(), Timestamp(10, 2));

    _resolver->join();
    ASSERT_FALSE(_resolver->isActive());
    ASSERT_EQUALS(candidate1, unittest::assertGet(_response.syncSourceStatus));
}

TEST_F(SyncSourceResolverTest,
       ResolverReturnsCallbackCanceledIfResolverIsShutdownAfterSchedulingFetcher) {
    ASSERT_OK(_resolver->startup());
    ASSERT_TRUE(_resolver->isActive());

    _resolver->shutdown();
    executor::NetworkInterfaceMock::InNetworkGuard(getNet())->runReadyNetworkOperations();

    _resolver->join();
    ASSERT_FALSE(_resolver->isActive());
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, _response.syncSourceStatus);
}

TEST_F(SyncSourceResolverTest,
       SyncSourceResolverReturnsCallbackCanceledIfExecutorIsShutdownAfterSchedulingFetcher) {
    ASSERT_OK(_resolver->startup());
    ASSERT_TRUE(_resolver->isActive());

    getExecutor().shutdown();

    _resolver->join();
    ASSERT_FALSE(_resolver->isActive());
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, _response.syncSourceStatus);
}

TEST_F(SyncSourceResolverTest,
       SyncSourceResolverWillTryOtherSourcesWhenTheFirstNodeDoesNotHaveOldEnoughData) {
    HostAndPort candidate1("node1", 12345);
    HostAndPort candidate2("node2", 12345);
    _selector->syncSource = candidate1;

    ASSERT_OK(_resolver->startup());
    ASSERT_TRUE(_resolver->isActive());

    _scheduleFirstOplogEntryFetcherResponse(
        getNet(), _selector.get(), candidate1, candidate2, Timestamp(200, 2));

    ASSERT_TRUE(_resolver->isActive());
    ASSERT_EQUALS(candidate1, _selector->blacklistHost);
    ASSERT_EQUALS(getExecutor().now() + SyncSourceResolver::kTooStaleBlacklistDuration,
                  _selector->blacklistUntil);

    _scheduleFirstOplogEntryFetcherResponse(
        getNet(), _selector.get(), candidate2, HostAndPort(), Timestamp(10, 2));

    _resolver->join();
    ASSERT_FALSE(_resolver->isActive());
    ASSERT_EQUALS(candidate2, unittest::assertGet(_response.syncSourceStatus));
}

TEST_F(SyncSourceResolverTest,
       SyncSourceResolverReturnsOplogStartMissingAndEarliestOpTimeAvailableWhenAllSourcesTooFresh) {
    HostAndPort candidate1("node1", 12345);
    HostAndPort candidate2("node2", 12345);
    HostAndPort candidate3("node3", 12345);
    _selector->syncSource = candidate1;

    ASSERT_OK(_resolver->startup());
    ASSERT_TRUE(_resolver->isActive());

    _scheduleFirstOplogEntryFetcherResponse(
        getNet(), _selector.get(), candidate1, candidate2, Timestamp(400, 2));

    _scheduleFirstOplogEntryFetcherResponse(
        getNet(), _selector.get(), candidate2, candidate3, Timestamp(200, 2));

    _scheduleFirstOplogEntryFetcherResponse(
        getNet(), _selector.get(), candidate3, HostAndPort(), Timestamp(300, 2));

    _resolver->join();
    ASSERT_FALSE(_resolver->isActive());
    ASSERT_EQUALS(ErrorCodes::OplogStartMissing, _response.syncSourceStatus);
    ASSERT_EQUALS(Timestamp(200, 2), _response.earliestOpTimeSeen.getTimestamp());
}

void _scheduleNetworkErrorForFirstNode(executor::NetworkInterfaceMock* net,
                                       SyncSourceSelectorMock* selector,
                                       HostAndPort currentSyncSource,
                                       HostAndPort nextSyncSource) {
    executor::NetworkInterfaceMock::InNetworkGuard networkGuard(net);
    ASSERT_TRUE(net->hasReadyRequests());
    auto request = net->scheduleErrorResponse(
        Status(ErrorCodes::HostUnreachable, "Sad message from the network :("));
    ASSERT_EQUALS(currentSyncSource, request.target);

    // Change next sync source candidate before delivering error to callback.
    selector->syncSource = nextSyncSource;

    net->runReadyNetworkOperations();
}

TEST_F(SyncSourceResolverTest,
       SyncSourceResolverWillTryOtherSourcesWhenTheFirstNodeHasANetworkError) {
    HostAndPort candidate1("node1", 12345);
    HostAndPort candidate2("node2", 12345);
    _selector->syncSource = candidate1;

    ASSERT_OK(_resolver->startup());
    ASSERT_TRUE(_resolver->isActive());

    _scheduleNetworkErrorForFirstNode(getNet(), _selector.get(), candidate1, candidate2);

    ASSERT_TRUE(_resolver->isActive());
    ASSERT_EQUALS(candidate1, _selector->blacklistHost);
    ASSERT_EQUALS(getExecutor().now() + SyncSourceResolver::kFetcherErrorBlacklistDuration,
                  _selector->blacklistUntil);

    _scheduleFirstOplogEntryFetcherResponse(
        getNet(), _selector.get(), candidate2, HostAndPort(), Timestamp(10, 2));

    _resolver->join();
    ASSERT_FALSE(_resolver->isActive());
    ASSERT_EQUALS(candidate2, unittest::assertGet(_response.syncSourceStatus));
}

TEST_F(SyncSourceResolverTest,
       SyncSourceResolverReturnsEmptyHostWhenNoViableNodeExistsAfterNetworkErrorOnFirstNode) {
    HostAndPort candidate1("node1", 12345);
    _selector->syncSource = candidate1;

    ASSERT_OK(_resolver->startup());
    ASSERT_TRUE(_resolver->isActive());

    _scheduleNetworkErrorForFirstNode(getNet(), _selector.get(), candidate1, HostAndPort());

    ASSERT_FALSE(_resolver->isActive());
    ASSERT_EQUALS(candidate1, _selector->blacklistHost);
    ASSERT_EQUALS(getExecutor().now() + SyncSourceResolver::kFetcherErrorBlacklistDuration,
                  _selector->blacklistUntil);

    ASSERT_EQUALS(HostAndPort(), unittest::assertGet(_response.syncSourceStatus));
}

TEST_F(SyncSourceResolverTest,
       SyncSourceResolverReturnsScheduleErrorWhenTheSchedulingCommandToSecondNodeFails) {
    HostAndPort candidate1("node1", 12345);
    HostAndPort candidate2("node2", 12345);
    _selector->syncSource = candidate1;

    ASSERT_OK(_resolver->startup());
    ASSERT_TRUE(_resolver->isActive());

    _shouldFailRequest = [candidate2](const executor::RemoteCommandRequest& request) {
        return candidate2 == request.target;
    };

    _scheduleNetworkErrorForFirstNode(getNet(), _selector.get(), candidate1, candidate2);

    ASSERT_FALSE(_resolver->isActive());
    ASSERT_EQUALS(candidate1, _selector->blacklistHost);
    ASSERT_EQUALS(getExecutor().now() + SyncSourceResolver::kFetcherErrorBlacklistDuration,
                  _selector->blacklistUntil);

    ASSERT_EQUALS(ErrorCodes::OperationFailed, _response.syncSourceStatus);
}

TEST_F(SyncSourceResolverTest,
       SyncSourceResolverWillTryOtherSourcesWhenTheFirstNodeHasAnEmptyOplog) {
    HostAndPort candidate1("node1", 12345);
    HostAndPort candidate2("node1", 12345);
    _selector->syncSource = candidate1;
    ASSERT_OK(_resolver->startup());
    ASSERT_TRUE(_resolver->isActive());

    _scheduleFirstOplogEntryFetcherResponse(
        getNet(), _selector.get(), candidate1, candidate2, std::vector<BSONObj>());

    ASSERT_TRUE(_resolver->isActive());
    ASSERT_EQUALS(candidate1, _selector->blacklistHost);
    ASSERT_EQUALS(getExecutor().now() + SyncSourceResolver::kOplogEmptyBlacklistDuration,
                  _selector->blacklistUntil);

    _scheduleFirstOplogEntryFetcherResponse(
        getNet(), _selector.get(), candidate2, HostAndPort(), Timestamp(10, 2));

    _resolver->join();
    ASSERT_FALSE(_resolver->isActive());
    ASSERT_EQUALS(candidate2, unittest::assertGet(_response.syncSourceStatus));
}

TEST_F(SyncSourceResolverTest,
       SyncSourceResolverWillTryOtherSourcesWhenTheFirstNodeHasAnEmptyFirstOplogEntry) {
    HostAndPort candidate1("node1", 12345);
    HostAndPort candidate2("node1", 12345);
    _selector->syncSource = candidate1;
    ASSERT_OK(_resolver->startup());
    ASSERT_TRUE(_resolver->isActive());

    _scheduleFirstOplogEntryFetcherResponse(
        getNet(), _selector.get(), candidate1, candidate2, {BSONObj()});

    ASSERT_TRUE(_resolver->isActive());
    ASSERT_EQUALS(candidate1, _selector->blacklistHost);
    ASSERT_EQUALS(getExecutor().now() + SyncSourceResolver::kFirstOplogEntryEmptyBlacklistDuration,
                  _selector->blacklistUntil);

    _scheduleFirstOplogEntryFetcherResponse(
        getNet(), _selector.get(), candidate2, HostAndPort(), Timestamp(10, 2));

    _resolver->join();
    ASSERT_FALSE(_resolver->isActive());
    ASSERT_EQUALS(candidate2, unittest::assertGet(_response.syncSourceStatus));
}

TEST_F(SyncSourceResolverTest,
       SyncSourceResolverWillTryOtherSourcesWhenFirstNodeContainsOplogEntryWithNullTimestamp) {
    HostAndPort candidate1("node1", 12345);
    HostAndPort candidate2("node1", 12345);
    _selector->syncSource = candidate1;
    ASSERT_OK(_resolver->startup());
    ASSERT_TRUE(_resolver->isActive());

    _scheduleFirstOplogEntryFetcherResponse(
        getNet(), _selector.get(), candidate1, candidate2, Timestamp(0, 0));

    ASSERT_TRUE(_resolver->isActive());
    ASSERT_EQUALS(candidate1, _selector->blacklistHost);
    ASSERT_EQUALS(getExecutor().now() +
                      SyncSourceResolver::kFirstOplogEntryNullTimestampBlacklistDuration,
                  _selector->blacklistUntil);

    _scheduleFirstOplogEntryFetcherResponse(
        getNet(), _selector.get(), candidate2, HostAndPort(), Timestamp(10, 2));

    _resolver->join();
    ASSERT_FALSE(_resolver->isActive());
    ASSERT_EQUALS(candidate2, unittest::assertGet(_response.syncSourceStatus));
}

/**
 * Constructs and schedules a network interface response using the given documents to the required
 * optime on the sync source candidate.
 */
void _scheduleRequiredOpTimeFetcherResponse(executor::NetworkInterfaceMock* net,
                                            SyncSourceSelectorMock* selector,
                                            HostAndPort currentSyncSource,
                                            OpTime requiredOpTime,
                                            std::vector<BSONObj> docs) {
    executor::NetworkInterfaceMock::InNetworkGuard networkGuard(net);
    ASSERT_TRUE(net->hasReadyRequests());
    auto request = net->scheduleSuccessfulResponse(makeCursorResponse(0, nss, docs));
    ASSERT_EQUALS(currentSyncSource, request.target);
    ASSERT_EQUALS(SyncSourceResolver::kLocalOplogNss.db(), request.dbname);
    ASSERT_EQUALS(SyncSourceResolver::kFetcherTimeout, request.timeout);
    auto firstElement = request.cmdObj.firstElement();
    ASSERT_EQUALS("find"_sd, firstElement.fieldNameStringData());
    ASSERT_EQUALS(SyncSourceResolver::kLocalOplogNss.coll(), firstElement.String());
    ASSERT_TRUE(request.cmdObj.getBoolField("oplogReplay"));
    auto filter = request.cmdObj.getObjectField("filter");
    ASSERT_TRUE(filter.hasField("ts")) << request.cmdObj;
    auto tsFilter = filter.getObjectField("ts");
    ASSERT_TRUE(tsFilter.hasField("$gte")) << request.cmdObj;
    ASSERT_EQUALS(requiredOpTime.getTimestamp(), tsFilter["$gte"].timestamp()) << request.cmdObj;
    ASSERT_TRUE(tsFilter.hasField("$lte")) << request.cmdObj;
    ASSERT_EQUALS(requiredOpTime.getTimestamp(), tsFilter["$lte"].timestamp()) << request.cmdObj;


    net->runReadyNetworkOperations();
}

/**
 * Constructs and schedules a network interface response using the given optime to the required
 * optime on the sync source candidate.
 */
void _scheduleRequiredOpTimeFetcherResponse(executor::NetworkInterfaceMock* net,
                                            SyncSourceSelectorMock* selector,
                                            HostAndPort currentSyncSource,
                                            OpTime requiredOpTime) {
    _scheduleRequiredOpTimeFetcherResponse(
        net,
        selector,
        currentSyncSource,
        requiredOpTime,
        {BSON("ts" << requiredOpTime.getTimestamp() << "t" << requiredOpTime.getTerm())});
}

const OpTime requiredOpTime(Timestamp(200, 1U), 1LL);

TEST_F(
    SyncSourceResolverTest,
    SyncSourceResolverWillCheckForRequiredOpTimeUsingOplogReplayQueryIfRequiredOpTimeIsProvided) {
    _resolver = _makeResolver(lastOpTimeFetched, requiredOpTime);

    HostAndPort candidate1("node1", 12345);
    _selector->syncSource = candidate1;
    ASSERT_OK(_resolver->startup());
    ASSERT_TRUE(_resolver->isActive());

    _scheduleFirstOplogEntryFetcherResponse(
        getNet(), _selector.get(), candidate1, HostAndPort(), Timestamp(10, 0));

    ASSERT_TRUE(_resolver->isActive());

    _scheduleRequiredOpTimeFetcherResponse(getNet(), _selector.get(), candidate1, requiredOpTime);

    _resolver->join();
    ASSERT_FALSE(_resolver->isActive());
    ASSERT_EQUALS(candidate1, unittest::assertGet(_response.syncSourceStatus));
}

TEST_F(SyncSourceResolverTest,
       SyncSourceResolverRejectsRemoteOpTimeWhenCheckingRequiredOpTimeIfRemoteTermIsUninitialized) {
    _resolver = _makeResolver(lastOpTimeFetched, requiredOpTime);

    HostAndPort candidate1("node1", 12345);
    HostAndPort candidate2("node2", 12345);
    _selector->syncSource = candidate1;
    ASSERT_OK(_resolver->startup());
    ASSERT_TRUE(_resolver->isActive());

    _scheduleFirstOplogEntryFetcherResponse(
        getNet(), _selector.get(), candidate1, candidate2, Timestamp(10, 0));

    ASSERT_TRUE(_resolver->isActive());

    _scheduleRequiredOpTimeFetcherResponse(
        getNet(),
        _selector.get(),
        candidate1,
        requiredOpTime,
        {BSON("ts" << requiredOpTime.getTimestamp() << "t" << OpTime::kUninitializedTerm)});

    ASSERT_TRUE(_resolver->isActive());
    ASSERT_EQUALS(candidate1, _selector->blacklistHost);
    ASSERT_EQUALS(getExecutor().now() + SyncSourceResolver::kNoRequiredOpTimeBlacklistDuration,
                  _selector->blacklistUntil);

    _scheduleFirstOplogEntryFetcherResponse(
        getNet(), _selector.get(), candidate2, HostAndPort(), Timestamp(10, 0));
    _scheduleRequiredOpTimeFetcherResponse(getNet(), _selector.get(), candidate2, requiredOpTime);

    _resolver->join();
    ASSERT_FALSE(_resolver->isActive());
    ASSERT_EQUALS(candidate2, unittest::assertGet(_response.syncSourceStatus));
}

TEST_F(
    SyncSourceResolverTest,
    SyncSourceResolverRejectsRemoteOpTimeWhenCheckingRequiredOpTimeIfRequiredOpTimesTermIsUninitialized) {
    auto requireOpTimeWithUninitializedTerm =
        OpTime(requiredOpTime.getTimestamp(), OpTime::kUninitializedTerm);
    _resolver = _makeResolver(lastOpTimeFetched, requireOpTimeWithUninitializedTerm);

    HostAndPort candidate1("node1", 12345);
    HostAndPort candidate2("node2", 12345);
    _selector->syncSource = candidate1;
    ASSERT_OK(_resolver->startup());
    ASSERT_TRUE(_resolver->isActive());

    _scheduleFirstOplogEntryFetcherResponse(
        getNet(), _selector.get(), candidate1, candidate2, Timestamp(10, 0));

    ASSERT_TRUE(_resolver->isActive());

    _scheduleRequiredOpTimeFetcherResponse(getNet(), _selector.get(), candidate1, requiredOpTime);

    ASSERT_TRUE(_resolver->isActive());
    ASSERT_EQUALS(candidate1, _selector->blacklistHost);
    ASSERT_EQUALS(getExecutor().now() + SyncSourceResolver::kNoRequiredOpTimeBlacklistDuration,
                  _selector->blacklistUntil);

    _scheduleFirstOplogEntryFetcherResponse(
        getNet(), _selector.get(), candidate2, HostAndPort(), Timestamp(10, 0));
    _scheduleRequiredOpTimeFetcherResponse(
        getNet(), _selector.get(), candidate2, requireOpTimeWithUninitializedTerm);

    _resolver->join();
    ASSERT_FALSE(_resolver->isActive());
    ASSERT_EQUALS(candidate2, unittest::assertGet(_response.syncSourceStatus));
}

TEST_F(SyncSourceResolverTest,
       SyncSourceResolverWillTryOtherSourcesIfRequiredOpTimeIsNotFoundInRemoteOplog) {
    _resolver = _makeResolver(lastOpTimeFetched, requiredOpTime);

    HostAndPort candidate1("node1", 12345);
    HostAndPort candidate2("node2", 12345);
    _selector->syncSource = candidate1;
    ASSERT_OK(_resolver->startup());
    ASSERT_TRUE(_resolver->isActive());

    _scheduleFirstOplogEntryFetcherResponse(
        getNet(), _selector.get(), candidate1, candidate2, Timestamp(10, 0));

    ASSERT_TRUE(_resolver->isActive());

    _scheduleRequiredOpTimeFetcherResponse(
        getNet(), _selector.get(), candidate1, requiredOpTime, {});

    ASSERT_TRUE(_resolver->isActive());
    ASSERT_EQUALS(candidate1, _selector->blacklistHost);
    ASSERT_EQUALS(getExecutor().now() + SyncSourceResolver::kNoRequiredOpTimeBlacklistDuration,
                  _selector->blacklistUntil);

    _scheduleFirstOplogEntryFetcherResponse(
        getNet(), _selector.get(), candidate2, HostAndPort(), Timestamp(10, 0));
    _scheduleRequiredOpTimeFetcherResponse(getNet(), _selector.get(), candidate2, requiredOpTime);

    _resolver->join();
    ASSERT_FALSE(_resolver->isActive());
    ASSERT_EQUALS(candidate2, unittest::assertGet(_response.syncSourceStatus));
}

TEST_F(SyncSourceResolverTest,
       SyncSourceResolverWillTryOtherSourcesIfRequiredOpTimesTermIsNotFoundInRemoteOplog) {
    _resolver = _makeResolver(lastOpTimeFetched, requiredOpTime);

    HostAndPort candidate1("node1", 12345);
    HostAndPort candidate2("node2", 12345);
    _selector->syncSource = candidate1;
    ASSERT_OK(_resolver->startup());
    ASSERT_TRUE(_resolver->isActive());

    _scheduleFirstOplogEntryFetcherResponse(
        getNet(), _selector.get(), candidate1, candidate2, Timestamp(10, 0));

    ASSERT_TRUE(_resolver->isActive());

    _scheduleRequiredOpTimeFetcherResponse(
        getNet(),
        _selector.get(),
        candidate1,
        requiredOpTime,
        {BSON("ts" << requiredOpTime.getTimestamp() << "t" << requiredOpTime.getTerm() + 1)});

    ASSERT_TRUE(_resolver->isActive());
    ASSERT_EQUALS(candidate1, _selector->blacklistHost);
    ASSERT_EQUALS(getExecutor().now() + SyncSourceResolver::kNoRequiredOpTimeBlacklistDuration,
                  _selector->blacklistUntil);

    _scheduleFirstOplogEntryFetcherResponse(
        getNet(), _selector.get(), candidate2, HostAndPort(), Timestamp(10, 0));
    _scheduleRequiredOpTimeFetcherResponse(getNet(), _selector.get(), candidate2, requiredOpTime);

    _resolver->join();
    ASSERT_FALSE(_resolver->isActive());
    ASSERT_EQUALS(candidate2, unittest::assertGet(_response.syncSourceStatus));
}

TEST_F(SyncSourceResolverTest,
       SyncSourceResolverReturnsScheduleErrorWhenSchedulingRequiredOpTimeFindCommandFails) {
    _resolver = _makeResolver(lastOpTimeFetched, requiredOpTime);

    _shouldFailRequest = [](const executor::RemoteCommandRequest& request) {
        return request.cmdObj.getBoolField("oplogReplay");
    };

    HostAndPort candidate1("node1", 12345);
    _selector->syncSource = candidate1;
    ASSERT_OK(_resolver->startup());
    ASSERT_TRUE(_resolver->isActive());

    _scheduleFirstOplogEntryFetcherResponse(
        getNet(), _selector.get(), candidate1, HostAndPort(), Timestamp(10, 0));

    _resolver->join();
    ASSERT_FALSE(_resolver->isActive());
    ASSERT_EQUALS(ErrorCodes::OperationFailed, _response.syncSourceStatus);
}

TEST_F(
    SyncSourceResolverTest,
    SyncSourceResolverReturnsCallbackCanceledIfResolverIsShutdownAfterSchedulingRequiredOpTimeFetcher) {
    _resolver = _makeResolver(lastOpTimeFetched, requiredOpTime);

    HostAndPort candidate1("node1", 12345);
    _selector->syncSource = candidate1;
    ASSERT_OK(_resolver->startup());
    ASSERT_TRUE(_resolver->isActive());

    _scheduleFirstOplogEntryFetcherResponse(
        getNet(), _selector.get(), candidate1, HostAndPort(), Timestamp(10, 0));

    ASSERT_TRUE(_resolver->isActive());

    _resolver->shutdown();
    executor::NetworkInterfaceMock::InNetworkGuard(getNet())->runReadyNetworkOperations();

    _resolver->join();
    ASSERT_FALSE(_resolver->isActive());
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, _response.syncSourceStatus);
}

TEST_F(
    SyncSourceResolverTest,
    SyncSourceResolverReturnsCallbackCanceledIfExecutorIsShutdownAfterSchedulingRequiredOpTimeFetcher) {
    _resolver = _makeResolver(lastOpTimeFetched, requiredOpTime);

    HostAndPort candidate1("node1", 12345);
    _selector->syncSource = candidate1;
    ASSERT_OK(_resolver->startup());
    ASSERT_TRUE(_resolver->isActive());

    _scheduleFirstOplogEntryFetcherResponse(
        getNet(), _selector.get(), candidate1, HostAndPort(), Timestamp(10, 0));

    ASSERT_TRUE(_resolver->isActive());

    getExecutor().shutdown();

    _resolver->join();
    ASSERT_FALSE(_resolver->isActive());
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, _response.syncSourceStatus);
}

TEST_F(
    SyncSourceResolverTest,
    SyncSourceResolverReturnsEmptyHostIfNoViableHostExistsAfterNetworkErrorOnRequiredOpTimeCommand) {
    _resolver = _makeResolver(lastOpTimeFetched, requiredOpTime);

    HostAndPort candidate1("node1", 12345);
    _selector->syncSource = candidate1;
    ASSERT_OK(_resolver->startup());
    ASSERT_TRUE(_resolver->isActive());

    _scheduleFirstOplogEntryFetcherResponse(
        getNet(), _selector.get(), candidate1, candidate1, Timestamp(10, 0));

    _scheduleNetworkErrorForFirstNode(getNet(), _selector.get(), candidate1, HostAndPort());

    ASSERT_FALSE(_resolver->isActive());
    ASSERT_EQUALS(candidate1, _selector->blacklistHost);
    ASSERT_EQUALS(getExecutor().now() + SyncSourceResolver::kFetcherErrorBlacklistDuration,
                  _selector->blacklistUntil);

    _resolver->join();
    ASSERT_FALSE(_resolver->isActive());
    ASSERT_EQUALS(HostAndPort(), unittest::assertGet(_response.syncSourceStatus));
}

}  // namespace
