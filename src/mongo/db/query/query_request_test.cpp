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
#include "mongo/db/query/query_request.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using std::unique_ptr;
using unittest::assertGet;

static const NamespaceString testns("testdb.testcoll");

TEST(QueryRequestTest, LimitWithNToReturn) {
    QueryRequest qr(testns);
    qr.setLimit(0);
    qr.setNToReturn(0);
    ASSERT_NOT_OK(qr.validate());
}

TEST(QueryRequestTest, BatchSizeWithNToReturn) {
    QueryRequest qr(testns);
    qr.setBatchSize(0);
    qr.setNToReturn(0);
    ASSERT_NOT_OK(qr.validate());
}

TEST(QueryRequestTest, NegativeSkip) {
    QueryRequest qr(testns);
    qr.setSkip(-1);
    ASSERT_NOT_OK(qr.validate());
}

TEST(QueryRequestTest, ZeroSkip) {
    QueryRequest qr(testns);
    qr.setSkip(0);
    ASSERT_OK(qr.validate());
}

TEST(QueryRequestTest, PositiveSkip) {
    QueryRequest qr(testns);
    qr.setSkip(1);
    ASSERT_OK(qr.validate());
}

TEST(QueryRequestTest, NegativeLimit) {
    QueryRequest qr(testns);
    qr.setLimit(-1);
    ASSERT_NOT_OK(qr.validate());
}

TEST(QueryRequestTest, ZeroLimit) {
    QueryRequest qr(testns);
    qr.setLimit(0);
    ASSERT_OK(qr.validate());
}

TEST(QueryRequestTest, PositiveLimit) {
    QueryRequest qr(testns);
    qr.setLimit(1);
    ASSERT_OK(qr.validate());
}

TEST(QueryRequestTest, NegativeBatchSize) {
    QueryRequest qr(testns);
    qr.setBatchSize(-1);
    ASSERT_NOT_OK(qr.validate());
}

TEST(QueryRequestTest, ZeroBatchSize) {
    QueryRequest qr(testns);
    qr.setBatchSize(0);
    ASSERT_OK(qr.validate());
}

TEST(QueryRequestTest, PositiveBatchSize) {
    QueryRequest qr(testns);
    qr.setBatchSize(1);
    ASSERT_OK(qr.validate());
}

TEST(QueryRequestTest, NegativeNToReturn) {
    QueryRequest qr(testns);
    qr.setNToReturn(-1);
    ASSERT_NOT_OK(qr.validate());
}

TEST(QueryRequestTest, ZeroNToReturn) {
    QueryRequest qr(testns);
    qr.setNToReturn(0);
    ASSERT_OK(qr.validate());
}

TEST(QueryRequestTest, PositiveNToReturn) {
    QueryRequest qr(testns);
    qr.setNToReturn(1);
    ASSERT_OK(qr.validate());
}

TEST(QueryRequestTest, NegativeMaxScan) {
    QueryRequest qr(testns);
    qr.setMaxScan(-1);
    ASSERT_NOT_OK(qr.validate());
}

TEST(QueryRequestTest, ZeroMaxScan) {
    QueryRequest qr(testns);
    qr.setMaxScan(0);
    ASSERT_OK(qr.validate());
}

TEST(QueryRequestTest, PositiveMaxScan) {
    QueryRequest qr(testns);
    qr.setMaxScan(1);
    ASSERT_OK(qr.validate());
}

TEST(QueryRequestTest, NegativeMaxTimeMS) {
    QueryRequest qr(testns);
    qr.setMaxTimeMS(-1);
    ASSERT_NOT_OK(qr.validate());
}

TEST(QueryRequestTest, ZeroMaxTimeMS) {
    QueryRequest qr(testns);
    qr.setMaxTimeMS(0);
    ASSERT_OK(qr.validate());
}

TEST(QueryRequestTest, PositiveMaxTimeMS) {
    QueryRequest qr(testns);
    qr.setMaxTimeMS(1);
    ASSERT_OK(qr.validate());
}

