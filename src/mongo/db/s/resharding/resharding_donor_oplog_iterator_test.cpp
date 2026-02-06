/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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


#include "mongo/db/s/resharding/resharding_donor_oplog_iterator.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/client.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface_factory.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/s/resharding/resharding_donor_oplog_pipeline.h"
#include "mongo/db/s/resharding/resharding_noop_o2_field_gen.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/thread_pool_mock.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/uuid.h"

#include <cstddef>
#include <functional>
#include <string>
#include <tuple>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace {

const ReshardingDonorOplogId kResumeFromBeginning{Timestamp::min(), Timestamp::min()};

repl::MutableOplogEntry makeOplog(const NamespaceString& nss,
                                  const UUID& uuid,
                                  const repl::OpTypeEnum& opType,
                                  const BSONObj& oField,
                                  const BSONObj& o2Field,
                                  const ReshardingDonorOplogId& oplogId) {
    repl::MutableOplogEntry oplogEntry;
    oplogEntry.setNss(nss);
    oplogEntry.setUuid(uuid);
    oplogEntry.setOpType(opType);
    oplogEntry.setObject(oField);

    if (!o2Field.isEmpty()) {
        oplogEntry.setObject2(o2Field);
    }

    oplogEntry.setOpTime({{}, {}});
    oplogEntry.setWallClockTime({});
    oplogEntry.set_id(Value(oplogId.toBSON()));

    return oplogEntry;
}

class OnInsertAlwaysReady : public resharding::OnInsertAwaitable {
public:
    Future<void> awaitInsert(const ReshardingDonorOplogId& lastSeen) override {
        return Future<void>::makeReady();
    }
} onInsertAlwaysReady;

template <typename T>
struct MockResult {
    explicit MockResult(StatusWith<T> result) : result(std::move(result)) {}

    StatusWith<T> result;
    std::function<void()> beforeReturnResult = [] {
    };
};

class MockReshardingDonorOplogPipeline : public ReshardingDonorOplogPipelineInterface {
public:
    MockReshardingDonorOplogPipeline(
        std::vector<MockResult<std::vector<repl::OplogEntry>>> resultSequence)
        : _resultSequence(std::move(resultSequence)) {}

    ScopedPipeline initWithOperationContext(OperationContext* opCtx,
                                            ReshardingDonorOplogId resumeToken) override {
        _lastResumeToken = resumeToken;
        return ScopedPipeline(opCtx, this);
    }

    void dispose(OperationContext* opCtx) override {}

    ReshardingDonorOplogId getLastResumeTokenPassedToInit() {
        return _lastResumeToken;
    }

protected:
    std::vector<repl::OplogEntry> _getNextBatch(size_t batchSize) override {
        if (_resultSequence.empty()) {
            uasserted(11399600,
                      "No more results available. Make sure to include final oplog entry when "
                      "creating mock if you need to exhaust the results.");
        }

        auto mockResult = _resultSequence.front();
        _resultSequence.erase(_resultSequence.begin());
        mockResult.beforeReturnResult();

        uassertStatusOK(mockResult.result);
        return std::move(mockResult.result.getValue());
    }

    void _detachFromOperationContext() override {}

private:
    std::vector<MockResult<std::vector<repl::OplogEntry>>> _resultSequence;

    ReshardingDonorOplogId _lastResumeToken;
};

repl::OplogEntry toOplogEntry(const repl::MutableOplogEntry& oplog) {
    return repl::OplogEntry(oplog.toBSON());
}

class ReshardingDonorOplogIterTest : public ShardServerTestFixture {
public:
    repl::MutableOplogEntry makeInsertOplog(ReshardingDonorOplogId oplogId, BSONObj doc) {
        return makeOplog(_crudNss, _uuid, repl::OpTypeEnum::kInsert, doc, {}, oplogId);
    }

    repl::MutableOplogEntry makeInsertOplog(Timestamp ts, BSONObj doc) {
        ReshardingDonorOplogId oplogId(ts, ts);
        return makeInsertOplog(oplogId, doc);
    }

    repl::MutableOplogEntry makeFinalOplog(Timestamp ts) {
        ReshardingDonorOplogId oplogId(ts, ts);
        const BSONObj oField(BSON("msg" << "Created temporary resharding collection"));
        const BSONObj o2Field(
            BSON("type" << resharding::kReshardFinalOpLogType << "reshardingUUID" << UUID::gen()));
        return makeOplog(_crudNss, _uuid, repl::OpTypeEnum::kNoop, oField, o2Field, oplogId);
    }

