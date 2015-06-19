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

    TEST(LiteParsedQueryTest, InitSortOrder) {
        ASSERT_OK(LiteParsedQuery::makeAsOpQuery("testns",
                                                 0,
                                                 1,
                                                 0,
                                                 BSONObj(),
                                                 BSONObj(),
                                                 fromjson("{a: 1}"),
                                                 BSONObj(),
                                                 BSONObj(),
                                                 BSONObj(),
                                                 false,  // snapshot
                                                 false)  // explain
                    .getStatus());
    }

    TEST(LiteParsedQueryTest, InitSortOrderString) {
        ASSERT_NOT_OK(LiteParsedQuery::makeAsOpQuery("testns",
                                                     0,
                                                     1,
                                                     0,
                                                     BSONObj(),
                                                     BSONObj(),
                                                     fromjson("{a: \"\"}"),
                                                     BSONObj(),
                                                     BSONObj(),
                                                     BSONObj(),
                                                     false,  // snapshot
                                                     false)  // explain
                        .getStatus());
    }

    TEST(LiteParsedQueryTest, GetFilter) {
        unique_ptr<LiteParsedQuery> lpq(
            assertGet(LiteParsedQuery::makeAsOpQuery("testns",
                                                     5,
                                                     6,
                                                     9,
                                                     BSON("x" << 5),
                                                     BSONObj(),
                                                     BSONObj(),
                                                     BSONObj(),
                                                     BSONObj(),
                                                     BSONObj(),
                                                     false, // snapshot
                                                     false))); // explain

        ASSERT_EQUALS(BSON("x" << 5 ), lpq->getFilter());
    }

    TEST(LiteParsedQueryTest, NumToReturn) {
        unique_ptr<LiteParsedQuery> lpq(
            assertGet(LiteParsedQuery::makeAsOpQuery("testns",
                                                     5,
                                                     6,
                                                     9,
                                                     BSON("x" << 5),
                                                     BSONObj(),
                                                     BSONObj(),
                                                     BSONObj(),
                                                     BSONObj(),
                                                     BSONObj(),
                                                     false,      // snapshot
                                                     false)));   // explain

        ASSERT_EQUALS(6, lpq->getBatchSize());
        ASSERT(lpq->wantMore());
    }

    TEST(LiteParsedQueryTest, NumToReturnNegative) {
        unique_ptr<LiteParsedQuery> lpq(
            assertGet(LiteParsedQuery::makeAsOpQuery("testns",
                                                     5,
                                                     -6,
                                                     9,
                                                     BSON("x" << 5),
                                                     BSONObj(),
                                                     BSONObj(),
                                                     BSONObj(),
                                                     BSONObj(),
                                                     BSONObj(),
                                                     false,      // snapshot
                                                     false)));   // explain

        ASSERT_EQUALS(6, lpq->getBatchSize());
        ASSERT(!lpq->wantMore());
    }

    TEST(LiteParsedQueryTest, MinFieldsNotPrefixOfMax) {
        ASSERT_NOT_OK(LiteParsedQuery::makeAsOpQuery("testns",
                                                     0,
                                                     0,
                                                     0,
                                                     BSONObj(),
                                                     BSONObj(),
                                                     BSONObj(),
                                                     BSONObj(),
                                                     fromjson("{a: 1}"),
                                                     fromjson("{b: 1}"),
                                                     false,  // snapshot
                                                     false)  // explain
                        .getStatus());
    }

    TEST(LiteParsedQueryTest, MinFieldsMoreThanMax) {
        ASSERT_NOT_OK(LiteParsedQuery::makeAsOpQuery("testns",
                                                     0,
                                                     0,
                                                     0,
                                                     BSONObj(),
                                                     BSONObj(),
                                                     BSONObj(),
                                                     BSONObj(),
                                                     fromjson("{a: 1, b: 1}"),
                                                     fromjson("{a: 1}"),
                                                     false,  // snapshot
                                                     false)  // explain
                        .getStatus());
    }

    TEST(LiteParsedQueryTest, MinFieldsLessThanMax) {
        ASSERT_NOT_OK(LiteParsedQuery::makeAsOpQuery("testns",
                                                     0,
                                                     0,
                                                     0,
                                                     BSONObj(),
                                                     BSONObj(),
                                                     BSONObj(),
                                                     BSONObj(),
                                                     fromjson("{a: 1}"),
                                                     fromjson("{a: 1, b: 1}"),
                                                     false,  // snapshot
                                                     false)  // explain
                        .getStatus());
    }

    // Helper function which returns the Status of creating a LiteParsedQuery object with the given
    // parameters.
    void assertLiteParsedQuerySuccess(const BSONObj& query,
                                      const BSONObj& proj,
                                      const BSONObj& sort) {

        unique_ptr<LiteParsedQuery> lpq(
            assertGet(LiteParsedQuery::makeAsOpQuery("testns",
                                                     0,
                                                     0,
                                                     0,
                                                     query,
                                                     proj,
                                                     sort,
                                                     BSONObj(),
                                                     BSONObj(),
                                                     BSONObj(),
                                                     false,      // snapshot
                                                     false)));   // explain
    }

    //
    // Test compatibility of various projection and sort objects.
    //

    TEST(LiteParsedQueryTest, ValidSortProj) {
        assertLiteParsedQuerySuccess(BSONObj(),
                                     fromjson("{a: 1}"),
                                     fromjson("{a: 1}"));

        assertLiteParsedQuerySuccess(BSONObj(),
                                     fromjson("{a: {$meta: \"textScore\"}}"),
                                     fromjson("{a: {$meta: \"textScore\"}}"));
    }

    TEST(LiteParsedQueryTest, ForbidNonMetaSortOnFieldWithMetaProject) {
        ASSERT_NOT_OK(LiteParsedQuery::makeAsOpQuery("testns",
                                                     0,
                                                     0,
                                                     0,
                                                     BSONObj(),
                                                     fromjson("{a: {$meta: \"textScore\"}}"),
                                                     fromjson("{a: 1}"),
                                                     BSONObj(),
                                                     BSONObj(),
                                                     BSONObj(),
                                                     false,  // snapshot
                                                     false)  // explain
                        .getStatus());

        assertLiteParsedQuerySuccess(BSONObj(),
                                     fromjson("{a: {$meta: \"textScore\"}}"),
                                     fromjson("{b: 1}"));
    }

    TEST(LiteParsedQueryTest, ForbidMetaSortOnFieldWithoutMetaProject) {
        ASSERT_NOT_OK(LiteParsedQuery::makeAsOpQuery("testns",
                                                     0,
                                                     0,
                                                     0,
                                                     BSONObj(),
                                                     fromjson("{a: 1}"),
                                                     fromjson("{a: {$meta: \"textScore\"}}"),
                                                     BSONObj(),
                                                     BSONObj(),
                                                     BSONObj(),
                                                     false,  // snapshot
                                                     false)  // explain
                        .getStatus());

        ASSERT_NOT_OK(LiteParsedQuery::makeAsOpQuery("testns",
                                                     0,
                                                     0,
                                                     0,
                                                     BSONObj(),
                                                     fromjson("{b: 1}"),
                                                     fromjson("{a: {$meta: \"textScore\"}}"),
                                                     BSONObj(),
                                                     BSONObj(),
                                                     BSONObj(),
                                                     false,  // snapshot
                                                     false)  // explain
                        .getStatus());
    }

    TEST(LiteParsedQueryTest, MakeFindCmd) {
        auto result = LiteParsedQuery::makeAsFindCmd(NamespaceString("test.ns"),
                                                     BSON("x" << 1),
                                                     2);
        ASSERT_OK(result.getStatus());

        auto&& lpq = result.getValue();
        ASSERT_EQUALS("test.ns", lpq->ns());
        ASSERT_EQUALS(BSON("x" << 1), lpq->getFilter());
        ASSERT_EQUALS(2, lpq->getLimit());

        ASSERT_EQUALS(BSONObj(), lpq->getProj());
        ASSERT_EQUALS(BSONObj(), lpq->getSort());
        ASSERT_EQUALS(BSONObj(), lpq->getHint());
        ASSERT_EQUALS(BSONObj(), lpq->getMin());
        ASSERT_EQUALS(BSONObj(), lpq->getMax());

        ASSERT_EQUALS(0, lpq->getSkip());
        ASSERT_EQUALS(0, lpq->getMaxScan());
        ASSERT_EQUALS(0, lpq->getMaxTimeMS());
        ASSERT_EQUALS(0, lpq->getOptions());

        ASSERT_FALSE(lpq->getBatchSize());

        ASSERT_TRUE(lpq->isFromFindCommand());
        ASSERT_FALSE(lpq->isExplain());
        ASSERT_FALSE(lpq->returnKey());
        ASSERT_FALSE(lpq->showRecordId());
        ASSERT_FALSE(lpq->isSnapshot());
        ASSERT_FALSE(lpq->hasReadPref());
        ASSERT_FALSE(lpq->isTailable());
        ASSERT_FALSE(lpq->isSlaveOk());
        ASSERT_FALSE(lpq->isOplogReplay());
        ASSERT_FALSE(lpq->isNoCursorTimeout());
        ASSERT_FALSE(lpq->isAwaitData());
        ASSERT_FALSE(lpq->isExhaust());
        ASSERT_FALSE(lpq->isPartial());
    }

    TEST(LiteParsedQueryTest, MakeFindCmdNoLimit) {
        auto result = LiteParsedQuery::makeAsFindCmd(NamespaceString("test.ns"),
                                                     BSON("x" << 1),
                                                     boost::none);
        ASSERT_OK(result.getStatus());

        auto&& lpq = result.getValue();
        ASSERT_EQUALS("test.ns", lpq->ns());
        ASSERT_EQUALS(BSON("x" << 1), lpq->getFilter());

        ASSERT_EQUALS(BSONObj(), lpq->getProj());
        ASSERT_EQUALS(BSONObj(), lpq->getSort());
        ASSERT_EQUALS(BSONObj(), lpq->getHint());
        ASSERT_EQUALS(BSONObj(), lpq->getMin());
        ASSERT_EQUALS(BSONObj(), lpq->getMax());

        ASSERT_EQUALS(0, lpq->getSkip());
        ASSERT_EQUALS(0, lpq->getMaxScan());
        ASSERT_EQUALS(0, lpq->getMaxTimeMS());
        ASSERT_EQUALS(0, lpq->getOptions());

        ASSERT_FALSE(lpq->getBatchSize());
        ASSERT_FALSE(lpq->getLimit());

        ASSERT_TRUE(lpq->isFromFindCommand());
        ASSERT_FALSE(lpq->isExplain());
        ASSERT_FALSE(lpq->returnKey());
        ASSERT_FALSE(lpq->showRecordId());
        ASSERT_FALSE(lpq->isSnapshot());
        ASSERT_FALSE(lpq->hasReadPref());
        ASSERT_FALSE(lpq->isTailable());
        ASSERT_FALSE(lpq->isSlaveOk());
        ASSERT_FALSE(lpq->isOplogReplay());
        ASSERT_FALSE(lpq->isNoCursorTimeout());
        ASSERT_FALSE(lpq->isAwaitData());
        ASSERT_FALSE(lpq->isExhaust());
        ASSERT_FALSE(lpq->isPartial());
    }

    TEST(LiteParsedQueryTest, MakeFindCmdBadLimit) {
        auto status = LiteParsedQuery::makeAsFindCmd(NamespaceString("test.ns"),
                                                     BSON("x" << 1),
                                                     0).getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::BadValue, status.code());
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
        ASSERT_FALSE(LiteParsedQuery::isValidSortOrder(fromjson("{a: {$meta: \"textScore\","
                                                                " b: 1}}")));
        ASSERT_FALSE(LiteParsedQuery::isValidSortOrder(fromjson("{'': 1}")));
        ASSERT_FALSE(LiteParsedQuery::isValidSortOrder(fromjson("{'': -1}")));
    }

    //
    // Tests for parsing a lite parsed query from a command BSON object.
    //

    TEST(LiteParsedQueryTest, ParseFromCommandBasic) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "filter: {a: 3},"
                                   "sort: {a: 1},"
                                   "projection: {_id: 0, a: 1}}");
        const NamespaceString nss("test.testns");
        bool isExplain = false;
        auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
        ASSERT_OK(result.getStatus());
    }

    TEST(LiteParsedQueryTest, ParseFromCommandWithOptions) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
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
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "filter:  {a: 1},"
                                   "hint: 'foo_1'}");
        const NamespaceString nss("test.testns");
        bool isExplain = false;
        unique_ptr<LiteParsedQuery> lpq(
            assertGet(LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain)));

        BSONObj hintObj = lpq->getHint();
        ASSERT_EQUALS(BSON("$hint" << "foo_1"), hintObj);
    }

    TEST(LiteParsedQueryTest, ParseFromCommandValidSortProj) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "projection: {a: 1},"
                                   "sort: {a: 1}}");
        const NamespaceString nss("test.testns");
        bool isExplain = false;
        ASSERT_OK(LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain).getStatus());
    }

    TEST(LiteParsedQueryTest, ParseFromCommandValidSortProjMeta) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "projection: {a: {$meta: 'textScore'}},"
                                   "sort: {a: {$meta: 'textScore'}}}");
        const NamespaceString nss("test.testns");
        bool isExplain = false;
        ASSERT_OK(LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain).getStatus());
    }

    TEST(LiteParsedQueryTest, ParseFromCommandAllFlagsTrue) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "tailable: true,"
                                   "slaveOk: true,"
                                   "oplogReplay: true,"
                                   "noCursorTimeout: true,"
                                   "awaitData: true,"
                                   "partial: true}");
        const NamespaceString nss("test.testns");
        bool isExplain = false;
        unique_ptr<LiteParsedQuery> lpq(
            assertGet(LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain)));

        // Test that all the flags got set to true.
        ASSERT(lpq->isTailable());
        ASSERT(lpq->isSlaveOk());
        ASSERT(lpq->isOplogReplay());
        ASSERT(lpq->isNoCursorTimeout());
        ASSERT(lpq->isAwaitData());
        ASSERT(lpq->isPartial());
    }

    TEST(LiteParsedQueryTest, ParseFromCommandCommentWithValidMinMax) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
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
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "filter: {a: 1},"
                                   "sort: {b: 1},"
                                   "projection: {c: 1},"
                                   "hint: {d: 1},"
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
        ASSERT_EQUALS(3, lpq->getLimit());
        ASSERT_EQUALS(5, lpq->getSkip());
        ASSERT_EQUALS(90, lpq->getBatchSize());
        ASSERT(lpq->wantMore());
    }

    //
    // Parsing errors where a field has the wrong type.
    //

    TEST(LiteParsedQueryTest, ParseFromCommandQueryWrongType) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "filter: 3}");
        const NamespaceString nss("test.testns");
        bool isExplain = false;
        auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
        ASSERT_NOT_OK(result.getStatus());
    }

    TEST(LiteParsedQueryTest, ParseFromCommandSortWrongType) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "filter:  {a: 1},"
                                   "sort: 3}");
        const NamespaceString nss("test.testns");
        bool isExplain = false;
        auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
        ASSERT_NOT_OK(result.getStatus());
    }

    TEST(LiteParsedQueryTest, ParseFromCommandProjWrongType) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "filter:  {a: 1},"
                                   "projection: 'foo'}");
        const NamespaceString nss("test.testns");
        bool isExplain = false;
        auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
        ASSERT_NOT_OK(result.getStatus());
    }

    TEST(LiteParsedQueryTest, ParseFromCommandSkipWrongType) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "filter:  {a: 1},"
                                   "skip: '5',"
                                   "projection: {a: 1}}");
        const NamespaceString nss("test.testns");
        bool isExplain = false;
        auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
        ASSERT_NOT_OK(result.getStatus());
    }

    TEST(LiteParsedQueryTest, ParseFromCommandLimitWrongType) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "filter:  {a: 1},"
                                   "limit: '5',"
                                   "projection: {a: 1}}");
        const NamespaceString nss("test.testns");
        bool isExplain = false;
        auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
        ASSERT_NOT_OK(result.getStatus());
    }

    TEST(LiteParsedQueryTest, ParseFromCommandSingleBatchWrongType) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "filter:  {a: 1},"
                                   "singleBatch: 'false',"
                                   "projection: {a: 1}}");
        const NamespaceString nss("test.testns");
        bool isExplain = false;
        auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
        ASSERT_NOT_OK(result.getStatus());
    }

    TEST(LiteParsedQueryTest, ParseFromCommandCommentWrongType) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "filter:  {a: 1},"
                                   "comment: 1}");
        const NamespaceString nss("test.testns");
        bool isExplain = false;
        auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
        ASSERT_NOT_OK(result.getStatus());
    }

    TEST(LiteParsedQueryTest, ParseFromCommandMaxScanWrongType) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "filter:  {a: 1},"
                                   "maxScan: true,"
                                   "comment: 'foo'}");
        const NamespaceString nss("test.testns");
        bool isExplain = false;
        auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
        ASSERT_NOT_OK(result.getStatus());
    }

    TEST(LiteParsedQueryTest, ParseFromCommandMaxTimeMSWrongType) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "filter:  {a: 1},"
                                   "maxTimeMS: true}");
        const NamespaceString nss("test.testns");
        bool isExplain = false;
        auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
        ASSERT_NOT_OK(result.getStatus());
    }

    TEST(LiteParsedQueryTest, ParseFromCommandMaxWrongType) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "filter:  {a: 1},"
                                   "max: 3}");
        const NamespaceString nss("test.testns");
        bool isExplain = false;
        auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
        ASSERT_NOT_OK(result.getStatus());
    }

    TEST(LiteParsedQueryTest, ParseFromCommandMinWrongType) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "filter:  {a: 1},"
                                   "min: 3}");
        const NamespaceString nss("test.testns");
        bool isExplain = false;
        auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
        ASSERT_NOT_OK(result.getStatus());
    }

    TEST(LiteParsedQueryTest, ParseFromCommandReturnKeyWrongType) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "filter:  {a: 1},"
                                   "returnKey: 3}");
        const NamespaceString nss("test.testns");
        bool isExplain = false;
        auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
        ASSERT_NOT_OK(result.getStatus());
    }


    TEST(LiteParsedQueryTest, ParseFromCommandShowRecordIdWrongType) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "filter:  {a: 1},"
                                   "showRecordId: 3}");
        const NamespaceString nss("test.testns");
        bool isExplain = false;
        auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
        ASSERT_NOT_OK(result.getStatus());
    }

    TEST(LiteParsedQueryTest, ParseFromCommandSnapshotWrongType) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "filter:  {a: 1},"
                                   "snapshot: 3}");
        const NamespaceString nss("test.testns");
        bool isExplain = false;
        auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
        ASSERT_NOT_OK(result.getStatus());
    }

    TEST(LiteParsedQueryTest, ParseFromCommandTailableWrongType) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "filter:  {a: 1},"
                                   "tailable: 3}");
        const NamespaceString nss("test.testns");
        bool isExplain = false;
        auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
        ASSERT_NOT_OK(result.getStatus());
    }

    TEST(LiteParsedQueryTest, ParseFromCommandSlaveOkWrongType) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "filter:  {a: 1},"
                                   "slaveOk: 3}");
        const NamespaceString nss("test.testns");
        bool isExplain = false;
        auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
        ASSERT_NOT_OK(result.getStatus());
    }

    TEST(LiteParsedQueryTest, ParseFromCommandOplogReplayWrongType) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "filter:  {a: 1},"
                                   "oplogReplay: 3}");
        const NamespaceString nss("test.testns");
        bool isExplain = false;
        auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
        ASSERT_NOT_OK(result.getStatus());
    }

    TEST(LiteParsedQueryTest, ParseFromCommandNoCursorTimeoutWrongType) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "filter:  {a: 1},"
                                   "noCursorTimeout: 3}");
        const NamespaceString nss("test.testns");
        bool isExplain = false;
        auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
        ASSERT_NOT_OK(result.getStatus());
    }

    TEST(LiteParsedQueryTest, ParseFromCommandAwaitDataWrongType) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "filter:  {a: 1},"
                                   "tailable: true,"
                                   "awaitData: 3}");
        const NamespaceString nss("test.testns");
        bool isExplain = false;
        auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
        ASSERT_NOT_OK(result.getStatus());
    }

    TEST(LiteParsedQueryTest, ParseFromCommandExhaustWrongType) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "filter:  {a: 1},"
                                   "exhaust: 3}");
        const NamespaceString nss("test.testns");
        bool isExplain = false;
        auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
        ASSERT_NOT_OK(result.getStatus());
    }

    TEST(LiteParsedQueryTest, ParseFromCommandPartialWrongType) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "filter:  {a: 1},"
                                   "exhaust: 3}");
        const NamespaceString nss("test.testns");
        bool isExplain = false;
        auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
        ASSERT_NOT_OK(result.getStatus());
    }

    //
    // Parsing errors where a field has the right type but a bad value.
    //

    TEST(LiteParsedQueryTest, ParseFromCommandNegativeSkipError) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "skip: -3,"
                                   "filter: {a: 3}}");
        const NamespaceString nss("test.testns");
        bool isExplain = false;
        auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
        ASSERT_NOT_OK(result.getStatus());
    }

    TEST(LiteParsedQueryTest, ParseFromCommandNegativeLimitError) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "limit: -3,"
                                   "filter: {a: 3}}");
        const NamespaceString nss("test.testns");
        bool isExplain = false;
        auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
        ASSERT_NOT_OK(result.getStatus());
    }

    TEST(LiteParsedQueryTest, ParseFromCommandNegativeBatchSizeError) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
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
        ASSERT_EQ(0, lpq->getBatchSize());

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
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "min: {a: 3},"
                                   "max: {b: 4}}");
        const NamespaceString nss("test.testns");
        bool isExplain = false;
        auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
        ASSERT_NOT_OK(result.getStatus());
    }

    TEST(LiteParsedQueryTest, ParseFromCommandSnapshotPlusSortError) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "sort: {a: 3},"
                                   "snapshot: true}");
        const NamespaceString nss("test.testns");
        bool isExplain = false;
        auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
        ASSERT_NOT_OK(result.getStatus());
    }

    TEST(LiteParsedQueryTest, ParseFromCommandSnapshotPlusHintError) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "snapshot: true,"
                                   "hint: {a: 1}}");
        const NamespaceString nss("test.testns");
        bool isExplain = false;
        auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
        ASSERT_NOT_OK(result.getStatus());
    }

    TEST(LiteParsedQueryTest, ParseCommandForbidNonMetaSortOnFieldWithMetaProject) {
        BSONObj cmdObj;

        cmdObj = fromjson("{find: 'testns',"
                           "projection: {a: {$meta: 'textScore'}},"
                           "sort: {a: 1}}");
        const NamespaceString nss("test.testns");
        bool isExplain = false;
        auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
        ASSERT_NOT_OK(result.getStatus());

        cmdObj = fromjson("{find: 'testns',"
                           "projection: {a: {$meta: 'textScore'}},"
                           "sort: {b: 1}}");
        ASSERT_OK(LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain).getStatus());
    }

    TEST(LiteParsedQueryTest, ParseCommandForbidMetaSortOnFieldWithoutMetaProject) {
        BSONObj cmdObj;

        cmdObj = fromjson("{find: 'testns',"
                           "projection: {a: 1},"
                           "sort: {a: {$meta: 'textScore'}}}");
        const NamespaceString nss("test.testns");
        bool isExplain = false;
        auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
        ASSERT_NOT_OK(result.getStatus());

        cmdObj = fromjson("{find: 'testns',"
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

        ASSERT(lpq->isFromFindCommand());
    }

    TEST(LiteParsedQueryTest, ParseCommandNotFromFindCommand) {
        std::unique_ptr<LiteParsedQuery> lpq(
            assertGet(LiteParsedQuery::makeAsOpQuery("testns",
                                                     5,
                                                     6,
                                                     9,
                                                     BSON( "x" << 5 ),
                                                     BSONObj(),
                                                     BSONObj(),
                                                     BSONObj(),
                                                     BSONObj(),
                                                     BSONObj(),
                                                     false,      // snapshot
                                                     false)));   // explain
        ASSERT(!lpq->isFromFindCommand());
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

    TEST(LiteParsedQueryTest, DefaultQueryParametersCorrect) {
        BSONObj cmdObj = fromjson("{find: 'testns'}");

        const NamespaceString nss("test.testns");
        std::unique_ptr<LiteParsedQuery> lpq(
            assertGet(LiteParsedQuery::makeFromFindCommand(nss, cmdObj, false)));

        ASSERT_EQUALS(0, lpq->getSkip());
        ASSERT_EQUALS(true, lpq->wantMore());
        ASSERT_EQUALS(true, lpq->isFromFindCommand());
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
        ASSERT_EQUALS(false, lpq->isPartial());
    }

    //
    // Extra fields cause the parse to fail.
    //

    TEST(LiteParsedQueryTest, ParseFromCommandForbidExtraField) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "snapshot: true,"
                                   "foo: {a: 1}}");
        const NamespaceString nss("test.testns");
        bool isExplain = false;
        auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
        ASSERT_NOT_OK(result.getStatus());
    }

    TEST(LiteParsedQueryTest, ParseFromCommandForbidExtraOption) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "snapshot: true,"
                                   "foo: true}");
        const NamespaceString nss("test.testns");
        bool isExplain = false;
        auto result = LiteParsedQuery::makeFromFindCommand(nss, cmdObj, isExplain);
        ASSERT_NOT_OK(result.getStatus());
    }

} // namespace mongo
} // namespace
