/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/bson/json.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/stages/ix_scan.h"
#include "mongo/db/exec/sbe/stages/scan.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/multiple_collection_accessor.h"
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
    void _doTest() override {}

    const NamespaceString _nss =
        NamespaceString::createNamespaceString_forTest("testdb.sbe_scan_stage");
};

class ScanStageBenchmarkFixture : public benchmark::Fixture {
public:
    void simpleScan(benchmark::State& state, ScanDummyTest& dummyTest) {
        auto colls = dummyTest.createCollection(
            {fromjson("{_id: 0, a: 10}"), fromjson("{_id: 1, a: 20}")}, BSON("a" << 1));

        UUID uuid = colls.getMainCollection()->uuid();
        DatabaseName dbName = dummyTest._nss.dbName();
        auto [inputTag, inputVal] = stage_builder::makeValue(BSONArray());
        value::ValueGuard inputGuard{inputTag, inputVal};
        BSONArrayBuilder outputBab;
        outputBab << 10 << 20;
        auto [expectedTag, expectedVal] = stage_builder::makeValue(outputBab.arr());

        value::ValueGuard expectedGuard{expectedTag, expectedVal};
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
