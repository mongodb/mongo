/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/exec/queued_data_stage.h"
#include "mongo/db/exec/sort_key_generator.h"
#include "mongo/db/exec/working_set_computed_data.h"
#include "mongo/db/json.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {

BSONObj extractKeyFromKeyGenStage(SortKeyGeneratorStage* sortKeyGen, WorkingSet* workingSet) {
    WorkingSetID wsid;
    PlanStage::StageState state = PlanStage::NEED_TIME;
    while (state == PlanStage::NEED_TIME) {
        state = sortKeyGen->work(&wsid);
    }

    ASSERT_EQ(state, PlanStage::ADVANCED);
    auto wsm = workingSet->get(wsid);
    auto sortKeyComputedData =
        static_cast<const SortKeyComputedData*>(wsm->getComputed(WSM_SORT_KEY));
    return sortKeyComputedData->getSortKey();
}

/**
 * Given a JSON string 'sortSpec' representing a sort pattern, returns the corresponding sort key
 * from 'doc', a JSON string representation of a user document. Does so using the SORT_KEY_GENERATOR
 * stage.
 *
 * The 'collator' is used to specify the string comparison semantics that should be used when
 * generating the sort key.
 */
BSONObj extractSortKey(const char* sortSpec, const char* doc, const CollatorInterface* collator) {
    QueryTestServiceContext serviceContext;
    auto opCtx = serviceContext.makeOperationContext();

    WorkingSet workingSet;

    auto mockStage = stdx::make_unique<QueuedDataStage>(opCtx.get(), &workingSet);
    auto wsid = workingSet.allocate();
    auto wsm = workingSet.get(wsid);
    wsm->obj = Snapshotted<BSONObj>(SnapshotId(), fromjson(doc));
    wsm->transitionToOwnedObj();
    mockStage->pushBack(wsid);

    BSONObj sortPattern = fromjson(sortSpec);
    SortKeyGeneratorStage sortKeyGen{
        opCtx.get(), mockStage.release(), &workingSet, std::move(sortPattern), collator};
    return extractKeyFromKeyGenStage(&sortKeyGen, &workingSet);
}

/**
 * Given a JSON string 'sortSpec' representing a sort pattern, returns the corresponding sort key
 * from the index key 'ikd'. Does so using the SORT_KEY_GENERATOR stage.
 *
 * The 'collator' is used to specify the string comparison semantics that should be used when
 * generating the sort key.
 */
BSONObj extractSortKeyCovered(const char* sortSpec,
                              const IndexKeyDatum& ikd,
                              const CollatorInterface* collator) {
    QueryTestServiceContext serviceContext;
    auto opCtx = serviceContext.makeOperationContext();

    WorkingSet workingSet;

    auto mockStage = stdx::make_unique<QueuedDataStage>(opCtx.get(), &workingSet);
    auto wsid = workingSet.allocate();
    auto wsm = workingSet.get(wsid);
    wsm->keyData.push_back(ikd);
    workingSet.transitionToRecordIdAndIdx(wsid);
    mockStage->pushBack(wsid);

    BSONObj sortPattern = fromjson(sortSpec);
    SortKeyGeneratorStage sortKeyGen{
        opCtx.get(), mockStage.release(), &workingSet, std::move(sortPattern), collator};
    return extractKeyFromKeyGenStage(&sortKeyGen, &workingSet);
}

TEST(SortKeyGeneratorStageTest, SortKeyNormal) {
    BSONObj actualOut = extractSortKey("{a: 1}", "{_id: 0, a: 5}", nullptr);
    BSONObj expectedOut = BSON("" << 5);
    ASSERT_BSONOBJ_EQ(actualOut, expectedOut);
}

TEST(SortKeyGeneratorStageTest, SortKeyNormal2) {
    BSONObj actualOut = extractSortKey("{a: 1}", "{_id: 0, z: 10, a: 6, b: 16}", nullptr);
    BSONObj expectedOut = BSON("" << 6);
    ASSERT_BSONOBJ_EQ(actualOut, expectedOut);
}

TEST(SortKeyGeneratorStageTest, SortKeyString) {
    BSONObj actualOut =
        extractSortKey("{a: 1}", "{_id: 0, z: 'thing1', a: 'thing2', b: 16}", nullptr);
    BSONObj expectedOut = BSON(""
                               << "thing2");
    ASSERT_BSONOBJ_EQ(actualOut, expectedOut);
}

TEST(SortKeyGeneratorStageTest, SortKeyCompound) {
    BSONObj actualOut =
        extractSortKey("{a: 1, b: 1}", "{_id: 0, z: 'thing1', a: 99, c: {a: 4}, b: 16}", nullptr);
    BSONObj expectedOut = BSON("" << 99 << "" << 16);
    ASSERT_BSONOBJ_EQ(actualOut, expectedOut);
}

TEST(SortKeyGeneratorStageTest, SortKeyEmbedded) {
    BSONObj actualOut = extractSortKey(
        "{'c.a': 1, b: 1}", "{_id: 0, z: 'thing1', a: 99, c: {a: 4}, b: 16}", nullptr);
    BSONObj expectedOut = BSON("" << 4 << "" << 16);
    ASSERT_BSONOBJ_EQ(actualOut, expectedOut);
}

TEST(SortKeyGeneratorStageTest, SortKeyArray) {
    BSONObj actualOut = extractSortKey(
        "{'c': 1, b: 1}", "{_id: 0, z: 'thing1', a: 99, c: [2, 4, 1], b: 16}", nullptr);
    BSONObj expectedOut = BSON("" << 1 << "" << 16);
    ASSERT_BSONOBJ_EQ(actualOut, expectedOut);
}

