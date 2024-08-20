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

#include <memory>
#include <utility>

#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/index/sort_key_generator.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/intrusive_counter.h"

namespace mongo {
namespace {

// A method to create mock ExpressionContexts with a specified collation
std::unique_ptr<SortKeyGenerator> makeSortKeyGen(const BSONObj& sortSpec,
                                                 const CollatorInterface* collator) {
    boost::intrusive_ptr<ExpressionContext> pExpCtx(new ExpressionContextForTest());
    pExpCtx->setCollator(CollatorInterface::cloneCollator(collator));
    SortPattern sortPattern{sortSpec, pExpCtx};
    return std::make_unique<SortKeyGenerator>(std::move(sortPattern), collator);
}

TEST(SortKeyGeneratorTest, ExtractNumberKeyForNonCompoundSortNonNested) {
    auto sortKeyGen = makeSortKeyGen(BSON("a" << 1), nullptr);
    auto sortKey = sortKeyGen->computeSortKeyFromDocument(Document{{"_id", 5}, {"a", 5}});
    ASSERT_VALUE_EQ(sortKey, Value{5});
}

TEST(SortKeyGeneratorTest, ExtractNumberKeyFromDocWithSeveralFields) {
    auto sortKeyGen = makeSortKeyGen(BSON("a" << 1), nullptr);
    auto sortKey = sortKeyGen->computeSortKeyFromDocument(
        Document{{"_id", 0}, {"z", 10}, {"a", 6}, {"b", 16}});
    ASSERT_VALUE_EQ(sortKey, Value{6});
}

TEST(SortKeyGeneratorTest, ExtractStringKeyNonCompoundNonNested) {
    auto sortKeyGen = makeSortKeyGen(BSON("a" << 1), nullptr);
    auto sortKey = sortKeyGen->computeSortKeyFromDocument(
        Document{{"_id", 0}, {"z", "thing1"_sd}, {"a", "thing2"_sd}, {"b", 16}});
    ASSERT_VALUE_EQ(sortKey, Value{"thing2"_sd});
}

TEST(SortKeyGeneratorTest, CompoundSortPattern) {
    auto sortKeyGen = makeSortKeyGen(BSON("a" << 1 << "b" << 1), nullptr);
    auto sortKey = sortKeyGen->computeSortKeyFromDocument(
        Document{{"_id", 0}, {"z", "thing1"_sd}, {"a", 99}, {"c", Document{{"a", 4}}}, {"b", 16}});
    ASSERT_VALUE_EQ(sortKey, (Value{std::vector<Value>{Value{99}, Value{16}}}));
}

TEST(SortKeyGeneratorTest, CompoundSortPatternWithDottedPath) {
    auto sortKeyGen = makeSortKeyGen(BSON("c.a" << 1 << "b" << 1), nullptr);
    auto sortKey = sortKeyGen->computeSortKeyFromDocument(
        Document{{"_id", 0}, {"z", "thing1"_sd}, {"a", 99}, {"c", Document{{"a", 4}}}, {"b", 16}});
    ASSERT_VALUE_EQ(sortKey, (Value{std::vector<Value>{Value{4}, Value{16}}}));
}

TEST(SortKeyGeneratorTest, CompoundPatternLeadingFieldIsArray) {
    auto sortKeyGen = makeSortKeyGen(BSON("c" << 1 << "b" << 1), nullptr);
    auto sortKey = sortKeyGen->computeSortKeyFromDocument(Document{
        {"_id", 0}, {"z", "thing1"_sd}, {"a", 99}, {"c", std::vector{2, 4, 1}}, {"b", 16}});
    ASSERT_VALUE_EQ(sortKey, (Value{std::vector<Value>{Value{1}, Value{16}}}));
}

TEST(SortKeyGeneratorTest, ExtractStringSortKeyWithCollatorUsesComparisonKey) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto sortKeyGen = makeSortKeyGen(BSON("a" << 1), &collator);
    auto sortKey = sortKeyGen->computeSortKeyFromDocument(
        Document{{"_id", 0}, {"z", "thing1"_sd}, {"a", "thing2"_sd}, {"b", 16}});
    ASSERT_VALUE_EQ(sortKey, Value{"2gniht"_sd});
}

TEST(SortKeyGeneratorTest, CollatorHasNoEffectWhenExtractingNonStringSortKey) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto sortKeyGen = makeSortKeyGen(BSON("a" << 1), &collator);
    auto sortKey = sortKeyGen->computeSortKeyFromDocument(
        Document{{"_id", 0}, {"z", 10}, {"a", 6}, {"b", 16}});
    ASSERT_VALUE_EQ(sortKey, Value{6});
}

