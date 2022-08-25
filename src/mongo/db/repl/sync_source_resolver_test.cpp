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

#include <functional>
#include <memory>

#include "mongo/db/cursor_id.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/sync_source_resolver.h"
#include "mongo/db/repl/sync_source_selector.h"
#include "mongo/db/repl/sync_source_selector_mock.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/unittest/task_executor_proxy.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace {

using namespace mongo;
using namespace mongo::repl;

class TaskExecutorWithFailureInScheduleRemoteCommand : public unittest::TaskExecutorProxy {
public:
    using ShouldFailRequestFn = std::function<bool(const executor::RemoteCommandRequest&)>;

    TaskExecutorWithFailureInScheduleRemoteCommand(executor::TaskExecutor* executor,
                                                   ShouldFailRequestFn shouldFailRequest)
        : unittest::TaskExecutorProxy(executor), _shouldFailRequest(shouldFailRequest) {}

    StatusWith<CallbackHandle> scheduleRemoteCommand(const executor::RemoteCommandRequest& request,
                                                     const RemoteCommandCallbackFn& cb,
                                                     const BatonHandle& baton = nullptr) override {
        if (_shouldFailRequest(request)) {
            return Status(ErrorCodes::OperationFailed, "failed to schedule remote command");
        }
        return getExecutor()->scheduleRemoteCommand(request, cb, baton);
    }

private:
    ShouldFailRequestFn _shouldFailRequest;
};

class SyncSourceResolverTest : public executor::ThreadPoolExecutorTest {
public:
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
    _executorProxy = std::make_unique<TaskExecutorWithFailureInScheduleRemoteCommand>(
        &getExecutor(), [this](const executor::RemoteCommandRequest& request) {
            return _shouldFailRequest(request);
        });

    _response.syncSourceStatus = getDetectableErrorStatus();
    _onCompletion = [this](const SyncSourceResolverResponse& response) { _response = response; };

    _selector = std::make_unique<SyncSourceSelectorMock>();
    _resolver = _makeResolver(lastOpTimeFetched, OpTime());

    launchExecutorThread();
}

void SyncSourceResolverTest::tearDown() {
    getExecutor().shutdown();
    getExecutor().join();

    _resolver.reset();
    _selector.reset();
    _executorProxy.reset();
}

std::unique_ptr<SyncSourceResolver> SyncSourceResolverTest::_makeResolver(
    const OpTime& lastOpTimeFetched, const OpTime& requiredOpTime) {
    return std::make_unique<SyncSourceResolver>(
        _executorProxy.get(),
        _selector.get(),
        lastOpTimeFetched,
        requiredOpTime,
        [this](const SyncSourceResolverResponse& response) { _onCompletion(response); });
}

const NamespaceString nss("local.oplog.rs");

const OpTime requiredOpTime(Timestamp(200, 1U), 1LL);
class SyncSourceResolverRequiredOpTimeTest : public SyncSourceResolverTest {
public:
    void setUp() override;
};

void SyncSourceResolverRequiredOpTimeTest::setUp() {
    SyncSourceResolverTest::setUp();
    // Initialize a resolver and set _requiredOpTime to null. This will prevent the sync source
    // resolver from running the RBID and requiredOpTime checks.
    _resolver = _makeResolver(lastOpTimeFetched, requiredOpTime);
}

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
        AssertionException,
        ErrorCodes::BadValue,
        "task executor cannot be null");

    // Null sync source selector.
    ASSERT_THROWS_CODE_AND_WHAT(
        SyncSourceResolver(
            &getExecutor(), nullptr, lastOpTimeFetched, requiredOpTime, onCompletion),
        AssertionException,
        ErrorCodes::BadValue,
        "sync source selector cannot be null");

    // Null last fetched optime.
    ASSERT_THROWS_CODE_AND_WHAT(
        SyncSourceResolver(&getExecutor(), &selector, OpTime(), requiredOpTime, onCompletion),
        AssertionException,
        ErrorCodes::BadValue,
        "last fetched optime cannot be null");

    // If provided, required optime must be more recent than last fetched optime.
    ASSERT_THROWS_CODE_AND_WHAT(SyncSourceResolver(&getExecutor(),
                                                   &selector,
                                                   lastOpTimeFetched,
                                                   OpTime(Timestamp(Seconds(50), 1U), 1LL),
                                                   onCompletion),
                                AssertionException,
                                ErrorCodes::BadValue,
                                "required optime (if provided) must be more recent than last "
                                "fetched optime. requiredOpTime: { ts: Timestamp(50, 1), t: 1 }, "
                                "lastOpTimeFetched: { ts: Timestamp(100, 1), t: 1 }");
    ASSERT_THROWS_CODE_AND_WHAT(
        SyncSourceResolver(
            &getExecutor(), &selector, lastOpTimeFetched, lastOpTimeFetched, onCompletion),
        AssertionException,
        ErrorCodes::BadValue,
        "required optime (if provided) must be more recent than last fetched optime. "
        "requiredOpTime: { ts: Timestamp(100, 1), t: 1 }, lastOpTimeFetched: { ts: Timestamp("
        "100, 1), t: 1 }");

    // Null callback function.
    ASSERT_THROWS_CODE_AND_WHAT(SyncSourceResolver(&getExecutor(),
                                                   &selector,
                                                   lastOpTimeFetched,
                                                   requiredOpTime,
                                                   SyncSourceResolver::OnCompletionFn()),
                                AssertionException,
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
    _selector->setChooseNewSyncSourceResult_forTest(HostAndPort("node1", 12345));
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
    _selector->setChooseNewSyncSourceResult_forTest(HostAndPort());
    ASSERT_OK(_resolver->startup());

    // Resolver invokes callback with empty host and becomes inactive immediately.
    ASSERT_FALSE(_resolver->isActive());
    ASSERT_EQUALS(HostAndPort(), unittest::assertGet(_response.syncSourceStatus));
    ASSERT_EQUALS(lastOpTimeFetched, _selector->getChooseNewSyncSourceOpTime_forTest());

    // Cannot restart a completed resolver.
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, _resolver->startup());
}

