// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/stages/exchange_gather.h"
#include "mongo/db/exec/sbe/stages/filter.h"
#include "mongo/db/exec/sbe/stages/generic_scan.h"
#include "mongo/db/exec/sbe/stages/scan.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_explainer_sbe.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

constexpr size_t kThousand = 1000;
constexpr size_t kMillion = kThousand * kThousand;

namespace mongo::sbe {

class ScanDummyTest : public PlanStageTestFixture {
public:
    void setUp() override {
        PlanStageTestFixture::setUp();
        size_t sizes[] = {100 * kThousand, 1 * kMillion, 5 * kMillion, 10 * kMillion};
        for (int64_t numDocs : sizes) {
            auto nss = NamespaceString::createNamespaceString_forTest("testdb.d" +
                                                                      std::format("{}", numDocs));
            std::vector<BSONObj> docs;
            docs.reserve(numDocs);
            for (int64_t i = 0; i < numDocs; ++i) {
                docs.push_back(BSON("_id" << i << "a" << i));
            }
            ASSERT_OK(
                storageInterface()->createCollection(operationContext(), nss, CollectionOptions()));
            std::vector<InsertStatement> inserts{docs.begin(), docs.end()};

            AutoGetCollection agc(operationContext(), nss, LockMode::MODE_IX);
            {
                WriteUnitOfWork wuow{operationContext()};
                ASSERT_OK(collection_internal::insertDocuments(operationContext(),
                                                               *agc,
                                                               inserts.begin(),
                                                               inserts.end(),
                                                               nullptr /* opDebug */));
                wuow.commit();
            }
        }
    }

    MultipleCollectionAccessor acquire(NamespaceString nss) {
        auto localColl =
            acquireCollection(operationContext(),
                              CollectionAcquisitionRequest::fromOpCtx(
                                  operationContext(), nss, AcquisitionPrerequisites::kRead),
                              MODE_IS);
        MultipleCollectionAccessor colls(localColl);
        return colls;
    }

