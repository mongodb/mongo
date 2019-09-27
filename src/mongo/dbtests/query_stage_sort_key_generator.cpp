/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include <memory>

#include "mongo/db/exec/queued_data_stage.h"
#include "mongo/db/exec/sort_key_generator.h"
#include "mongo/db/json.h"
#include "mongo/db/pipeline/document_value_test_util.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {

Value extractKeyFromKeyGenStage(SortKeyGeneratorStage* sortKeyGen, WorkingSet* workingSet) {
    WorkingSetID wsid;
    PlanStage::StageState state = PlanStage::NEED_TIME;
    while (state == PlanStage::NEED_TIME) {
        state = sortKeyGen->work(&wsid);
    }

    ASSERT_EQ(state, PlanStage::ADVANCED);
    auto wsm = workingSet->get(wsid);
    return wsm->metadata().getSortKey();
}

/**
 * Given a JSON string 'sortSpec' representing a sort pattern, returns the corresponding sort key
 * from 'doc', a JSON string representation of a user document. Does so using the SORT_KEY_GENERATOR
 * stage.
 *
 * The 'collator' is used to specify the string comparison semantics that should be used when
 * generating the sort key.
 */
Value extractSortKey(const char* sortSpec, const char* doc, const CollatorInterface* collator) {
    QueryTestServiceContext serviceContext;
    auto opCtx = serviceContext.makeOperationContext();
    boost::intrusive_ptr<ExpressionContext> pExpCtx(new ExpressionContext(opCtx.get(), collator));

    WorkingSet workingSet;

    auto mockStage = std::make_unique<QueuedDataStage>(opCtx.get(), &workingSet);
    auto wsid = workingSet.allocate();
    auto wsm = workingSet.get(wsid);
    wsm->doc = {SnapshotId(), Document{fromjson(doc)}};
    wsm->transitionToOwnedObj();
    mockStage->pushBack(wsid);

    BSONObj sortPattern = fromjson(sortSpec);
    SortKeyGeneratorStage sortKeyGen{
        pExpCtx, std::move(mockStage), &workingSet, std::move(sortPattern)};
    return extractKeyFromKeyGenStage(&sortKeyGen, &workingSet);
}

/**
 * Given a JSON string 'sortSpec' representing a sort pattern, returns the corresponding sort key
 * from the index key 'ikd'. Does so using the SORT_KEY_GENERATOR stage.
 *
 * The 'collator' is used to specify the string comparison semantics that should be used when
 * generating the sort key.
 */
Value extractSortKeyCovered(const char* sortSpec,
                            const IndexKeyDatum& ikd,
                            const CollatorInterface* collator) {
    QueryTestServiceContext serviceContext;
    auto opCtx = serviceContext.makeOperationContext();
    boost::intrusive_ptr<ExpressionContext> pExpCtx(new ExpressionContext(opCtx.get(), collator));

    WorkingSet workingSet;

    auto mockStage = std::make_unique<QueuedDataStage>(opCtx.get(), &workingSet);
    auto wsid = workingSet.allocate();
    auto wsm = workingSet.get(wsid);
    wsm->keyData.push_back(ikd);
    workingSet.transitionToRecordIdAndIdx(wsid);
    mockStage->pushBack(wsid);

    BSONObj sortPattern = fromjson(sortSpec);
    SortKeyGeneratorStage sortKeyGen{
        pExpCtx, std::move(mockStage), &workingSet, std::move(sortPattern)};
    return extractKeyFromKeyGenStage(&sortKeyGen, &workingSet);
}

TEST(SortKeyGeneratorStageTest, SortKeyNormal) {
    Value actualOut = extractSortKey("{a: 1}", "{_id: 0, a: 5}", nullptr);
    Value expectedOut(5);
    ASSERT_VALUE_EQ(actualOut, expectedOut);
}

TEST(SortKeyGeneratorStageTest, SortKeyNormal2) {
    Value actualOut = extractSortKey("{a: 1}", "{_id: 0, z: 10, a: 6, b: 16}", nullptr);
    Value expectedOut(6);
    ASSERT_VALUE_EQ(actualOut, expectedOut);
}

TEST(SortKeyGeneratorStageTest, SortKeyString) {
    Value actualOut =
        extractSortKey("{a: 1}", "{_id: 0, z: 'thing1', a: 'thing2', b: 16}", nullptr);
    Value expectedOut("thing2"_sd);
    ASSERT_VALUE_EQ(actualOut, expectedOut);
}

TEST(SortKeyGeneratorStageTest, SortKeyCompound) {
    Value actualOut =
        extractSortKey("{a: 1, b: 1}", "{_id: 0, z: 'thing1', a: 99, c: {a: 4}, b: 16}", nullptr);
    Value expectedOut(std::vector<Value>{Value(99), Value(16)});
    ASSERT_VALUE_EQ(actualOut, expectedOut);
}

TEST(SortKeyGeneratorStageTest, SortKeyEmbedded) {
    Value actualOut = extractSortKey(
        "{'c.a': 1, b: 1}", "{_id: 0, z: 'thing1', a: 99, c: {a: 4}, b: 16}", nullptr);
    Value expectedOut = Value(std::vector<Value>{Value(4), Value(16)});
    ASSERT_VALUE_EQ(actualOut, expectedOut);
}