TEST(QueryRequestTest, ValidSortOrder) {
    QueryRequest qr(testns);
    qr.setSort(fromjson("{a: 1}"));
    ASSERT_OK(qr.validate());
}

TEST(QueryRequestTest, InvalidSortOrderString) {
    QueryRequest qr(testns);
    qr.setSort(fromjson("{a: \"\"}"));
    ASSERT_NOT_OK(qr.validate());
}

TEST(QueryRequestTest, MinFieldsNotPrefixOfMax) {
    QueryRequest qr(testns);
    qr.setMin(fromjson("{a: 1}"));
    qr.setMax(fromjson("{b: 1}"));
    ASSERT_NOT_OK(qr.validate());
}

TEST(QueryRequestTest, MinFieldsMoreThanMax) {
    QueryRequest qr(testns);
    qr.setMin(fromjson("{a: 1, b: 1}"));
    qr.setMax(fromjson("{a: 1}"));
    ASSERT_NOT_OK(qr.validate());
}

TEST(QueryRequestTest, MinFieldsLessThanMax) {
    QueryRequest qr(testns);
    qr.setMin(fromjson("{a: 1}"));
    qr.setMax(fromjson("{a: 1, b: 1}"));
    ASSERT_NOT_OK(qr.validate());
}

TEST(QueryRequestTest, ForbidTailableWithNonNaturalSort) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "tailable: true,"
        "sort: {a: 1}}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(QueryRequestTest, ForbidTailableWithSingleBatch) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "tailable: true,"
        "singleBatch: true}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(QueryRequestTest, AllowTailableWithNaturalSort) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "tailable: true,"
        "sort: {$natural: 1}}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_OK(result.getStatus());
    ASSERT_TRUE(result.getValue()->isTailable());
    ASSERT_EQ(result.getValue()->getSort(), BSON("$natural" << 1));
}

TEST(QueryRequestTest, IsIsolatedReturnsTrueWithIsolated) {
    ASSERT_TRUE(QueryRequest::isQueryIsolated(BSON("$isolated" << 1)));
}

TEST(QueryRequestTest, IsIsolatedReturnsTrueWithAtomic) {
    ASSERT_TRUE(QueryRequest::isQueryIsolated(BSON("$atomic" << 1)));
}

TEST(QueryRequestTest, IsIsolatedReturnsFalseWithIsolated) {
    ASSERT_FALSE(QueryRequest::isQueryIsolated(BSON("$isolated" << false)));
}

TEST(QueryRequestTest, IsIsolatedReturnsFalseWithAtomic) {
    ASSERT_FALSE(QueryRequest::isQueryIsolated(BSON("$atomic" << false)));
}

//
// Test compatibility of various projection and sort objects.
//

TEST(QueryRequestTest, ValidSortProj) {
    QueryRequest qr(testns);
    qr.setProj(fromjson("{a: 1}"));
    qr.setSort(fromjson("{a: 1}"));
    ASSERT_OK(qr.validate());

    QueryRequest metaQR(testns);
    metaQR.setProj(fromjson("{a: {$meta: \"textScore\"}}"));
    metaQR.setSort(fromjson("{a: {$meta: \"textScore\"}}"));
    ASSERT_OK(metaQR.validate());
}

TEST(QueryRequestTest, ForbidNonMetaSortOnFieldWithMetaProject) {
    QueryRequest badQR(testns);
    badQR.setProj(fromjson("{a: {$meta: \"textScore\"}}"));
    badQR.setSort(fromjson("{a: 1}"));
    ASSERT_NOT_OK(badQR.validate());

    QueryRequest goodQR(testns);
    goodQR.setProj(fromjson("{a: {$meta: \"textScore\"}}"));
    goodQR.setSort(fromjson("{b: 1}"));
    ASSERT_OK(goodQR.validate());
}