    repl::MutableOplogEntry makeProgressMarkOplogEntry(Timestamp ts) {
        ReshardingDonorOplogId oplogId(ts, ts);
        const BSONObj oField(BSON("msg" << "Latest oplog ts from donor's cursor response"));
        ReshardProgressMarkO2Field o2Field;
        o2Field.setType(resharding::kReshardProgressMarkOpLogType);
        return makeOplog(
            _crudNss, _uuid, repl::OpTypeEnum::kNoop, oField, o2Field.toBSON(), oplogId);
    }

    const NamespaceString& oplogNss() const {
        return _oplogNss;
    }

    BSONObj getId(const repl::MutableOplogEntry& oplog) {
        return oplog.get_id()->getDocument().toBson();
    }

    BSONObj getId(const repl::DurableOplogEntry& oplog) {
        return oplog.get_id()->getDocument().toBson();
    }

    BSONObj getId(const repl::OplogEntry& oplog) {
        return oplog.get_id()->getDocument().toBson();
    }

    std::shared_ptr<executor::ThreadPoolTaskExecutor> makeTaskExecutorForIterator() {
        // The ReshardingDonorOplogIterator expects there to already be a Client associated with the
        // thread from the thread pool. We set up the ThreadPoolTaskExecutor similarly to how the
        // recipient's primary-only service is set up.
        executor::ThreadPoolMock::Options threadPoolOptions;
        threadPoolOptions.onCreateThread = [] {
            Client::initThread("TestReshardingDonorOplogIterator",
                               getGlobalServiceContext()->getService());
        };

        auto executor = executor::makeThreadPoolTestExecutor(
            std::make_unique<executor::NetworkInterfaceMock>(), std::move(threadPoolOptions));

        executor->startup();
        return executor;
    }

    CancelableOperationContextFactory makeCancelableOpCtx() {
        auto cancelableOpCtxExecutor = std::make_shared<ThreadPool>([] {
            ThreadPool::Options options;
            options.poolName = "TestReshardOplogFetcherCancelableOpCtxPool";
            options.minThreads = 1;
            options.maxThreads = 1;
            return options;
        }());

        return CancelableOperationContextFactory(operationContext()->getCancellationToken(),
                                                 cancelableOpCtxExecutor);
    }

    std::unique_ptr<ReshardingDonorOplogPipeline> makePipeline() {
        return std::make_unique<ReshardingDonorOplogPipeline>(
            oplogNss(), std::make_unique<MongoProcessInterfaceFactoryImpl>());
    }

    auto getNextBatch(ReshardingDonorOplogIterator* iter,
                      std::shared_ptr<executor::TaskExecutor> executor,
                      CancelableOperationContextFactory factory,
                      CancellationToken cancelToken) {
        // There isn't a guarantee that the reference count to `executor` has been decremented after
        // .get() returns. We schedule a trivial task on the task executor to ensure the callback's
        // destructor has run. Otherwise `executor` could end up outliving the ServiceContext and
        // triggering an invariant due to the task executor's thread having a Client still.
        return ExecutorFuture(executor)
            .then([iter, executor, cancelToken, factory]() mutable {
                return iter->getNextBatch(std::move(executor), cancelToken, factory);
            })
            .then([](auto x) { return x; })
            .get();
    }

    auto getNextBatch(ReshardingDonorOplogIterator* iter,
                      std::shared_ptr<executor::TaskExecutor> executor,
                      CancelableOperationContextFactory factory) {
        return getNextBatch(iter, std::move(executor), factory, CancellationToken::uncancelable());
    }

    ServiceContext::UniqueClient makeKillableClient() {
        auto client = getServiceContext()->getService()->makeClient("ReshardingDonorOplogIterator");
        return client;
    }

private:
    const NamespaceString _oplogNss = NamespaceString::createNamespaceString_forTest(
        DatabaseName::kConfig,
        fmt::format("{}xxx.yyy", NamespaceString::kReshardingLocalOplogBufferPrefix));
    const NamespaceString _crudNss = NamespaceString::createNamespaceString_forTest("test.foo");
    const UUID _uuid{UUID::gen()};

    RAIIServerParameterControllerForTest controller{"reshardingOplogBatchLimitOperations", 1};
};

