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

#include "mongo/bson/json.h"
#include "mongo/db/index/sort_key_generator.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(SortKeyGeneratorTest, ExtractNumberKeyForNonCompoundSortNonNested) {
    auto sortKeyGen = std::make_unique<SortKeyGenerator>(BSON("a" << 1), nullptr);
    auto sortKey = sortKeyGen->getSortKeyFromDocument(fromjson("{_id: 0, a: 5}"), nullptr);
    ASSERT_OK(sortKey.getStatus());
    ASSERT_BSONOBJ_EQ(sortKey.getValue(), BSON("" << 5));
}

TEST(SortKeyGeneratorTest, ExtractNumberKeyFromDocWithSeveralFields) {
    auto sortKeyGen = std::make_unique<SortKeyGenerator>(BSON("a" << 1), nullptr);
    auto sortKey =
        sortKeyGen->getSortKeyFromDocument(fromjson("{_id: 0, z: 10, a: 6, b: 16}"), nullptr);
    ASSERT_OK(sortKey.getStatus());
    ASSERT_BSONOBJ_EQ(sortKey.getValue(), BSON("" << 6));
}

TEST(SortKeyGeneratorTest, ExtractStringKeyNonCompoundNonNested) {
    auto sortKeyGen = std::make_unique<SortKeyGenerator>(BSON("a" << 1), nullptr);
    auto sortKey = sortKeyGen->getSortKeyFromDocument(
        fromjson("{_id: 0, z: 'thing1', a: 'thing2', b: 16}"), nullptr);
    ASSERT_OK(sortKey.getStatus());
    ASSERT_BSONOBJ_EQ(sortKey.getValue(),
                      BSON(""
                           << "thing2"));
}

TEST(SortKeyGeneratorTest, CompoundSortPattern) {
    auto sortKeyGen = std::make_unique<SortKeyGenerator>(BSON("a" << 1 << "b" << 1), nullptr);
    auto sortKey = sortKeyGen->getSortKeyFromDocument(
        fromjson("{_id: 0, z: 'thing1', a: 99, c: {a: 4}, b: 16}"), nullptr);
    ASSERT_OK(sortKey.getStatus());
    ASSERT_BSONOBJ_EQ(sortKey.getValue(), BSON("" << 99 << "" << 16));
}

TEST(SortKeyGeneratorTest, CompoundSortPatternWithDottedPath) {
    auto sortKeyGen = std::make_unique<SortKeyGenerator>(BSON("c.a" << 1 << "b" << 1), nullptr);
    auto sortKey = sortKeyGen->getSortKeyFromDocument(
        fromjson("{_id: 0, z: 'thing1', a: 99, c: {a: 4}, b: 16}"), nullptr);
    ASSERT_OK(sortKey.getStatus());
    ASSERT_BSONOBJ_EQ(sortKey.getValue(), BSON("" << 4 << "" << 16));
}

TEST(SortKeyGeneratorTest, CompoundPatternLeadingFieldIsArray) {
    auto sortKeyGen = std::make_unique<SortKeyGenerator>(BSON("c" << 1 << "b" << 1), nullptr);
    auto sortKey = sortKeyGen->getSortKeyFromDocument(
        fromjson("{_id: 0, z: 'thing1', a: 99, c: [2, 4, 1], b: 16}"), nullptr);
    ASSERT_OK(sortKey.getStatus());
    ASSERT_BSONOBJ_EQ(sortKey.getValue(), BSON("" << 1 << "" << 16));
}

TEST(SortKeyGeneratorTest, ExtractStringSortKeyWithCollatorUsesComparisonKey) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto sortKeyGen = std::make_unique<SortKeyGenerator>(BSON("a" << 1), &collator);
    auto sortKey = sortKeyGen->getSortKeyFromDocument(
        fromjson("{_id: 0, z: 'thing1', a: 'thing2', b: 16}"), nullptr);
    ASSERT_OK(sortKey.getStatus());
    ASSERT_BSONOBJ_EQ(sortKey.getValue(),
                      BSON(""
                           << "2gniht"));
}