TEST(QueryRequestTest, ForbidMetaSortOnFieldWithoutMetaProject) {
    QueryRequest qrMatching(testns);
    qrMatching.setProj(fromjson("{a: 1}"));
    qrMatching.setSort(fromjson("{a: {$meta: \"textScore\"}}"));
    ASSERT_NOT_OK(qrMatching.validate());

    QueryRequest qrNonMatching(testns);
    qrNonMatching.setProj(fromjson("{b: 1}"));
    qrNonMatching.setSort(fromjson("{a: {$meta: \"textScore\"}}"));
    ASSERT_NOT_OK(qrNonMatching.validate());
}

//
// Text meta BSON element validation
//

bool isFirstElementTextScoreMeta(const char* sortStr) {
    BSONObj sortObj = fromjson(sortStr);
    BSONElement elt = sortObj.firstElement();
    bool result = QueryRequest::isTextScoreMeta(elt);
    return result;
}

// Check validation of $meta expressions
TEST(QueryRequestTest, IsTextScoreMeta) {
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

TEST(QueryRequestTest, ValidateSortOrder) {
    // Valid sorts
    ASSERT(QueryRequest::isValidSortOrder(fromjson("{}")));
    ASSERT(QueryRequest::isValidSortOrder(fromjson("{a: 1}")));
    ASSERT(QueryRequest::isValidSortOrder(fromjson("{a: -1}")));
    ASSERT(QueryRequest::isValidSortOrder(fromjson("{a: {$meta: \"textScore\"}}")));

    // Invalid sorts
    ASSERT_FALSE(QueryRequest::isValidSortOrder(fromjson("{a: 100}")));
    ASSERT_FALSE(QueryRequest::isValidSortOrder(fromjson("{a: 0}")));
    ASSERT_FALSE(QueryRequest::isValidSortOrder(fromjson("{a: -100}")));
    ASSERT_FALSE(QueryRequest::isValidSortOrder(fromjson("{a: Infinity}")));
    ASSERT_FALSE(QueryRequest::isValidSortOrder(fromjson("{a: -Infinity}")));
    ASSERT_FALSE(QueryRequest::isValidSortOrder(fromjson("{a: true}")));
    ASSERT_FALSE(QueryRequest::isValidSortOrder(fromjson("{a: false}")));
    ASSERT_FALSE(QueryRequest::isValidSortOrder(fromjson("{a: null}")));
    ASSERT_FALSE(QueryRequest::isValidSortOrder(fromjson("{a: {}}")));
    ASSERT_FALSE(QueryRequest::isValidSortOrder(fromjson("{a: {b: 1}}")));
    ASSERT_FALSE(QueryRequest::isValidSortOrder(fromjson("{a: []}")));
    ASSERT_FALSE(QueryRequest::isValidSortOrder(fromjson("{a: [1, 2, 3]}")));
    ASSERT_FALSE(QueryRequest::isValidSortOrder(fromjson("{a: \"\"}")));
    ASSERT_FALSE(QueryRequest::isValidSortOrder(fromjson("{a: \"bb\"}")));
    ASSERT_FALSE(QueryRequest::isValidSortOrder(fromjson("{a: {$meta: 1}}")));
    ASSERT_FALSE(QueryRequest::isValidSortOrder(fromjson("{a: {$meta: \"image\"}}")));
    ASSERT_FALSE(QueryRequest::isValidSortOrder(fromjson("{a: {$world: \"textScore\"}}")));
    ASSERT_FALSE(
        QueryRequest::isValidSortOrder(fromjson("{a: {$meta: \"textScore\","
                                                " b: 1}}")));
    ASSERT_FALSE(QueryRequest::isValidSortOrder(fromjson("{'': 1}")));
    ASSERT_FALSE(QueryRequest::isValidSortOrder(fromjson("{'': -1}")));
}

//
// Tests for parsing a query request from a command BSON object.
//

TEST(QueryRequestTest, ParseFromCommandBasic) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter: {a: 3},"
        "sort: {a: 1},"
        "projection: {_id: 0, a: 1}}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_OK(result.getStatus());
}