    void TestBody() override {}
};

namespace {
/**
 * Returns the process-wide fixture, setting it up on first use.
 *
 * Google Benchmark re-runs a benchmark body while it searches for an iteration count (1, 10, 100,
 * ...) and once more per repetition, and the version vendored here has no once-per-benchmark Setup
 * callback, so a fixture built inside the body would restart the storage engine several times per
 * configuration. Besides being slow, that would leave WiredTiger with a cold cache for the first
 * measured iterations.
 *
 * The fixture is intentionally never torn down: running its tearDown() at static destruction time
 * would shut the service context down after the rest of the process has already started
 * unwinding. The OS reclaims everything at exit.
 */
ScanDummyTest& dbFixture() {
    static ScanDummyTest* fixture = [] {
        auto* f = new ScanDummyTest();
        f->setUp();
        return f;
    }();
    return *fixture;
}

std::unique_ptr<PlanStage> makeGenericScanStage(const UUID& uuid,
                                                const DatabaseName& dbName,
                                                value::SlotId scanSlot,
                                                value::SlotId recordIdSlot,
                                                value::SlotId fieldSlot) {
    return makeS<GenericScanStage>(uuid,
                                   dbName,
                                   scanSlot,
                                   recordIdSlot,
                                   /*snapshotIdSlot=*/boost::none,
                                   /*indexIdentSlot=*/boost::none,
                                   /*indexKeySlot=*/boost::none,
                                   /*indexKeyPatternSlot=*/boost::none,
                                   /*scanFieldNames=*/std::vector<std::string>{"a"},
                                   /*scanFieldSlots=*/sbe::value::SlotVector{fieldSlot},
                                   /*forward=*/true,
                                   /*yieldPolicy=*/nullptr,
                                   kEmptyPlanNodeId,
                                   /*scanOpenCallback=*/nullptr,
                                   /*participateInTrialRunTracking=*/false);
}

std::unique_ptr<PlanStage> makeScanStage(const UUID& uuid,
                                         const DatabaseName& dbName,
                                         value::SlotId scanSlot,
                                         value::SlotId recordIdSlot,
                                         value::SlotId fieldSlot) {
    return makeS<ScanStage>(uuid,
                            dbName,
                            scanSlot,
                            recordIdSlot,
                            /*snapshotIdSlot=*/boost::none,
                            /*indexIdentSlot=*/boost::none,
                            /*indexKeySlot=*/boost::none,
                            /*indexKeyPatternSlot=*/boost::none,
                            /*scanFieldNames=*/std::vector<std::string>{"a"},
                            /*scanFieldSlots=*/sbe::value::SlotVector{fieldSlot},
                            /*minRecordIdSlot=*/boost::none,
                            /*maxRecordIdSlot=*/boost::none,
                            /*forward=*/true,
                            /*yieldPolicy=*/nullptr,
                            kEmptyPlanNodeId,
                            /*scanOpenCallback=*/nullptr,
                            /*participateInTrialRunTracking=*/false);
}
/**
 * Returns the stats of 'root' in the same shape as the "executionStats" section of an explain run
 * with execution info, i.e. the summary fields explain pulls out of the root stage plus the whole
 * per-stage tree under "executionStages".
 *
 * This goes through the same helpers explain itself uses, so the output stays in sync with it.
 * Note that the per-stage timings only show up if the plan was built to collect them; see
 * PlanStage::_commonStats.executionTime and the 'collectTimingInfo' plumbing.
 */
BSONObj executionStatsOf(const PlanStage& root) {
    std::unique_ptr<PlanStageStats> stats = root.getStats(true /* includeDebugInfo */);
    PlanSummaryStats summary = collectExecutionStatsSummary(*stats);

    BSONObjBuilder bob;
    bob.appendNumber("nReturned", static_cast<long long>(summary.nReturned));
    appendExecutionTimeFields(bob, summary.executionTime);
    bob.appendNumber("totalKeysExamined", static_cast<long long>(summary.totalKeysExamined));
    bob.appendNumber("totalDocsExamined", static_cast<long long>(summary.totalDocsExamined));
    {
        BSONObjBuilder execBob(bob.subobjStart("executionStages"));
        statsToBSON(stats.get(), &execBob, &bob);
    }
    return bob.obj();
}
}  // namespace

class ScanStageBenchmarkFixture : public benchmark::Fixture {
public:
    /**
     * Populates a collection, then measures a collection scan built by 'makeScanStageFn', which is
     * called with the UUID and database name of the collection along with the slots the scan must
     * expose. The collection holds 'numDocs' documents. When 'dop' is greater than one, the scan is
     * wrapped in an ExchangeGather.
     */
    template <typename MakeScanStageFn>
    void runScanBenchmark(benchmark::State& state,
                          ScanDummyTest& dummyTest,
                          int numDocs,
                          size_t dop,
                          bool filter,
                          MakeScanStageFn makeScanStageFn) {
        auto nss =
            NamespaceString::createNamespaceString_forTest("testdb.d" + std::format("{}", numDocs));

        auto colls = dummyTest.acquire(nss);
        dummyTest.attachCollectionAcquisition(colls);

        UUID uuid = colls.getMainCollection()->uuid();
        DatabaseName dbName = nss.dbName();

        value::TagValueOwned inputArr =
            value::TagValueOwned::fromRaw(stage_builder::makeValue(BSONArray()));

        auto makeStageFn = [dop, filter, uuid, dbName, makeScanStageFn](
                               value::SlotId scanSlot,
                               std::unique_ptr<PlanStage> stage,
                               auto genSlotFn) {
            auto recordIdSlot = genSlotFn();
            auto fieldSlot = genSlotFn();
            std::unique_ptr<PlanStage> scanStage =
                makeScanStageFn(uuid, dbName, scanSlot, recordIdSlot, fieldSlot);

            if (filter) {
                scanStage = makeS<FilterStage<false>>(
                    std::move(scanStage),
                    makeE<EPrimBinary>(
                        EPrimBinary::eq,
                        makeE<EFunction>(EFn::kMod,
                                         makeEs(makeE<EVariable>(fieldSlot),
                                                makeE<EConstant>(value::TypeTags::NumberInt32,
                                                                 value::bitcastFrom<int32_t>(50)))),
                        makeE<EConstant>(value::TypeTags::NumberInt32,
                                         value::bitcastFrom<int32_t>(0))),
                    kEmptyPlanNodeId);
            }
            if (dop > 1) {
                scanStage = makeS<ExchangeGather>(std::move(scanStage),
                                                  value::SlotVector{scanSlot},
                                                  dop,
                                                  /*yieldPolicy=*/nullptr,
                                                  kEmptyPlanNodeId,
                                                  /*participateInTrialRunTracking=*/false);
            }
            return std::make_pair(scanSlot, std::move(scanStage));
        };

        // Run it once to get the plan with timings
        auto [counter, stage] = dummyTest.runFast(
            inputArr.tag(),
            inputArr.value(),
            [&](value::SlotId scanSlot, std::unique_ptr<PlanStage> stage, auto genSlotFn) {
                auto [slot, scanStage] = makeStageFn(scanSlot, std::move(stage), genSlotFn);
                scanStage->markShouldCollectTimingInfo(QueryExecTimerPrecision::kNanos);
                return std::make_pair(slot, std::move(scanStage));
            });
        if (stage) {
            LOGV2(13436600,
                  "Plan metrics",
                  "statsOfLastExecution"_attr =
                      executionStatsOf(*stage).jsonString(ExtendedRelaxedV2_0_0, /*pretty=*/false));
        }
        for (auto _ : state) {
            auto [count, plan] = dummyTest.runFast(inputArr.tag(), inputArr.value(), makeStageFn);
            ASSERT_EQ(count, filter ? numDocs / 50 : numDocs);
        }
    }

    /**
     * Runs the benchmark configuration encoded in 'state's arguments: whether to use
     * GenericScanStage or ScanStage, the number of documents in the collection, the degree of
     * parallelism, and whether to apply a filter.
     */
    void runBenchmark(benchmark::State& state) {
        const bool generic = state.range(0) != 0;
        const auto numDocs = static_cast<int>(state.range(1));
        const bool filter = state.range(2) != 0;
        const auto dop = static_cast<size_t>(state.range(3));

        ScanDummyTest& dummyTest = dbFixture();
        if (generic) {
            runScanBenchmark(state, dummyTest, numDocs, dop, filter, makeGenericScanStage);
        } else {
            runScanBenchmark(state, dummyTest, numDocs, dop, filter, makeScanStage);
        }
    }
};
BENCHMARK_DEFINE_F(ScanStageBenchmarkFixture, Scan)(benchmark::State& state) {
    runBenchmark(state);
}

BENCHMARK_REGISTER_F(ScanStageBenchmarkFixture, Scan)
    ->ArgNames({"generic", "numDocs", "filter", "dop"})
    ->ArgsProduct({{0, 1},
                   {100 * kThousand, 1 * kMillion, 5 * kMillion, 10 * kMillion},
                   {0, 1},
                   {1, 2, 4, 8}})
    ->UseRealTime()
    ->MeasureProcessCPUTime();
}  // namespace mongo::sbe

