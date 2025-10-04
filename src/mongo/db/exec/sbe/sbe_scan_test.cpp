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

/**
 * This file contains minimal tests for sbe::ScanStage and sbe::ParallelScanStage.
 */

#include "mongo/base/string_data.h"
#include "mongo/bson/json.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::sbe {

class ScanStageTest : public PlanStageTestFixture {
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

protected:
    const NamespaceString _nss =
        NamespaceString::createNamespaceString_forTest("testdb.sbe_scan_stage");
};

TEST_F(ScanStageTest, scanStage) {
    auto colls =
        createCollection({fromjson("{_id: 0, a: 1}"), fromjson("{_id: 1, a: 2}")}, boost::none);
    UUID uuid = colls.getMainCollection()->uuid();
    DatabaseName dbName = _nss.dbName();

    auto [inputTag, inputVal] = stage_builder::makeValue(BSONArray());
    value::ValueGuard inputGuard{inputTag, inputVal};
    auto [expectedTag, expectedVal] = stage_builder::makeValue(
        BSON_ARRAY(BSON("_id" << 0 << "a" << 1) << BSON("_id" << 1 << "a" << 2)));
    value::ValueGuard expectedGuard{expectedTag, expectedVal};

    auto makeStageFn = [uuid, dbName](value::SlotId scanSlot, std::unique_ptr<PlanStage> stage) {
        sbe::value::SlotVector scanFieldSlots;
        auto scanStage = sbe::makeS<sbe::ScanStage>(uuid,
                                                    dbName,
                                                    scanSlot,
                                                    boost::none /* recordIdSlot */,
                                                    boost::none /* snapshotIdSlot */,
                                                    boost::none /* indexIdentSlot */,
                                                    boost::none /* indexKeySlot */,
                                                    boost::none /* indexKeyPatternSlot */,
                                                    std::vector<std::string>{} /* scanFieldNames */,
                                                    scanFieldSlots,
                                                    boost::none /* seekRecordIdSlot */,
                                                    boost::none /* minRecordIdSlot */,
                                                    boost::none /* maxRecordIdSlot */,
                                                    true /* forward */,
                                                    nullptr /* yieldPolicy */,
                                                    kEmptyPlanNodeId,
                                                    ScanCallbacks{},
                                                    false /* useRandomCursor */,
                                                    false /* participateInTrialRunTracking */,
                                                    false /* includeScanStartRecordId */,
                                                    false /* includeScanEndRecordId */);

        return std::make_pair(scanSlot, std::move(scanStage));
    };

    inputGuard.reset();
    expectedGuard.reset();
    runTest(inputTag, inputVal, expectedTag, expectedVal, makeStageFn);
}

TEST_F(ScanStageTest, ParallelScanStage) {
    auto colls =
        createCollection({fromjson("{_id: 0, a: 1}"), fromjson("{_id: 1, a: 2}")}, boost::none);
    UUID uuid = colls.getMainCollection()->uuid();
    DatabaseName dbName = _nss.dbName();

    auto [inputTag, inputVal] = stage_builder::makeValue(BSONArray());
    value::ValueGuard inputGuard{inputTag, inputVal};
    auto [expectedTag, expectedVal] = stage_builder::makeValue(
        BSON_ARRAY(BSON("_id" << 0 << "a" << 1) << BSON("_id" << 1 << "a" << 2)));
    value::ValueGuard expectedGuard{expectedTag, expectedVal};

    auto makeStageFn = [uuid, dbName](value::SlotId scanSlot, std::unique_ptr<PlanStage> stage) {
        sbe::value::SlotVector scanFieldSlots;
        auto scanStage =
            sbe::makeS<sbe::ParallelScanStage>(uuid,
                                               dbName,
                                               scanSlot,
                                               boost::none /* recordIdSlot */,
                                               boost::none /* snapshotIdSlot */,
                                               boost::none /* indexIdentSlot */,
                                               boost::none /* indexKeySlot */,
                                               boost::none /* indexKeyPatternSlot */,
                                               std::vector<std::string>{} /* scanFieldNames */,
                                               scanFieldSlots,
                                               nullptr /* yieldPolicy */,
                                               kEmptyPlanNodeId,
                                               ScanCallbacks{},
                                               false /* participateInTrialRunTracking */
            );
        return std::make_pair(scanSlot, std::move(scanStage));
    };

    inputGuard.reset();
    expectedGuard.reset();
    runTest(inputTag, inputVal, expectedTag, expectedVal, makeStageFn);
}

}  // namespace mongo::sbe