TEST_F(ReshardingDonorOplogIterTest, BasicExhaust) {
    const auto oplog1 = makeInsertOplog(Timestamp(2, 4), BSON("x" << 1));
    const auto oplog2 = makeInsertOplog(Timestamp(33, 6), BSON("y" << 1));
    const auto finalOplog = makeFinalOplog(Timestamp(43, 24));

    DBDirectClient client(operationContext());
    const auto nss = oplogNss();
    client.insert(nss, oplog1.toBSON());
    client.insert(nss, oplog2.toBSON());
    client.insert(nss, finalOplog.toBSON());

    ReshardingDonorOplogIterator iter(makePipeline(), kResumeFromBeginning, &onInsertAlwaysReady);
    auto executor = makeTaskExecutorForIterator();
    auto factory = makeCancelableOpCtx();
    auto altClient = makeKillableClient();
    AlternativeClientRegion acr(altClient);

    auto next = getNextBatch(&iter, executor, factory);
    ASSERT_EQ(next.size(), 1U);
    ASSERT_BSONOBJ_EQ(getId(oplog1), getId(next[0]));

    next = getNextBatch(&iter, executor, factory);
    ASSERT_EQ(next.size(), 1U);
    ASSERT_BSONOBJ_EQ(getId(oplog2), getId(next[0]));

    next = getNextBatch(&iter, executor, factory);
    ASSERT_TRUE(next.empty());

    next = getNextBatch(&iter, executor, factory);
    ASSERT_TRUE(next.empty());
}

TEST_F(ReshardingDonorOplogIterTest, ResumeFromMiddle) {
    const auto oplog1 = makeInsertOplog(Timestamp(2, 4), BSON("x" << 1));
    const auto oplog2 = makeInsertOplog(Timestamp(33, 6), BSON("y" << 1));
    const auto finalOplog = makeFinalOplog(Timestamp(43, 24));

    DBDirectClient client(operationContext());
    const auto nss = oplogNss();
    client.insert(nss, oplog1.toBSON());
    client.insert(nss, oplog2.toBSON());
    client.insert(nss, finalOplog.toBSON());

    ReshardingDonorOplogId resumeToken(Timestamp(2, 4), Timestamp(2, 4));
    ReshardingDonorOplogIterator iter(makePipeline(), resumeToken, &onInsertAlwaysReady);
    auto executor = makeTaskExecutorForIterator();
    auto factory = makeCancelableOpCtx();
    auto altClient = makeKillableClient();
    AlternativeClientRegion acr(altClient);

    auto next = getNextBatch(&iter, executor, factory);
    ASSERT_EQ(next.size(), 1U);
    ASSERT_BSONOBJ_EQ(getId(oplog2), getId(next[0]));

    next = getNextBatch(&iter, executor, factory);
    ASSERT_TRUE(next.empty());
}

TEST_F(ReshardingDonorOplogIterTest, ExhaustWithIncomingInserts) {
    const auto oplog1 = makeInsertOplog(Timestamp(2, 4), BSON("x" << 1));
    const auto oplog2 = makeInsertOplog(Timestamp(33, 6), BSON("y" << 1));
    const auto finalOplog = makeFinalOplog(Timestamp(43, 24));

    DBDirectClient client(operationContext());
    const auto nss = oplogNss();
    client.insert(nss, oplog1.toBSON());

    class InsertNotifier : public resharding::OnInsertAwaitable {
    public:
        using Callback = std::function<void(OperationContext*, size_t)>;

        InsertNotifier(ServiceContext* serviceContext, Callback onAwaitInsertCalled)
            : _serviceContext(serviceContext), _onAwaitInsertCalled(onAwaitInsertCalled) {}

        Future<void> awaitInsert(const ReshardingDonorOplogId& lastSeen) override {
            ++numCalls;

            auto client = _serviceContext->getService()->makeClient("onAwaitInsertCalled");
            AlternativeClientRegion acr(client);
            auto opCtx = cc().makeOperationContext();
            _onAwaitInsertCalled(opCtx.get(), numCalls);

            return Future<void>::makeReady();
        }

        size_t numCalls = 0;

    private:
        ServiceContext* _serviceContext;
        Callback _onAwaitInsertCalled;
    } insertNotifier{getServiceContext(), [&](OperationContext* opCtx, size_t numCalls) {
                         DBDirectClient client(opCtx);

                         if (numCalls == 1) {
                             client.insert(nss, oplog2.toBSON());
                         } else {
                             client.insert(nss, finalOplog.toBSON());
                         }
                     }};

    ReshardingDonorOplogIterator iter(makePipeline(), kResumeFromBeginning, &insertNotifier);
    auto executor = makeTaskExecutorForIterator();
    auto factory = makeCancelableOpCtx();
    auto altClient = makeKillableClient();
    AlternativeClientRegion acr(altClient);

    auto next = getNextBatch(&iter, executor, factory);
    ASSERT_EQ(next.size(), 1U);
    ASSERT_BSONOBJ_EQ(getId(oplog1), getId(next[0]));

    next = getNextBatch(&iter, executor, factory);
    ASSERT_EQ(next.size(), 1U);
    ASSERT_BSONOBJ_EQ(getId(oplog2), getId(next[0]));

    next = getNextBatch(&iter, executor, factory);
    ASSERT_TRUE(next.empty());

    next = getNextBatch(&iter, executor, factory);
    ASSERT_TRUE(next.empty());

    ASSERT_EQ(insertNotifier.numCalls, 2U);
}

