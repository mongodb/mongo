/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
#include "mongo/db/exec/sbe/stages/generic_scan.h"
#include "mongo/db/exec/sbe/stages/scan.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
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

    void _doTest() override {}

    const NamespaceString _nss =
        NamespaceString::createNamespaceString_forTest("testdb.sbe_scan_stage");
};

class ScanStageBenchmarkFixture : public benchmark::Fixture {
public:
    void genericScan(benchmark::State& state, ScanDummyTest& dummyTest) {
        std::vector<BSONObj> docs;
        for (int i = 0; i < 10000; ++i) {
            docs.push_back(BSON("_id" << i << "a" << i));
        }
        auto colls = dummyTest.createCollection(docs, boost::none);

        UUID uuid = colls.getMainCollection()->uuid();
        DatabaseName dbName = dummyTest._nss.dbName();
        auto [inputTag, inputVal] = stage_builder::makeValue(BSONArray());
        value::ValueGuard inputGuard{inputTag, inputVal};

        auto makeStageFn = [uuid, dbName](value::SlotId scanSlot,
                                          std::unique_ptr<PlanStage> stage,
                                          auto genSlotFn) {
            sbe::value::SlotVector scanFieldSlots;
            auto recordIdSlot = genSlotFn();
            auto scanStage =
                sbe::makeS<sbe::GenericScanStage>(uuid,
                                                  dbName,
                                                  scanSlot,
                                                  recordIdSlot,
                                                  boost::none /* snapshotIdSlot */,
                                                  boost::none /* indexIdentSlot */,
                                                  boost::none /* indexKeySlot */,
                                                  boost::none /* indexKeyPatternSlot */,
                                                  std::vector<std::string>{} /* scanFieldNames */,
                                                  scanFieldSlots,
                                                  true /* forward */,
                                                  nullptr /* yieldPolicy */,
                                                  kEmptyPlanNodeId,
                                                  nullptr /* scanOpenCallback */,
                                                  false /* participateInTrialRunTracking */);

            return std::make_pair(scanSlot, std::move(scanStage));
        };

        for (auto _ : state) {
            dummyTest.runFast(inputTag, inputVal, makeStageFn);
        }
    }

    void scan(benchmark::State& state, ScanDummyTest& dummyTest) {
        std::vector<BSONObj> docs;
        for (int i = 0; i < 10000; ++i) {
            docs.push_back(BSON("_id" << i << "a" << i));
        }
        auto colls = dummyTest.createCollection(docs, boost::none);

        UUID uuid = colls.getMainCollection()->uuid();
        DatabaseName dbName = dummyTest._nss.dbName();
        auto [inputTag, inputVal] = stage_builder::makeValue(BSONArray());
        value::ValueGuard inputGuard{inputTag, inputVal};

        auto makeStageFn = [uuid, dbName](value::SlotId scanSlot,
                                          std::unique_ptr<PlanStage> stage,
                                          auto genSlotFn) {
            sbe::value::SlotVector scanFieldSlots;
            auto recordIdSlot = genSlotFn();
            auto scanStage =
                sbe::makeS<sbe::ScanStage>(uuid,
                                           dbName,
                                           scanSlot,
                                           recordIdSlot,
                                           boost::none /* snapshotIdSlot */,
                                           boost::none /* indexIdentSlot */,
                                           boost::none /* indexKeySlot */,
                                           boost::none /* indexKeyPatternSlot */,
                                           std::vector<std::string>{} /* scanFieldNames */,
                                           scanFieldSlots,
                                           boost::none /* minRecordIdSlot */,
                                           boost::none /* maxRecordIdSlot */,
                                           true /* forward */,
                                           nullptr /* yieldPolicy */,
                                           kEmptyPlanNodeId,
                                           nullptr /* scanOpenCallback */,
                                           false /* participateInTrialRunTracking */);

            return std::make_pair(scanSlot, std::move(scanStage));
        };

        for (auto _ : state) {
            dummyTest.runFast(inputTag, inputVal, makeStageFn);
        }
    }

    void runBenchmark(benchmark::State& state) {
        ScanDummyTest dummyTest;
        dummyTest.setUp();
        genericScan(state, dummyTest);
        dummyTest.tearDown();
    }

    void runBenchmarkScan(benchmark::State& state) {
        ScanDummyTest dummyTest;
        dummyTest.setUp();
        scan(state, dummyTest);
        dummyTest.tearDown();
    }
};
BENCHMARK_F(ScanStageBenchmarkFixture, GenericScanSmall)(benchmark::State& state) {
    runBenchmark(state);
}

BENCHMARK_F(ScanStageBenchmarkFixture, ScanSmall)(benchmark::State& state) {
    runBenchmarkScan(state);
}
}  // namespace mongo::sbe

