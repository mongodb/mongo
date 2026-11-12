// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/plan_executor_sbe.h"

#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/tick_source_mock.h"

namespace mongo {
namespace {

class CleanupStage final : public sbe::PlanStage {
public:
    CleanupStage(TickSourceMock<>* ticks, bool initiallyEOF, bool expectDetached)
        : PlanStage("coscan", nullptr, kEmptyPlanNodeId, false),
          _ticks(ticks),
          _expectDetached(expectDetached) {
        _commonStats.isEOF = initiallyEOF;
    }

    std::unique_ptr<sbe::PlanStage> clone() const override {
        return std::make_unique<CleanupStage>(_ticks, _commonStats.isEOF, _expectDetached);
    }
    void prepare(sbe::CompileCtx&) override {}
    sbe::value::SlotAccessor* getAccessor(sbe::CompileCtx&, sbe::value::SlotId) override {
        return nullptr;
    }
    void open(bool) override {}
    sbe::PlanState getNext() override {
        return trackPlanState(sbe::PlanState::IS_EOF);
    }
    void close() override {
        if (_expectDetached) {
            ASSERT_FALSE(_opCtx);
        }
        _ticks->advance(Milliseconds{100});
        trackClose();
    }
    std::unique_ptr<sbe::PlanStageStats> getStats(bool) const override {
        return std::make_unique<sbe::PlanStageStats>(_commonStats);
    }
    const SpecificStats* getSpecificStats() const override {
        return nullptr;
    }
    size_t estimateCompileTimeSize() const override {
        return sizeof(*this);
    }
    void doDebugPrint(std::vector<sbe::DebugPrinter::Block>&, sbe::DebugPrintInfo&) const override {
    }

private:
    TickSourceMock<>* _ticks;
    bool _expectDetached;
};

class PlanExecutorSBEDisposalTest : public CatalogTestFixture {
public:
    enum class ClosePath { kDetachedDisposal, kAlreadyExhausted, kFetchedEOF };

    Options makeServiceContextOptions() override {
        return CatalogTestFixture::makeServiceContextOptions().useMockTickSource(true);
    }

    std::unique_ptr<PlanExecutorSBE> makeExecutor(bool initiallyEOF = false,
                                                  bool expectDetached = false) {
        const auto nss = NamespaceString::createNamespaceString_forTest("test.dispose");
        auto* opCtx = operationContext();
        ASSERT_OK(storageInterface()->createCollection(opCtx, nss, CollectionOptions{}));
        auto acquisition = acquireCollection(
            opCtx,
            CollectionAcquisitionRequest::fromOpCtx(opCtx, nss, AcquisitionPrerequisites::kRead),
            MODE_IS);
        auto* ticks = static_cast<TickSourceMock<>*>(getServiceContext()->getTickSource());
        auto root = std::make_unique<CleanupStage>(ticks, initiallyEOF, expectDetached);
        stage = root.get();
        root->attachToOperationContext(opCtx);
        sbe::plan_ranker::CandidatePlan plan{
            .root = std::move(root),
            .data = {.stageData = stage_builder::PlanStageData{
                         stage_builder::Environment{std::make_unique<sbe::RuntimeEnvironment>()},
                         std::make_shared<stage_builder::PlanStageStaticData>()}}};
        auto yieldPolicy = PlanYieldPolicySBE::make(opCtx,
                                                    PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
                                                    getServiceContext()->getFastClockSource(),
                                                    128,
                                                    Milliseconds{10});
        return std::make_unique<PlanExecutorSBE>(opCtx,
                                                 nullptr,
                                                 std::move(plan),
                                                 false,
                                                 nss,
                                                 true,
                                                 std::move(yieldPolicy),
                                                 boost::none,
                                                 nullptr,
                                                 nullptr,
                                                 nullptr,
                                                 MultipleCollectionAccessor{acquisition});
    }

    void assertCloseExcluded(ClosePath path) {
        unittest::ServerParameterGuard interval("overdueInterruptCheckIntervalMillis", 10);
        auto* opCtx = operationContext();
        auto exec = makeExecutor(path == ClosePath::kAlreadyExhausted,
                                 path == ClosePath::kDetachedDisposal);
        auto* ticks = static_cast<TickSourceMock<>*>(getServiceContext()->getTickSource());
        if (path == ClosePath::kDetachedDisposal) {
            exec->saveState();
            exec->detachFromOperationContext();
        }
        opCtx->trackOverdueInterruptChecks(ticks->getTicks());
        ticks->advance(Milliseconds{50});
        if (path == ClosePath::kDetachedDisposal) {
            exec->dispose(opCtx);
            ASSERT_FALSE(exec->getOpCtx());
        } else {
            BSONObj result;
            ASSERT_EQ(exec->getNext(&result, nullptr), PlanExecutor::ExecState::IS_EOF);
        }
        const auto* stats = operationContext()->overdueInterruptCheckStats();
        ASSERT_EQ(stats->overdueInterruptChecks.loadRelaxed(), 1);
        ASSERT_EQ(stats->overdueAccumulator.loadRelaxed(), Milliseconds{40});
        ASSERT_EQ(stage->getCommonStats()->closes, 1);
        opCtx->updateInterruptCheckCounters();
        ASSERT_EQ(stats->overdueInterruptChecks.loadRelaxed(), 1);
        exec->dispose(opCtx);
        ASSERT_EQ(stage->getCommonStats()->closes, 1);
    }

    CleanupStage* stage = nullptr;
};

TEST_F(PlanExecutorSBEDisposalTest, DetachedDisposalExcludesCleanupAndPreservesExecutionGap) {
    assertCloseExcluded(ClosePath::kDetachedDisposal);
}

TEST_F(PlanExecutorSBEDisposalTest, AlreadyExhaustedRootCloseExcludesCleanup) {
    assertCloseExcluded(ClosePath::kAlreadyExhausted);
}

TEST_F(PlanExecutorSBEDisposalTest, FetchEOFRootCloseExcludesCleanup) {
    assertCloseExcluded(ClosePath::kFetchedEOF);
}

}  // namespace
}  // namespace mongo
