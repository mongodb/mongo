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


#include "mongo/platform/basic.h"

#include <utility>

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/s/resharding/resharding_donor_oplog_iterator.h"
#include "mongo/db/s/resharding/resharding_server_parameters_gen.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/s/shard_server_test_fixture.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace {

using namespace fmt::literals;

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

class ReshardingDonorOplogIterTest : public ShardServerTestFixture {
public:
    repl::MutableOplogEntry makeInsertOplog(Timestamp ts, BSONObj doc) {
        ReshardingDonorOplogId oplogId(ts, ts);
        return makeOplog(_crudNss, _uuid, repl::OpTypeEnum::kInsert, std::move(doc), {}, oplogId);
    }

    repl::MutableOplogEntry makeFinalOplog(Timestamp ts) {
        ReshardingDonorOplogId oplogId(ts, ts);
        const BSONObj oField(BSON("msg"
                                  << "Created temporary resharding collection"));
        const BSONObj o2Field(
            BSON("type" << resharding::kReshardFinalOpLogType << "reshardingUUID" << UUID::gen()));
        return makeOplog(_crudNss, _uuid, repl::OpTypeEnum::kNoop, oField, o2Field, oplogId);
    }

    repl::MutableOplogEntry makeProgressMarkOplogEntry(Timestamp ts) {
        ReshardingDonorOplogId oplogId(ts, ts);
        const BSONObj oField(BSON("msg"
                                  << "Latest oplog ts from donor's cursor response"));
        const BSONObj o2Field(BSON("type" << resharding::kReshardProgressMark));
        return makeOplog(_crudNss, _uuid, repl::OpTypeEnum::kNoop, oField, o2Field, oplogId);
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
            Client::initThread("TestReshardingDonorOplogIterator");
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

    auto getNextBatch(ReshardingDonorOplogIterator* iter,
                      std::shared_ptr<executor::TaskExecutor> executor,
                      CancelableOperationContextFactory factory) {
        // There isn't a guarantee that the reference count to `executor` has been decremented after
        // .get() returns. We schedule a trivial task on the task executor to ensure the callback's
        // destructor has run. Otherwise `executor` could end up outliving the ServiceContext and
        // triggering an invariant due to the task executor's thread having a Client still.
        return ExecutorFuture(executor)
            .then([iter, executor, factory] {
                return iter->getNextBatch(
                    std::move(executor), CancellationToken::uncancelable(), factory);
            })
            .then([](auto x) { return x; })
            .get();
    }

    ServiceContext::UniqueClient makeKillableClient() {
        auto client = getServiceContext()->makeClient("ReshardingDonorOplogIterator");
        return client;
    }

private:
    const NamespaceString _oplogNss = NamespaceString::createNamespaceString_forTest(
        DatabaseName::kConfig,
        "{}xxx.yyy"_format(NamespaceString::kReshardingLocalOplogBufferPrefix));
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

    ReshardingDonorOplogIterator iter(oplogNss(), kResumeFromBeginning, &onInsertAlwaysReady);
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
    ReshardingDonorOplogIterator iter(oplogNss(), resumeToken, &onInsertAlwaysReady);
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

            auto client = _serviceContext->makeClient("onAwaitInsertCalled");
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

    ReshardingDonorOplogIterator iter(oplogNss(), kResumeFromBeginning, &insertNotifier);
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

    ReshardingDonorOplogIterator iter(oplogNss(), kResumeFromBeginning, &onInsertAlwaysReady);
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

DEATH_TEST_REGEX_F(ReshardingDonorOplogIterTest,
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

    ReshardingDonorOplogIterator iter(oplogNss(), kResumeFromBeginning, &onInsertAlwaysReady);
    auto executor = makeTaskExecutorForIterator();
    auto factory = makeCancelableOpCtx();
    auto altClient = makeKillableClient();
    AlternativeClientRegion acr(altClient);

    ASSERT_THROWS_CODE(getNextBatch(&iter, executor, factory), DBException, 6077499);
}

}  // anonymous namespace
}  // namespace mongo