TEST(SortKeyGeneratorTest, SortKeyGenerationForArraysChoosesCorrectKey) {
    auto sortKeyGen = makeSortKeyGen(BSON("a" << -1), nullptr);
    auto sortKey =
        sortKeyGen->computeSortKeyFromDocument(Document{{"_id", 0}, {"a", {1, 2, 3, 4}}});
    ASSERT_VALUE_EQ(sortKey, Value{4});
}

TEST(SortKeyGeneratorTest, EnsureSortKeyGenerationForArraysRespectsCollation) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto sortKeyGen = makeSortKeyGen(BSON("a" << 1), &collator);
    auto sortKey = sortKeyGen->computeSortKeyFromDocument(
        Document{{"_id", {0}}, {"a", {"aaz"_sd, "zza"_sd, "yya"_sd, "zzb"_sd}}});
    ASSERT_VALUE_EQ(sortKey, Value{"ayy"_sd});
}

TEST(SortKeyGeneratorTest, SortKeyGenerationForArraysRespectsCompoundOrdering) {
    auto sortKeyGen = makeSortKeyGen(BSON("a.b" << 1 << "a.c" << -1), nullptr);
    auto sortKey = sortKeyGen->computeSortKeyFromDocument(
        Document{{"_id", 0},
                 {"a",
                  std::vector{Document{{"b", 1}, {"c", 0}},
                              Document{{"b", 0}, {"c", 3}},
                              Document{{"b", 0}, {"c", 1}}}}});
    ASSERT_VALUE_EQ(sortKey, (Value{std::vector<Value>{Value{0}, Value{3}}}));
}

TEST(SortKeyGeneratorTest, SortKeyGenerationForMissingField) {
    auto sortKeyGen = makeSortKeyGen(BSON("b" << 1), nullptr);
    auto sortKey = sortKeyGen->computeSortKeyFromDocument(Document{{"a", {1}}});
    ASSERT_VALUE_EQ(sortKey, Value{BSONNULL});
}

TEST(SortKeyGeneratorTest, SortKeyGenerationForMissingFieldInCompoundSortPattern) {
    auto sortKeyGen = makeSortKeyGen(BSON("a" << 1 << "b" << 1), nullptr);
    auto sortKey = sortKeyGen->computeSortKeyFromDocument(Document{{"b", {1}}});
    ASSERT_VALUE_EQ(sortKey, (Value{std::vector<Value>{Value{BSONNULL}, Value{1}}}));
}

TEST(SortKeyGeneratorTest, SortKeyGenerationForMissingFieldInEmbeddedDocument) {
    auto sortKeyGen = makeSortKeyGen(BSON("a.b" << 1), nullptr);
    auto sortKey = sortKeyGen->computeSortKeyFromDocument(Document{{"a", Document{{"c", 1}}}});
    ASSERT_VALUE_EQ(sortKey, Value{BSONNULL});
}

TEST(SortKeyGeneratorTest, SortKeyGenerationForMissingFieldInArrayElement) {
    auto sortKeyGen = makeSortKeyGen(BSON("a.b" << 1), nullptr);
    auto sortKey = sortKeyGen->computeSortKeyFromDocument(Document{{"a", std::vector{Document{}}}});
    ASSERT_VALUE_EQ(sortKey, Value{BSONNULL});
}

TEST(SortKeyGeneratorTest, SortKeyGenerationForInvalidPath) {
    auto sortKeyGen = makeSortKeyGen(BSON("a.b" << 1), nullptr);
    auto sortKey = sortKeyGen->computeSortKeyFromDocument(Document{{"a", 1}});
    ASSERT_VALUE_EQ(sortKey, Value{BSONNULL});
}

TEST(SortKeyGeneratorTest, SortPatternComponentWithStringUasserts) {
    ASSERT_THROWS_CODE_AND_WHAT(makeSortKeyGen(BSON("a"
                                                    << "foo"),
                                               nullptr),
                                AssertionException,
                                15974,
                                "Illegal key in $sort specification: a: \"foo\"");
}

