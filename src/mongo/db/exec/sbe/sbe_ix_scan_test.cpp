// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/stages/ix_scan.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_entry.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

#include <string>
#include <utility>
#include <variant>

namespace mongo::sbe {
namespace {

using IndexScanStageTest = PlanStageTestFixture;

// Regression test for SERVER-134335: outstanding accessors have to be reset (and thus contain
// Nothing) when keys contain different component counts.
TEST_F(IndexScanStageTest, WithIxComponentSizeVariance) {
    auto nss = NamespaceString::createNamespaceString_forTest("testdb.sbe_ix_scan_test");
    auto opCtx = operationContext();

    ASSERT_OK(storageInterface()->createCollection(opCtx, nss, CollectionOptions()));
    ASSERT_OK(storageInterface()->createIndexesOnEmptyCollection(
        opCtx,
        nss,
        {BSON("v" << 2 << "name" << "a_1_b_1" << "key" << BSON("a" << 1 << "b" << 1))}));

    {
        AutoGetCollection autoColl(opCtx, nss, MODE_IX);
        auto entry = autoColl->getIndexCatalog()->findIndexByName(opCtx, "a_1_b_1");
        auto sdi = entry->accessMethod()->asSortedData()->getSortedDataInterface();
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx);

        // Key with 2 components.
        key_string::Builder longKey(sdi->getKeyStringVersion(), sdi->getOrdering());
        longKey.appendString(std::string(200, 'A'));
        longKey.appendString(std::string(75, 'B'));
        longKey.appendRecordId(RecordId{1});

        // Key with 1 component.
        key_string::Builder shortKey(sdi->getKeyStringVersion(), sdi->getOrdering());
        shortKey.appendString(std::string(60, 'C'));
        shortKey.appendRecordId(RecordId{2});

        WriteUnitOfWork wuow(opCtx);
        ASSERT_OK(std::get<Status>(sdi->insert(opCtx, ru, longKey.getValueCopy(), false)));
        ASSERT_OK(std::get<Status>(sdi->insert(opCtx, ru, shortKey.getValueCopy(), false)));
        wuow.commit();
    }

    auto coll = acquireCollection(
        opCtx,
        CollectionAcquisitionRequest::fromOpCtx(opCtx, nss, AcquisitionPrerequisites::kRead),
        MODE_IS);
    MultipleCollectionAccessor mca(coll);
    attachCollectionAcquisition(mca);

    auto ctx = makeCompileCtx();
    auto varA = generateSlotId();
    auto varB = generateSlotId();
    IndexKeysInclusionSet indexKeysToInclude;
    indexKeysToInclude.set(0);
    indexKeysToInclude.set(1);

    auto stage = makeS<SimpleIndexScanStage>(coll.uuid(),
                                             nss.dbName(),
                                             "a_1_b_1",
                                             true /* forward */,
                                             boost::none /* indexKeySlot */,
                                             boost::none /* recordIdSlot */,
                                             boost::none /* snapshotIdSlot */,
                                             boost::none /* indexIdentSlot */,
                                             indexKeysToInclude,
                                             makeSV(varA, varB),
                                             nullptr /* seekKeyLow */,
                                             nullptr /* seekKeyHigh */,
                                             getYieldPolicy(),
                                             kEmptyPlanNodeId);

    auto accessors = prepareTree(ctx.get(), stage.get(), makeSV(varA, varB));

    // Read first key.
    ASSERT_EQ(stage->getNext(), PlanState::ADVANCED);
    auto [aTag1, aVal1] = accessors[0]->getViewOfValue();
    auto [bTag1, bVal1] = accessors[1]->getViewOfValue();
    ASSERT(value::isString(aTag1));
    ASSERT(value::isString(bTag1));
    ASSERT_EQ(value::getStringView(aTag1, aVal1), std::string(200, 'A'));
    ASSERT_EQ(value::getStringView(bTag1, bVal1), std::string(75, 'B'));

    // Read second key.
    ASSERT_EQ(stage->getNext(), PlanState::ADVANCED);
    auto [aTag2, aVal2] = accessors[0]->getViewOfValue();
    auto [bTag2, bVal2] = accessors[1]->getViewOfValue();
    ASSERT(value::isString(aTag2));
    ASSERT_EQ(value::getStringView(aTag2, aVal2), std::string(60, 'C'));
    ASSERT_EQ(value::TypeTags::Nothing, bTag2);

    stage->close();
}

}  // namespace
}  // namespace mongo::sbe
