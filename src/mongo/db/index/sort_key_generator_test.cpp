/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/index/sort_key_generator.h"

#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(SortKeyGeneratorTest, ExtractNumberKeyForNonCompoundSortNonNested) {
    auto sortKeyGen = stdx::make_unique<SortKeyGenerator>(BSON("a" << 1), nullptr);
    auto sortKey = sortKeyGen->getSortKey(fromjson("{_id: 0, a: 5}"), nullptr);
    ASSERT_OK(sortKey.getStatus());
    ASSERT_BSONOBJ_EQ(sortKey.getValue(), BSON("" << 5));
}

TEST(SortKeyGeneratorTest, ExtractNumberKeyFromDocWithSeveralFields) {
    auto sortKeyGen = stdx::make_unique<SortKeyGenerator>(BSON("a" << 1), nullptr);
    auto sortKey = sortKeyGen->getSortKey(fromjson("{_id: 0, z: 10, a: 6, b: 16}"), nullptr);
    ASSERT_OK(sortKey.getStatus());
    ASSERT_BSONOBJ_EQ(sortKey.getValue(), BSON("" << 6));
}

TEST(SortKeyGeneratorTest, ExtractStringKeyNonCompoundNonNested) {
    auto sortKeyGen = stdx::make_unique<SortKeyGenerator>(BSON("a" << 1), nullptr);
    auto sortKey =
        sortKeyGen->getSortKey(fromjson("{_id: 0, z: 'thing1', a: 'thing2', b: 16}"), nullptr);
    ASSERT_OK(sortKey.getStatus());
    ASSERT_BSONOBJ_EQ(sortKey.getValue(),
                      BSON(""
                           << "thing2"));
}

TEST(SortKeyGeneratorTest, CompoundSortPattern) {
    auto sortKeyGen = stdx::make_unique<SortKeyGenerator>(BSON("a" << 1 << "b" << 1), nullptr);
    auto sortKey =
        sortKeyGen->getSortKey(fromjson("{_id: 0, z: 'thing1', a: 99, c: {a: 4}, b: 16}"), nullptr);
    ASSERT_OK(sortKey.getStatus());
    ASSERT_BSONOBJ_EQ(sortKey.getValue(), BSON("" << 99 << "" << 16));
}

TEST(SortKeyGeneratorTest, CompoundSortPatternWithDottedPath) {
    auto sortKeyGen = stdx::make_unique<SortKeyGenerator>(BSON("c.a" << 1 << "b" << 1), nullptr);
    auto sortKey =
        sortKeyGen->getSortKey(fromjson("{_id: 0, z: 'thing1', a: 99, c: {a: 4}, b: 16}"), nullptr);
    ASSERT_OK(sortKey.getStatus());
    ASSERT_BSONOBJ_EQ(sortKey.getValue(), BSON("" << 4 << "" << 16));
}

TEST(SortKeyGeneratorTest, CompoundPatternLeadingFieldIsArray) {
    auto sortKeyGen = stdx::make_unique<SortKeyGenerator>(BSON("c" << 1 << "b" << 1), nullptr);
    auto sortKey = sortKeyGen->getSortKey(
        fromjson("{_id: 0, z: 'thing1', a: 99, c: [2, 4, 1], b: 16}"), nullptr);
    ASSERT_OK(sortKey.getStatus());
    ASSERT_BSONOBJ_EQ(sortKey.getValue(), BSON("" << 1 << "" << 16));
}

TEST(SortKeyGeneratorTest, ExtractStringSortKeyWithCollatorUsesComparisonKey) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto sortKeyGen = stdx::make_unique<SortKeyGenerator>(BSON("a" << 1), &collator);
    auto sortKey =
        sortKeyGen->getSortKey(fromjson("{_id: 0, z: 'thing1', a: 'thing2', b: 16}"), nullptr);
    ASSERT_OK(sortKey.getStatus());
    ASSERT_BSONOBJ_EQ(sortKey.getValue(),
                      BSON(""
                           << "2gniht"));
}

TEST(SortKeyGeneratorTest, CollatorHasNoEffectWhenExtractingNonStringSortKey) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto sortKeyGen = stdx::make_unique<SortKeyGenerator>(BSON("a" << 1), &collator);
    auto sortKey = sortKeyGen->getSortKey(fromjson("{_id: 0, z: 10, a: 6, b: 16}"), nullptr);
    ASSERT_OK(sortKey.getStatus());
    ASSERT_BSONOBJ_EQ(sortKey.getValue(), BSON("" << 6));
}

TEST(SortKeyGeneratorTest, SortKeyGenerationForArraysChoosesCorrectKey) {
    auto sortKeyGen = stdx::make_unique<SortKeyGenerator>(BSON("a" << -1), nullptr);
    auto sortKey = sortKeyGen->getSortKey(fromjson("{_id: 0, a: [1, 2, 3, 4]}"), nullptr);
    ASSERT_OK(sortKey.getStatus());
    ASSERT_BSONOBJ_EQ(sortKey.getValue(), BSON("" << 4));
}

TEST(SortKeyGeneratorTest, EnsureSortKeyGenerationForArraysRespectsCollation) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto sortKeyGen = stdx::make_unique<SortKeyGenerator>(BSON("a" << 1), &collator);
    auto sortKey =
        sortKeyGen->getSortKey(fromjson("{_id: 0, a: ['aaz', 'zza', 'yya', 'zzb']}"), nullptr);
    ASSERT_OK(sortKey.getStatus());
    ASSERT_BSONOBJ_EQ(sortKey.getValue(),
                      BSON(""
                           << "ayy"));
}