TEST(QueryRequestTest, ParseFromCommandWithOptions) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter: {a: 3},"
        "sort: {a: 1},"
        "projection: {_id: 0, a: 1},"
        "showRecordId: true,"
        "maxScan: 1000}}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    unique_ptr<QueryRequest> qr(
        assertGet(QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain)));

    // Make sure the values from the command BSON are reflected in the QR.
    ASSERT(qr->showRecordId());
    ASSERT_EQUALS(1000, qr->getMaxScan());
}

TEST(QueryRequestTest, ParseFromCommandHintAsString) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "hint: 'foo_1'}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    unique_ptr<QueryRequest> qr(
        assertGet(QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain)));

    BSONObj hintObj = qr->getHint();
    ASSERT_EQUALS(BSON("$hint"
                       << "foo_1"),
                  hintObj);
}

TEST(QueryRequestTest, ParseFromCommandValidSortProj) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "projection: {a: 1},"
        "sort: {a: 1}}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    ASSERT_OK(QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain).getStatus());
}

TEST(QueryRequestTest, ParseFromCommandValidSortProjMeta) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "projection: {a: {$meta: 'textScore'}},"
        "sort: {a: {$meta: 'textScore'}}}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    ASSERT_OK(QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain).getStatus());
}

TEST(QueryRequestTest, ParseFromCommandAllFlagsTrue) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "tailable: true,"
        "oplogReplay: true,"
        "noCursorTimeout: true,"
        "awaitData: true,"
        "allowPartialResults: true}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    unique_ptr<QueryRequest> qr(
        assertGet(QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain)));

    // Test that all the flags got set to true.
    ASSERT(qr->isTailable());
    ASSERT(!qr->isSlaveOk());
    ASSERT(qr->isOplogReplay());
    ASSERT(qr->isNoCursorTimeout());
    ASSERT(qr->isAwaitData());
    ASSERT(qr->isAllowPartialResults());
}

TEST(QueryRequestTest, ParseFromCommandCommentWithValidMinMax) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "comment: 'the comment',"
        "min: {a: 1},"
        "max: {a: 2}}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    unique_ptr<QueryRequest> qr(
        assertGet(QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain)));

    ASSERT_EQUALS("the comment", qr->getComment());
    BSONObj expectedMin = BSON("a" << 1);
    ASSERT_EQUALS(0, expectedMin.woCompare(qr->getMin()));
    BSONObj expectedMax = BSON("a" << 2);
    ASSERT_EQUALS(0, expectedMax.woCompare(qr->getMax()));
}

TEST(QueryRequestTest, ParseFromCommandAllNonOptionFields) {
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
    unique_ptr<QueryRequest> qr(
        assertGet(QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain)));

    // Check the values inside the QR.
    BSONObj expectedQuery = BSON("a" << 1);
    ASSERT_EQUALS(0, expectedQuery.woCompare(qr->getFilter()));
    BSONObj expectedSort = BSON("b" << 1);
    ASSERT_EQUALS(0, expectedSort.woCompare(qr->getSort()));
    BSONObj expectedProj = BSON("c" << 1);
    ASSERT_EQUALS(0, expectedProj.woCompare(qr->getProj()));
    BSONObj expectedHint = BSON("d" << 1);
    ASSERT_EQUALS(0, expectedHint.woCompare(qr->getHint()));
    BSONObj expectedReadConcern = BSON("e" << 1);
    ASSERT_EQUALS(0, expectedReadConcern.woCompare(qr->getReadConcern()));
    BSONObj expectedCollation = BSON("f" << 1);
    ASSERT_EQUALS(0, expectedCollation.woCompare(qr->getCollation()));
    ASSERT_EQUALS(3, *qr->getLimit());
    ASSERT_EQUALS(5, *qr->getSkip());
    ASSERT_EQUALS(90, *qr->getBatchSize());
    ASSERT(qr->wantMore());
}

TEST(QueryRequestTest, ParseFromCommandLargeLimit) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter: {a: 1},"
        "limit: 8000000000}");  // 8 * 1000 * 1000 * 1000
    const NamespaceString nss("test.testns");
    const bool isExplain = false;
    unique_ptr<QueryRequest> qr(
        assertGet(QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain)));

    ASSERT_EQUALS(8LL * 1000 * 1000 * 1000, *qr->getLimit());
}