TEST_F(
    SyncSourceResolverTest,
    SyncSourceResolverReturnsCallbackCanceledIfResolverIsShutdownBeforeReturningEmptySyncSource) {
    _selector->setChooseNewSyncSourceResult_forTest(HostAndPort());
    _selector->setChooseNewSyncSourceHook_forTest([this]() { _resolver->shutdown(); });
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, _resolver->startup());
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, _response.syncSourceStatus);
}

TEST_F(SyncSourceResolverTest,
       SyncSourceResolverConvertsExceptionToStatusIfChoosingViableSyncSourceThrowsException) {
    _selector->setChooseNewSyncSourceHook_forTest(
        [this]() { uassert(ErrorCodes::InternalError, "", false); });
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
    selector->setChooseNewSyncSourceResult_forTest(nextSyncSource);

    net->runReadyNetworkOperations();
}

/**
 * Generates oplog entries with the given optime.
 */
BSONObj _makeOplogEntry(Timestamp ts, long long term) {
    return DurableOplogEntry(OpTime(ts, term),                 // optime
                             OpTypeEnum::kNoop,                // op type
                             NamespaceString("a.a"),           // namespace
                             boost::none,                      // uuid
                             boost::none,                      // fromMigrate
                             repl::OplogEntry::kOplogVersion,  // version
                             BSONObj(),                        // o
                             boost::none,                      // o2
                             {},                               // sessionInfo
                             boost::none,                      // upsert
                             Date_t(),                         // wall clock time
                             {},                               // statement ids
                             boost::none,  // optime of previous write within same transaction
                             boost::none,  // pre-image optime
                             boost::none,  // post-image optime
                             boost::none,  // ShardId of resharding recipient
                             boost::none,  // _id
                             boost::none)  // needsRetryImage
        .toBSON();
}

void _scheduleFirstOplogEntryFetcherResponse(executor::NetworkInterfaceMock* net,
                                             SyncSourceSelectorMock* selector,
                                             HostAndPort currentSyncSource,
                                             HostAndPort nextSyncSource,
                                             Timestamp ts) {
    _scheduleFirstOplogEntryFetcherResponse(
        net, selector, currentSyncSource, nextSyncSource, {_makeOplogEntry(ts, 0LL)});
}

void _scheduleRBIDResponse(executor::NetworkInterfaceMock* net,
                           HostAndPort currentSyncSource,
                           const BSONObj& reply = BSON("ok" << 1 << "rbid" << 1)) {
    executor::NetworkInterfaceMock::InNetworkGuard networkGuard(net);
    ASSERT_TRUE(net->hasReadyRequests());
    auto request = net->scheduleSuccessfulResponse(reply);
    ASSERT_EQUALS(currentSyncSource, request.target);
    ASSERT_EQUALS("admin", request.dbname);
    ASSERT_EQUALS(SyncSourceResolver::kFetcherTimeout, request.timeout);
    ASSERT_BSONOBJ_EQ(BSON("replSetGetRBID" << 1), request.cmdObj);
    net->runReadyNetworkOperations();
}

