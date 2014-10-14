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

/**
 * This file contains tests for mongo/db/query/list_parsed_query.h
 */

#include "mongo/db/query/lite_parsed_query.h"

#include "mongo/db/json.h"
#include "mongo/unittest/unittest.h"

using namespace mongo;

namespace {

    TEST(LiteParsedQueryTest, InitSortOrder) {
        LiteParsedQuery* lpq = NULL;
        Status result = LiteParsedQuery::make("testns", 0, 1, 0, BSONObj(), BSONObj(),
                                              fromjson("{a: 1}"), BSONObj(),
                                              BSONObj(), BSONObj(),
                                              false, // snapshot
                                              false, // explain
                                              &lpq);
        ASSERT_OK(result);
        delete lpq;
    }

    TEST(LiteParsedQueryTest, InitSortOrderString) {
        LiteParsedQuery* lpq = NULL;
        Status result = LiteParsedQuery::make("testns", 0, 1, 0, BSONObj(), BSONObj(),
                                              fromjson("{a: \"\"}"), BSONObj(),
                                              BSONObj(), BSONObj(),
                                              false, // snapshot
                                              false, // explain
                                              &lpq);
        ASSERT_NOT_OK(result);
    }

    TEST(LiteParsedQueryTest, GetFilter) {
        LiteParsedQuery* lpq = NULL;
        Status result = LiteParsedQuery::make("testns", 5, 6, 9, BSON( "x" << 5 ), BSONObj(),
                                              BSONObj(), BSONObj(),
                                              BSONObj(), BSONObj(),
                                              false, // snapshot
                                              false, // explain
                                              &lpq);
        ASSERT_OK(result);
        ASSERT_EQUALS(BSON("x" << 5 ), lpq->getFilter());
        delete lpq;
    }

    TEST(LiteParsedQueryTest, NumToReturn) {
        LiteParsedQuery* lpq = NULL;
        Status result = LiteParsedQuery::make("testns", 5, 6, 9, BSON( "x" << 5 ), BSONObj(),
                                              BSONObj(), BSONObj(),
                                              BSONObj(), BSONObj(),
                                              false, // snapshot
                                              false, // explain
                                              &lpq);
        ASSERT_OK(result);
        ASSERT_EQUALS(6, lpq->getNumToReturn());
        ASSERT(lpq->wantMore());
        delete lpq;

        lpq = NULL;
        result = LiteParsedQuery::make("testns", 5, -6, 9, BSON( "x" << 5 ), BSONObj(),
                                       BSONObj(), BSONObj(),
                                       BSONObj(), BSONObj(),
                                       false, // snapshot
                                       false, // explain
                                       &lpq);
        ASSERT_OK(result);
        ASSERT_EQUALS(6, lpq->getNumToReturn());
        ASSERT(!lpq->wantMore());
        delete lpq;
    }

    TEST(LiteParsedQueryTest, MinFieldsNotPrefixOfMax) {
        LiteParsedQuery* lpq = NULL;
        Status result = LiteParsedQuery::make("testns", 0, 0, 0, BSONObj(), BSONObj(),
                                              BSONObj(), BSONObj(),
                                              fromjson("{a: 1}"), fromjson("{b: 1}"),
                                              false, // snapshot
                                              false, // explain
                                              &lpq);
        ASSERT_NOT_OK(result);
    }

    TEST(LiteParsedQueryTest, MinFieldsMoreThanMax) {
        LiteParsedQuery* lpq = NULL;
        Status result = LiteParsedQuery::make("testns", 0, 0, 0, BSONObj(), BSONObj(),
                                              BSONObj(), BSONObj(),
                                              fromjson("{a: 1, b: 1}"), fromjson("{a: 1}"),
                                              false, // snapshot
                                              false, // explain
                                              &lpq);
        ASSERT_NOT_OK(result);
    }

    TEST(LiteParsedQueryTest, MinFieldsLessThanMax) {
        LiteParsedQuery* lpq = NULL;
        Status result = LiteParsedQuery::make("testns", 0, 0, 0, BSONObj(), BSONObj(),
                                              BSONObj(), BSONObj(),
                                              fromjson("{a: 1}"), fromjson("{a: 1, b: 1}"),
                                              false, // snapshot
                                              false, // explain
                                              &lpq);
        ASSERT_NOT_OK(result);
    }