TEST(QueryRequestTest, ParseFromCommandLargeBatchSize) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter: {a: 1},"
        "batchSize: 8000000000}");  // 8 * 1000 * 1000 * 1000
    const NamespaceString nss("test.testns");
    const bool isExplain = false;
    unique_ptr<QueryRequest> qr(
        assertGet(QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain)));

    ASSERT_EQUALS(8LL * 1000 * 1000 * 1000, *qr->getBatchSize());
}

TEST(QueryRequestTest, ParseFromCommandLargeSkip) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter: {a: 1},"
        "skip: 8000000000}");  // 8 * 1000 * 1000 * 1000
    const NamespaceString nss("test.testns");
    const bool isExplain = false;
    unique_ptr<QueryRequest> qr(
        assertGet(QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain)));

    ASSERT_EQUALS(8LL * 1000 * 1000 * 1000, *qr->getSkip());
}

//
// Parsing errors where a field has the wrong type.
//

TEST(QueryRequestTest, ParseFromCommandQueryWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter: 3}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(QueryRequestTest, ParseFromCommandSortWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "sort: 3}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(QueryRequestTest, ParseFromCommandProjWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "projection: 'foo'}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(QueryRequestTest, ParseFromCommandSkipWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "skip: '5',"
        "projection: {a: 1}}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(QueryRequestTest, ParseFromCommandLimitWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "limit: '5',"
        "projection: {a: 1}}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(QueryRequestTest, ParseFromCommandSingleBatchWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "singleBatch: 'false',"
        "projection: {a: 1}}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(QueryRequestTest, ParseFromCommandCommentWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "comment: 1}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(QueryRequestTest, ParseFromCommandMaxScanWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "maxScan: true,"
        "comment: 'foo'}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(QueryRequestTest, ParseFromCommandMaxTimeMSWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "maxTimeMS: true}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(QueryRequestTest, ParseFromCommandMaxWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "max: 3}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(QueryRequestTest, ParseFromCommandMinWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "min: 3}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(QueryRequestTest, ParseFromCommandReturnKeyWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "returnKey: 3}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}


TEST(QueryRequestTest, ParseFromCommandShowRecordIdWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "showRecordId: 3}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(QueryRequestTest, ParseFromCommandSnapshotWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "snapshot: 3}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(QueryRequestTest, ParseFromCommandTailableWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "tailable: 3}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(QueryRequestTest, ParseFromCommandSlaveOkWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "slaveOk: 3}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(QueryRequestTest, ParseFromCommandOplogReplayWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "oplogReplay: 3}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(QueryRequestTest, ParseFromCommandNoCursorTimeoutWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "noCursorTimeout: 3}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(QueryRequestTest, ParseFromCommandAwaitDataWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "tailable: true,"
        "awaitData: 3}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(QueryRequestTest, ParseFromCommandExhaustWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "exhaust: 3}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(QueryRequestTest, ParseFromCommandPartialWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "allowPartialResults: 3}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(QueryRequestTest, ParseFromCommandReadConcernWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "readConcern: 'foo'}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(QueryRequestTest, ParseFromCommandCollationWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "collation: 'foo'}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}
//
// Parsing errors where a field has the right type but a bad value.
//

TEST(QueryRequestTest, ParseFromCommandNegativeSkipError) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "skip: -3,"
        "filter: {a: 3}}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(QueryRequestTest, ParseFromCommandSkipIsZero) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "skip: 0,"
        "filter: {a: 3}}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    unique_ptr<QueryRequest> qr(
        assertGet(QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain)));
    ASSERT_EQ(BSON("a" << 3), qr->getFilter());
    ASSERT_FALSE(qr->getSkip());
}

