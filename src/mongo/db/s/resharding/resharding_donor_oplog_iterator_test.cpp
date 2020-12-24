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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/platform/basic.h"

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/s/resharding/resharding_donor_oplog_iterator.h"
#include "mongo/db/s/resharding/resharding_server_parameters_gen.h"
#include "mongo/db/s/resharding_util.h"
#include "mongo/db/s/sharding_mongod_test_fixture.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/unittest/unittest.h"

#include "mongo/logv2/log.h"

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

    oplogEntry.setOpTimeAndWallTimeBase(repl::OpTimeAndWallTimeBase({}, {}));
    oplogEntry.set_id(Value(oplogId.toBSON()));

    return oplogEntry;
}

class OnInsertAlwaysReady : public resharding::OnInsertAwaitable {
public:
    Future<void> awaitInsert(const ReshardingDonorOplogId& lastSeen) override {
        return Future<void>::makeReady();
    }
} onInsertAlwaysReady;

class ScopedServerParameterChange {
public:
    ScopedServerParameterChange(int* param, int newValue) : _param(param), _originalValue(*_param) {
        *param = newValue;
    }

    ~ScopedServerParameterChange() {
        *_param = _originalValue;
    }

private:
    int* const _param;
    const int _originalValue;
};

class ReshardingDonorOplogIterTest : public ShardingMongodTestFixture {
public:
    repl::MutableOplogEntry makeInsertOplog(const Timestamp& id, BSONObj doc) {
        ReshardingDonorOplogId oplogId(id, id);
        return makeOplog(_crudNss, _uuid, repl::OpTypeEnum::kInsert, BSON("x" << 1), {}, oplogId);
    }

    repl::MutableOplogEntry makeFinalOplog(const Timestamp& id) {
        ReshardingDonorOplogId oplogId(id, id);
        const BSONObj oField(BSON("msg"
                                  << "Created temporary resharding collection"));
        const BSONObj o2Field(
            BSON("type" << kReshardFinalOpLogType << "reshardingUUID" << UUID::gen()));
        return makeOplog(_crudNss, _uuid, repl::OpTypeEnum::kNoop, oField, o2Field, oplogId);
    }

    const NamespaceString& oplogNss() const {
        return _oplogNss;
    }

    BSONObj getId(const repl::MutableOplogEntry& oplog) {
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
            auto& client = cc();
            {
                stdx::lock_guard<Client> lk(client);
                client.setSystemOperationKillableByStepdown(lk);
            }
        };

        auto executor = executor::makeThreadPoolTestExecutor(
            std::make_unique<executor::NetworkInterfaceMock>(), std::move(threadPoolOptions));

        executor->startup();
        return executor;
    }

    auto getNextBatch(ReshardingDonorOplogIterator* iter,
                      std::shared_ptr<executor::TaskExecutor> executor) {
        // There isn't a guarantee that the reference count to `executor` has been decremented after
        // .get() returns. We schedule a trivial task on the task executor to ensure the callback's
        // destructor has run. Otherwise `executor` could end up outliving the ServiceContext and
        // triggering an invariant due to the task executor's thread having a Client still.
        return ExecutorFuture(executor)
            .then([iter, executor] { return iter->getNextBatch(std::move(executor)); })
            .then([](auto x) { return x; })
            .get();
    }

    ServiceContext::UniqueClient makeKillableClient() {
        auto client = getServiceContext()->makeClient("ReshardingDonorOplogIterator");
        stdx::lock_guard<Client> lk(*client);
        client->setSystemOperationKillableByStepdown(lk);
        return client;
    }

private:
    const NamespaceString _oplogNss{"config.localReshardingOplogBuffer.xxx.yyy"};
    const NamespaceString _crudNss{"test.foo"};
    const UUID _uuid{UUID::gen()};

    ScopedServerParameterChange _iteratorBatchSize{&resharding::gReshardingBatchLimitOperations, 1};
};