TEST(SortKeyGeneratorTest, SortKeyGenerationForArraysRespectsCompoundOrdering) {
    auto sortKeyGen = stdx::make_unique<SortKeyGenerator>(BSON("a.b" << 1 << "a.c" << -1), nullptr);
    auto sortKey = sortKeyGen->getSortKey(
        fromjson("{_id: 0, a: [{b: 1, c: 0}, {b: 0, c: 3}, {b: 0, c: 1}]}"), nullptr);
    ASSERT_OK(sortKey.getStatus());
    ASSERT_BSONOBJ_EQ(sortKey.getValue(), BSON("" << 0 << "" << 3));
}

DEATH_TEST(SortKeyGeneratorTest,
           SortPatternComponentWithStringIsFatal,
           "Invariant failure elt.type() == BSONType::Object") {
    stdx::make_unique<SortKeyGenerator>(BSON("a"
                                             << "foo"),
                                        nullptr);
}

DEATH_TEST(SortKeyGeneratorTest,
           SortPatternComponentWhoseObjectHasMultipleKeysIsFatal,
           "Invariant failure elt.embeddedObject().nFields() == 1") {
    stdx::make_unique<SortKeyGenerator>(BSON("a" << BSON("$meta"
                                                         << "textScore"
                                                         << "extra"
                                                         << 1)),
                                        nullptr);
}

DEATH_TEST(SortKeyGeneratorTest,
           SortPatternComponentWithNonMetaObjectSortIsFatal,
           "Invariant failure metaElem.fieldNameStringData() == \"$meta\"_sd") {
    stdx::make_unique<SortKeyGenerator>(BSON("a" << BSON("$unknown"
                                                         << "textScore")),
                                        nullptr);
}

DEATH_TEST(SortKeyGeneratorTest,
           SortPatternComponentWithUnknownMetaKeywordIsFatal,
           "Invariant failure metaElem.valueStringData() == \"randVal\"_sd") {
    stdx::make_unique<SortKeyGenerator>(BSON("a" << BSON("$meta"
                                                         << "unknown")),
                                        nullptr);
}

DEATH_TEST(SortKeyGeneratorTest,
           NoMetadataWhenPatternHasMetaTextScoreIsFatal,
           "Invariant failure metadata") {
    auto sortKeyGen = stdx::make_unique<SortKeyGenerator>(BSON("a" << BSON("$meta"
                                                                           << "textScore")),
                                                          nullptr);
    uassertStatusOK(sortKeyGen->getSortKey(BSONObj{}, nullptr).getStatus());
}

DEATH_TEST(SortKeyGeneratorTest,
           NoMetadataWhenPatternHasMetaRandValIsFatal,
           "Invariant failure metadata") {
    auto sortKeyGen = stdx::make_unique<SortKeyGenerator>(BSON("a" << BSON("$meta"
                                                                           << "randVal")),
                                                          nullptr);
    uassertStatusOK(sortKeyGen->getSortKey(BSONObj{}, nullptr).getStatus());
}

TEST(SortKeyGeneratorTest, CanGenerateKeysForTextScoreMetaSort) {
    auto sortKeyGen = stdx::make_unique<SortKeyGenerator>(BSON("a" << BSON("$meta"
                                                                           << "textScore")),
                                                          nullptr);
    SortKeyGenerator::Metadata metadata;
    metadata.textScore = 1.5;
    auto sortKey = sortKeyGen->getSortKey(BSONObj{}, &metadata);
    ASSERT_OK(sortKey.getStatus());
    ASSERT_BSONOBJ_EQ(sortKey.getValue(), BSON("" << 1.5));
}

TEST(SortKeyGeneratorTest, CanGenerateKeysForRandValMetaSort) {
    auto sortKeyGen = stdx::make_unique<SortKeyGenerator>(BSON("a" << BSON("$meta"
                                                                           << "randVal")),
                                                          nullptr);
    SortKeyGenerator::Metadata metadata;
    metadata.randVal = 0.3;
    auto sortKey = sortKeyGen->getSortKey(BSONObj{}, &metadata);
    ASSERT_OK(sortKey.getStatus());
    ASSERT_BSONOBJ_EQ(sortKey.getValue(), BSON("" << 0.3));
}

TEST(SortKeyGeneratorTest, CanGenerateKeysForCompoundMetaSort) {
    BSONObj pattern = fromjson(
        "{a: 1, b: {$meta: 'randVal'}, c: {$meta: 'textScore'}, d: -1, e: {$meta: 'textScore'}}");
    auto sortKeyGen = stdx::make_unique<SortKeyGenerator>(pattern, nullptr);
    SortKeyGenerator::Metadata metadata;
    metadata.randVal = 0.3;
    metadata.textScore = 1.5;
    auto sortKey = sortKeyGen->getSortKey(BSON("a" << 4 << "d" << 5), &metadata);
    ASSERT_OK(sortKey.getStatus());
    ASSERT_BSONOBJ_EQ(sortKey.getValue(),
                      BSON("" << 4 << "" << 0.3 << "" << 1.5 << "" << 5 << "" << 1.5));
}

}  // namespace
}  // namespace mongo