TEST_F(SyncSourceResolverTest,
       SyncSourceResolverReturnsStatusOkAndTheFoundHostWhenAnEligibleSyncSourceExists) {
    HostAndPort candidate1("node1", 12345);
    _selector->setChooseNewSyncSourceResult_forTest(candidate1);
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
    _selector->setChooseNewSyncSourceResult_forTest(candidate1);
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
    _selector->setChooseNewSyncSourceResult_forTest(candidate1);

    ASSERT_OK(_resolver->startup());
    ASSERT_TRUE(_resolver->isActive());

    _scheduleFirstOplogEntryFetcherResponse(
        getNet(), _selector.get(), candidate1, candidate2, Timestamp(200, 2));

    ASSERT_TRUE(_resolver->isActive());
    ASSERT_EQUALS(candidate1, _selector->getLastDenylistedSyncSource_forTest());
    ASSERT_EQUALS(getExecutor().now() + SyncSourceResolver::kTooStaleDenylistDuration,
                  _selector->getLastDenylistExpiration_forTest());

    _scheduleFirstOplogEntryFetcherResponse(
        getNet(), _selector.get(), candidate2, HostAndPort(), Timestamp(10, 2));

    _resolver->join();
    ASSERT_FALSE(_resolver->isActive());
    ASSERT_EQUALS(candidate2, unittest::assertGet(_response.syncSourceStatus));
}

TEST_F(SyncSourceResolverTest,
       SyncSourceResolverReturnsTooStaleAndEarliestOpTimeAvailableWhenAllSourcesTooFresh) {
    HostAndPort candidate1("node1", 12345);
    HostAndPort candidate2("node2", 12345);
    HostAndPort candidate3("node3", 12345);
    _selector->setChooseNewSyncSourceResult_forTest(candidate1);

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
    ASSERT_EQUALS(ErrorCodes::TooStaleToSyncFromSource, _response.syncSourceStatus);
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
    selector->setChooseNewSyncSourceResult_forTest(nextSyncSource);

    net->runReadyNetworkOperations();
}

TEST_F(SyncSourceResolverTest,
       SyncSourceResolverWillTryOtherSourcesWhenTheFirstNodeHasANetworkError) {
    HostAndPort candidate1("node1", 12345);
    HostAndPort candidate2("node2", 12345);
    _selector->setChooseNewSyncSourceResult_forTest(candidate1);

    ASSERT_OK(_resolver->startup());
    ASSERT_TRUE(_resolver->isActive());

    _scheduleNetworkErrorForFirstNode(getNet(), _selector.get(), candidate1, candidate2);

    ASSERT_TRUE(_resolver->isActive());
    ASSERT_EQUALS(candidate1, _selector->getLastDenylistedSyncSource_forTest());
    ASSERT_EQUALS(getExecutor().now() + SyncSourceResolver::kFetcherErrorDenylistDuration,
                  _selector->getLastDenylistExpiration_forTest());

    _scheduleFirstOplogEntryFetcherResponse(
        getNet(), _selector.get(), candidate2, HostAndPort(), Timestamp(10, 2));

    _resolver->join();
    ASSERT_FALSE(_resolver->isActive());
    ASSERT_EQUALS(candidate2, unittest::assertGet(_response.syncSourceStatus));
}

TEST_F(SyncSourceResolverTest,
       SyncSourceResolverReturnsEmptyHostWhenNoViableNodeExistsAfterNetworkErrorOnFirstNode) {
    HostAndPort candidate1("node1", 12345);
    _selector->setChooseNewSyncSourceResult_forTest(candidate1);

    ASSERT_OK(_resolver->startup());
    ASSERT_TRUE(_resolver->isActive());

    _scheduleNetworkErrorForFirstNode(getNet(), _selector.get(), candidate1, HostAndPort());

    ASSERT_FALSE(_resolver->isActive());
    ASSERT_EQUALS(candidate1, _selector->getLastDenylistedSyncSource_forTest());
    ASSERT_EQUALS(getExecutor().now() + SyncSourceResolver::kFetcherErrorDenylistDuration,
                  _selector->getLastDenylistExpiration_forTest());

    ASSERT_EQUALS(HostAndPort(), unittest::assertGet(_response.syncSourceStatus));
}