TEST_F(ReshardingDonorOplogIterTest, BatchIncludesProgressMarkEntries) {
    const auto oplog1 = makeInsertOplog(Timestamp(2, 4), BSON("x" << 1));
    const auto progressMarkOplog1 = makeProgressMarkOplogEntry(Timestamp(15, 3));
    const auto finalOplog = makeFinalOplog(Timestamp(43, 24));

    DBDirectClient client(operationContext());
    const auto nss = oplogNss();
    client.insert(nss, oplog1.toBSON());
    client.insert(nss, progressMarkOplog1.toBSON());
    client.insert(nss, finalOplog.toBSON());

    ReshardingDonorOplogIterator iter(makePipeline(), kResumeFromBeginning, &onInsertAlwaysReady);
    auto executor = makeTaskExecutorForIterator();
    auto factory = makeCancelableOpCtx();
    auto altClient = makeKillableClient();
    AlternativeClientRegion acr(altClient);

    auto next = getNextBatch(&iter, executor, factory);
    ASSERT_EQ(next.size(), 1U);
    ASSERT_BSONOBJ_EQ(getId(oplog1), getId(next[0]));

    next = getNextBatch(&iter, executor, factory);
    ASSERT_EQ(next.size(), 1U);
    ASSERT_BSONOBJ_EQ(getId(progressMarkOplog1), getId(next[0]));

    next = getNextBatch(&iter, executor, factory);
    ASSERT_TRUE(next.empty());
}

using ReshardingDonorOplogIterTestDeathTest = ReshardingDonorOplogIterTest;
DEATH_TEST_REGEX_F(ReshardingDonorOplogIterTestDeathTest,
                   ThrowsIfProgressMarkEntriesAfterFinalOp,
                   "Tripwire assertion.*6077499") {
    RAIIServerParameterControllerForTest controller{"reshardingOplogBatchLimitOperations", 100};

    const auto oplog1 = makeInsertOplog(Timestamp(2, 4), BSON("x" << 1));
    const auto progressMarkOplog1 = makeProgressMarkOplogEntry(Timestamp(15, 3));
    const auto finalOplog = makeFinalOplog(Timestamp(43, 24));
    // reshardProgressMark entries inserted after the reshardFinalOp entry should be ignored.
    const auto progressMarkOplog2 = makeProgressMarkOplogEntry(Timestamp(65, 2));
    const auto progressMarkOplog3 = makeProgressMarkOplogEntry(Timestamp(65, 3));
    const auto progressMarkOplog4 = makeProgressMarkOplogEntry(Timestamp(65, 4));

    DBDirectClient client(operationContext());
    const auto nss = oplogNss();
    client.insert(nss, oplog1.toBSON());
    client.insert(nss, progressMarkOplog1.toBSON());
    client.insert(nss, finalOplog.toBSON());
    client.insert(nss, progressMarkOplog2.toBSON());
    client.insert(nss, progressMarkOplog3.toBSON());
    client.insert(nss, progressMarkOplog4.toBSON());

    ReshardingDonorOplogIterator iter(makePipeline(), kResumeFromBeginning, &onInsertAlwaysReady);
    auto executor = makeTaskExecutorForIterator();
    auto factory = makeCancelableOpCtx();
    auto altClient = makeKillableClient();
    AlternativeClientRegion acr(altClient);

    ASSERT_THROWS_CODE(getNextBatch(&iter, executor, factory), DBException, 6077499);
}

