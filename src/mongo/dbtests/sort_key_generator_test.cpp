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

#include "mongo/db/exec/sort_key_generator.h"
#include "mongo/db/json.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {

/**
 * Test function to verify that the SortKeyGenerator can generate a sortKey from a fetched document.
 *
 * sortSpec - The JSON representation of the sort spec BSONObj.
 * doc - The JSON representation of the BSON document.
 * queryObj - The JSON representation of the query predicate. Used when generating the sort key from
 * an array.
 * collator - The collation for the sort that we are are generating keys for.
 *
 * Returns the BSON representation of the sort key, to be checked against the expected sort key.
 */
BSONObj extractSortKey(const char* sortSpec,
                       const char* doc,
                       const char* query,
                       const CollatorInterface* collator) {
    QueryTestServiceContext serviceContext;
    auto txn = serviceContext.makeOperationContext();

    WorkingSetMember wsm;
    wsm.obj = Snapshotted<BSONObj>(SnapshotId(), fromjson(doc));
    wsm.transitionToOwnedObj();

    BSONObj sortKey;
    auto sortKeyGen = stdx::make_unique<SortKeyGenerator>(
        txn.get(), fromjson(sortSpec), fromjson(query), collator);
    ASSERT_OK(sortKeyGen->getSortKey(wsm, &sortKey));

    return sortKey;
}

/**
 * Test function to verify that the SortKeyGenerator can generate a sortKey while using only index
 * data (that is, the document will not be fetched). For SERVER-20117.
 *
 * sortSpec - The JSON representation of the sort spec BSONObj.
 * ikd - The data stored in the index.
 * collator - The collation for the sort that we are are generating keys for.
 *
 * Returns the BSON representation of the sort key, to be checked against the expected sort key.
 */
BSONObj extractSortKeyCovered(const char* sortSpec,
                              const IndexKeyDatum& ikd,
                              const CollatorInterface* collator) {
    QueryTestServiceContext serviceContext;
    auto txn = serviceContext.makeOperationContext();

    WorkingSet ws;
    WorkingSetID wsid = ws.allocate();
    WorkingSetMember* wsm = ws.get(wsid);
    wsm->keyData.push_back(ikd);
    ws.transitionToRecordIdAndIdx(wsid);

    BSONObj sortKey;
    auto sortKeyGen =
        stdx::make_unique<SortKeyGenerator>(txn.get(), fromjson(sortSpec), BSONObj(), collator);
    ASSERT_OK(sortKeyGen->getSortKey(*wsm, &sortKey));

    return sortKey;
}

TEST(SortKeyGeneratorTest, SortKeyNormal) {
    BSONObj actualOut = extractSortKey("{a: 1}", "{_id: 0, a: 5}", "", nullptr);
    BSONObj expectedOut = BSON("" << 5);
    ASSERT_EQ(actualOut, expectedOut);
}

TEST(SortKeyGeneratorTest, SortKeyNormal2) {
    BSONObj actualOut = extractSortKey("{a: 1}", "{_id: 0, z: 10, a: 6, b: 16}", "", nullptr);
    BSONObj expectedOut = BSON("" << 6);
    ASSERT_EQ(actualOut, expectedOut);
}

TEST(SortKeyGeneratorTest, SortKeyString) {
    BSONObj actualOut =
        extractSortKey("{a: 1}", "{_id: 0, z: 'thing1', a: 'thing2', b: 16}", "", nullptr);
    BSONObj expectedOut = BSON(""
                               << "thing2");
    ASSERT_EQ(actualOut, expectedOut);
}

TEST(SortKeyGeneratorTest, SortKeyCompound) {
    BSONObj actualOut = extractSortKey(
        "{a: 1, b: 1}", "{_id: 0, z: 'thing1', a: 99, c: {a: 4}, b: 16}", "", nullptr);
    BSONObj expectedOut = BSON("" << 99 << "" << 16);
    ASSERT_EQ(actualOut, expectedOut);
}

TEST(SortKeyGeneratorTest, SortKeyEmbedded) {
    BSONObj actualOut = extractSortKey(
        "{'c.a': 1, b: 1}", "{_id: 0, z: 'thing1', a: 99, c: {a: 4}, b: 16}", "", nullptr);
    BSONObj expectedOut = BSON("" << 4 << "" << 16);
    ASSERT_EQ(actualOut, expectedOut);
}

TEST(SortKeyGeneratorTest, SortKeyArray) {
    BSONObj actualOut = extractSortKey(
        "{'c': 1, b: 1}", "{_id: 0, z: 'thing1', a: 99, c: [2, 4, 1], b: 16}", "", nullptr);
    BSONObj expectedOut = BSON("" << 1 << "" << 16);
    ASSERT_EQ(actualOut, expectedOut);
}