TEST_F(SyncSourceResolverTest,
       SyncSourceResolverReturnsScheduleErrorWhenTheSchedulingCommandToSecondNodeFails) {
    HostAndPort candidate1("node1", 12345);
    HostAndPort candidate2("node2", 12345);
    _selector->setChooseNewSyncSourceResult_forTest(candidate1);

    ASSERT_OK(_resolver->startup());
    ASSERT_TRUE(_resolver->isActive());

    _shouldFailRequest = [candidate2](const executor::RemoteCommandRequest& request) {
        return candidate2 == request.target;
    };

    _scheduleNetworkErrorForFirstNode(getNet(), _selector.get(), candidate1, candidate2);

    ASSERT_FALSE(_resolver->isActive());
    ASSERT_EQUALS(candidate1, _selector->getLastDenylistedSyncSource_forTest());
    ASSERT_EQUALS(getExecutor().now() + SyncSourceResolver::kFetcherErrorDenylistDuration,
                  _selector->getLastDenylistExpiration_forTest());

    ASSERT_EQUALS(ErrorCodes::OperationFailed, _response.syncSourceStatus);
}

TEST_F(SyncSourceResolverTest,
       SyncSourceResolverWillTryOtherSourcesWhenTheFirstNodeHasAnEmptyOplog) {
    HostAndPort candidate1("node1", 12345);
    HostAndPort candidate2("node2", 12345);
    _selector->setChooseNewSyncSourceResult_forTest(candidate1);
    ASSERT_OK(_resolver->startup());
    ASSERT_TRUE(_resolver->isActive());

    _scheduleFirstOplogEntryFetcherResponse(
        getNet(), _selector.get(), candidate1, candidate2, std::vector<BSONObj>());

    ASSERT_TRUE(_resolver->isActive());
    ASSERT_EQUALS(candidate1, _selector->getLastDenylistedSyncSource_forTest());
    ASSERT_EQUALS(getExecutor().now() + SyncSourceResolver::kOplogEmptyDenylistDuration,
                  _selector->getLastDenylistExpiration_forTest());

    _scheduleFirstOplogEntryFetcherResponse(
        getNet(), _selector.get(), candidate2, HostAndPort(), Timestamp(10, 2));

    _resolver->join();
    ASSERT_FALSE(_resolver->isActive());
    ASSERT_EQUALS(candidate2, unittest::assertGet(_response.syncSourceStatus));
}

TEST_F(SyncSourceResolverTest,
       SyncSourceResolverWillTryOtherSourcesWhenTheFirstNodeHasAnEmptyFirstOplogEntry) {
    HostAndPort candidate1("node1", 12345);
    HostAndPort candidate2("node2", 12345);
    _selector->setChooseNewSyncSourceResult_forTest(candidate1);
    ASSERT_OK(_resolver->startup());
    ASSERT_TRUE(_resolver->isActive());

    _scheduleFirstOplogEntryFetcherResponse(
        getNet(), _selector.get(), candidate1, candidate2, {BSONObj()});

    ASSERT_TRUE(_resolver->isActive());
    ASSERT_EQUALS(candidate1, _selector->getLastDenylistedSyncSource_forTest());
    ASSERT_EQUALS(getExecutor().now() + SyncSourceResolver::kFirstOplogEntryEmptyDenylistDuration,
                  _selector->getLastDenylistExpiration_forTest());

    _scheduleFirstOplogEntryFetcherResponse(
        getNet(), _selector.get(), candidate2, HostAndPort(), Timestamp(10, 2));

    _resolver->join();
    ASSERT_FALSE(_resolver->isActive());
    ASSERT_EQUALS(candidate2, unittest::assertGet(_response.syncSourceStatus));
}

TEST_F(SyncSourceResolverTest,
       SyncSourceResolverWillTryOtherSourcesWhenFirstNodeContainsBadOplogEntry) {
    HostAndPort candidate1("node1", 12345);
    HostAndPort candidate2("node2", 12345);
    _selector->setChooseNewSyncSourceResult_forTest(candidate1);
    ASSERT_OK(_resolver->startup());
    ASSERT_TRUE(_resolver->isActive());

    _scheduleFirstOplogEntryFetcherResponse(
        getNet(), _selector.get(), candidate1, candidate2, {BSON("t" << 1LL)});

    ASSERT_TRUE(_resolver->isActive());
    ASSERT_EQUALS(candidate1, _selector->getLastDenylistedSyncSource_forTest());
    ASSERT_EQUALS(getExecutor().now() +
                      SyncSourceResolver::kFirstOplogEntryNullTimestampDenylistDuration,
                  _selector->getLastDenylistExpiration_forTest());

    _scheduleFirstOplogEntryFetcherResponse(
        getNet(), _selector.get(), candidate2, HostAndPort(), Timestamp(10, 2));

    _resolver->join();
    ASSERT_FALSE(_resolver->isActive());
    ASSERT_EQUALS(candidate2, unittest::assertGet(_response.syncSourceStatus));
}