TEST_F(ReshardingDonorOplogIterTest, GetNextBatchAutomaticallyRetriesOnRetryableError) {
    const auto oplog1Id = ReshardingDonorOplogId(Timestamp(2, 4), Timestamp(102, 104));

    const auto oplog1 = toOplogEntry(makeInsertOplog(oplog1Id, BSON("x" << 1)));
    const auto oplog2 = toOplogEntry(makeInsertOplog(Timestamp(330, 60), BSON("y" << 1)));
    const auto finalOplog = toOplogEntry(makeFinalOplog(Timestamp(430, 240)));

    std::vector<MockResult<std::vector<repl::OplogEntry>>> mockResponses;
    mockResponses.emplace_back(std::vector<repl::OplogEntry>{oplog1});
    mockResponses.emplace_back(Status(ErrorCodes::QueryPlanKilled, "mock error"));
    mockResponses.emplace_back(std::vector<repl::OplogEntry>{oplog2, finalOplog});
    auto mockPipeline = std::make_unique<MockReshardingDonorOplogPipeline>(mockResponses);
    auto mockPipelinePtr = mockPipeline.get();

    ReshardingDonorOplogIterator iter(
        std::move(mockPipeline), kResumeFromBeginning, &onInsertAlwaysReady);
    auto executor = makeTaskExecutorForIterator();
    auto factory = makeCancelableOpCtx();
    auto altClient = makeKillableClient();
    AlternativeClientRegion acr(altClient);

    auto next = getNextBatch(&iter, executor, factory);
    ASSERT_EQ(next.size(), 1U);
    ASSERT_BSONOBJ_EQ(getId(oplog1), getId(next[0]));
    ASSERT_EQ(ReshardingDonorOplogId(), mockPipelinePtr->getLastResumeTokenPassedToInit());

    next = getNextBatch(&iter, executor, factory);
    ASSERT_EQ(next.size(), 1U);
    ASSERT_BSONOBJ_EQ(getId(oplog2), getId(next[0]));
    ASSERT_EQ(oplog1Id, mockPipelinePtr->getLastResumeTokenPassedToInit());

    next = getNextBatch(&iter, executor, factory);
    ASSERT_TRUE(next.empty());
    ASSERT_EQ(oplog1Id, mockPipelinePtr->getLastResumeTokenPassedToInit());
}

TEST_F(ReshardingDonorOplogIterTest, GetNextBatchPassesHighestSeenOplogIdAsResumeToken) {
    const auto oplog1Id = ReshardingDonorOplogId(Timestamp(2, 4), Timestamp(102, 104));
    const auto oplog2Id = ReshardingDonorOplogId(Timestamp(33, 6), Timestamp(133, 106));
    const auto oplog3Id = ReshardingDonorOplogId(Timestamp(43, 24), Timestamp(143, 124));
    const auto oplog4Id = ReshardingDonorOplogId(Timestamp(49, 85), Timestamp(149, 185));

    const auto oplog1 = toOplogEntry(makeInsertOplog(oplog1Id, BSON("x" << 1)));
    const auto oplog2 = toOplogEntry(makeInsertOplog(oplog2Id, BSON("y" << 1)));
    const auto oplog3 = toOplogEntry(makeInsertOplog(oplog3Id, BSON("z" << 1)));
    const auto oplog4 = toOplogEntry(makeInsertOplog(oplog4Id, BSON("a" << 1)));
    const auto finalOplog = toOplogEntry(makeFinalOplog(Timestamp(55, 7)));

    std::vector<MockResult<std::vector<repl::OplogEntry>>> mockResponses;
    mockResponses.emplace_back(std::vector<repl::OplogEntry>{oplog1});
    mockResponses.emplace_back(std::vector<repl::OplogEntry>{oplog2, oplog3});
    mockResponses.emplace_back(Status(ErrorCodes::QueryPlanKilled, "mock error"));
    mockResponses.emplace_back(std::vector<repl::OplogEntry>{oplog4});
    mockResponses.emplace_back(std::vector<repl::OplogEntry>{finalOplog});
    auto mockPipeline = std::make_unique<MockReshardingDonorOplogPipeline>(mockResponses);
    auto mockPipelinePtr = mockPipeline.get();

    ReshardingDonorOplogIterator iter(
        std::move(mockPipeline), kResumeFromBeginning, &onInsertAlwaysReady);
    auto executor = makeTaskExecutorForIterator();
    auto factory = makeCancelableOpCtx();
    auto altClient = makeKillableClient();
    AlternativeClientRegion acr(altClient);

    auto next = getNextBatch(&iter, executor, factory);
    ASSERT_EQ(next.size(), 1U);
    ASSERT_BSONOBJ_EQ(getId(oplog1), getId(next[0]));
    ASSERT_EQ(ReshardingDonorOplogId(), mockPipelinePtr->getLastResumeTokenPassedToInit());

    next = getNextBatch(&iter, executor, factory);
    ASSERT_EQ(next.size(), 2U);
    ASSERT_BSONOBJ_EQ(getId(oplog2), getId(next[0]));
    ASSERT_BSONOBJ_EQ(getId(oplog3), getId(next[1]));
    ASSERT_EQ(oplog1Id, mockPipelinePtr->getLastResumeTokenPassedToInit());

    next = getNextBatch(&iter, executor, factory);
    ASSERT_EQ(next.size(), 1U);
    ASSERT_BSONOBJ_EQ(getId(oplog4), getId(next[0]));
    ASSERT_EQ(oplog3Id, mockPipelinePtr->getLastResumeTokenPassedToInit());

    next = getNextBatch(&iter, executor, factory);
    ASSERT_TRUE(next.empty());
    ASSERT_EQ(oplog4Id, mockPipelinePtr->getLastResumeTokenPassedToInit());
}