TEST(SortKeyGeneratorTest, SortKeyCoveredNormal) {
    CollatorInterface* collator = nullptr;
    BSONObj actualOut = extractSortKeyCovered(
        "{a: 1}", IndexKeyDatum(BSON("a" << 1), BSON("" << 5), nullptr), collator);
    BSONObj expectedOut = BSON("" << 5);
    ASSERT_EQ(actualOut, expectedOut);
}

TEST(SortKeyGeneratorTest, SortKeyCoveredEmbedded) {
    CollatorInterface* collator = nullptr;
    BSONObj actualOut = extractSortKeyCovered(
        "{'a.c': 1}",
        IndexKeyDatum(BSON("a.c" << 1 << "c" << 1), BSON("" << 5 << "" << 6), nullptr),
        collator);
    BSONObj expectedOut = BSON("" << 5);
    ASSERT_EQ(actualOut, expectedOut);
}

TEST(SortKeyGeneratorTest, SortKeyCoveredCompound) {
    CollatorInterface* collator = nullptr;
    BSONObj actualOut = extractSortKeyCovered(
        "{a: 1, c: 1}",
        IndexKeyDatum(BSON("a" << 1 << "c" << 1), BSON("" << 5 << "" << 6), nullptr),
        collator);
    BSONObj expectedOut = BSON("" << 5 << "" << 6);
    ASSERT_EQ(actualOut, expectedOut);
}

TEST(SortKeyGeneratorTest, SortKeyCoveredCompound2) {
    CollatorInterface* collator = nullptr;
    BSONObj actualOut = extractSortKeyCovered("{a: 1, b: 1}",
                                              IndexKeyDatum(BSON("a" << 1 << "b" << 1 << "c" << 1),
                                                            BSON("" << 5 << "" << 6 << "" << 4),
                                                            nullptr),
                                              collator);
    BSONObj expectedOut = BSON("" << 5 << "" << 6);
    ASSERT_EQ(actualOut, expectedOut);
}

TEST(SortKeyGeneratorTest, SortKeyCoveredCompound3) {
    CollatorInterface* collator = nullptr;
    BSONObj actualOut =
        extractSortKeyCovered("{b: 1, c: 1}",
                              IndexKeyDatum(BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1),
                                            BSON("" << 5 << "" << 6 << "" << 4 << "" << 9000),
                                            nullptr),
                              collator);
    BSONObj expectedOut = BSON("" << 6 << "" << 4);
    ASSERT_EQ(actualOut, expectedOut);
}

TEST(SortKeyGeneratorTest, ExtractStringSortKeyWithCollatorUsesComparisonKey) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    BSONObj actualOut =
        extractSortKey("{a: 1}", "{_id: 0, z: 'thing1', a: 'thing2', b: 16}", "", &collator);
    BSONObj expectedOut = BSON(""
                               << "2gniht");
    ASSERT_EQ(actualOut, expectedOut);
}

TEST(SortKeyGeneratorTest, CollatorHasNoEffectWhenExtractingNonStringSortKey) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    BSONObj actualOut = extractSortKey("{a: 1}", "{_id: 0, z: 10, a: 6, b: 16}", "", &collator);
    BSONObj expectedOut = BSON("" << 6);
    ASSERT_EQ(actualOut, expectedOut);
}

TEST(SortKeyGeneratorTest, CollatorHasNoAffectWhenExtractingCoveredSortKey) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    BSONObj actualOut = extractSortKeyCovered("{b: 1}",
                                              IndexKeyDatum(BSON("a" << 1 << "b" << 1),
                                                            BSON("" << 4 << ""
                                                                    << "foo"),
                                                            nullptr),
                                              &collator);
    BSONObj expectedOut = BSON(""
                               << "foo");
    ASSERT_EQ(actualOut, expectedOut);
}

TEST(SortKeyGeneratorTest, SortKeyGenerationForArraysUsesTheQueryPredicate) {
    BSONObj actualOut =
        extractSortKey("{a: -1}", "{_id: 0, a: [1, 2, 3, 4]}", "{a: {$lt: 3}}", nullptr);
    BSONObj expectedOut = BSON("" << 2);
    ASSERT_EQ(actualOut, expectedOut);
}

TEST(SortKeyGeneratorTest, EnsureSortKeyGenerationForArraysRespectsCollation) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    BSONObj actualOut =
        extractSortKey("{a: 1}", "{_id: 0, a: ['aaz', 'zza', 'yya', 'zzb']}", "", &collator);
    BSONObj expectedOut = BSON(""
                               << "ayy");
    ASSERT_EQ(actualOut, expectedOut);
}

TEST(SortKeyGeneratorTest, EnsureSortKeyGenerationForArraysWithPredicateRespectsCollation) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    BSONObj actualOut = extractSortKey(
        "{a: 1}", "{_id: 0, a: ['aaz', 'zza', 'yya', 'zzb']}", "{a: {$gt: 'yya'}}", &collator);
    BSONObj expectedOut = BSON(""
                               << "azz");
    ASSERT_EQ(actualOut, expectedOut);
}

}  // namespace
}  // namespace mongo