TEST_F(SyncSourceResolverTest,
       SyncSourceResolverWillTryOtherSourcesWhenFirstNodeContainsOplogEntryWithNullTimestamp) {
    HostAndPort candidate1("node1", 12345);
    HostAndPort candidate2("node2", 12345);
    _selector->setChooseNewSyncSourceResult_forTest(candidate1);
    ASSERT_OK(_resolver->startup());
    ASSERT_TRUE(_resolver->isActive());

    _scheduleFirstOplogEntryFetcherResponse(
        getNet(), _selector.get(), candidate1, candidate2, Timestamp(0, 0));

    ASSERT_TRUE(_resolver->isActive());
    ASSERT_EQUALS(candidate1, _selector->getLastDenylistedSyncSource_forTest());
    ASSERT_EQUALS(getExecutor().now() +
                      SyncSourceResolver::kFirstOplogEntryNullTimestampDenylistDuration,
                  _selector->getLastDenylistExpiration_forTest());

    _scheduleFirstOplogEntryFetcherResponse(
        getNet(), _selector.get(), candidate2, HostAndPort(), Timestamp(10, 2));

    _resolver->join();
    ASSERT_FALSE(_resolver->isActive());
    ASSERT_EQUALS(candidate2, unittest::assertGet(_response.syncSourceStatus));
}


TEST_F(SyncSourceResolverTest, SyncSourceResolverWillSucceedWithExtraFields) {
    HostAndPort candidate1("node1", 12345);
    _selector->setChooseNewSyncSourceResult_forTest(candidate1);
    ASSERT_OK(_resolver->startup());
    ASSERT_TRUE(_resolver->isActive());

    _scheduleFirstOplogEntryFetcherResponse(getNet(),
                                            _selector.get(),
                                            candidate1,
                                            HostAndPort(),
                                            {BSON("ts" << Timestamp(1, 1) << "t" << 1LL << "note"
                                                       << "a")});

    _resolver->join();
    ASSERT_FALSE(_resolver->isActive());
    ASSERT_EQUALS(candidate1, unittest::assertGet(_response.syncSourceStatus));
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
    auto filter = request.cmdObj.getObjectField("filter");
    ASSERT_TRUE(filter.hasField("ts")) << request.cmdObj;
    auto tsFilter = filter.getObjectField("ts");
    ASSERT_TRUE(tsFilter.hasField("$gte")) << request.cmdObj;
    ASSERT_EQUALS(requiredOpTime.getTimestamp(), tsFilter["$gte"].timestamp()) << request.cmdObj;
    ASSERT_TRUE(tsFilter.hasField("$lte")) << request.cmdObj;
    ASSERT_EQUALS(requiredOpTime.getTimestamp(), tsFilter["$lte"].timestamp()) << request.cmdObj;

    // The find command executed against the sync source should not specify the deprecated
    // 'oplogReplay' flag.
    ASSERT_FALSE(request.cmdObj["oplogReplay"]);

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
        {_makeOplogEntry(requiredOpTime.getTimestamp(), requiredOpTime.getTerm())});
}

TEST_F(SyncSourceResolverRequiredOpTimeTest,
       SyncSourceResolverWillCheckForRequiredOpTimeIfRequiredOpTimeIsProvided) {
    HostAndPort candidate1("node1", 12345);
    _selector->setChooseNewSyncSourceResult_forTest(candidate1);
    ASSERT_OK(_resolver->startup());
    ASSERT_TRUE(_resolver->isActive());

    _scheduleFirstOplogEntryFetcherResponse(
        getNet(), _selector.get(), candidate1, HostAndPort(), Timestamp(10, 0));

    ASSERT_TRUE(_resolver->isActive());

    _scheduleRBIDResponse(getNet(), candidate1, BSON("ok" << 1 << "rbid" << 7));
    _scheduleRequiredOpTimeFetcherResponse(getNet(), _selector.get(), candidate1, requiredOpTime);

    _resolver->join();
    ASSERT_FALSE(_resolver->isActive());
    ASSERT_EQUALS(candidate1, unittest::assertGet(_response.syncSourceStatus));
    ASSERT(_response.rbid);
    ASSERT_EQ(_response.rbid, 7);
}