TEST(SortKeyGeneratorTest, CollatorHasNoEffectWhenExtractingNonStringSortKey) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto sortKeyGen = std::make_unique<SortKeyGenerator>(BSON("a" << 1), &collator);
    auto sortKey =
        sortKeyGen->getSortKeyFromDocument(fromjson("{_id: 0, z: 10, a: 6, b: 16}"), nullptr);
    ASSERT_OK(sortKey.getStatus());
    ASSERT_BSONOBJ_EQ(sortKey.getValue(), BSON("" << 6));
}

TEST(SortKeyGeneratorTest, SortKeyGenerationForArraysChoosesCorrectKey) {
    auto sortKeyGen = std::make_unique<SortKeyGenerator>(BSON("a" << -1), nullptr);
    auto sortKey =
        sortKeyGen->getSortKeyFromDocument(fromjson("{_id: 0, a: [1, 2, 3, 4]}"), nullptr);
    ASSERT_OK(sortKey.getStatus());
    ASSERT_BSONOBJ_EQ(sortKey.getValue(), BSON("" << 4));
}

TEST(SortKeyGeneratorTest, EnsureSortKeyGenerationForArraysRespectsCollation) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto sortKeyGen = std::make_unique<SortKeyGenerator>(BSON("a" << 1), &collator);
    auto sortKey = sortKeyGen->getSortKeyFromDocument(
        fromjson("{_id: 0, a: ['aaz', 'zza', 'yya', 'zzb']}"), nullptr);
    ASSERT_OK(sortKey.getStatus());
    ASSERT_BSONOBJ_EQ(sortKey.getValue(),
                      BSON(""
                           << "ayy"));
}

TEST(SortKeyGeneratorTest, SortKeyGenerationForArraysRespectsCompoundOrdering) {
    auto sortKeyGen = std::make_unique<SortKeyGenerator>(BSON("a.b" << 1 << "a.c" << -1), nullptr);
    auto sortKey = sortKeyGen->getSortKeyFromDocument(
        fromjson("{_id: 0, a: [{b: 1, c: 0}, {b: 0, c: 3}, {b: 0, c: 1}]}"), nullptr);
    ASSERT_OK(sortKey.getStatus());
    ASSERT_BSONOBJ_EQ(sortKey.getValue(), BSON("" << 0 << "" << 3));
}

DEATH_TEST(SortKeyGeneratorTest,
           SortPatternComponentWithStringIsFatal,
           "Invariant failure elt.type() == BSONType::Object") {
    MONGO_COMPILER_VARIABLE_UNUSED auto ignored = std::make_unique<SortKeyGenerator>(BSON("a"
                                                                                          << "foo"),
                                                                                     nullptr);
}

DEATH_TEST(SortKeyGeneratorTest,
           SortPatternComponentWhoseObjectHasMultipleKeysIsFatal,
           "Invariant failure elt.embeddedObject().nFields() == 1") {
    MONGO_COMPILER_VARIABLE_UNUSED auto ignored =
        std::make_unique<SortKeyGenerator>(BSON("a" << BSON("$meta"
                                                            << "textScore"
                                                            << "extra" << 1)),
                                           nullptr);
}

DEATH_TEST(SortKeyGeneratorTest,
           SortPatternComponentWithNonMetaObjectSortIsFatal,
           "Invariant failure metaElem.fieldNameStringData() == \"$meta\"_sd") {
    MONGO_COMPILER_VARIABLE_UNUSED auto ignored =
        std::make_unique<SortKeyGenerator>(BSON("a" << BSON("$unknown"
                                                            << "textScore")),
                                           nullptr);
}

DEATH_TEST(SortKeyGeneratorTest,
           SortPatternComponentWithUnknownMetaKeywordIsFatal,
           "Invariant failure metaElem.valueStringData() == \"randVal\"_sd") {
    MONGO_COMPILER_VARIABLE_UNUSED auto ignored =
        std::make_unique<SortKeyGenerator>(BSON("a" << BSON("$meta"
                                                            << "unknown")),
                                           nullptr);
}

