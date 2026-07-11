// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/stages/generic_scan.h"
#include "mongo/db/exec/sbe/stages/scan.h"
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

    void TestBody() override {}

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
        dummyTest.attachCollectionAcquisition(colls);

        UUID uuid = colls.getMainCollection()->uuid();
        DatabaseName dbName = dummyTest._nss.dbName();
        value::TagValueOwned inputArr =
            value::TagValueOwned::fromRaw(stage_builder::makeValue(BSONArray()));

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
            dummyTest.runFast(inputArr.tag(), inputArr.value(), makeStageFn);
        }
    }

    void scan(benchmark::State& state, ScanDummyTest& dummyTest) {
        std::vector<BSONObj> docs;
        for (int i = 0; i < 10000; ++i) {
            docs.push_back(BSON("_id" << i << "a" << i));
        }
        auto colls = dummyTest.createCollection(docs, boost::none);
        dummyTest.attachCollectionAcquisition(colls);

        UUID uuid = colls.getMainCollection()->uuid();
        DatabaseName dbName = dummyTest._nss.dbName();
        value::TagValueOwned inputArr =
            value::TagValueOwned::fromRaw(stage_builder::makeValue(BSONArray()));

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
            dummyTest.runFast(inputArr.tag(), inputArr.value(), makeStageFn);
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