TEST_F(SyncSourceResolverRequiredOpTimeTest,
       SyncSourceResolverRejectsRemoteOpTimeWhenCheckingRequiredOpTimeIfRemoteTermIsUninitialized) {
    HostAndPort candidate1("node1", 12345);
    HostAndPort candidate2("node2", 12345);
    _selector->setChooseNewSyncSourceResult_forTest(candidate1);
    ASSERT_OK(_resolver->startup());
    ASSERT_TRUE(_resolver->isActive());

    _scheduleFirstOplogEntryFetcherResponse(
        getNet(), _selector.get(), candidate1, candidate2, Timestamp(10, 0));

    ASSERT_TRUE(_resolver->isActive());

    _scheduleRBIDResponse(getNet(), candidate1);
    _scheduleRequiredOpTimeFetcherResponse(
        getNet(),
        _selector.get(),
        candidate1,
        requiredOpTime,
        {_makeOplogEntry(requiredOpTime.getTimestamp(), OpTime::kUninitializedTerm)});

    ASSERT_TRUE(_resolver->isActive());
    ASSERT_EQUALS(candidate1, _selector->getLastDenylistedSyncSource_forTest());
    ASSERT_EQUALS(getExecutor().now() + SyncSourceResolver::kNoRequiredOpTimeDenylistDuration,
                  _selector->getLastDenylistExpiration_forTest());

    _scheduleFirstOplogEntryFetcherResponse(
        getNet(), _selector.get(), candidate2, HostAndPort(), Timestamp(10, 0));
    _scheduleRBIDResponse(getNet(), candidate2);
    _scheduleRequiredOpTimeFetcherResponse(getNet(), _selector.get(), candidate2, requiredOpTime);

    _resolver->join();
    ASSERT_FALSE(_resolver->isActive());
    ASSERT_EQUALS(candidate2, unittest::assertGet(_response.syncSourceStatus));
}

TEST_F(
    SyncSourceResolverRequiredOpTimeTest,
    SyncSourceResolverRejectsRemoteOpTimeWhenCheckingRequiredOpTimeIfRequiredOpTimesTermIsUninitialized) {
    auto requireOpTimeWithUninitializedTerm =
        OpTime(requiredOpTime.getTimestamp(), OpTime::kUninitializedTerm);
    _resolver = _makeResolver(lastOpTimeFetched, requireOpTimeWithUninitializedTerm);

    HostAndPort candidate1("node1", 12345);
    HostAndPort candidate2("node2", 12345);
    _selector->setChooseNewSyncSourceResult_forTest(candidate1);
    ASSERT_OK(_resolver->startup());
    ASSERT_TRUE(_resolver->isActive());

    _scheduleFirstOplogEntryFetcherResponse(
        getNet(), _selector.get(), candidate1, candidate2, Timestamp(10, 0));

    ASSERT_TRUE(_resolver->isActive());

    _scheduleRBIDResponse(getNet(), candidate1);
    _scheduleRequiredOpTimeFetcherResponse(getNet(), _selector.get(), candidate1, requiredOpTime);

    ASSERT_TRUE(_resolver->isActive());
    ASSERT_EQUALS(candidate1, _selector->getLastDenylistedSyncSource_forTest());
    ASSERT_EQUALS(getExecutor().now() + SyncSourceResolver::kNoRequiredOpTimeDenylistDuration,
                  _selector->getLastDenylistExpiration_forTest());

    _scheduleFirstOplogEntryFetcherResponse(
        getNet(), _selector.get(), candidate2, HostAndPort(), Timestamp(10, 0));
    _scheduleRBIDResponse(getNet(), candidate2);
    _scheduleRequiredOpTimeFetcherResponse(
        getNet(), _selector.get(), candidate2, requireOpTimeWithUninitializedTerm);

    _resolver->join();
    ASSERT_FALSE(_resolver->isActive());
    ASSERT_EQUALS(candidate2, unittest::assertGet(_response.syncSourceStatus));
}

TEST_F(SyncSourceResolverRequiredOpTimeTest,
       SyncSourceResolverWillTryOtherSourcesIfRequiredOpTimeIsNotFoundInRemoteOplog) {
    HostAndPort candidate1("node1", 12345);
    HostAndPort candidate2("node2", 12345);
    _selector->setChooseNewSyncSourceResult_forTest(candidate1);
    ASSERT_OK(_resolver->startup());
    ASSERT_TRUE(_resolver->isActive());

    _scheduleFirstOplogEntryFetcherResponse(
        getNet(), _selector.get(), candidate1, candidate2, Timestamp(10, 0));

    ASSERT_TRUE(_resolver->isActive());

    _scheduleRBIDResponse(getNet(), candidate1);
    _scheduleRequiredOpTimeFetcherResponse(
        getNet(), _selector.get(), candidate1, requiredOpTime, {});

    ASSERT_TRUE(_resolver->isActive());
    ASSERT_EQUALS(candidate1, _selector->getLastDenylistedSyncSource_forTest());
    ASSERT_EQUALS(getExecutor().now() + SyncSourceResolver::kNoRequiredOpTimeDenylistDuration,
                  _selector->getLastDenylistExpiration_forTest());

    _scheduleFirstOplogEntryFetcherResponse(
        getNet(), _selector.get(), candidate2, HostAndPort(), Timestamp(10, 0));
    _scheduleRBIDResponse(getNet(), candidate2);
    _scheduleRequiredOpTimeFetcherResponse(getNet(), _selector.get(), candidate2, requiredOpTime);

    _resolver->join();
    ASSERT_FALSE(_resolver->isActive());
    ASSERT_EQUALS(candidate2, unittest::assertGet(_response.syncSourceStatus));
}