TEST(QueryRequestTest, ParseFromCommandNegativeLimitError) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "limit: -3,"
        "filter: {a: 3}}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(QueryRequestTest, ParseFromCommandLimitIsZero) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "limit: 0,"
        "filter: {a: 3}}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    unique_ptr<QueryRequest> qr(
        assertGet(QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain)));
    ASSERT_EQ(BSON("a" << 3), qr->getFilter());
    ASSERT_FALSE(qr->getLimit());
}

TEST(QueryRequestTest, ParseFromCommandNegativeBatchSizeError) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "batchSize: -10,"
        "filter: {a: 3}}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(QueryRequestTest, ParseFromCommandBatchSizeZero) {
    BSONObj cmdObj = fromjson("{find: 'testns', batchSize: 0}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    unique_ptr<QueryRequest> qr(
        assertGet(QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain)));

    ASSERT(qr->getBatchSize());
    ASSERT_EQ(0, *qr->getBatchSize());

    ASSERT(!qr->getLimit());
}

TEST(QueryRequestTest, ParseFromCommandDefaultBatchSize) {
    BSONObj cmdObj = fromjson("{find: 'testns'}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    unique_ptr<QueryRequest> qr(
        assertGet(QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain)));

    ASSERT(!qr->getBatchSize());
    ASSERT(!qr->getLimit());
}

//
// Errors checked in QueryRequest::validate().
//

TEST(QueryRequestTest, ParseFromCommandMinMaxDifferentFieldsError) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "min: {a: 3},"
        "max: {b: 4}}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(QueryRequestTest, ParseFromCommandSnapshotPlusSortError) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "sort: {a: 3},"
        "snapshot: true}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(QueryRequestTest, ParseFromCommandSnapshotPlusHintError) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "snapshot: true,"
        "hint: {a: 1}}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(QueryRequestTest, ParseCommandForbidNonMetaSortOnFieldWithMetaProject) {
    BSONObj cmdObj;

    cmdObj = fromjson(
        "{find: 'testns',"
        "projection: {a: {$meta: 'textScore'}},"
        "sort: {a: 1}}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());

    cmdObj = fromjson(
        "{find: 'testns',"
        "projection: {a: {$meta: 'textScore'}},"
        "sort: {b: 1}}");
    ASSERT_OK(QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain).getStatus());
}