TEST(SortKeyGeneratorTest, SortPatternComponentWhoseObjectHasMultipleKeysUasserts) {
    ASSERT_THROWS_CODE_AND_WHAT(makeSortKeyGen(BSON("a" << BSON("$meta"
                                                                << "textScore"
                                                                << "extra" << 1)),
                                               nullptr),
                                AssertionException,
                                ErrorCodes::FailedToParse,
                                "Cannot have additional keys in a $meta sort specification");
}

TEST(SortKeyGeneratorTest, SortPatternComponentWithNonMetaObjectSortUasserts) {
    ASSERT_THROWS_CODE_AND_WHAT(makeSortKeyGen(BSON("a" << BSON("$unknown"
                                                                << "textScore")),
                                               nullptr),
                                AssertionException,
                                17312,
                                "$meta is the only expression supported by $sort right now");
}

TEST(SortKeyGeneratorTest, SortPatternComponentWithUnknownMetaKeywordUasserts) {
    ASSERT_THROWS_CODE_AND_WHAT(makeSortKeyGen(BSON("a" << BSON("$meta"
                                                                << "unknown")),
                                               nullptr),
                                AssertionException,
                                31138,
                                "Illegal $meta sort: $meta: \"unknown\"");
}

TEST(SortKeyGeneratorTest, SortPatternComponentWithNonStringMetaKeywordUasserts) {
    ASSERT_THROWS_CODE_AND_WHAT(makeSortKeyGen(BSON("a" << BSON("$meta" << 0.1)), nullptr),
                                AssertionException,
                                31138,
                                "Illegal $meta sort: $meta: 0.1");
}

TEST(SortKeyGeneratorTest, SortPatternComponentWithSearchScoreMetaKeywordUasserts) {
    ASSERT_THROWS_CODE_AND_WHAT(makeSortKeyGen(BSON("a" << BSON("$meta"
                                                                << "searchScore")),
                                               nullptr),
                                AssertionException,
                                31218,
                                "$meta sort by 'searchScore' metadata is not supported");
}

TEST(SortKeyGeneratorTest, SortPatternComponentWithSearchHighlightsMetaKeywordUasserts) {
    ASSERT_THROWS_CODE_AND_WHAT(makeSortKeyGen(BSON("a" << BSON("$meta"
                                                                << "searchHighlights")),
                                               nullptr),
                                AssertionException,
                                31219,
                                "$meta sort by 'searchHighlights' metadata is not supported");
}

TEST(SortKeyGeneratorTest, SortPatternComponentWithSearchScoreDetailsMetaKeywordUasserts) {
    ASSERT_THROWS_CODE_AND_WHAT(makeSortKeyGen(BSON("a" << BSON("$meta"
                                                                << "searchScoreDetails")),
                                               nullptr),
                                AssertionException,
                                31138,
                                "Illegal $meta sort: $meta: \"searchScoreDetails\"");
}

TEST(SortKeyGeneratorTest, CanGenerateKeysForTextScoreMetaSort) {
    auto sortKeyGen = makeSortKeyGen(BSON("a" << BSON("$meta"
                                                      << "textScore")),
                                     nullptr);
    auto sortKey = sortKeyGen->computeSortKeyFromDocument(
        Document::fromBsonWithMetaData(BSON(Document::metaFieldTextScore << 1.5)));
    ASSERT_VALUE_EQ(sortKey, Value{1.5});
}

TEST(SortKeyGeneratorTest, CanGenerateKeysForRandValMetaSort) {
    auto sortKeyGen = makeSortKeyGen(BSON("a" << BSON("$meta"
                                                      << "randVal")),
                                     nullptr);
    auto sortKey = sortKeyGen->computeSortKeyFromDocument(
        Document::fromBsonWithMetaData(BSON(Document::metaFieldRandVal << 0.3)));
    ASSERT_VALUE_EQ(sortKey, Value{0.3});
}

TEST(SortKeyGeneratorTest, CanGenerateKeysForCompoundMetaSort) {
    BSONObj pattern = fromjson(
        "{a: 1, b: {$meta: 'randVal'}, c: {$meta: 'textScore'}, d: -1, e: {$meta: 'textScore'}}");
    auto sortKeyGen = makeSortKeyGen(pattern, nullptr);
    auto sortKey = sortKeyGen->computeSortKeyFromDocument(
        Document::fromBsonWithMetaData(BSON("a" << 4 << "d" << 5 << Document::metaFieldRandVal
                                                << 0.3 << Document::metaFieldTextScore << 1.5)));
    ASSERT_VALUE_EQ(
        sortKey,
        (Value{std::vector<Value>{Value{4}, Value{0.3}, Value{1.5}, Value{5}, Value{1.5}}}));
}