TEST_F(SyncSourceResolverRequiredOpTimeTest,
       SyncSourceResolverWillTryOtherSourcesIfRequiredOpTimesTermIsNotFoundInRemoteOplog) {
    HostAndPort candidate1("node1", 12345);
    HostAndPort candidate2("node2", 12345);
    _selector->setChooseNewSyncSourceResult_forTest(candidate1);
    ASSERT_OK(_resolver->startup());
    ASSERT_TRUE(_resolver->isActive());

    _scheduleFirstOplogEntryFetcherResponse(
        getNet(), _selector.get(), candidate1, candidate2, Timestamp(10, 0));

    ASSERT_TRUE(_resolver->isActive());

    _scheduleRBIDResponse(getNet(), candidate1);
    _scheduleRequiredOpTimeFetcherResponse(
        getNet(),
        _selector.get(),
        candidate1,
        requiredOpTime,
        {_makeOplogEntry(requiredOpTime.getTimestamp(), requiredOpTime.getTerm() + 1)});

    ASSERT_TRUE(_resolver->isActive());
    ASSERT_EQUALS(candidate1, _selector->getLastDenylistedSyncSource_forTest());
    ASSERT_EQUALS(getExecutor().now() + SyncSourceResolver::kNoRequiredOpTimeDenylistDuration,
                  _selector->getLastDenylistExpiration_forTest());

    _scheduleFirstOplogEntryFetcherResponse(
        getNet(), _selector.get(), candidate2, HostAndPort(), Timestamp(10, 0));
    _scheduleRBIDResponse(getNet(), candidate2);
    _scheduleRequiredOpTimeFetcherResponse(getNet(), _selector.get(), candidate2, requiredOpTime);

    _resolver->join();
    ASSERT_FALSE(_resolver->isActive());
    ASSERT_EQUALS(candidate2, unittest::assertGet(_response.syncSourceStatus));
}

TEST_F(SyncSourceResolverRequiredOpTimeTest,
       SyncSourceResolverReturnsScheduleErrorWhenSchedulingRBIDCommandFails) {
    _shouldFailRequest = [](const executor::RemoteCommandRequest& request) {
        return request.cmdObj.firstElementFieldName() == "replSetGetRBID"_sd;
    };

    HostAndPort candidate1("node1", 12345);
    _selector->setChooseNewSyncSourceResult_forTest(candidate1);
    ASSERT_OK(_resolver->startup());
    ASSERT_TRUE(_resolver->isActive());

    _scheduleFirstOplogEntryFetcherResponse(
        getNet(), _selector.get(), candidate1, HostAndPort(), Timestamp(10, 0));

    _resolver->join();
    ASSERT_FALSE(_resolver->isActive());
    ASSERT_EQUALS(ErrorCodes::OperationFailed, _response.syncSourceStatus);
}

TEST_F(SyncSourceResolverRequiredOpTimeTest,
       SyncSourceResolverReturnsScheduleErrorWhenSchedulingRequiredOpTimeFindCommandFails) {
    _shouldFailRequest = [](const executor::RemoteCommandRequest& request) {
        // Fail find commands reading the oplog with filter containing a "ts" predicate.
        if (StringData{request.cmdObj.getStringField("find")} !=
            SyncSourceResolver::kLocalOplogNss.coll()) {
            return false;
        }

        auto filterElt = request.cmdObj["filter"];
        if (!filterElt || filterElt.type() != BSONType::Object) {
            return false;
        }

        // Fail the request if the filter has a predicate over the "ts" field.
        return static_cast<bool>(filterElt.embeddedObject()["ts"]);
    };

    HostAndPort candidate1("node1", 12345);
    _selector->setChooseNewSyncSourceResult_forTest(candidate1);
    ASSERT_OK(_resolver->startup());
    ASSERT_TRUE(_resolver->isActive());

    _scheduleFirstOplogEntryFetcherResponse(
        getNet(), _selector.get(), candidate1, HostAndPort(), Timestamp(10, 0));
    _scheduleRBIDResponse(getNet(), candidate1);

    _resolver->join();
    ASSERT_FALSE(_resolver->isActive());
    ASSERT_EQUALS(ErrorCodes::OperationFailed, _response.syncSourceStatus);
}

