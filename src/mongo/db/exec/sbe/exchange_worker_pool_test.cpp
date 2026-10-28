// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/stages/exchange_worker_pool.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/validated_tenancy_scope.h"
#include "mongo/db/auth/validated_tenancy_scope_factory.h"
#include "mongo/db/client.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/raw_data_operation.h"
#include "mongo/db/stats/direct_system_buckets_access.h"
#include "mongo/db/topology/user_write_block/replica_set_write_block_bypass.h"
#include "mongo/db/topology/user_write_block/user_write_block_bypass.h"
#include "mongo/db/version_context.h"
#include "mongo/otel/telemetry_context_holder.h"
#include "mongo/otel/traces/span/span.h"
#include "mongo/otel/traces/span/span_names.h"
#include "mongo/otel/traces/telemetry_context_serialization.h"
#include "mongo/otel/traces/traces_test_util.h"
#include "mongo/rpc/metadata/audit_client_attrs.h"
#include "mongo/rpc/metadata/audit_user_attrs.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/scopeguard.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fmt/format.h>

namespace mongo::sbe {
/**
 * This file contains tests for sbe::ExchangeWorkerPool and sbe::ExchangeWorker.
 */
class ExchangeWorkerPoolTest : public ServiceContextTest {};

TEST_F(ExchangeWorkerPoolTest, StatsTrackLeasedAndIdleWorkers) {
    auto& pool = ExchangeWorkerPool::get(getServiceContext());
    auto operationContext = makeOperationContext();

    auto statsBefore = [&] {
        BSONObjBuilder bob;
        pool.appendStats(bob);
        return bob.obj();
    }();
    long long createdBefore = statsBefore["created"].numberLong();
    long long idleBefore = statsBefore["idle"].numberLong();
    ASSERT_EQ(statsBefore["inUse"].numberLong(), 0);

    {
        auto workers = pool.acquire(operationContext.get(), 2);
        ASSERT_EQ(workers.size(), 2u);

        BSONObjBuilder bob;
        pool.appendStats(bob);
        auto stats = bob.obj();
        ASSERT_EQ(stats["inUse"].numberLong(), 2);
        ASSERT_EQ(stats["created"].numberLong() - createdBefore, 2 - std::min(idleBefore, 2LL));
        ASSERT_EQ(stats["idle"].numberLong(), idleBefore - std::min(idleBefore, 2LL));
        ASSERT_EQ(pool.leasedCount(), 2u);
    }

    BSONObjBuilder bob;
    pool.appendStats(bob);
    auto stats = bob.obj();
    ASSERT_EQ(stats["inUse"].numberLong(), 0);
    ASSERT_EQ(stats["idle"].numberLong(), stats["created"].numberLong());
}

TEST_F(ExchangeWorkerPoolTest, HonorsMaxWorkersKnob) {
    auto& pool = ExchangeWorkerPool::get(getServiceContext());
    auto operationContext = makeOperationContext();
    ASSERT_EQ(pool.createdCount(), 0u);

    unittest::ServerParameterGuard maxWorkersTwo("internalQuerySBEExchangeMaxWorkers", 2);
    {
        // Only two workers can be handed out.
        auto workers = pool.acquire(operationContext.get(), 5);
        ASSERT_EQ(workers.size(), 2u);
        ASSERT_EQ(pool.leasedCount(), 2u);
    }
    ASSERT_EQ(pool.leasedCount(), 0u);
    ASSERT_EQ(pool.idleCount(), 2u);

    std::optional<unittest::ServerParameterGuard> maxWorkersTen;
    maxWorkersTen.emplace("internalQuerySBEExchangeMaxWorkers", 10);
    auto firstBatch = pool.acquire(operationContext.get(), 5);
    ASSERT_EQ(firstBatch.size(), 5u);
    // Only the headroom left under the limit is handed out.
    auto secondBatch = pool.acquire(operationContext.get(), 6);
    ASSERT_EQ(secondBatch.size(), 5u);
    ASSERT_EQ(pool.leasedCount(), 10u);
    ASSERT_EQ(pool.createdCount(), 10u);

    // Lowering the limit while the workers are leased out trims the pool as they come back.
    // Threads are not immediately reclaimed and they keep working.
    maxWorkersTen.reset();
    ASSERT_EQ(pool.leasedCount(), 10u);
    ASSERT_EQ(pool.idleCount(), 0u);
    ASSERT_EQ(pool.createdCount(), 10u);

    // Return 5 threads, as we are above the limit they get destroyed instead of being put into idle
    // mode.
    firstBatch.clear();
    ASSERT_EQ(pool.leasedCount(), 5u);
    ASSERT_EQ(pool.idleCount(), 0u);
    ASSERT_EQ(pool.createdCount(), 5u);

    // Trying to get a new thread while we are above the limit gets us nothing.
    auto thirdBatch = pool.acquire(operationContext.get(), 1);
    ASSERT_EQ(thirdBatch.size(), 0u);

    // Releasing the second batch of threads puts only 2 threads into idle mode.
    secondBatch.clear();
    ASSERT_EQ(pool.leasedCount(), 0u);
    ASSERT_EQ(pool.idleCount(), 2u);
    ASSERT_EQ(pool.createdCount(), 2u);

    // These threads are recycled when re-requested.
    {
        auto workers = pool.acquire(operationContext.get(), 2);
        ASSERT_EQ(workers.size(), 2u);
    }
    ASSERT_EQ(pool.idleCount(), 2u);
    ASSERT_EQ(pool.createdCount(), 2u);

    std::optional<unittest::ServerParameterGuard> maxWorkersOne;
    maxWorkersOne.emplace("internalQuerySBEExchangeMaxWorkers", 1);
    // We started with 2 idle threads, but we get to recycle only 1 due to the new maximum.
    {
        auto workers = pool.acquire(operationContext.get(), 2);
        ASSERT_EQ(workers.size(), 1u);
        ASSERT_EQ(pool.idleCount(), 0u);
        ASSERT_EQ(pool.createdCount(), 1u);
    }
    ASSERT_EQ(pool.idleCount(), 1u);
    ASSERT_EQ(pool.createdCount(), 1u);
    maxWorkersOne.reset();

    // Raise the limit to 10 threads, acquire all of them in two batches.
    maxWorkersTen.emplace("internalQuerySBEExchangeMaxWorkers", 10);
    auto batchOne = pool.acquire(operationContext.get(), 5);
    ASSERT_EQ(batchOne.size(), 5u);
    auto batchTwo = pool.acquire(operationContext.get(), 5);
    ASSERT_EQ(batchTwo.size(), 5u);
    ASSERT_EQ(pool.leasedCount(), 10u);
    ASSERT_EQ(pool.idleCount(), 0u);
    ASSERT_EQ(pool.createdCount(), 10u);

    // Return 5 threads, then lower the limit to 6 threads.
    batchOne.clear();
    ASSERT_EQ(pool.leasedCount(), 5u);
    ASSERT_EQ(pool.idleCount(), 5u);
    ASSERT_EQ(pool.createdCount(), 10u);

    std::optional<unittest::ServerParameterGuard> maxWorkersSix;
    maxWorkersSix.emplace("internalQuerySBEExchangeMaxWorkers", 6);
    auto batchThree = pool.acquire(operationContext.get(), 5);
    ASSERT_EQ(batchThree.size(), 1u);
    ASSERT_EQ(pool.leasedCount(), 6u);
    ASSERT_EQ(pool.idleCount(), 0u);
    ASSERT_EQ(pool.createdCount(), 6u);
}

TEST_F(ExchangeWorkerPoolTest, SubmitAsPromiseReturnsTheResultOfTheJob) {
    ExchangeWorker worker;
    auto callerThread = std::this_thread::get_id();
    auto future = worker.submitAsPromise([callerThread] {
        ASSERT_NE(std::this_thread::get_id(), callerThread);
        return 42;
    });
    worker.waitIdle();
    ASSERT_EQ(std::move(future).get(), 42);
}

TEST_F(ExchangeWorkerPoolTest, SubmitAsPromisePropagatesExceptions) {
    ExchangeWorker worker;
    auto future =
        worker.submitAsPromise([] { uasserted(ErrorCodes::InternalError, "job failed"); });
    worker.waitIdle();
    ASSERT_THROWS_CODE(std::move(future).get(), DBException, ErrorCodes::InternalError);
}

TEST_F(ExchangeWorkerPoolTest, SubmitPropagatesCapturedExceptions) {
    ExchangeWorker worker;
    std::exception_ptr exc;
    worker.submit([&exc] {
        try {
            uasserted(ErrorCodes::InternalError, "job failed");
        } catch (...) {
            exc = std::current_exception();
        }
    });
    worker.waitIdle();
    ASSERT_NE(exc, nullptr);
    ASSERT_THROWS_CODE(std::rethrow_exception(exc), DBException, ErrorCodes::InternalError);
}

TEST_F(ExchangeWorkerPoolTest, WorkerOperationContextIsBoundToItsOwnClient) {
    auto& pool = ExchangeWorkerPool::get(getServiceContext());
    auto operationContext = makeOperationContext();

    auto workers = pool.acquire(operationContext.get(), 2);
    ASSERT_EQ(workers.size(), 2u);
    for (auto& worker : workers) {
        auto* workerOpCtx = worker->opCtxHolder.get();
        ASSERT(workerOpCtx);
        ASSERT_NE(workerOpCtx, operationContext.get());
        ASSERT_NE(workerOpCtx->getClient(), operationContext->getClient());
        ASSERT_EQ(workerOpCtx->getServiceContext(), operationContext->getServiceContext());
        auto workerClient = worker->submitAsPromise([] { return Client::getCurrent(); }).get();
        ASSERT_EQ(workerOpCtx->getClient(), workerClient);
    }
    ASSERT_NE(workers[0]->opCtxHolder->getClient(), workers[1]->opCtxHolder->getClient());
}

TEST_F(ExchangeWorkerPoolTest, WorkerOperationContextInheritsSettingsOfMainOperationContext) {
    auto& pool = ExchangeWorkerPool::get(getServiceContext());
    auto operationContext = makeOperationContext();
    auto* mainOpCtx = operationContext.get();

    const auto comment = BSON("comment" << "parallel query");
    {
        std::lock_guard lk(*mainOpCtx->getClient());
        mainOpCtx->setComment(comment);
    }
    isRawDataOperation(mainOpCtx) = true;
    isDirectSystemBucketsAccess(mainOpCtx) = true;
    WriteBlockBypass::get(mainOpCtx).set(true);

    // An operation FCV is also what makes the replica set write blocking bypass forwardable, as
    // featureFlagBlockReplicaSetWrites is checked against it.
    // (Generic FCV reference): used for testing.
    const VersionContext vCtx{multiversion::GenericFCV::kLatest};
    VersionContext::ScopedSetDecoration scopedVCtx(mainOpCtx, vCtx);
    ReplicaSetWriteBlockBypass::get(mainOpCtx).set(true);

    const rpc::AuditUserAttrs userAttrs(
        UserName("user", "admin"), {RoleName("readWrite", "test")}, true /* isImpersonating */);
    rpc::AuditUserAttrs::set(mainOpCtx, userAttrs);
    const rpc::AuditClientAttrs clientAttrs(HostAndPort("localhost", 27017),
                                            HostAndPort("10.0.0.1", 12345),
                                            HostAndPort("10.0.0.2", 23456),
                                            {HostAndPort("10.0.0.3", 34567)},
                                            true /* isImpersonating */);
    rpc::AuditClientAttrs::set(mainOpCtx->getClient(), clientAttrs);

    auto workers = pool.acquire(mainOpCtx, 2);
    ASSERT_EQ(workers.size(), 2u);
    for (auto& worker : workers) {
        auto* workerOpCtx = worker->opCtxHolder.get();
        ASSERT(workerOpCtx->getComment());
        ASSERT_BSONELT_EQ(*workerOpCtx->getComment(), comment.firstElement());
        ASSERT_TRUE(isRawDataOperation(workerOpCtx));
        ASSERT_TRUE(isDirectSystemBucketsAccess(workerOpCtx));
        ASSERT_TRUE(WriteBlockBypass::get(workerOpCtx).isWriteBlockBypassEnabled());
        ASSERT_TRUE(ReplicaSetWriteBlockBypass::get(workerOpCtx).isEnabled());
        ASSERT_EQ(VersionContext::getDecoration(workerOpCtx), vCtx);

        auto workerUserAttrs = rpc::AuditUserAttrs::get(workerOpCtx);
        ASSERT(workerUserAttrs);
        ASSERT_BSONOBJ_EQ(workerUserAttrs->toBSON(), userAttrs.toBSON());
        auto workerClientAttrs = rpc::AuditClientAttrs::get(workerOpCtx->getClient());
        ASSERT(workerClientAttrs);
        ASSERT_BSONOBJ_EQ(workerClientAttrs->toBSON(), clientAttrs.toBSON());
    }
}

TEST_F(ExchangeWorkerPoolTest, ReusedWorkerDoesNotRetainSettingsOfPreviousLease) {
    auto& pool = ExchangeWorkerPool::get(getServiceContext());
    unittest::ServerParameterGuard maxWorkersOne("internalQuerySBEExchangeMaxWorkers", 1);

    {
        auto operationContext = makeOperationContext();
        auto* mainOpCtx = operationContext.get();
        {
            std::lock_guard lk(*mainOpCtx->getClient());
            mainOpCtx->setComment(BSON("comment" << "first query"));
        }
        isRawDataOperation(mainOpCtx) = true;
        isDirectSystemBucketsAccess(mainOpCtx) = true;
        WriteBlockBypass::get(mainOpCtx).set(true);

        auto workers = pool.acquire(mainOpCtx, 1);
        ASSERT_EQ(workers.size(), 1u);
        ASSERT_TRUE(isRawDataOperation(workers[0]->opCtxHolder.get()));
    }
    ASSERT_EQ(pool.idleCount(), 1u);

    // The parked worker is handed out again, and must reflect the new main OperationContext.
    auto operationContext = makeOperationContext();
    auto workers = pool.acquire(operationContext.get(), 1);
    ASSERT_EQ(workers.size(), 1u);
    ASSERT_EQ(pool.createdCount(), 1u);
    auto* workerOpCtx = workers[0]->opCtxHolder.get();
    ASSERT_FALSE(workerOpCtx->getComment());
    ASSERT_FALSE(isRawDataOperation(workerOpCtx));
    ASSERT_FALSE(isDirectSystemBucketsAccess(workerOpCtx));
    ASSERT_FALSE(WriteBlockBypass::get(workerOpCtx).isWriteBlockBypassEnabled());
}

TEST_F(ExchangeWorkerPoolTest, WorkerOperationContextInheritsValidatedTenancyScope) {
    unittest::ServerParameterGuard multitenancy("multitenancySupport", true);
    unittest::ServerParameterGuard securityToken("featureFlagSecurityToken", true);
    unittest::ServerParameterGuard secret("testOnlyValidatedTenancyScopeKey", "secret");

    auto& pool = ExchangeWorkerPool::get(getServiceContext());
    auto operationContext = makeOperationContext();
    auto* mainOpCtx = operationContext.get();

    const TenantId tenantId(OID::gen());
    auto vts = auth::ValidatedTenancyScopeFactory::create(
        UserName("user", "admin", tenantId),
        "secret",
        auth::ValidatedTenancyScope::TenantProtocol::kDefault,
        auth::ValidatedTenancyScopeFactory::TokenForTestingTag{});
    auth::ValidatedTenancyScope::set(mainOpCtx, vts);

    auto workers = pool.acquire(mainOpCtx, 2);
    ASSERT_EQ(workers.size(), 2u);
    for (auto& worker : workers) {
        auto workerVts = auth::ValidatedTenancyScope::get(worker->opCtxHolder.get());
        ASSERT(workerVts);
        ASSERT_EQ(workerVts->getOriginalToken(), vts.getOriginalToken());
        ASSERT_TRUE(workerVts->hasTenantId());
        ASSERT_EQ(workerVts->tenantId(), tenantId);
    }
}

TEST_F(ExchangeWorkerPoolTest, WorkerOperationContextInheritsTelemetryContext) {
    otel::traces::OtelTracesCapturer capturer;
    if (!capturer.canReadSpans())
        GTEST_SKIP() << "OTel not configured";

    auto& pool = ExchangeWorkerPool::get(getServiceContext());
    auto operationContext = makeOperationContext();
    auto* mainOpCtx = operationContext.get();

    auto span = otel::traces::Span::start(mainOpCtx, otel::traces::span_names::kTest1);
    const auto expected = otel::traces::TelemetryContextSerializer::toBSON(
        otel::TelemetryContextHolder::getDecoration(mainOpCtx).getTelemetryContext());

    auto workers = pool.acquire(mainOpCtx, 2);
    ASSERT_EQ(workers.size(), 2u);
    for (auto& worker : workers) {
        auto& workerTelemetryCtx =
            otel::TelemetryContextHolder::getDecoration(worker->opCtxHolder.get())
                .getTelemetryContext();
        ASSERT(workerTelemetryCtx);
        ASSERT_BSONOBJ_EQ(otel::traces::TelemetryContextSerializer::toBSON(workerTelemetryCtx),
                          expected);
    }
}

TEST_F(ExchangeWorkerPoolTest, WorkerThreadIsNamedAfterTheAcquiringThread) {
    auto& pool = ExchangeWorkerPool::get(getServiceContext());
    auto operationContext = makeOperationContext();
    const std::string originalName{getThreadName()};
    ON_BLOCK_EXIT([&] { setThreadName(originalName); });

    auto assertWorkerNames = [&](const std::vector<ExchangeWorkerPool::Handle>& workers,
                                 std::string_view prefix) {
        for (size_t i = 0; i < workers.size(); i++) {
            const auto expected = fmt::format("{}-{}", prefix, i + 1);
            ASSERT_EQ(workers[i]->opCtxHolder->getClient()->desc(), expected);
            auto threadName =
                workers[i]->submitAsPromise([] { return std::string{getThreadName()}; }).get();
            ASSERT_EQ(threadName, expected);
        }
    };

    setThreadName("firstQuery");
    {
        auto workers = pool.acquire(operationContext.get(), 2);
        ASSERT_EQ(workers.size(), 2u);
        assertWorkerNames(workers, "firstQuery");
    }
    ASSERT_EQ(pool.idleCount(), 2u);

    // The two parked workers are reused and a third one is created: all of them must be renamed
    // after the new acquiring thread.
    setThreadName("secondQuery");
    auto workers = pool.acquire(operationContext.get(), 3);
    ASSERT_EQ(workers.size(), 3u);
    ASSERT_EQ(pool.createdCount(), 3u);
    assertWorkerNames(workers, "secondQuery");
}

}  // namespace mongo::sbe