TEST(SortKeyGeneratorStageTest, SortKeyArray) {
    Value actualOut = extractSortKey(
        "{'c': 1, b: 1}", "{_id: 0, z: 'thing1', a: 99, c: [2, 4, 1], b: 16}", nullptr);
    Value expectedOut(std::vector<Value>{Value(1), Value(16)});
    ASSERT_VALUE_EQ(actualOut, expectedOut);
}

TEST(SortKeyGeneratorStageTest, SortKeyCoveredNormal) {
    CollatorInterface* collator = nullptr;
    Value actualOut = extractSortKeyCovered(
        "{a: 1}", IndexKeyDatum(BSON("a" << 1), BSON("" << 5), 0, SnapshotId{}), collator);
    Value expectedOut({Value(5)});
    ASSERT_VALUE_EQ(actualOut, expectedOut);
}

TEST(SortKeyGeneratorStageTest, SortKeyCoveredEmbedded) {
    CollatorInterface* collator = nullptr;
    Value actualOut = extractSortKeyCovered(
        "{'a.c': 1}",
        IndexKeyDatum(BSON("a.c" << 1 << "c" << 1), BSON("" << 5 << "" << 6), 0, SnapshotId{}),
        collator);
    Value expectedOut(5);
    ASSERT_VALUE_EQ(actualOut, expectedOut);
}

TEST(SortKeyGeneratorStageTest, SortKeyCoveredCompound) {
    CollatorInterface* collator = nullptr;
    Value actualOut = extractSortKeyCovered(
        "{a: 1, c: 1}",
        IndexKeyDatum(BSON("a" << 1 << "c" << 1), BSON("" << 5 << "" << 6), 0, SnapshotId{}),
        collator);
    Value expectedOut(std::vector<Value>{Value(5), Value(6)});
    ASSERT_VALUE_EQ(actualOut, expectedOut);
}

TEST(SortKeyGeneratorStageTest, SortKeyCoveredCompound2) {
    CollatorInterface* collator = nullptr;
    Value actualOut = extractSortKeyCovered("{a: 1, b: 1}",
                                            IndexKeyDatum(BSON("a" << 1 << "b" << 1 << "c" << 1),
                                                          BSON("" << 5 << "" << 6 << "" << 4),
                                                          0,
                                                          SnapshotId{}),
                                            collator);
    Value expectedOut(std::vector<Value>{Value(5), Value(6)});
    ASSERT_VALUE_EQ(actualOut, expectedOut);
}

TEST(SortKeyGeneratorStageTest, SortKeyCoveredCompound3) {
    CollatorInterface* collator = nullptr;
    Value actualOut =
        extractSortKeyCovered("{b: 1, c: 1}",
                              IndexKeyDatum(BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1),
                                            BSON("" << 5 << "" << 6 << "" << 4 << "" << 9000),
                                            0,
                                            SnapshotId{}),
                              collator);
    Value expectedOut(std::vector<Value>{Value(6), Value(4)});
    ASSERT_VALUE_EQ(actualOut, expectedOut);
}

TEST(SortKeyGeneratorStageTest, ExtractStringSortKeyWithCollatorUsesComparisonKey) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    Value actualOut =
        extractSortKey("{a: 1}", "{_id: 0, z: 'thing1', a: 'thing2', b: 16}", &collator);
    Value expectedOut = Value("2gniht"_sd);
    ASSERT_VALUE_EQ(actualOut, expectedOut);
}

TEST(SortKeyGeneratorStageTest, CollatorHasNoEffectWhenExtractingNonStringSortKey) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    Value actualOut = extractSortKey("{a: 1}", "{_id: 0, z: 10, a: 6, b: 16}", &collator);
    Value expectedOut = Value(6);
    ASSERT_VALUE_EQ(actualOut, expectedOut);
}

TEST(SortKeyGeneratorStageTest, CollatorAppliesWhenExtractingCoveredSortKeyString) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    Value actualOut = extractSortKeyCovered("{b: 1}",
                                            IndexKeyDatum(BSON("a" << 1 << "b" << 1),
                                                          BSON("" << 4 << ""
                                                                  << "foo"),
                                                          0,
                                                          SnapshotId{}),
                                            &collator);
    Value expectedOut = Value("oof"_sd);
    ASSERT_VALUE_EQ(actualOut, expectedOut);
}

TEST(SortKeyGeneratorStageTest, SortKeyGenerationForArraysChoosesCorrectKey) {
    Value actualOut = extractSortKey("{a: -1}", "{_id: 0, a: [1, 2, 3, 4]}", nullptr);
    Value expectedOut(4);
    ASSERT_VALUE_EQ(actualOut, expectedOut);
}

TEST(SortKeyGeneratorStageTest, EnsureSortKeyGenerationForArraysRespectsCollation) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    Value actualOut =
        extractSortKey("{a: 1}", "{_id: 0, a: ['aaz', 'zza', 'yya', 'zzb']}", &collator);
    Value expectedOut("ayy"_sd);
    ASSERT_VALUE_EQ(actualOut, expectedOut);
}

TEST(SortKeyGeneratorStageTest, SortKeyGenerationForArraysRespectsCompoundOrdering) {
    Value actualOut = extractSortKey("{'a.b': 1, 'a.c': -1}",
                                     "{_id: 0, a: [{b: 1, c: 0}, {b: 0, c: 3}, {b: 0, c: 1}]}",
                                     nullptr);
    Value expectedOut(std::vector<Value>{Value(0), Value(3)});
    ASSERT_VALUE_EQ(actualOut, expectedOut);
}

}  // namespace
}  // namespace mongo
