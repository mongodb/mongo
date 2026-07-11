// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/json.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/stages/ix_scan.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::sbe {

// Dummy test fixture to run benchmarks on SBE ScanStage.
// PlanStageTextFixture provides the necessary setup and utilities to run SBE plan stages.
// However, ScanStageBenchmarkFixture cannot directly inherit from PlanStageTestFixture because
// it causes issues in the benchmark framework.
class ScanDummyTest : public PlanStageTestFixture {
public:
    void insertDocuments(const std::vector<BSONObj>& docs) {
        std::vector<InsertStatement> inserts{docs.begin(), docs.end()};

        AutoGetCollection agc(operationContext(), _nss, LockMode::MODE_IX);
        {
            WriteUnitOfWork wuow{operationContext()};
            ASSERT_OK(collection_internal::insertDocuments(
                operationContext(), *agc, inserts.begin(), inserts.end(), nullptr /* opDebug */));
            wuow.commit();
        }
    }

    MultipleCollectionAccessor createCollection(const std::vector<BSONObj>& docs,
                                                boost::optional<BSONObj> indexKeyPattern) {
        // Create collection and index
        ASSERT_OK(
            storageInterface()->createCollection(operationContext(), _nss, CollectionOptions()));
        if (indexKeyPattern) {
            ASSERT_OK(storageInterface()->createIndexesOnEmptyCollection(
                operationContext(),
                _nss,
                {BSON("v" << 2 << "name" << DBClientBase::genIndexName(*indexKeyPattern) << "key"
                          << *indexKeyPattern)}));
        }
        insertDocuments(docs);

        auto localColl =
            acquireCollection(operationContext(),
                              CollectionAcquisitionRequest::fromOpCtx(
                                  operationContext(), _nss, AcquisitionPrerequisites::kRead),
                              MODE_IS);
        MultipleCollectionAccessor colls(localColl);
        return colls;
    }

    // virtual function needs to be defined
    void TestBody() override {}

    const NamespaceString _nss =
        NamespaceString::createNamespaceString_forTest("testdb.sbe_scan_stage");
};

class ScanStageBenchmarkFixture : public benchmark::Fixture {
public:
    void simpleScan(benchmark::State& state, ScanDummyTest& dummyTest) {
        auto colls = dummyTest.createCollection(
            {fromjson("{_id: 0, a: 10}"), fromjson("{_id: 1, a: 20}")}, BSON("a" << 1));
        dummyTest.attachCollectionAcquisition(colls);

        UUID uuid = colls.getMainCollection()->uuid();
        DatabaseName dbName = dummyTest._nss.dbName();
        auto [inputTag, inputVal] = stage_builder::makeValue(BSONArray());
        value::TagValueOwned inputOwned = value::TagValueOwned::fromRaw(inputTag, inputVal);
        BSONArrayBuilder outputBab;
        outputBab << 10 << 20;
        auto [expectedTag, expectedVal] = stage_builder::makeValue(outputBab.arr());

        value::TagValueOwned expectedOwned =
            value::TagValueOwned::fromRaw(expectedTag, expectedVal);
        auto makeStageFn = [uuid, dbName](value::SlotId scanSlot,
                                          std::unique_ptr<PlanStage> stage) {
            // Create a slot for the index key and record ID
            value::SlotId indexKeySlot = 0;
            value::SlotId recordIdSlot = 0;

            // Define which index key fields to project (here we want field "a")
            IndexKeysInclusionSet indexKeysToInclude;
            indexKeysToInclude.flip(0);        // Include first field of index
            value::SlotVector vars{scanSlot};  // Slot to receive the projected field value

            // Create seek keys for the index scan (full range scan in this case)
            auto seekKeyLow =
                makeE<EConstant>(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0));
            auto seekKeyHigh =
                makeE<EConstant>(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(3));

            auto scanStage =
                sbe::makeS<sbe::SimpleIndexScanStage>(uuid,
                                                      dbName,
                                                      "a_1",  // Index name from BSON("a" << 1)
                                                      true,   // forward
                                                      indexKeySlot,
                                                      recordIdSlot,
                                                      boost::none,  // snapshotIdSlot
                                                      boost::none,  // indexIdentSlot
                                                      indexKeysToInclude,
                                                      vars,
                                                      nullptr,  // seekKeyLow
                                                      nullptr,  // seekKeyHigh
                                                      nullptr,  // yieldPolicy
                                                      kEmptyPlanNodeId);

            return std::make_pair(scanSlot, std::move(scanStage));
        };

        for (auto _ : state) {
            // ownership of input and expected values is transferred to runTest
            // so we need to make copies for each iteration
            auto [inputCopyTag, inputCopyVal] = value::copyValue(inputTag, inputVal);
            auto [expectedCopyTag, expectedCopyVal] = value::copyValue(expectedTag, expectedVal);
            dummyTest.runTest(
                inputCopyTag, inputCopyVal, expectedCopyTag, expectedCopyVal, makeStageFn);
        }
    }

    void runBenchmark(benchmark::State& state) {
        ScanDummyTest dummyTest;
        dummyTest.setUp();
        // need a separate scope to ensure teardown is called after benchmark
        simpleScan(state, dummyTest);
        dummyTest.tearDown();
    }
};
// Benchmark: simple scan over a very small collection (2 docs) to measure per-row overhead.
BENCHMARK_F(ScanStageBenchmarkFixture, SimpleScanSmall)(benchmark::State& state) {
    runBenchmark(state);
}

}  // namespace mongo::sbe
