/**
 *    Copyright (C) 2013 MongoDB Inc.
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

#include <boost/optional.hpp>
#include <boost/optional/optional_io.hpp>

#include "mongo/db/json.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/lite_parsed_query.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using std::unique_ptr;
using unittest::assertGet;

static const NamespaceString testns("testdb.testcoll");

TEST(LiteParsedQueryTest, LimitWithNToReturn) {
    LiteParsedQuery lpq(testns);
    lpq.setLimit(0);
    lpq.setNToReturn(0);
    ASSERT_NOT_OK(lpq.validate());
}

TEST(LiteParsedQueryTest, BatchSizeWithNToReturn) {
    LiteParsedQuery lpq(testns);
    lpq.setBatchSize(0);
    lpq.setNToReturn(0);
    ASSERT_NOT_OK(lpq.validate());
}

TEST(LiteParsedQueryTest, NegativeSkip) {
    LiteParsedQuery lpq(testns);
    lpq.setSkip(-1);
    ASSERT_NOT_OK(lpq.validate());
}

TEST(LiteParsedQueryTest, ZeroSkip) {
    LiteParsedQuery lpq(testns);
    lpq.setSkip(0);
    ASSERT_OK(lpq.validate());
}

TEST(LiteParsedQueryTest, PositiveSkip) {
    LiteParsedQuery lpq(testns);
    lpq.setSkip(1);
    ASSERT_OK(lpq.validate());
}

TEST(LiteParsedQueryTest, NegativeLimit) {
    LiteParsedQuery lpq(testns);
    lpq.setLimit(-1);
    ASSERT_NOT_OK(lpq.validate());
}

TEST(LiteParsedQueryTest, ZeroLimit) {
    LiteParsedQuery lpq(testns);
    lpq.setLimit(0);
    ASSERT_OK(lpq.validate());
}

TEST(LiteParsedQueryTest, PositiveLimit) {
    LiteParsedQuery lpq(testns);
    lpq.setLimit(1);
    ASSERT_OK(lpq.validate());
}

TEST(LiteParsedQueryTest, NegativeBatchSize) {
    LiteParsedQuery lpq(testns);
    lpq.setBatchSize(-1);
    ASSERT_NOT_OK(lpq.validate());
}

TEST(LiteParsedQueryTest, ZeroBatchSize) {
    LiteParsedQuery lpq(testns);
    lpq.setBatchSize(0);
    ASSERT_OK(lpq.validate());
}

TEST(LiteParsedQueryTest, PositiveBatchSize) {
    LiteParsedQuery lpq(testns);
    lpq.setBatchSize(1);
    ASSERT_OK(lpq.validate());
}

TEST(LiteParsedQueryTest, NegativeNToReturn) {
    LiteParsedQuery lpq(testns);
    lpq.setNToReturn(-1);
    ASSERT_NOT_OK(lpq.validate());
}

TEST(LiteParsedQueryTest, ZeroNToReturn) {
    LiteParsedQuery lpq(testns);
    lpq.setNToReturn(0);
    ASSERT_OK(lpq.validate());
}

TEST(LiteParsedQueryTest, PositiveNToReturn) {
    LiteParsedQuery lpq(testns);
    lpq.setNToReturn(1);
    ASSERT_OK(lpq.validate());
}

TEST(LiteParsedQueryTest, NegativeMaxScan) {
    LiteParsedQuery lpq(testns);
    lpq.setMaxScan(-1);
    ASSERT_NOT_OK(lpq.validate());
}

TEST(LiteParsedQueryTest, ZeroMaxScan) {
    LiteParsedQuery lpq(testns);
    lpq.setMaxScan(0);
    ASSERT_OK(lpq.validate());
}

TEST(LiteParsedQueryTest, PositiveMaxScan) {
    LiteParsedQuery lpq(testns);
    lpq.setMaxScan(1);
    ASSERT_OK(lpq.validate());
}

TEST(LiteParsedQueryTest, NegativeMaxTimeMS) {
    LiteParsedQuery lpq(testns);
    lpq.setMaxTimeMS(-1);
    ASSERT_NOT_OK(lpq.validate());
}

TEST(LiteParsedQueryTest, ZeroMaxTimeMS) {
    LiteParsedQuery lpq(testns);
    lpq.setMaxTimeMS(0);
    ASSERT_OK(lpq.validate());
}

TEST(LiteParsedQueryTest, PositiveMaxTimeMS) {
    LiteParsedQuery lpq(testns);
    lpq.setMaxTimeMS(1);
    ASSERT_OK(lpq.validate());
}

TEST(LiteParsedQueryTest, ValidSortOrder) {
    LiteParsedQuery lpq(testns);
    lpq.setSort(fromjson("{a: 1}"));
    ASSERT_OK(lpq.validate());
}

TEST(LiteParsedQueryTest, InvalidSortOrderString) {
    LiteParsedQuery lpq(testns);
    lpq.setSort(fromjson("{a: \"\"}"));
    ASSERT_NOT_OK(lpq.validate());
}

TEST(LiteParsedQueryTest, MinFieldsNotPrefixOfMax) {
    LiteParsedQuery lpq(testns);
    lpq.setMin(fromjson("{a: 1}"));
    lpq.setMax(fromjson("{b: 1}"));
    ASSERT_NOT_OK(lpq.validate());
}

TEST(LiteParsedQueryTest, MinFieldsMoreThanMax) {
    LiteParsedQuery lpq(testns);
    lpq.setMin(fromjson("{a: 1, b: 1}"));
    lpq.setMax(fromjson("{a: 1}"));
    ASSERT_NOT_OK(lpq.validate());
}

TEST(LiteParsedQueryTest, MinFieldsLessThanMax) {
    LiteParsedQuery lpq(testns);
    lpq.setMin(fromjson("{a: 1}"));
    lpq.setMax(fromjson("{a: 1, b: 1}"));
    ASSERT_NOT_OK(lpq.validate());
}

TEST(LiteParsedQueryTest, ForbidTailableWithNonNaturalSort) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "tailable: true,"
        "sort: {a: 1}}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(LiteParsedQueryTest, ForbidTailableWithSingleBatch) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "tailable: true,"
        "singleBatch: true}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(LiteParsedQueryTest, AllowTailableWithNaturalSort) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "tailable: true,"
        "sort: {$natural: 1}}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_OK(result.getStatus());
    ASSERT_TRUE(result.getValue()->isTailable());
    ASSERT_EQ(result.getValue()->getSort(), BSON("$natural" << 1));
}

TEST(LiteParsedQueryTest, IsIsolatedReturnsTrueWithIsolated) {
    ASSERT_TRUE(LiteParsedQuery::isQueryIsolated(BSON("$isolated" << 1)));
}

TEST(LiteParsedQueryTest, IsIsolatedReturnsTrueWithAtomic) {
    ASSERT_TRUE(LiteParsedQuery::isQueryIsolated(BSON("$atomic" << 1)));
}

TEST(LiteParsedQueryTest, IsIsolatedReturnsFalseWithIsolated) {
    ASSERT_FALSE(LiteParsedQuery::isQueryIsolated(BSON("$isolated" << false)));
}

TEST(LiteParsedQueryTest, IsIsolatedReturnsFalseWithAtomic) {
    ASSERT_FALSE(LiteParsedQuery::isQueryIsolated(BSON("$atomic" << false)));
}

//
// Test compatibility of various projection and sort objects.
//

TEST(LiteParsedQueryTest, ValidSortProj) {
    LiteParsedQuery lpq(testns);
    lpq.setProj(fromjson("{a: 1}"));
    lpq.setSort(fromjson("{a: 1}"));
    ASSERT_OK(lpq.validate());

    LiteParsedQuery metaLPQ(testns);
    metaLPQ.setProj(fromjson("{a: {$meta: \"textScore\"}}"));
    metaLPQ.setSort(fromjson("{a: {$meta: \"textScore\"}}"));
    ASSERT_OK(metaLPQ.validate());
}

TEST(LiteParsedQueryTest, ForbidNonMetaSortOnFieldWithMetaProject) {
    LiteParsedQuery badLPQ(testns);
    badLPQ.setProj(fromjson("{a: {$meta: \"textScore\"}}"));
    badLPQ.setSort(fromjson("{a: 1}"));
    ASSERT_NOT_OK(badLPQ.validate());

    LiteParsedQuery goodLPQ(testns);
    goodLPQ.setProj(fromjson("{a: {$meta: \"textScore\"}}"));
    goodLPQ.setSort(fromjson("{b: 1}"));
    ASSERT_OK(goodLPQ.validate());
}

TEST(LiteParsedQueryTest, ForbidMetaSortOnFieldWithoutMetaProject) {
    LiteParsedQuery lpqMatching(testns);
    lpqMatching.setProj(fromjson("{a: 1}"));
    lpqMatching.setSort(fromjson("{a: {$meta: \"textScore\"}}"));
    ASSERT_NOT_OK(lpqMatching.validate());

    LiteParsedQuery lpqNonMatching(testns);
    lpqNonMatching.setProj(fromjson("{b: 1}"));
    lpqNonMatching.setSort(fromjson("{a: {$meta: \"textScore\"}}"));
    ASSERT_NOT_OK(lpqNonMatching.validate());
}

//
// Text meta BSON element validation
//

bool isFirstElementTextScoreMeta(const char* sortStr) {
    BSONObj sortObj = fromjson(sortStr);
    BSONElement elt = sortObj.firstElement();
    bool result = LiteParsedQuery::isTextScoreMeta(elt);
    return result;
}

// Check validation of $meta expressions
TEST(LiteParsedQueryTest, IsTextScoreMeta) {
    // Valid textScore meta sort
    ASSERT(isFirstElementTextScoreMeta("{a: {$meta: \"textScore\"}}"));

    // Invalid textScore meta sorts
    ASSERT_FALSE(isFirstElementTextScoreMeta("{a: {$meta: 1}}"));
    ASSERT_FALSE(isFirstElementTextScoreMeta("{a: {$meta: \"image\"}}"));
    ASSERT_FALSE(isFirstElementTextScoreMeta("{a: {$world: \"textScore\"}}"));
    ASSERT_FALSE(isFirstElementTextScoreMeta("{a: {$meta: \"textScore\", b: 1}}"));
}

//
// Sort order validation
// In a valid sort order, each element satisfies one of:
// 1. a number with value 1
// 2. a number with value -1
// 3. isTextScoreMeta
//

TEST(LiteParsedQueryTest, ValidateSortOrder) {
    // Valid sorts
    ASSERT(LiteParsedQuery::isValidSortOrder(fromjson("{}")));
    ASSERT(LiteParsedQuery::isValidSortOrder(fromjson("{a: 1}")));
    ASSERT(LiteParsedQuery::isValidSortOrder(fromjson("{a: -1}")));
    ASSERT(LiteParsedQuery::isValidSortOrder(fromjson("{a: {$meta: \"textScore\"}}")));

    // Invalid sorts
    ASSERT_FALSE(LiteParsedQuery::isValidSortOrder(fromjson("{a: 100}")));
    ASSERT_FALSE(LiteParsedQuery::isValidSortOrder(fromjson("{a: 0}")));
    ASSERT_FALSE(LiteParsedQuery::isValidSortOrder(fromjson("{a: -100}")));
    ASSERT_FALSE(LiteParsedQuery::isValidSortOrder(fromjson("{a: Infinity}")));
    ASSERT_FALSE(LiteParsedQuery::isValidSortOrder(fromjson("{a: -Infinity}")));
    ASSERT_FALSE(LiteParsedQuery::isValidSortOrder(fromjson("{a: true}")));
    ASSERT_FALSE(LiteParsedQuery::isValidSortOrder(fromjson("{a: false}")));
    ASSERT_FALSE(LiteParsedQuery::isValidSortOrder(fromjson("{a: null}")));
    ASSERT_FALSE(LiteParsedQuery::isValidSortOrder(fromjson("{a: {}}")));
    ASSERT_FALSE(LiteParsedQuery::isValidSortOrder(fromjson("{a: {b: 1}}")));
    ASSERT_FALSE(LiteParsedQuery::isValidSortOrder(fromjson("{a: []}")));
    ASSERT_FALSE(LiteParsedQuery::isValidSortOrder(fromjson("{a: [1, 2, 3]}")));
    ASSERT_FALSE(LiteParsedQuery::isValidSortOrder(fromjson("{a: \"\"}")));
    ASSERT_FALSE(LiteParsedQuery::isValidSortOrder(fromjson("{a: \"bb\"}")));
    ASSERT_FALSE(LiteParsedQuery::isValidSortOrder(fromjson("{a: {$meta: 1}}")));
    ASSERT_FALSE(LiteParsedQuery::isValidSortOrder(fromjson("{a: {$meta: \"image\"}}")));
    ASSERT_FALSE(LiteParsedQuery::isValidSortOrder(fromjson("{a: {$world: \"textScore\"}}")));
    ASSERT_FALSE(
        LiteParsedQuery::isValidSortOrder(fromjson("{a: {$meta: \"textScore\","
                                                   " b: 1}}")));
    ASSERT_FALSE(LiteParsedQuery::isValidSortOrder(fromjson("{'': 1}")));
    ASSERT_FALSE(LiteParsedQuery::isValidSortOrder(fromjson("{'': -1}")));
}

//
// Tests for parsing a lite parsed query from a command BSON object.
//

TEST(LiteParsedQueryTest, ParseFromCommandBasic) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter: {a: 3},"
        "sort: {a: 1},"
        "projection: {_id: 0, a: 1}}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_OK(result.getStatus());
}

TEST(LiteParsedQueryTest, ParseFromCommandWithOptions) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter: {a: 3},"
        "sort: {a: 1},"
        "projection: {_id: 0, a: 1},"
        "showRecordId: true,"
        "maxScan: 1000}}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    unique_ptr<LiteParsedQuery> lpq(
        assertGet(LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain)));

    // Make sure the values from the command BSON are reflected in the LPQ.
    ASSERT(lpq->showRecordId());
    ASSERT_EQUALS(1000, lpq->getMaxScan());
}

TEST(LiteParsedQueryTest, ParseFromCommandHintAsString) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "hint: 'foo_1'}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    unique_ptr<LiteParsedQuery> lpq(
        assertGet(LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain)));

    BSONObj hintObj = lpq->getHint();
    ASSERT_EQUALS(BSON("$hint"
                       << "foo_1"),
                  hintObj);
}

TEST(LiteParsedQueryTest, ParseFromCommandValidSortProj) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "projection: {a: 1},"
        "sort: {a: 1}}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    ASSERT_OK(LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain).getStatus());
}

TEST(LiteParsedQueryTest, ParseFromCommandValidSortProjMeta) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "projection: {a: {$meta: 'textScore'}},"
        "sort: {a: {$meta: 'textScore'}}}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    ASSERT_OK(LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain).getStatus());
}

TEST(LiteParsedQueryTest, ParseFromCommandAllFlagsTrue) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "tailable: true,"
        "oplogReplay: true,"
        "noCursorTimeout: true,"
        "awaitData: true,"
        "allowPartialResults: true}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    unique_ptr<LiteParsedQuery> lpq(
        assertGet(LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain)));

    // Test that all the flags got set to true.
    ASSERT(lpq->isTailable());
    ASSERT(!lpq->isSlaveOk());
    ASSERT(lpq->isOplogReplay());
    ASSERT(lpq->isNoCursorTimeout());
    ASSERT(lpq->isAwaitData());
    ASSERT(lpq->isAllowPartialResults());
}

TEST(LiteParsedQueryTest, ParseFromCommandCommentWithValidMinMax) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "comment: 'the comment',"
        "min: {a: 1},"
        "max: {a: 2}}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    unique_ptr<LiteParsedQuery> lpq(
        assertGet(LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain)));

    ASSERT_EQUALS("the comment", lpq->getComment());
    BSONObj expectedMin = BSON("a" << 1);
    ASSERT_EQUALS(0, expectedMin.woCompare(lpq->getMin()));
    BSONObj expectedMax = BSON("a" << 2);
    ASSERT_EQUALS(0, expectedMax.woCompare(lpq->getMax()));
}

TEST(LiteParsedQueryTest, ParseFromCommandAllNonOptionFields) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter: {a: 1},"
        "sort: {b: 1},"
        "projection: {c: 1},"
        "hint: {d: 1},"
        "readConcern: {e: 1},"
        "collation: {f: 1},"
        "limit: 3,"
        "skip: 5,"
        "batchSize: 90,"
        "singleBatch: false}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    unique_ptr<LiteParsedQuery> lpq(
        assertGet(LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain)));

    // Check the values inside the LPQ.
    BSONObj expectedQuery = BSON("a" << 1);
    ASSERT_EQUALS(0, expectedQuery.woCompare(lpq->getFilter()));
    BSONObj expectedSort = BSON("b" << 1);
    ASSERT_EQUALS(0, expectedSort.woCompare(lpq->getSort()));
    BSONObj expectedProj = BSON("c" << 1);
    ASSERT_EQUALS(0, expectedProj.woCompare(lpq->getProj()));
    BSONObj expectedHint = BSON("d" << 1);
    ASSERT_EQUALS(0, expectedHint.woCompare(lpq->getHint()));
    BSONObj expectedReadConcern = BSON("e" << 1);
    ASSERT_EQUALS(0, expectedReadConcern.woCompare(lpq->getReadConcern()));
    BSONObj expectedCollation = BSON("f" << 1);
    ASSERT_EQUALS(0, expectedCollation.woCompare(lpq->getCollation()));
    ASSERT_EQUALS(3, *lpq->getLimit());
    ASSERT_EQUALS(5, *lpq->getSkip());
    ASSERT_EQUALS(90, *lpq->getBatchSize());
    ASSERT(lpq->wantMore());
}

TEST(LiteParsedQueryTest, ParseFromCommandLargeLimit) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter: {a: 1},"
        "limit: 8000000000}");  // 8 * 1000 * 1000 * 1000
    const NamespaceString nss("test.testns");
    const bool isExplain = false;
    unique_ptr<LiteParsedQuery> lpq(
        assertGet(LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain)));

    ASSERT_EQUALS(8LL * 1000 * 1000 * 1000, *lpq->getLimit());
}

TEST(LiteParsedQueryTest, ParseFromCommandLargeBatchSize) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter: {a: 1},"
        "batchSize: 8000000000}");  // 8 * 1000 * 1000 * 1000
    const NamespaceString nss("test.testns");
    const bool isExplain = false;
    unique_ptr<LiteParsedQuery> lpq(
        assertGet(LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain)));

    ASSERT_EQUALS(8LL * 1000 * 1000 * 1000, *lpq->getBatchSize());
}

TEST(LiteParsedQueryTest, ParseFromCommandLargeSkip) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter: {a: 1},"
        "skip: 8000000000}");  // 8 * 1000 * 1000 * 1000
    const NamespaceString nss("test.testns");
    const bool isExplain = false;
    unique_ptr<LiteParsedQuery> lpq(
        assertGet(LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain)));

    ASSERT_EQUALS(8LL * 1000 * 1000 * 1000, *lpq->getSkip());
}

//
// Parsing errors where a field has the wrong type.
//

TEST(LiteParsedQueryTest, ParseFromCommandQueryWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter: 3}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(LiteParsedQueryTest, ParseFromCommandSortWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "sort: 3}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(LiteParsedQueryTest, ParseFromCommandProjWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "projection: 'foo'}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(LiteParsedQueryTest, ParseFromCommandSkipWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "skip: '5',"
        "projection: {a: 1}}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(LiteParsedQueryTest, ParseFromCommandLimitWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "limit: '5',"
        "projection: {a: 1}}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(LiteParsedQueryTest, ParseFromCommandSingleBatchWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "singleBatch: 'false',"
        "projection: {a: 1}}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(LiteParsedQueryTest, ParseFromCommandCommentWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "comment: 1}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(LiteParsedQueryTest, ParseFromCommandMaxScanWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "maxScan: true,"
        "comment: 'foo'}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(LiteParsedQueryTest, ParseFromCommandMaxTimeMSWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "maxTimeMS: true}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(LiteParsedQueryTest, ParseFromCommandMaxWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "max: 3}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(LiteParsedQueryTest, ParseFromCommandMinWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "min: 3}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(LiteParsedQueryTest, ParseFromCommandReturnKeyWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "returnKey: 3}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}


TEST(LiteParsedQueryTest, ParseFromCommandShowRecordIdWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "showRecordId: 3}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(LiteParsedQueryTest, ParseFromCommandSnapshotWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "snapshot: 3}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(LiteParsedQueryTest, ParseFromCommandTailableWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "tailable: 3}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(LiteParsedQueryTest, ParseFromCommandSlaveOkWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "slaveOk: 3}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(LiteParsedQueryTest, ParseFromCommandOplogReplayWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "oplogReplay: 3}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(LiteParsedQueryTest, ParseFromCommandNoCursorTimeoutWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "noCursorTimeout: 3}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(LiteParsedQueryTest, ParseFromCommandAwaitDataWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "tailable: true,"
        "awaitData: 3}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(LiteParsedQueryTest, ParseFromCommandExhaustWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "exhaust: 3}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(LiteParsedQueryTest, ParseFromCommandPartialWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "allowPartialResults: 3}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(LiteParsedQueryTest, ParseFromCommandReadConcernWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "readConcern: 'foo'}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(LiteParsedQueryTest, ParseFromCommandCollationWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "collation: 'foo'}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}
//
// Parsing errors where a field has the right type but a bad value.
//

TEST(LiteParsedQueryTest, ParseFromCommandNegativeSkipError) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "skip: -3,"
        "filter: {a: 3}}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(LiteParsedQueryTest, ParseFromCommandSkipIsZero) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "skip: 0,"
        "filter: {a: 3}}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    unique_ptr<LiteParsedQuery> lpq(
        assertGet(LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain)));
    ASSERT_EQ(BSON("a" << 3), lpq->getFilter());
    ASSERT_FALSE(lpq->getSkip());
}

TEST(LiteParsedQueryTest, ParseFromCommandNegativeLimitError) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "limit: -3,"
        "filter: {a: 3}}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(LiteParsedQueryTest, ParseFromCommandLimitIsZero) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "limit: 0,"
        "filter: {a: 3}}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    unique_ptr<LiteParsedQuery> lpq(
        assertGet(LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain)));
    ASSERT_EQ(BSON("a" << 3), lpq->getFilter());
    ASSERT_FALSE(lpq->getLimit());
}

TEST(LiteParsedQueryTest, ParseFromCommandNegativeBatchSizeError) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "batchSize: -10,"
        "filter: {a: 3}}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(LiteParsedQueryTest, ParseFromCommandBatchSizeZero) {
    BSONObj cmdObj = fromjson("{find: 'testns', batchSize: 0}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    unique_ptr<LiteParsedQuery> lpq(
        assertGet(LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain)));

    ASSERT(lpq->getBatchSize());
    ASSERT_EQ(0, *lpq->getBatchSize());

    ASSERT(!lpq->getLimit());
}

TEST(LiteParsedQueryTest, ParseFromCommandDefaultBatchSize) {
    BSONObj cmdObj = fromjson("{find: 'testns'}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    unique_ptr<LiteParsedQuery> lpq(
        assertGet(LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain)));

    ASSERT(!lpq->getBatchSize());
    ASSERT(!lpq->getLimit());
}

//
// Errors checked in LiteParsedQuery::validate().
//

TEST(LiteParsedQueryTest, ParseFromCommandMinMaxDifferentFieldsError) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "min: {a: 3},"
        "max: {b: 4}}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(LiteParsedQueryTest, ParseFromCommandSnapshotPlusSortError) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "sort: {a: 3},"
        "snapshot: true}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(LiteParsedQueryTest, ParseFromCommandSnapshotPlusHintError) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "snapshot: true,"
        "hint: {a: 1}}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(LiteParsedQueryTest, ParseCommandForbidNonMetaSortOnFieldWithMetaProject) {
    BSONObj cmdObj;

    cmdObj = fromjson(
        "{find: 'testns',"
        "projection: {a: {$meta: 'textScore'}},"
        "sort: {a: 1}}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());

    cmdObj = fromjson(
        "{find: 'testns',"
        "projection: {a: {$meta: 'textScore'}},"
        "sort: {b: 1}}");
    ASSERT_OK(LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain).getStatus());
}

TEST(LiteParsedQueryTest, ParseCommandForbidMetaSortOnFieldWithoutMetaProject) {
    BSONObj cmdObj;

    cmdObj = fromjson(
        "{find: 'testns',"
        "projection: {a: 1},"
        "sort: {a: {$meta: 'textScore'}}}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());

    cmdObj = fromjson(
        "{find: 'testns',"
        "projection: {b: 1},"
        "sort: {a: {$meta: 'textScore'}}}");
    result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(LiteParsedQueryTest, ParseCommandForbidExhaust) {
    BSONObj cmdObj = fromjson("{find: 'testns', exhaust: true}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(LiteParsedQueryTest, ParseCommandIsFromFindCommand) {
    BSONObj cmdObj = fromjson("{find: 'testns'}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    unique_ptr<LiteParsedQuery> lpq(
        assertGet(LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain)));

    ASSERT_FALSE(lpq->getNToReturn());
}

TEST(LiteParsedQueryTest, ParseCommandAwaitDataButNotTailable) {
    const NamespaceString nss("test.testns");
    BSONObj cmdObj = fromjson("{find: 'testns', awaitData: true}");
    bool isExplain = false;
    auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(LiteParsedQueryTest, ParseCommandFirstFieldNotString) {
    BSONObj cmdObj = fromjson("{find: 1}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(LiteParsedQueryTest, ParseCommandIgnoreShardVersionField) {
    BSONObj cmdObj = fromjson("{find: 'test.testns', shardVersion: 'foo'}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_OK(result.getStatus());
}

TEST(LiteParsedQueryTest, DefaultQueryParametersCorrect) {
    BSONObj cmdObj = fromjson("{find: 'testns'}");

    const NamespaceString nss("test.testns");
    std::unique_ptr<LiteParsedQuery> lpq(
        assertGet(LiteParsedQuery::makeFromFindCommand(nss, cmdObj, false)));

    ASSERT_FALSE(lpq->getSkip());
    ASSERT_FALSE(lpq->getLimit());

    ASSERT_EQUALS(true, lpq->wantMore());
    ASSERT_FALSE(lpq->getNToReturn());
    ASSERT_EQUALS(false, lpq->isExplain());
    ASSERT_EQUALS(0, lpq->getMaxScan());
    ASSERT_EQUALS(0, lpq->getMaxTimeMS());
    ASSERT_EQUALS(false, lpq->returnKey());
    ASSERT_EQUALS(false, lpq->showRecordId());
    ASSERT_EQUALS(false, lpq->isSnapshot());
    ASSERT_EQUALS(false, lpq->hasReadPref());
    ASSERT_EQUALS(false, lpq->isTailable());
    ASSERT_EQUALS(false, lpq->isSlaveOk());
    ASSERT_EQUALS(false, lpq->isOplogReplay());
    ASSERT_EQUALS(false, lpq->isNoCursorTimeout());
    ASSERT_EQUALS(false, lpq->isAwaitData());
    ASSERT_EQUALS(false, lpq->isExhaust());
    ASSERT_EQUALS(false, lpq->isAllowPartialResults());
}

//
// Extra fields cause the parse to fail.
//

TEST(LiteParsedQueryTest, ParseFromCommandForbidExtraField) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "snapshot: true,"
        "foo: {a: 1}}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(LiteParsedQueryTest, ParseFromCommandForbidExtraOption) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "snapshot: true,"
        "foo: true}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(LiteParsedQueryTest, ParseMaxTimeMSStringValueFails) {
    BSONObj maxTimeObj = BSON(LiteParsedQuery::cmdOptionMaxTimeMS << "foo");
    ASSERT_NOT_OK(LiteParsedQuery::parseMaxTimeMS(maxTimeObj[LiteParsedQuery::cmdOptionMaxTimeMS]));
}

TEST(LiteParsedQueryTest, ParseMaxTimeMSNonIntegralValueFails) {
    BSONObj maxTimeObj = BSON(LiteParsedQuery::cmdOptionMaxTimeMS << 100.3);
    ASSERT_NOT_OK(LiteParsedQuery::parseMaxTimeMS(maxTimeObj[LiteParsedQuery::cmdOptionMaxTimeMS]));
}

TEST(LiteParsedQueryTest, ParseMaxTimeMSOutOfRangeDoubleFails) {
    BSONObj maxTimeObj = BSON(LiteParsedQuery::cmdOptionMaxTimeMS << 1e200);
    ASSERT_NOT_OK(LiteParsedQuery::parseMaxTimeMS(maxTimeObj[LiteParsedQuery::cmdOptionMaxTimeMS]));
}

TEST(LiteParsedQueryTest, ParseMaxTimeMSNegativeValueFails) {
    BSONObj maxTimeObj = BSON(LiteParsedQuery::cmdOptionMaxTimeMS << -400);
    ASSERT_NOT_OK(LiteParsedQuery::parseMaxTimeMS(maxTimeObj[LiteParsedQuery::cmdOptionMaxTimeMS]));
}

TEST(LiteParsedQueryTest, ParseMaxTimeMSZeroSucceeds) {
    BSONObj maxTimeObj = BSON(LiteParsedQuery::cmdOptionMaxTimeMS << 0);
    auto maxTime = LiteParsedQuery::parseMaxTimeMS(maxTimeObj[LiteParsedQuery::cmdOptionMaxTimeMS]);
    ASSERT_OK(maxTime);
    ASSERT_EQ(maxTime.getValue(), 0);
}

TEST(LiteParsedQueryTest, ParseMaxTimeMSPositiveInRangeSucceeds) {
    BSONObj maxTimeObj = BSON(LiteParsedQuery::cmdOptionMaxTimeMS << 300);
    auto maxTime = LiteParsedQuery::parseMaxTimeMS(maxTimeObj[LiteParsedQuery::cmdOptionMaxTimeMS]);
    ASSERT_OK(maxTime);
    ASSERT_EQ(maxTime.getValue(), 300);
}

}  // namespace mongo
}  // namespace