TEST(SortKeyGeneratorTest, SortKeyComponentVectorTopLevelFieldTest) {
    auto sortKeyGen = makeSortKeyGen(BSON("a" << 1), nullptr);
    std::vector<BSONElement> elemOut(1);
    auto inputObj = BSON("a" << 5);
    sortKeyGen->generateSortKeyComponentVector(inputObj, &elemOut);
    auto expectedObj = BSON("a" << 5);
    std::vector<BSONElement> expected{expectedObj["a"]};
    ASSERT_BSONELT_EQ(elemOut[0], expected[0]);
}

TEST(SortKeyGeneratorTest, SortKeyComponentVectorNestedFieldTest) {
    auto sortKeyGen = makeSortKeyGen(BSON("a.b" << 1), nullptr);
    std::vector<BSONElement> elemOut(1);
    auto inputObj = BSON("a" << BSON("b" << 5));
    sortKeyGen->generateSortKeyComponentVector(inputObj, &elemOut);
    auto expectedObj = BSON("a" << BSON("b" << 5));
    std::vector<BSONElement> expected{expectedObj["a"]["b"]};
    ASSERT_BSONELT_EQ(elemOut[0], expected[0]);
}

TEST(SortKeyGeneratorTest, SortKeyComponentVectorArrayTest) {
    auto sortKeyGen = makeSortKeyGen(BSON("a" << 1), nullptr);
    std::vector<BSONElement> elemOut(1);
    auto inputObj = BSON("a" << BSON_ARRAY(1 << 2 << 3));
    sortKeyGen->generateSortKeyComponentVector(inputObj, &elemOut);
    auto expectedObj = BSON("" << 1);
    std::vector<BSONElement> expected{expectedObj[""]};
    ASSERT_BSONELT_EQ(elemOut[0], expected[0]);
}

TEST(SortKeyGeneratorTest, SortKeyComponentVectorNestedArrayTest) {
    auto sortKeyGen = makeSortKeyGen(BSON("a" << 1), nullptr);
    std::vector<BSONElement> elemOut(1);
    auto inputObj = BSON("a" << BSON_ARRAY(BSON_ARRAY(1 << 2 << 3) << BSON_ARRAY(2 << 3 << 4)));
    sortKeyGen->generateSortKeyComponentVector(inputObj, &elemOut);
    auto expectedObj = BSON("" << BSON_ARRAY(1 << 2 << 3));
    std::vector<BSONElement> expected{expectedObj[""]};
    ASSERT_BSONELT_EQ(elemOut[0], expected[0]);
}

TEST(SortKeyGeneratorTest, SortKeyComponentVectorCompoundSortPatternTest) {
    auto sortKeyGen = makeSortKeyGen(BSON("a" << 1 << "b" << 1), nullptr);
    std::vector<BSONElement> elemOut(2);
    auto inputObj = BSON("a" << 5 << "b" << 3);
    sortKeyGen->generateSortKeyComponentVector(inputObj, &elemOut);
    BSONObj expectedObj = BSON("a" << 5 << "b" << 3);
    std::vector<BSONElement> expected{expectedObj["a"], expectedObj["b"]};
    for (size_t i = 0; i < elemOut.size(); ++i) {
        ASSERT_BSONELT_EQ(elemOut[i], expected[i]);
    }
}

TEST(SortKeyGeneratorTest, SortKeyComponentVectorArrayWithNestedPathsTest) {
    auto sortKeyGen = makeSortKeyGen(BSON("a.b" << 1), nullptr);
    std::vector<BSONElement> elemOut(1);
    auto inputObj = BSON("a" << BSON_ARRAY(BSON("b" << 1) << BSON("b" << 2) << BSON("b" << 3)));
    sortKeyGen->generateSortKeyComponentVector(inputObj, &elemOut);
    BSONObj expectedObj = BSON("" << 1);
    std::vector<BSONElement> expected{expectedObj[""]};
    ASSERT_BSONELT_EQ(elemOut[0], expected[0]);
}