TEST_F(ReshardingDonorOplogIterTest, GetNextBatchStopsWhenCancellationTokenIsCanceled) {
    CancellationSource cancelSource;
    auto cancelToken = cancelSource.token();

    const auto oplog1 = toOplogEntry(makeInsertOplog(Timestamp(2, 4), BSON("x" << 1)));
    const auto oplog2 = toOplogEntry(makeInsertOplog(Timestamp(33, 6), BSON("y" << 1)));
    const auto finalOplog = toOplogEntry(makeFinalOplog(Timestamp(43, 24)));

    std::vector<MockResult<std::vector<repl::OplogEntry>>> mockResponses;
    mockResponses.emplace_back(std::vector<repl::OplogEntry>{oplog1});

    {
        MockResult<std::vector<repl::OplogEntry>> mockResult(
            Status(ErrorCodes::InterruptedDueToReplStateChange, "fake stepdown"));
        mockResult.beforeReturnResult = [&] {
            cancelSource.cancel();
        };
        mockResponses.emplace_back(mockResult);
    }

    mockResponses.emplace_back(std::vector<repl::OplogEntry>{oplog2, finalOplog});
    auto mockPipeline = std::make_unique<MockReshardingDonorOplogPipeline>(mockResponses);

    ReshardingDonorOplogIterator iter(
        std::move(mockPipeline), kResumeFromBeginning, &onInsertAlwaysReady);
    auto executor = makeTaskExecutorForIterator();
    auto factory = makeCancelableOpCtx();
    auto altClient = makeKillableClient();
    AlternativeClientRegion acr(altClient);

    auto next = getNextBatch(&iter, executor, factory, cancelToken);
    ASSERT_EQ(next.size(), 1U);
    ASSERT_BSONOBJ_EQ(getId(oplog1), getId(next[0]));

    ASSERT_THROWS_CODE(getNextBatch(&iter, executor, factory, cancelToken),
                       DBException,
                       ErrorCodes::CallbackCanceled);
}

TEST_F(ReshardingDonorOplogIterTest, GetNextBatchThrowsOnNonRetryableError) {
    const auto oplog1 = toOplogEntry(makeInsertOplog(Timestamp(2, 4), BSON("x" << 1)));

    std::vector<MockResult<std::vector<repl::OplogEntry>>> mockResponses;
    mockResponses.emplace_back(std::vector<repl::OplogEntry>{oplog1});
    mockResponses.emplace_back(Status(ErrorCodes::InternalError, "fake test error"));
    auto mockPipeline = std::make_unique<MockReshardingDonorOplogPipeline>(mockResponses);

    ReshardingDonorOplogIterator iter(
        std::move(mockPipeline), kResumeFromBeginning, &onInsertAlwaysReady);
    auto executor = makeTaskExecutorForIterator();
    auto factory = makeCancelableOpCtx();
    auto altClient = makeKillableClient();
    AlternativeClientRegion acr(altClient);

    auto next = getNextBatch(&iter, executor, factory);
    ASSERT_EQ(next.size(), 1U);
    ASSERT_BSONOBJ_EQ(getId(oplog1), getId(next[0]));

    ASSERT_THROWS_CODE(
        getNextBatch(&iter, executor, factory), DBException, ErrorCodes::InternalError);
}

}  // anonymous namespace
}  // namespace mongo