DEATH_TEST(SortKeyGeneratorTest,
           NoMetadataWhenPatternHasMetaTextScoreIsFatal,
           "Invariant failure metadata") {
    auto sortKeyGen = std::make_unique<SortKeyGenerator>(BSON("a" << BSON("$meta"
                                                                          << "textScore")),
                                                         nullptr);
    uassertStatusOK(sortKeyGen->getSortKeyFromDocument(BSONObj{}, nullptr).getStatus());
}

DEATH_TEST(SortKeyGeneratorTest,
           NoMetadataWhenPatternHasMetaRandValIsFatal,
           "Invariant failure metadata") {
    auto sortKeyGen = std::make_unique<SortKeyGenerator>(BSON("a" << BSON("$meta"
                                                                          << "randVal")),
                                                         nullptr);
    uassertStatusOK(sortKeyGen->getSortKeyFromDocument(BSONObj{}, nullptr).getStatus());
}

TEST(SortKeyGeneratorTest, CanGenerateKeysForTextScoreMetaSort) {
    auto sortKeyGen = std::make_unique<SortKeyGenerator>(BSON("a" << BSON("$meta"
                                                                          << "textScore")),
                                                         nullptr);
    SortKeyGenerator::Metadata metadata;
    metadata.textScore = 1.5;
    auto sortKey = sortKeyGen->getSortKeyFromDocument(BSONObj{}, &metadata);
    ASSERT_OK(sortKey.getStatus());
    ASSERT_BSONOBJ_EQ(sortKey.getValue(), BSON("" << 1.5));
}

TEST(SortKeyGeneratorTest, CanGenerateKeysForRandValMetaSort) {
    auto sortKeyGen = std::make_unique<SortKeyGenerator>(BSON("a" << BSON("$meta"
                                                                          << "randVal")),
                                                         nullptr);
    SortKeyGenerator::Metadata metadata;
    metadata.randVal = 0.3;
    auto sortKey = sortKeyGen->getSortKeyFromDocument(BSONObj{}, &metadata);
    ASSERT_OK(sortKey.getStatus());
    ASSERT_BSONOBJ_EQ(sortKey.getValue(), BSON("" << 0.3));
}

TEST(SortKeyGeneratorTest, CanGenerateKeysForCompoundMetaSort) {
    BSONObj pattern = fromjson(
        "{a: 1, b: {$meta: 'randVal'}, c: {$meta: 'textScore'}, d: -1, e: {$meta: 'textScore'}}");
    auto sortKeyGen = std::make_unique<SortKeyGenerator>(pattern, nullptr);
    SortKeyGenerator::Metadata metadata;
    metadata.randVal = 0.3;
    metadata.textScore = 1.5;
    auto sortKey = sortKeyGen->getSortKeyFromDocument(BSON("a" << 4 << "d" << 5), &metadata);
    ASSERT_OK(sortKey.getStatus());
    ASSERT_BSONOBJ_EQ(sortKey.getValue(),
                      BSON("" << 4 << "" << 0.3 << "" << 1.5 << "" << 5 << "" << 1.5));
}

// A test fixture which creates a WorkingSet and allocates a WorkingSetMember inside of it. Used for
// testing sort key generation against a working set member.
class SortKeyGeneratorWorkingSetTest : public mongo::unittest::Test {
public:
    explicit SortKeyGeneratorWorkingSetTest()
        : _wsid(_workingSet.allocate()), _member(_workingSet.get(_wsid)) {}

    void setRecordIdAndObj(BSONObj obj) {
        _member->obj = {SnapshotId(), std::move(obj)};
        _workingSet.transitionToRecordIdAndObj(_wsid);
    }

    void setOwnedObj(BSONObj obj) {
        _member->obj = {SnapshotId(), std::move(obj)};
        _workingSet.transitionToOwnedObj(_wsid);
    }