    // Helper function which returns the Status of creating a LiteParsedQuery object with the given
    // parameters.
    Status makeLiteParsedQuery(const BSONObj& query, const BSONObj& proj, const BSONObj& sort) {
        LiteParsedQuery* lpqRaw;
        Status result = LiteParsedQuery::make("testns", 0, 0, 0, query, proj, sort, BSONObj(),
                                              BSONObj(), BSONObj(),
                                              false, // snapshot
                                              false, // explain
                                              &lpqRaw);
        if (result.isOK()) {
            boost::scoped_ptr<LiteParsedQuery> lpq(lpqRaw);
        }

        return result;
    }

    //
    // Test compatibility of various projection and sort objects.
    //

    TEST(LiteParsedQueryTest, ValidSortProj) {
        Status result = Status::OK();

        result = makeLiteParsedQuery(BSONObj(),
                                     fromjson("{a: 1}"),
                                     fromjson("{a: 1}"));
        ASSERT_OK(result);

        result = makeLiteParsedQuery(BSONObj(),
                                     fromjson("{a: {$meta: \"textScore\"}}"),
                                     fromjson("{a: {$meta: \"textScore\"}}"));
        ASSERT_OK(result);

    }

    TEST(LiteParsedQueryTest, ForbidNonMetaSortOnFieldWithMetaProject) {
        Status result = Status::OK();

        result = makeLiteParsedQuery(BSONObj(),
                                     fromjson("{a: {$meta: \"textScore\"}}"),
                                     fromjson("{a: 1}"));
        ASSERT_NOT_OK(result);

        result = makeLiteParsedQuery(BSONObj(),
                                     fromjson("{a: {$meta: \"textScore\"}}"),
                                     fromjson("{b: 1}"));
        ASSERT_OK(result);
    }

    TEST(LiteParsedQueryTest, ForbidMetaSortOnFieldWithoutMetaProject) {
        Status result = Status::OK();

        result = makeLiteParsedQuery(BSONObj(),
                                     fromjson("{a: 1}"),
                                     fromjson("{a: {$meta: \"textScore\"}}"));
        ASSERT_NOT_OK(result);

        result = makeLiteParsedQuery(BSONObj(),
                                     fromjson("{b: 1}"),
                                     fromjson("{a: {$meta: \"textScore\"}}"));
        ASSERT_NOT_OK(result);
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
                                   "query: {a: 3},"
                                   "sort: {a: 1},"
                                   "projection: {_id: 0, a: 1}}");