TEST_F(
    SyncSourceResolverRequiredOpTimeTest,
    SyncSourceResolverReturnsCallbackCanceledIfResolverIsShutdownAfterSchedulingRequiredOpTimeFetcher) {
    HostAndPort candidate1("node1", 12345);
    _selector->setChooseNewSyncSourceResult_forTest(candidate1);
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
    SyncSourceResolverRequiredOpTimeTest,
    SyncSourceResolverReturnsCallbackCanceledIfExecutorIsShutdownAfterSchedulingRequiredOpTimeFetcher) {
    HostAndPort candidate1("node1", 12345);
    _selector->setChooseNewSyncSourceResult_forTest(candidate1);
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

TEST_F(SyncSourceResolverRequiredOpTimeTest,
       SyncSourceResolverReturnsEmptyHostIfNoViableHostExistsAfterNetworkErrorOnRBIDCommand) {
    HostAndPort candidate1("node1", 12345);
    _selector->setChooseNewSyncSourceResult_forTest(candidate1);
    ASSERT_OK(_resolver->startup());
    ASSERT_TRUE(_resolver->isActive());

    _scheduleFirstOplogEntryFetcherResponse(
        getNet(), _selector.get(), candidate1, candidate1, Timestamp(10, 0));

    _scheduleNetworkErrorForFirstNode(getNet(), _selector.get(), candidate1, HostAndPort());

    ASSERT_FALSE(_resolver->isActive());
    ASSERT_EQUALS(candidate1, _selector->getLastDenylistedSyncSource_forTest());
    ASSERT_EQUALS(getExecutor().now() + SyncSourceResolver::kFetcherErrorDenylistDuration,
                  _selector->getLastDenylistExpiration_forTest());

    _resolver->join();
    ASSERT_FALSE(_resolver->isActive());
    ASSERT_EQUALS(HostAndPort(), unittest::assertGet(_response.syncSourceStatus));
}

TEST_F(SyncSourceResolverRequiredOpTimeTest,
       SyncSourceResolverReturnsEmptyHostIfNoViableHostExistsAfterRBIDCommandNotOk) {
    HostAndPort candidate1("node1", 12345);
    _selector->setChooseNewSyncSourceResult_forTest(candidate1);
    ASSERT_OK(_resolver->startup());
    ASSERT_TRUE(_resolver->isActive());

    _scheduleFirstOplogEntryFetcherResponse(
        getNet(), _selector.get(), candidate1, candidate1, Timestamp(10, 0));

    // _scheduleNetworkErrorForFirstNode does this for us.
    _selector->setChooseNewSyncSourceResult_forTest(HostAndPort());

    _scheduleRBIDResponse(getNet(),
                          candidate1,
                          BSON("ok" << 0 << "code" << 9000 << "errmsg"
                                    << "I'm sorry Dave, I'm afraid I can't do that."));

    ASSERT_FALSE(_resolver->isActive());
    ASSERT_EQUALS(candidate1, _selector->getLastDenylistedSyncSource_forTest());
    ASSERT_EQUALS(getExecutor().now() + SyncSourceResolver::kFetcherErrorDenylistDuration,
                  _selector->getLastDenylistExpiration_forTest());

    _resolver->join();
    ASSERT_FALSE(_resolver->isActive());
    ASSERT_EQUALS(HostAndPort(), unittest::assertGet(_response.syncSourceStatus));
}

TEST_F(
    SyncSourceResolverRequiredOpTimeTest,
    SyncSourceResolverReturnsEmptyHostIfNoViableHostExistsAfterNetworkErrorOnRequiredOpTimeCommand) {
    HostAndPort candidate1("node1", 12345);
    _selector->setChooseNewSyncSourceResult_forTest(candidate1);
    ASSERT_OK(_resolver->startup());
    ASSERT_TRUE(_resolver->isActive());

    _scheduleFirstOplogEntryFetcherResponse(
        getNet(), _selector.get(), candidate1, candidate1, Timestamp(10, 0));

    _scheduleRBIDResponse(getNet(), candidate1);
    _scheduleNetworkErrorForFirstNode(getNet(), _selector.get(), candidate1, HostAndPort());

    ASSERT_FALSE(_resolver->isActive());
    ASSERT_EQUALS(candidate1, _selector->getLastDenylistedSyncSource_forTest());
    ASSERT_EQUALS(getExecutor().now() + SyncSourceResolver::kFetcherErrorDenylistDuration,
                  _selector->getLastDenylistExpiration_forTest());

    _resolver->join();
    ASSERT_FALSE(_resolver->isActive());
    ASSERT_EQUALS(HostAndPort(), unittest::assertGet(_response.syncSourceStatus));
}

}  // namespace