TEST_F(ReshardingDonorOplogIterTest, BasicExhaust) {
    const auto oplog1 = makeInsertOplog(Timestamp(2, 4), BSON("x" << 1));
    const auto oplog2 = makeInsertOplog(Timestamp(33, 6), BSON("y" << 1));
    const auto finalOplog = makeFinalOplog(Timestamp(43, 24));
    const auto oplogBeyond = makeInsertOplog(Timestamp(123, 46), BSON("z" << 1));

    DBDirectClient client(operationContext());
    const auto ns = oplogNss().ns();
    client.insert(ns, oplog1.toBSON());
    client.insert(ns, oplog2.toBSON());
    client.insert(ns, finalOplog.toBSON());
    client.insert(ns, oplogBeyond.toBSON());

    ReshardingDonorOplogIterator iter(oplogNss(), kResumeFromBeginning, &onInsertAlwaysReady);
    auto executor = makeTaskExecutorForIterator();
    auto altClient = makeKillableClient();
    AlternativeClientRegion acr(altClient);

    auto next = getNextBatch(&iter, executor);
    ASSERT_EQ(next.size(), 1U);
    ASSERT_BSONOBJ_EQ(getId(oplog1), getId(next[0]));

    next = getNextBatch(&iter, executor);
    ASSERT_EQ(next.size(), 1U);
    ASSERT_BSONOBJ_EQ(getId(oplog2), getId(next[0]));

    next = getNextBatch(&iter, executor);
    ASSERT_TRUE(next.empty());

    next = getNextBatch(&iter, executor);
    ASSERT_TRUE(next.empty());
}

TEST_F(ReshardingDonorOplogIterTest, ResumeFromMiddle) {
    const auto oplog1 = makeInsertOplog(Timestamp(2, 4), BSON("x" << 1));
    const auto oplog2 = makeInsertOplog(Timestamp(33, 6), BSON("y" << 1));
    const auto finalOplog = makeFinalOplog(Timestamp(43, 24));

    DBDirectClient client(operationContext());
    const auto ns = oplogNss().ns();
    client.insert(ns, oplog1.toBSON());
    client.insert(ns, oplog2.toBSON());
    client.insert(ns, finalOplog.toBSON());

    ReshardingDonorOplogId resumeToken(Timestamp(2, 4), Timestamp(2, 4));
    ReshardingDonorOplogIterator iter(oplogNss(), resumeToken, &onInsertAlwaysReady);
    auto executor = makeTaskExecutorForIterator();
    auto altClient = makeKillableClient();
    AlternativeClientRegion acr(altClient);

    auto next = getNextBatch(&iter, executor);
    ASSERT_EQ(next.size(), 1U);
    ASSERT_BSONOBJ_EQ(getId(oplog2), getId(next[0]));

    next = getNextBatch(&iter, executor);
    ASSERT_TRUE(next.empty());
}

TEST_F(ReshardingDonorOplogIterTest, ExhaustWithIncomingInserts) {
    const auto oplog1 = makeInsertOplog(Timestamp(2, 4), BSON("x" << 1));
    const auto oplog2 = makeInsertOplog(Timestamp(33, 6), BSON("y" << 1));
    const auto finalOplog = makeFinalOplog(Timestamp(43, 24));
    const auto oplogBeyond = makeInsertOplog(Timestamp(123, 46), BSON("z" << 1));

    DBDirectClient client(operationContext());
    const auto ns = oplogNss().ns();
    client.insert(ns, oplog1.toBSON());

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
                             client.insert(ns, oplog2.toBSON());
                         } else {
                             client.insert(ns, finalOplog.toBSON());
                             client.insert(ns, oplogBeyond.toBSON());
                         }
                     }};

    ReshardingDonorOplogIterator iter(oplogNss(), kResumeFromBeginning, &insertNotifier);
    auto executor = makeTaskExecutorForIterator();
    auto altClient = makeKillableClient();
    AlternativeClientRegion acr(altClient);

    auto next = getNextBatch(&iter, executor);
    ASSERT_EQ(next.size(), 1U);
    ASSERT_BSONOBJ_EQ(getId(oplog1), getId(next[0]));

    next = getNextBatch(&iter, executor);
    ASSERT_EQ(next.size(), 1U);
    ASSERT_BSONOBJ_EQ(getId(oplog2), getId(next[0]));

    next = getNextBatch(&iter, executor);
    ASSERT_TRUE(next.empty());

    next = getNextBatch(&iter, executor);
    ASSERT_TRUE(next.empty());

    ASSERT_EQ(insertNotifier.numCalls, 2U);
}

}  // anonymous namespace
}  // namespace mongo