TEST(SortKeyGeneratorStageTest, SortKeyCoveredNormal) {
    CollatorInterface* collator = nullptr;
    BSONObj actualOut = extractSortKeyCovered(
        "{a: 1}", IndexKeyDatum(BSON("a" << 1), BSON("" << 5), nullptr), collator);
    BSONObj expectedOut = BSON("" << 5);
    ASSERT_BSONOBJ_EQ(actualOut, expectedOut);
}

TEST(SortKeyGeneratorStageTest, SortKeyCoveredEmbedded) {
    CollatorInterface* collator = nullptr;
    BSONObj actualOut = extractSortKeyCovered(
        "{'a.c': 1}",
        IndexKeyDatum(BSON("a.c" << 1 << "c" << 1), BSON("" << 5 << "" << 6), nullptr),
        collator);
    BSONObj expectedOut = BSON("" << 5);
    ASSERT_BSONOBJ_EQ(actualOut, expectedOut);
}

TEST(SortKeyGeneratorStageTest, SortKeyCoveredCompound) {
    CollatorInterface* collator = nullptr;
    BSONObj actualOut = extractSortKeyCovered(
        "{a: 1, c: 1}",
        IndexKeyDatum(BSON("a" << 1 << "c" << 1), BSON("" << 5 << "" << 6), nullptr),
        collator);
    BSONObj expectedOut = BSON("" << 5 << "" << 6);
    ASSERT_BSONOBJ_EQ(actualOut, expectedOut);
}

TEST(SortKeyGeneratorStageTest, SortKeyCoveredCompound2) {
    CollatorInterface* collator = nullptr;
    BSONObj actualOut = extractSortKeyCovered("{a: 1, b: 1}",
                                              IndexKeyDatum(BSON("a" << 1 << "b" << 1 << "c" << 1),
                                                            BSON("" << 5 << "" << 6 << "" << 4),
                                                            nullptr),
                                              collator);
    BSONObj expectedOut = BSON("" << 5 << "" << 6);
    ASSERT_BSONOBJ_EQ(actualOut, expectedOut);
}

TEST(SortKeyGeneratorStageTest, SortKeyCoveredCompound3) {
    CollatorInterface* collator = nullptr;
    BSONObj actualOut =
        extractSortKeyCovered("{b: 1, c: 1}",
                              IndexKeyDatum(BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1),
                                            BSON("" << 5 << "" << 6 << "" << 4 << "" << 9000),
                                            nullptr),
                              collator);
    BSONObj expectedOut = BSON("" << 6 << "" << 4);
    ASSERT_BSONOBJ_EQ(actualOut, expectedOut);
}

TEST(SortKeyGeneratorStageTest, ExtractStringSortKeyWithCollatorUsesComparisonKey) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    BSONObj actualOut =
        extractSortKey("{a: 1}", "{_id: 0, z: 'thing1', a: 'thing2', b: 16}", &collator);
    BSONObj expectedOut = BSON(""
                               << "2gniht");
    ASSERT_BSONOBJ_EQ(actualOut, expectedOut);
}

TEST(SortKeyGeneratorStageTest, CollatorHasNoEffectWhenExtractingNonStringSortKey) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    BSONObj actualOut = extractSortKey("{a: 1}", "{_id: 0, z: 10, a: 6, b: 16}", &collator);
    BSONObj expectedOut = BSON("" << 6);
    ASSERT_BSONOBJ_EQ(actualOut, expectedOut);
}

TEST(SortKeyGeneratorStageTest, CollatorHasNoAffectWhenExtractingCoveredSortKey) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    BSONObj actualOut = extractSortKeyCovered("{b: 1}",
                                              IndexKeyDatum(BSON("a" << 1 << "b" << 1),
                                                            BSON("" << 4 << ""
                                                                    << "foo"),
                                                            nullptr),
                                              &collator);
    BSONObj expectedOut = BSON(""
                               << "foo");
    ASSERT_BSONOBJ_EQ(actualOut, expectedOut);
}

TEST(SortKeyGeneratorStageTest, SortKeyGenerationForArraysChoosesCorrectKey) {
    BSONObj actualOut = extractSortKey("{a: -1}", "{_id: 0, a: [1, 2, 3, 4]}", nullptr);
    BSONObj expectedOut = BSON("" << 4);
    ASSERT_BSONOBJ_EQ(actualOut, expectedOut);
}

TEST(SortKeyGeneratorStageTest, EnsureSortKeyGenerationForArraysRespectsCollation) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    BSONObj actualOut =
        extractSortKey("{a: 1}", "{_id: 0, a: ['aaz', 'zza', 'yya', 'zzb']}", &collator);
    BSONObj expectedOut = BSON(""
                               << "ayy");
    ASSERT_BSONOBJ_EQ(actualOut, expectedOut);
}

TEST(SortKeyGeneratorStageTest, SortKeyGenerationForArraysRespectsCompoundOrdering) {
    BSONObj actualOut = extractSortKey("{'a.b': 1, 'a.c': -1}",
                                       "{_id: 0, a: [{b: 1, c: 0}, {b: 0, c: 3}, {b: 0, c: 1}]}",
                                       nullptr);
    BSONObj expectedOut = BSON("" << 0 << "" << 3);
    ASSERT_BSONOBJ_EQ(actualOut, expectedOut);
}

}  // namespace
}  // namespace mongo