        LiteParsedQuery* rawLpq;
        bool isExplain = false;
        Status status = LiteParsedQuery::make("testns", cmdObj, isExplain, &rawLpq);
        ASSERT_OK(status);
        scoped_ptr<LiteParsedQuery> lpq(rawLpq);
    }

    TEST(LiteParsedQueryTest, ParseFromCommandWithOptions) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "query: {a: 3},"
                                   "sort: {a: 1},"
                                   "projection: {_id: 0, a: 1},"
                                   "options: {showDiskLoc: true, maxScan: 1000}}");

        LiteParsedQuery* rawLpq;
        bool isExplain = false;
        Status status = LiteParsedQuery::make("testns", cmdObj, isExplain, &rawLpq);
        ASSERT_OK(status);
        scoped_ptr<LiteParsedQuery> lpq(rawLpq);

        // Make sure the values from the command BSON are reflected in the LPQ.
        ASSERT(lpq->getOptions().showDiskLoc);
        ASSERT_EQUALS(1000, lpq->getMaxScan());
    }

    TEST(LiteParsedQueryTest, ParseFromCommandHintAsString) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "query:  {a: 1},"
                                   "hint: 'foo_1'}");

        LiteParsedQuery* rawLpq;
        bool isExplain = false;
        Status status = LiteParsedQuery::make("testns", cmdObj, isExplain, &rawLpq);
        ASSERT_OK(status);
        scoped_ptr<LiteParsedQuery> lpq(rawLpq);

        ASSERT_EQUALS("foo_1", lpq->getHint().firstElement().str());
    }

    TEST(LiteParsedQueryTest, ParseFromCommandValidSortProj) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "projection: {a: 1},"
                                   "sort: {a: 1}}");
        LiteParsedQuery* rawLpq;
        bool isExplain = false;
        Status status = LiteParsedQuery::make("testns", cmdObj, isExplain, &rawLpq);
        ASSERT_OK(status);
        scoped_ptr<LiteParsedQuery> lpq(rawLpq);
    }

    TEST(LiteParsedQueryTest, ParseFromCommandValidSortProjMeta) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "projection: {a: {$meta: 'textScore'}},"
                                   "sort: {a: {$meta: 'textScore'}}}");
        LiteParsedQuery* rawLpq;
        bool isExplain = false;
        Status status = LiteParsedQuery::make("testns", cmdObj, isExplain, &rawLpq);
        ASSERT_OK(status);
        scoped_ptr<LiteParsedQuery> lpq(rawLpq);
    }

    TEST(LiteParsedQueryTest, ParseFromCommandAllFlagsTrue) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "options: {"
                                       "tailable: true,"
                                       "slaveOk: true,"
                                       "oplogReplay: true,"
                                       "noCursorTimeout: true,"
                                       "awaitData: true,"
                                       "exhaust: true,"
                                       "partial: true"
                                  "}}");

        LiteParsedQuery* rawLpq;
        bool isExplain = false;
        Status status = LiteParsedQuery::make("testns", cmdObj, isExplain, &rawLpq);
        ASSERT_OK(status);
        scoped_ptr<LiteParsedQuery> lpq(rawLpq);

        // Test that all the flags got set to true.
        ASSERT(lpq->getOptions().tailable);
        ASSERT(lpq->getOptions().slaveOk);
        ASSERT(lpq->getOptions().oplogReplay);
        ASSERT(lpq->getOptions().noCursorTimeout);
        ASSERT(lpq->getOptions().awaitData);
        ASSERT(lpq->getOptions().exhaust);
        ASSERT(lpq->getOptions().partial);
    }

    TEST(LiteParsedQueryTest, ParseFromCommandCommentWithValidMinMax) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "options: {"
                                       "comment: 'the comment',"
                                       "min: {a: 1},"
                                       "max: {a: 2}"
                                  "}}");

        LiteParsedQuery* rawLpq;
        bool isExplain = false;
        Status status = LiteParsedQuery::make("testns", cmdObj, isExplain, &rawLpq);
        ASSERT_OK(status);
        scoped_ptr<LiteParsedQuery> lpq(rawLpq);

        ASSERT_EQUALS("the comment", lpq->getOptions().comment);
        BSONObj expectedMin = BSON("a" << 1);
        ASSERT_EQUALS(0, expectedMin.woCompare(lpq->getMin()));
        BSONObj expectedMax = BSON("a" << 2);
        ASSERT_EQUALS(0, expectedMax.woCompare(lpq->getMax()));
    }

    TEST(LiteParsedQueryTest, ParseFromCommandAllNonOptionFields) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "query: {a: 1},"
                                   "sort: {b: 1},"
                                   "projection: {c: 1},"
                                   "hint: {d: 1},"
                                   "limit: 3,"
                                   "skip: 5,"
                                   "batchSize: 90,"
                                   "singleBatch: false}");

        LiteParsedQuery* rawLpq;
        bool isExplain = false;
        Status status = LiteParsedQuery::make("testns", cmdObj, isExplain, &rawLpq);
        ASSERT_OK(status);
        scoped_ptr<LiteParsedQuery> lpq(rawLpq);

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
                                   "query:  3}");

        LiteParsedQuery* rawLpq;
        bool isExplain = false;
        Status status = LiteParsedQuery::make("testns", cmdObj, isExplain, &rawLpq);
        ASSERT_NOT_OK(status);
    }

    TEST(LiteParsedQueryTest, ParseFromCommandSortWrongType) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "query:  {a: 1},"
                                   "sort: 3}");

        LiteParsedQuery* rawLpq;
        bool isExplain = false;
        Status status = LiteParsedQuery::make("testns", cmdObj, isExplain, &rawLpq);
        ASSERT_NOT_OK(status);
    }

    TEST(LiteParsedQueryTest, ParseFromCommandProjWrongType) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "query:  {a: 1},"
                                   "projection: 'foo'}");

        LiteParsedQuery* rawLpq;
        bool isExplain = false;
        Status status = LiteParsedQuery::make("testns", cmdObj, isExplain, &rawLpq);
        ASSERT_NOT_OK(status);
    }

    TEST(LiteParsedQueryTest, ParseFromCommandSkipWrongType) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "query:  {a: 1},"
                                   "skip: '5',"
                                   "projection: {a: 1}}");

        LiteParsedQuery* rawLpq;
        bool isExplain = false;
        Status status = LiteParsedQuery::make("testns", cmdObj, isExplain, &rawLpq);
        ASSERT_NOT_OK(status);
    }

    TEST(LiteParsedQueryTest, ParseFromCommandLimitWrongType) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "query:  {a: 1},"
                                   "limit: '5',"
                                   "projection: {a: 1}}");

        LiteParsedQuery* rawLpq;
        bool isExplain = false;
        Status status = LiteParsedQuery::make("testns", cmdObj, isExplain, &rawLpq);
        ASSERT_NOT_OK(status);
    }

    TEST(LiteParsedQueryTest, ParseFromCommandSingleBatchWrongType) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "query:  {a: 1},"
                                   "singleBatch: 'false',"
                                   "projection: {a: 1}}");

        LiteParsedQuery* rawLpq;
        bool isExplain = false;
        Status status = LiteParsedQuery::make("testns", cmdObj, isExplain, &rawLpq);
        ASSERT_NOT_OK(status);
    }

    TEST(LiteParsedQueryTest, ParseFromCommandOptionsWrongType) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "query:  {a: 1},"
                                   "options: [{snapshot: true}],"
                                   "projection: {a: 1}}");

        LiteParsedQuery* rawLpq;
        bool isExplain = false;
        Status status = LiteParsedQuery::make("testns", cmdObj, isExplain, &rawLpq);
        ASSERT_NOT_OK(status);
    }

    TEST(LiteParsedQueryTest, ParseFromCommandCommentWrongType) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "query:  {a: 1},"
                                   "options: {comment: 1}}");

        LiteParsedQuery* rawLpq;
        bool isExplain = false;
        Status status = LiteParsedQuery::make("testns", cmdObj, isExplain, &rawLpq);
        ASSERT_NOT_OK(status);
    }

    TEST(LiteParsedQueryTest, ParseFromCommandMaxScanWrongType) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "query:  {a: 1},"
                                   "options: {maxScan: true, comment: 'foo'}}");

        LiteParsedQuery* rawLpq;
        bool isExplain = false;
        Status status = LiteParsedQuery::make("testns", cmdObj, isExplain, &rawLpq);
        ASSERT_NOT_OK(status);
    }

    TEST(LiteParsedQueryTest, ParseFromCommandMaxTimeMSWrongType) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "query:  {a: 1},"
                                   "options: {maxTimeMS: true}}");

        LiteParsedQuery* rawLpq;
        bool isExplain = false;
        Status status = LiteParsedQuery::make("testns", cmdObj, isExplain, &rawLpq);
        ASSERT_NOT_OK(status);
    }

    TEST(LiteParsedQueryTest, ParseFromCommandMaxWrongType) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "query:  {a: 1},"
                                   "options: {max: 3}}");

        LiteParsedQuery* rawLpq;
        bool isExplain = false;
        Status status = LiteParsedQuery::make("testns", cmdObj, isExplain, &rawLpq);
        ASSERT_NOT_OK(status);
    }

    TEST(LiteParsedQueryTest, ParseFromCommandMinWrongType) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "query:  {a: 1},"
                                   "options: {min: 3}}");

        LiteParsedQuery* rawLpq;
        bool isExplain = false;
        Status status = LiteParsedQuery::make("testns", cmdObj, isExplain, &rawLpq);
        ASSERT_NOT_OK(status);
    }

    TEST(LiteParsedQueryTest, ParseFromCommandReturnKeyWrongType) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "query:  {a: 1},"
                                   "options: {returnKey: 3}}");

        LiteParsedQuery* rawLpq;
        bool isExplain = false;
        Status status = LiteParsedQuery::make("testns", cmdObj, isExplain, &rawLpq);
        ASSERT_NOT_OK(status);
    }


    TEST(LiteParsedQueryTest, ParseFromCommandShowDiskLocWrongType) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "query:  {a: 1},"
                                   "options: {showDiskLoc: 3}}");

        LiteParsedQuery* rawLpq;
        bool isExplain = false;
        Status status = LiteParsedQuery::make("testns", cmdObj, isExplain, &rawLpq);
        ASSERT_NOT_OK(status);
    }

    TEST(LiteParsedQueryTest, ParseFromCommandSnapshotWrongType) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "query:  {a: 1},"
                                   "options: {snapshot: 3}}");

        LiteParsedQuery* rawLpq;
        bool isExplain = false;
        Status status = LiteParsedQuery::make("testns", cmdObj, isExplain, &rawLpq);
        ASSERT_NOT_OK(status);
    }

    TEST(LiteParsedQueryTest, ParseFromCommandTailableWrongType) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "query:  {a: 1},"
                                   "options: {tailable: 3}}");

        LiteParsedQuery* rawLpq;
        bool isExplain = false;
        Status status = LiteParsedQuery::make("testns", cmdObj, isExplain, &rawLpq);
        ASSERT_NOT_OK(status);
    }

    TEST(LiteParsedQueryTest, ParseFromCommandSlaveOkWrongType) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "query:  {a: 1},"
                                   "options: {slaveOk: 3}}");

        LiteParsedQuery* rawLpq;
        bool isExplain = false;
        Status status = LiteParsedQuery::make("testns", cmdObj, isExplain, &rawLpq);
        ASSERT_NOT_OK(status);
    }

    TEST(LiteParsedQueryTest, ParseFromCommandOplogReplayWrongType) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "query:  {a: 1},"
                                   "options: {oplogReplay: 3}}");

        LiteParsedQuery* rawLpq;
        bool isExplain = false;
        Status status = LiteParsedQuery::make("testns", cmdObj, isExplain, &rawLpq);
        ASSERT_NOT_OK(status);
    }

    TEST(LiteParsedQueryTest, ParseFromCommandNoCursorTimeoutWrongType) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "query:  {a: 1},"
                                   "options: {noCursorTimeout: 3}}");

        LiteParsedQuery* rawLpq;
        bool isExplain = false;
        Status status = LiteParsedQuery::make("testns", cmdObj, isExplain, &rawLpq);
        ASSERT_NOT_OK(status);
    }

    TEST(LiteParsedQueryTest, ParseFromCommandAwaitDataWrongType) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "query:  {a: 1},"
                                   "options: {awaitData: 3}}");

        LiteParsedQuery* rawLpq;
        bool isExplain = false;
        Status status = LiteParsedQuery::make("testns", cmdObj, isExplain, &rawLpq);
        ASSERT_NOT_OK(status);
    }

    TEST(LiteParsedQueryTest, ParseFromCommandExhaustWrongType) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "query:  {a: 1},"
                                   "options: {exhaust: 3}}");

        LiteParsedQuery* rawLpq;
        bool isExplain = false;
        Status status = LiteParsedQuery::make("testns", cmdObj, isExplain, &rawLpq);
        ASSERT_NOT_OK(status);
    }

    TEST(LiteParsedQueryTest, ParseFromCommandPartialWrongType) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "query:  {a: 1},"
                                   "options: {exhaust: 3}}");

        LiteParsedQuery* rawLpq;
        bool isExplain = false;
        Status status = LiteParsedQuery::make("testns", cmdObj, isExplain, &rawLpq);
        ASSERT_NOT_OK(status);
    }

    //
    // Parsing errors where a field has the right type but a bad value.
    //

    TEST(LiteParsedQueryTest, ParseFromCommandNegativeSkipError) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "skip: -3,"
                                   "query: {a: 3}}");

        LiteParsedQuery* rawLpq;
        bool isExplain = false;
        Status status = LiteParsedQuery::make("testns", cmdObj, isExplain, &rawLpq);
        ASSERT_NOT_OK(status);
    }

    TEST(LiteParsedQueryTest, ParseFromCommandNegativeLimitError) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "limit: -3,"
                                   "query: {a: 3}}");

        LiteParsedQuery* rawLpq;
        bool isExplain = false;
        Status status = LiteParsedQuery::make("testns", cmdObj, isExplain, &rawLpq);
        ASSERT_NOT_OK(status);
    }

    TEST(LiteParsedQueryTest, ParseFromCommandNegativeBatchSizeError) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "batchSize: -10,"
                                   "query: {a: 3}}");

        LiteParsedQuery* rawLpq;
        bool isExplain = false;
        Status status = LiteParsedQuery::make("testns", cmdObj, isExplain, &rawLpq);
        ASSERT_NOT_OK(status);
    }

    //
    // Errors checked in LiteParsedQuery::validate().
    //

    TEST(LiteParsedQueryTest, ParseFromCommandMinMaxDifferentFieldsError) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "options: {min: {a: 3}, max: {b: 4}}}");

        LiteParsedQuery* rawLpq;
        bool isExplain = false;
        Status status = LiteParsedQuery::make("testns", cmdObj, isExplain, &rawLpq);
        ASSERT_NOT_OK(status);
    }

    TEST(LiteParsedQueryTest, ParseFromCommandSnapshotPlusSortError) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "sort: {a: 3},"
                                   "options: {snapshot: true}}");

        LiteParsedQuery* rawLpq;
        bool isExplain = false;
        Status status = LiteParsedQuery::make("testns", cmdObj, isExplain, &rawLpq);
        ASSERT_NOT_OK(status);
    }

    TEST(LiteParsedQueryTest, ParseFromCommandSnapshotPlusHintError) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "options: {snapshot: true},"
                                   "hint: {a: 1}}");

        LiteParsedQuery* rawLpq;
        bool isExplain = false;
        Status status = LiteParsedQuery::make("testns", cmdObj, isExplain, &rawLpq);
        ASSERT_NOT_OK(status);
    }

    TEST(LiteParsedQueryTest, ParseCommandForbidNonMetaSortOnFieldWithMetaProject) {
        Status status = Status::OK();
        BSONObj cmdObj;

        cmdObj = fromjson("{find: 'testns',"
                           "projection: {a: {$meta: 'textScore'}},"
                           "sort: {a: 1}}");
        LiteParsedQuery* rawLpq;
        bool isExplain = false;
        status = LiteParsedQuery::make("testns", cmdObj, isExplain, &rawLpq);
        ASSERT_NOT_OK(status);


        cmdObj = fromjson("{find: 'testns',"
                           "projection: {a: {$meta: 'textScore'}},"
                           "sort: {b: 1}}");
        status = LiteParsedQuery::make("testns", cmdObj, isExplain, &rawLpq);
        ASSERT_OK(status);
        scoped_ptr<LiteParsedQuery> lpq(rawLpq);
    }

    TEST(LiteParsedQueryTest, ParseCommandForbidMetaSortOnFieldWithoutMetaProject) {
        Status status = Status::OK();
        BSONObj cmdObj;

        cmdObj = fromjson("{find: 'testns',"
                           "projection: {a: 1},"
                           "sort: {a: {$meta: 'textScore'}}}");
        LiteParsedQuery* rawLpq;
        bool isExplain = false;
        status = LiteParsedQuery::make("testns", cmdObj, isExplain, &rawLpq);
        ASSERT_NOT_OK(status);

        cmdObj = fromjson("{find: 'testns',"
                           "projection: {b: 1},"
                           "sort: {a: {$meta: 'textScore'}}}");
        status = LiteParsedQuery::make("testns", cmdObj, isExplain, &rawLpq);
        ASSERT_NOT_OK(status);
    }

    //
    // Extra fields cause the parse to fail.
    //

    TEST(LiteParsedQueryTest, ParseFromCommandForbidExtraField) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "options: {snapshot: true},"
                                   "foo: {a: 1}}");

        LiteParsedQuery* rawLpq;
        bool isExplain = false;
        Status status = LiteParsedQuery::make("testns", cmdObj, isExplain, &rawLpq);
        ASSERT_NOT_OK(status);
    }

    TEST(LiteParsedQueryTest, ParseFromCommandForbidExtraOption) {
        BSONObj cmdObj = fromjson("{find: 'testns',"
                                   "options: {snapshot: true, foo: true}}");

        LiteParsedQuery* rawLpq;
        bool isExplain = false;
        Status status = LiteParsedQuery::make("testns", cmdObj, isExplain, &rawLpq);
        ASSERT_NOT_OK(status);
    }

}  // namespace