    void setRecordIdAndIdx(BSONObj keyPattern, BSONObj key) {
        _member->keyData.push_back(IndexKeyDatum(std::move(keyPattern), std::move(key), nullptr));
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
    auto sortKeyGen = std::make_unique<SortKeyGenerator>(BSON("a" << 1), nullptr);
    setRecordIdAndObj(BSON("x" << 1 << "a" << 2 << "y" << 3));
    auto sortKey = sortKeyGen->getSortKey(member());
    ASSERT_OK(sortKey);
    ASSERT_BSONOBJ_EQ(BSON("" << 2), sortKey.getValue());
}

TEST_F(SortKeyGeneratorWorkingSetTest, CanGetSortKeyFromWorkingSetMemberWithOwnedObj) {
    auto sortKeyGen = std::make_unique<SortKeyGenerator>(BSON("a" << 1), nullptr);
    setOwnedObj(BSON("x" << 1 << "a" << 2 << "y" << 3));
    auto sortKey = sortKeyGen->getSortKey(member());
    ASSERT_OK(sortKey);
    ASSERT_BSONOBJ_EQ(BSON("" << 2), sortKey.getValue());
}

TEST_F(SortKeyGeneratorWorkingSetTest, CanGenerateKeyFromWSMForTextScoreMetaSort) {
    BSONObj pattern = fromjson("{a: 1, b: {$meta: 'textScore'}, c: -1}}");
    auto sortKeyGen = std::make_unique<SortKeyGenerator>(pattern, nullptr);
    setOwnedObj(BSON("x" << 1 << "a" << 2 << "y" << 3 << "c" << BSON_ARRAY(4 << 5 << 6)));
    member().metadata().setTextScore(9.9);
    auto sortKey = sortKeyGen->getSortKey(member());
    ASSERT_OK(sortKey);
    ASSERT_BSONOBJ_EQ(BSON("" << 2 << "" << 9.9 << "" << 6), sortKey.getValue());
}

TEST_F(SortKeyGeneratorWorkingSetTest, CanGenerateSortKeyFromWSMInIndexKeyState) {
    auto sortKeyGen = std::make_unique<SortKeyGenerator>(BSON("a" << 1), nullptr);
    setRecordIdAndIdx(BSON("a" << 1 << "b" << 1), BSON("" << 2 << "" << 3));
    auto sortKey = sortKeyGen->getSortKey(member());
    ASSERT_OK(sortKey);
    ASSERT_BSONOBJ_EQ(BSON("" << 2), sortKey.getValue());
}

TEST_F(SortKeyGeneratorWorkingSetTest, CanGenerateSortKeyFromWSMInIndexKeyStateWithCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto sortKeyGen = std::make_unique<SortKeyGenerator>(BSON("a" << 1), &collator);
    setRecordIdAndIdx(BSON("a" << 1 << "b" << 1),
                      BSON(""
                           << "string1"
                           << ""
                           << "string2"));
    auto sortKey = sortKeyGen->getSortKey(member());
    ASSERT_OK(sortKey);
    ASSERT_BSONOBJ_EQ(BSON(""
                           << "1gnirts"),
                      sortKey.getValue());
}

DEATH_TEST_F(SortKeyGeneratorWorkingSetTest,
             DeathOnAttemptToGetSortKeyFromIndexKeyWithMetadata,
             "Invariant failure !_sortHasMeta") {
    BSONObj pattern = fromjson("{z: {$meta: 'textScore'}}");
    auto sortKeyGen = std::make_unique<SortKeyGenerator>(pattern, nullptr);
    setRecordIdAndIdx(BSON("a" << 1 << "b" << 1), BSON("" << 2 << "" << 3));
    member().metadata().setTextScore(9.9);
    MONGO_COMPILER_VARIABLE_UNUSED auto ignored = sortKeyGen->getSortKey(member());
}

}  // namespace
}  // namespace mongo