// A test fixture which creates a WorkingSet and allocates a WorkingSetMember inside of it. Used for
// testing sort key generation against a working set member.
class SortKeyGeneratorWorkingSetTest : public mongo::unittest::Test {
public:
    explicit SortKeyGeneratorWorkingSetTest()
        : _wsid(_workingSet.allocate()), _member(_workingSet.get(_wsid)) {}

    void setRecordIdAndObj(BSONObj obj) {
        _member->doc = {SnapshotId(), Document{obj}};
        _workingSet.transitionToRecordIdAndObj(_wsid);
    }

    void setOwnedObj(BSONObj obj) {
        _member->doc = {SnapshotId(), Document{obj}};
        _workingSet.transitionToOwnedObj(_wsid);
    }

    void setRecordIdAndIdx(BSONObj keyPattern, BSONObj key) {
        _member->keyData.push_back(
            IndexKeyDatum(std::move(keyPattern), std::move(key), 0, SnapshotId{}));
        _workingSet.transitionToRecordIdAndIdx(_wsid);
    }

    WorkingSetMember& member() {
        return *_member;
    }

private:
    WorkingSet _workingSet;
    WorkingSetID _wsid;
    WorkingSetMember* _member;
};

TEST_F(SortKeyGeneratorWorkingSetTest, CanGetSortKeyFromWorkingSetMemberWithObj) {
    auto sortKeyGen = makeSortKeyGen(BSON("a" << 1), nullptr);
    setRecordIdAndObj(BSON("x" << 1 << "a" << 2 << "y" << 3));
    auto sortKey = sortKeyGen->computeSortKey(member());
    ASSERT_VALUE_EQ(Value(2), sortKey);
}

TEST_F(SortKeyGeneratorWorkingSetTest, CanGetSortKeyFromWorkingSetMemberWithOwnedObj) {
    auto sortKeyGen = makeSortKeyGen(BSON("a" << 1), nullptr);
    setOwnedObj(BSON("x" << 1 << "a" << 2 << "y" << 3));
    auto sortKey = sortKeyGen->computeSortKey(member());
    ASSERT_VALUE_EQ(Value(2), sortKey);
}

TEST_F(SortKeyGeneratorWorkingSetTest, CanGenerateKeyFromWSMForTextScoreMetaSort) {
    BSONObj pattern = fromjson("{a: 1, b: {$meta: 'textScore'}, c: -1}}");
    auto sortKeyGen = makeSortKeyGen(pattern, nullptr);
    setOwnedObj(BSON("x" << 1 << "a" << 2 << "y" << 3 << "c" << BSON_ARRAY(4 << 5 << 6)));
    member().metadata().setTextScore(9.9);
    auto sortKey = sortKeyGen->computeSortKey(member());
    ASSERT_VALUE_EQ(Value({Value(2), Value(9.9), Value(6)}), sortKey);
}

TEST_F(SortKeyGeneratorWorkingSetTest, CanGenerateSortKeyFromWSMInIndexKeyState) {
    auto sortKeyGen = makeSortKeyGen(BSON("a" << 1), nullptr);
    setRecordIdAndIdx(BSON("a" << 1 << "b" << 1), BSON("" << 2 << "" << 3));
    auto sortKey = sortKeyGen->computeSortKey(member());
    ASSERT_VALUE_EQ(Value(2), sortKey);
}

TEST_F(SortKeyGeneratorWorkingSetTest, CanGenerateSortKeyFromWSMInIndexKeyStateWithCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto sortKeyGen = makeSortKeyGen(BSON("a" << 1), &collator);
    setRecordIdAndIdx(BSON("a" << 1 << "b" << 1),
                      BSON(""
                           << "string1"
                           << ""
                           << "string2"));
    auto sortKey = sortKeyGen->computeSortKey(member());
    ASSERT_VALUE_EQ(Value("1gnirts"_sd), sortKey);
}

DEATH_TEST_REGEX_F(SortKeyGeneratorWorkingSetTest,
                   DeathOnAttemptToGetSortKeyFromIndexKeyWithMetadata,
                   "Invariant failure.*!_sortHasMeta") {
    BSONObj pattern = fromjson("{z: {$meta: 'textScore'}}");
    auto sortKeyGen = makeSortKeyGen(pattern, nullptr);
    setRecordIdAndIdx(BSON("a" << 1 << "b" << 1), BSON("" << 2 << "" << 3));
    member().metadata().setTextScore(9.9);
    [[maybe_unused]] auto ignored = sortKeyGen->computeSortKey(member());
}

}  // namespace
}  // namespace mongo