TEST(QueryRequestTest, ParseCommandForbidMetaSortOnFieldWithoutMetaProject) {
    BSONObj cmdObj;

    cmdObj = fromjson(
        "{find: 'testns',"
        "projection: {a: 1},"
        "sort: {a: {$meta: 'textScore'}}}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());

    cmdObj = fromjson(
        "{find: 'testns',"
        "projection: {b: 1},"
        "sort: {a: {$meta: 'textScore'}}}");
    result = QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(QueryRequestTest, ParseCommandForbidExhaust) {
    BSONObj cmdObj = fromjson("{find: 'testns', exhaust: true}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(QueryRequestTest, ParseCommandIsFromFindCommand) {
    BSONObj cmdObj = fromjson("{find: 'testns'}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    unique_ptr<QueryRequest> qr(
        assertGet(QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain)));

    ASSERT_FALSE(qr->getNToReturn());
}

TEST(QueryRequestTest, ParseCommandAwaitDataButNotTailable) {
    const NamespaceString nss("test.testns");
    BSONObj cmdObj = fromjson("{find: 'testns', awaitData: true}");
    bool isExplain = false;
    auto result = QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(QueryRequestTest, ParseCommandFirstFieldNotString) {
    BSONObj cmdObj = fromjson("{find: 1}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(QueryRequestTest, ParseCommandIgnoreShardVersionField) {
    BSONObj cmdObj = fromjson("{find: 'test.testns', shardVersion: 'foo'}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_OK(result.getStatus());
}

TEST(QueryRequestTest, DefaultQueryParametersCorrect) {
    BSONObj cmdObj = fromjson("{find: 'testns'}");

    const NamespaceString nss("test.testns");
    std::unique_ptr<QueryRequest> qr(
        assertGet(QueryRequest::makeFromFindCommand(nss, cmdObj, false)));

    ASSERT_FALSE(qr->getSkip());
    ASSERT_FALSE(qr->getLimit());

    ASSERT_EQUALS(true, qr->wantMore());
    ASSERT_FALSE(qr->getNToReturn());
    ASSERT_EQUALS(false, qr->isExplain());
    ASSERT_EQUALS(0, qr->getMaxScan());
    ASSERT_EQUALS(0, qr->getMaxTimeMS());
    ASSERT_EQUALS(false, qr->returnKey());
    ASSERT_EQUALS(false, qr->showRecordId());
    ASSERT_EQUALS(false, qr->isSnapshot());
    ASSERT_EQUALS(false, qr->hasReadPref());
    ASSERT_EQUALS(false, qr->isTailable());
    ASSERT_EQUALS(false, qr->isSlaveOk());
    ASSERT_EQUALS(false, qr->isOplogReplay());
    ASSERT_EQUALS(false, qr->isNoCursorTimeout());
    ASSERT_EQUALS(false, qr->isAwaitData());
    ASSERT_EQUALS(false, qr->isExhaust());
    ASSERT_EQUALS(false, qr->isAllowPartialResults());
}

//
// Extra fields cause the parse to fail.
//

TEST(QueryRequestTest, ParseFromCommandForbidExtraField) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "snapshot: true,"
        "foo: {a: 1}}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(QueryRequestTest, ParseFromCommandForbidExtraOption) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "snapshot: true,"
        "foo: true}");
    const NamespaceString nss("test.testns");
    bool isExplain = false;
    auto result = QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(QueryRequestTest, ParseMaxTimeMSStringValueFails) {
    BSONObj maxTimeObj = BSON(QueryRequest::cmdOptionMaxTimeMS << "foo");
    ASSERT_NOT_OK(QueryRequest::parseMaxTimeMS(maxTimeObj[QueryRequest::cmdOptionMaxTimeMS]));
}

TEST(QueryRequestTest, ParseMaxTimeMSNonIntegralValueFails) {
    BSONObj maxTimeObj = BSON(QueryRequest::cmdOptionMaxTimeMS << 100.3);
    ASSERT_NOT_OK(QueryRequest::parseMaxTimeMS(maxTimeObj[QueryRequest::cmdOptionMaxTimeMS]));
}

TEST(QueryRequestTest, ParseMaxTimeMSOutOfRangeDoubleFails) {
    BSONObj maxTimeObj = BSON(QueryRequest::cmdOptionMaxTimeMS << 1e200);
    ASSERT_NOT_OK(QueryRequest::parseMaxTimeMS(maxTimeObj[QueryRequest::cmdOptionMaxTimeMS]));
}

TEST(QueryRequestTest, ParseMaxTimeMSNegativeValueFails) {
    BSONObj maxTimeObj = BSON(QueryRequest::cmdOptionMaxTimeMS << -400);
    ASSERT_NOT_OK(QueryRequest::parseMaxTimeMS(maxTimeObj[QueryRequest::cmdOptionMaxTimeMS]));
}

TEST(QueryRequestTest, ParseMaxTimeMSZeroSucceeds) {
    BSONObj maxTimeObj = BSON(QueryRequest::cmdOptionMaxTimeMS << 0);
    auto maxTime = QueryRequest::parseMaxTimeMS(maxTimeObj[QueryRequest::cmdOptionMaxTimeMS]);
    ASSERT_OK(maxTime);
    ASSERT_EQ(maxTime.getValue(), 0);
}

TEST(QueryRequestTest, ParseMaxTimeMSPositiveInRangeSucceeds) {
    BSONObj maxTimeObj = BSON(QueryRequest::cmdOptionMaxTimeMS << 300);
    auto maxTime = QueryRequest::parseMaxTimeMS(maxTimeObj[QueryRequest::cmdOptionMaxTimeMS]);
    ASSERT_OK(maxTime);
    ASSERT_EQ(maxTime.getValue(), 300);
}

}  // namespace mongo
}  // namespace
