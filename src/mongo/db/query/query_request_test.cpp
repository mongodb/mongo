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

#include <algorithm>
#include <boost/optional.hpp>
#include <boost/optional/optional_io.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_mock.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/json.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using std::unique_ptr;
using unittest::assertGet;

static const NamespaceString testns("testdb.testcoll");

TEST(QueryRequestTest, LimitWithNToReturn) {
    FindCommand findCommand(testns);
    findCommand.setLimit(1);
    findCommand.setNtoreturn(0);
    ASSERT_NOT_OK(query_request_helper::validateFindCommand(findCommand));
}

TEST(QueryRequestTest, BatchSizeWithNToReturn) {
    FindCommand findCommand(testns);
    findCommand.setBatchSize(0);
    findCommand.setNtoreturn(0);
    ASSERT_NOT_OK(query_request_helper::validateFindCommand(findCommand));
}

TEST(QueryRequestTest, NegativeSkip) {
    FindCommand findCommand(testns);
    ASSERT_THROWS_CODE(findCommand.setSkip(-1), DBException, 51024);
}

TEST(QueryRequestTest, ZeroSkip) {
    FindCommand findCommand(testns);
    findCommand.setSkip(0);
    ASSERT_OK(query_request_helper::validateFindCommand(findCommand));
}

TEST(QueryRequestTest, PositiveSkip) {
    FindCommand findCommand(testns);
    findCommand.setSkip(1);
    ASSERT_OK(query_request_helper::validateFindCommand(findCommand));
}

TEST(QueryRequestTest, NegativeLimit) {
    FindCommand findCommand(testns);
    ASSERT_THROWS_CODE(findCommand.setLimit(-1), DBException, 51024);
}

TEST(QueryRequestTest, ZeroLimit) {
    FindCommand findCommand(testns);
    findCommand.setLimit(0);
    ASSERT_OK(query_request_helper::validateFindCommand(findCommand));
}

TEST(QueryRequestTest, PositiveLimit) {
    FindCommand findCommand(testns);
    findCommand.setLimit(1);
    ASSERT_OK(query_request_helper::validateFindCommand(findCommand));
}

TEST(QueryRequestTest, NegativeBatchSize) {
    FindCommand findCommand(testns);
    ASSERT_THROWS_CODE(findCommand.setBatchSize(-1), DBException, 51024);
}

TEST(QueryRequestTest, ZeroBatchSize) {
    FindCommand findCommand(testns);
    findCommand.setBatchSize(0);
    ASSERT_OK(query_request_helper::validateFindCommand(findCommand));
}

TEST(QueryRequestTest, PositiveBatchSize) {
    FindCommand findCommand(testns);
    findCommand.setBatchSize(1);
    ASSERT_OK(query_request_helper::validateFindCommand(findCommand));
}

TEST(QueryRequestTest, NegativeNToReturn) {
    FindCommand findCommand(testns);
    ASSERT_THROWS_CODE(findCommand.setNtoreturn(-1), DBException, 51024);
}

TEST(QueryRequestTest, ZeroNToReturn) {
    FindCommand findCommand(testns);
    findCommand.setNtoreturn(0);
    ASSERT_OK(query_request_helper::validateFindCommand(findCommand));
}

TEST(QueryRequestTest, PositiveNToReturn) {
    FindCommand findCommand(testns);
    findCommand.setNtoreturn(1);
    ASSERT_OK(query_request_helper::validateFindCommand(findCommand));
}

TEST(QueryRequestTest, NegativeMaxTimeMS) {
    FindCommand findCommand(testns);
    ASSERT_THROWS_CODE(findCommand.setMaxTimeMS(-1), DBException, 51024);
}

TEST(QueryRequestTest, ZeroMaxTimeMS) {
    FindCommand findCommand(testns);
    findCommand.setMaxTimeMS(0);
    ASSERT_OK(query_request_helper::validateFindCommand(findCommand));
}

TEST(QueryRequestTest, PositiveMaxTimeMS) {
    FindCommand findCommand(testns);
    findCommand.setMaxTimeMS(1);
    ASSERT_OK(query_request_helper::validateFindCommand(findCommand));
}

TEST(QueryRequestTest, ValidSortOrder) {
    FindCommand findCommand(testns);
    findCommand.setSort(fromjson("{a: 1}"));
    ASSERT_OK(query_request_helper::validateFindCommand(findCommand));
}

TEST(QueryRequestTest, DoesNotErrorOnInvalidSortPattern) {
    FindCommand findCommand(testns);
    findCommand.setSort(fromjson("{a: \"\"}"));
    // FindCommand isn't responsible for validating the sort pattern, so it is considered valid
    // even though the sort pattern {a: ""} is not well-formed.
    ASSERT_OK(query_request_helper::validateFindCommand(findCommand));
}

TEST(QueryRequestTest, MinFieldsNotPrefixOfMax) {
    FindCommand findCommand(testns);
    findCommand.setMin(fromjson("{a: 1}"));
    findCommand.setMax(fromjson("{b: 1}"));
    ASSERT_NOT_OK(query_request_helper::validateFindCommand(findCommand));
}

TEST(QueryRequestTest, MinFieldsMoreThanMax) {
    FindCommand findCommand(testns);
    findCommand.setMin(fromjson("{a: 1, b: 1}"));
    findCommand.setMax(fromjson("{a: 1}"));
    ASSERT_NOT_OK(query_request_helper::validateFindCommand(findCommand));
}

TEST(QueryRequestTest, MinFieldsLessThanMax) {
    FindCommand findCommand(testns);
    findCommand.setMin(fromjson("{a: 1}"));
    findCommand.setMax(fromjson("{a: 1, b: 1}"));
    ASSERT_NOT_OK(query_request_helper::validateFindCommand(findCommand));
}

TEST(QueryRequestTest, ForbidTailableWithNonNaturalSort) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "tailable: true,"
        "sort: {a: 1}, '$db': 'test'}");

    ASSERT_THROWS_CODE(query_request_helper::makeFromFindCommandForTests(cmdObj),
                       DBException,
                       ErrorCodes::BadValue);
}

TEST(QueryRequestTest, ForbidTailableWithSingleBatch) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "tailable: true,"
        "singleBatch: true, '$db': 'test'}");

    ASSERT_THROWS_CODE(query_request_helper::makeFromFindCommandForTests(cmdObj),
                       DBException,
                       ErrorCodes::BadValue);
}

TEST(QueryRequestTest, AllowTailableWithNaturalSort) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "tailable: true,"
        "sort: {$natural: 1}, '$db': 'test'}");

    auto findCommand = query_request_helper::makeFromFindCommandForTests(cmdObj);
    ASSERT_TRUE(findCommand->getTailable());
    ASSERT_BSONOBJ_EQ(findCommand->getSort(), BSON("$natural" << 1));
}

//
// Test compatibility of various projection and sort objects.
//

TEST(QueryRequestTest, ValidSortProj) {
    FindCommand findCommand(testns);
    findCommand.setProjection(fromjson("{a: 1}"));
    findCommand.setSort(fromjson("{a: 1}"));
    ASSERT_OK(query_request_helper::validateFindCommand(findCommand));

    FindCommand metaFC(testns);
    metaFC.setProjection(fromjson("{a: {$meta: \"textScore\"}}"));
    metaFC.setSort(fromjson("{a: {$meta: \"textScore\"}}"));
    ASSERT_OK(query_request_helper::validateFindCommand(metaFC));
}

TEST(QueryRequestTest, TextScoreMetaSortOnFieldDoesNotRequireMetaProjection) {
    FindCommand findCommand(testns);
    findCommand.setProjection(fromjson("{b: 1}"));
    findCommand.setSort(fromjson("{a: {$meta: 'textScore'}}"));
    ASSERT_OK(query_request_helper::validateFindCommand(findCommand));
}

TEST(QueryRequestTest, TextScoreMetaProjectionDoesNotRequireTextScoreMetaSort) {
    FindCommand findCommand(testns);
    findCommand.setProjection(fromjson("{a: {$meta: \"textScore\"}}"));
    findCommand.setSort(fromjson("{b: 1}"));
    ASSERT_OK(query_request_helper::validateFindCommand(findCommand));
}

TEST(QueryRequestTest, RequestResumeTokenWithHint) {
    FindCommand findCommand(testns);
    findCommand.setRequestResumeToken(true);
    ASSERT_NOT_OK(query_request_helper::validateFindCommand(findCommand));
    findCommand.setHint(fromjson("{a: 1}"));
    ASSERT_NOT_OK(query_request_helper::validateFindCommand(findCommand));
    findCommand.setHint(fromjson("{$natural: 1}"));
    ASSERT_OK(query_request_helper::validateFindCommand(findCommand));
}

TEST(QueryRequestTest, RequestResumeTokenWithSort) {
    FindCommand findCommand(testns);
    findCommand.setRequestResumeToken(true);
    // Hint must be explicitly set for the query request to validate.
    findCommand.setHint(fromjson("{$natural: 1}"));
    ASSERT_OK(query_request_helper::validateFindCommand(findCommand));
    findCommand.setSort(fromjson("{a: 1}"));
    ASSERT_NOT_OK(query_request_helper::validateFindCommand(findCommand));
    findCommand.setSort(fromjson("{$natural: 1}"));
    ASSERT_OK(query_request_helper::validateFindCommand(findCommand));
}

TEST(QueryRequestTest, InvalidResumeAfterWrongRecordIdType) {
    FindCommand findCommand(testns);
    BSONObj resumeAfter = BSON("$recordId" << 1);
    findCommand.setResumeAfter(resumeAfter);
    findCommand.setRequestResumeToken(true);
    // Hint must be explicitly set for the query request to validate.
    findCommand.setHint(fromjson("{$natural: 1}"));
    ASSERT_NOT_OK(query_request_helper::validateFindCommand(findCommand));
    resumeAfter = BSON("$recordId" << 1LL);
    findCommand.setResumeAfter(resumeAfter);
    ASSERT_OK(query_request_helper::validateFindCommand(findCommand));
}

TEST(QueryRequestTest, InvalidResumeAfterExtraField) {
    FindCommand findCommand(testns);
    BSONObj resumeAfter = BSON("$recordId" << 1LL << "$extra" << 1);
    findCommand.setResumeAfter(resumeAfter);
    findCommand.setRequestResumeToken(true);
    // Hint must be explicitly set for the query request to validate.
    findCommand.setHint(fromjson("{$natural: 1}"));
    ASSERT_NOT_OK(query_request_helper::validateFindCommand(findCommand));
}

TEST(QueryRequestTest, ResumeAfterWithHint) {
    FindCommand findCommand(testns);
    BSONObj resumeAfter = BSON("$recordId" << 1LL);
    findCommand.setResumeAfter(resumeAfter);
    findCommand.setRequestResumeToken(true);
    ASSERT_NOT_OK(query_request_helper::validateFindCommand(findCommand));
    findCommand.setHint(fromjson("{a: 1}"));
    ASSERT_NOT_OK(query_request_helper::validateFindCommand(findCommand));
    findCommand.setHint(fromjson("{$natural: 1}"));
    ASSERT_OK(query_request_helper::validateFindCommand(findCommand));
}

TEST(QueryRequestTest, ResumeAfterWithSort) {
    FindCommand findCommand(testns);
    BSONObj resumeAfter = BSON("$recordId" << 1LL);
    findCommand.setResumeAfter(resumeAfter);
    findCommand.setRequestResumeToken(true);
    // Hint must be explicitly set for the query request to validate.
    findCommand.setHint(fromjson("{$natural: 1}"));
    ASSERT_OK(query_request_helper::validateFindCommand(findCommand));
    findCommand.setSort(fromjson("{a: 1}"));
    ASSERT_NOT_OK(query_request_helper::validateFindCommand(findCommand));
    findCommand.setSort(fromjson("{$natural: 1}"));
    ASSERT_OK(query_request_helper::validateFindCommand(findCommand));
}

TEST(QueryRequestTest, ResumeNoSpecifiedRequestResumeToken) {
    FindCommand findCommand(testns);
    BSONObj resumeAfter = BSON("$recordId" << 1LL);
    findCommand.setResumeAfter(resumeAfter);
    // Hint must be explicitly set for the query request to validate.
    findCommand.setHint(fromjson("{$natural: 1}"));
    ASSERT_NOT_OK(query_request_helper::validateFindCommand(findCommand));
    findCommand.setRequestResumeToken(true);
    ASSERT_OK(query_request_helper::validateFindCommand(findCommand));
}

TEST(QueryRequestTest, ExplicitEmptyResumeAfter) {
    FindCommand findCommand(NamespaceString::kRsOplogNamespace);
    BSONObj resumeAfter = fromjson("{}");
    // Hint must be explicitly set for the query request to validate.
    findCommand.setHint(fromjson("{$natural: 1}"));
    findCommand.setResumeAfter(resumeAfter);
    ASSERT_OK(query_request_helper::validateFindCommand(findCommand));
    findCommand.setRequestResumeToken(true);
    ASSERT_OK(query_request_helper::validateFindCommand(findCommand));
}

//
// Text meta BSON element validation
//

bool isFirstElementTextScoreMeta(const char* sortStr) {
    BSONObj sortObj = fromjson(sortStr);
    BSONElement elt = sortObj.firstElement();
    bool result = query_request_helper::isTextScoreMeta(elt);
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
// Tests for parsing a query request from a command BSON object.
//

TEST(QueryRequestTest, ParseFromCommandBasic) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter: {a: 3},"
        "sort: {a: 1},"
        "projection: {_id: 0, a: 1}, '$db': 'test'}");

    query_request_helper::makeFromFindCommandForTests(cmdObj);
}

TEST(QueryRequestTest, ParseFromCommandWithOptions) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter: {a: 3},"
        "sort: {a: 1},"
        "projection: {_id: 0, a: 1},"
        "showRecordId: true, '$db': 'test'}");

    unique_ptr<FindCommand> findCommand(query_request_helper::makeFromFindCommandForTests(cmdObj));

    // Make sure the values from the command BSON are reflected in the QR.
    ASSERT(findCommand->getShowRecordId());
}

TEST(QueryRequestTest, ParseFromCommandHintAsString) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "hint: 'foo_1', '$db': 'test'}");

    unique_ptr<FindCommand> findCommand(query_request_helper::makeFromFindCommandForTests(cmdObj));

    BSONObj hintObj = findCommand->getHint();
    ASSERT_BSONOBJ_EQ(BSON("$hint"
                           << "foo_1"),
                      hintObj);
}

TEST(QueryRequestTest, ParseFromCommandValidSortProj) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "projection: {a: 1},"
        "sort: {a: 1}, '$db': 'test'}");

    query_request_helper::makeFromFindCommandForTests(cmdObj);
}

TEST(QueryRequestTest, ParseFromCommandValidSortProjMeta) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "projection: {a: {$meta: 'textScore'}},"
        "sort: {a: {$meta: 'textScore'}}, '$db': 'test'}");

    query_request_helper::makeFromFindCommandForTests(cmdObj);
}

TEST(QueryRequestTest, ParseFromCommandAllFlagsTrue) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "tailable: true,"
        "noCursorTimeout: true,"
        "awaitData: true,"
        "allowPartialResults: true,"
        "readOnce: true,"
        "allowSpeculativeMajorityRead: true, '$db': 'test'}");

    unique_ptr<FindCommand> findCommand(query_request_helper::makeFromFindCommandForTests(cmdObj));

    // Test that all the flags got set to true.
    ASSERT(findCommand->getTailable());
    ASSERT(findCommand->getNoCursorTimeout());
    ASSERT(findCommand->getTailable() && findCommand->getAwaitData());
    ASSERT(findCommand->getAllowPartialResults());
    ASSERT(findCommand->getReadOnce());
    ASSERT(findCommand->getAllowSpeculativeMajorityRead());
}

TEST(QueryRequestTest, OplogReplayFlagIsAllowedButIgnored) {
    auto cmdObj = BSON("find"
                       << "testns"
                       << "oplogReplay" << true << "tailable" << true << "$db"
                       << "test");
    const NamespaceString nss{"test.testns"};
    auto findCommand = query_request_helper::makeFromFindCommandForTests(cmdObj);

    // Verify that the 'oplogReplay' flag does not appear if we reserialize the request.
    auto reserialized = findCommand->toBSON(BSONObj());
    ASSERT_BSONOBJ_EQ(reserialized,
                      BSON("find"
                           << "testns"
                           << "tailable" << true));
}

TEST(QueryRequestTest, ParseFromCommandReadOnceDefaultsToFalse) {
    BSONObj cmdObj = fromjson("{find: 'testns', '$db': 'test'}");

    unique_ptr<FindCommand> findCommand(query_request_helper::makeFromFindCommandForTests(cmdObj));
    ASSERT(!findCommand->getReadOnce());
}

TEST(QueryRequestTest, ParseFromCommandValidMinMax) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "comment: 'the comment',"
        "min: {a: 1},"
        "max: {a: 2}, '$db': 'test'}");

    unique_ptr<FindCommand> findCommand(query_request_helper::makeFromFindCommandForTests(cmdObj));
    BSONObj expectedMin = BSON("a" << 1);
    ASSERT_EQUALS(0, expectedMin.woCompare(findCommand->getMin()));
    BSONObj expectedMax = BSON("a" << 2);
    ASSERT_EQUALS(0, expectedMax.woCompare(findCommand->getMax()));
}

TEST(QueryRequestTest, ParseFromCommandAllNonOptionFields) {
    LegacyRuntimeConstants rtc{Date_t::now(), Timestamp(1, 1)};
    BSONObj rtcObj = BSON("runtimeConstants" << rtc.toBSON());
    BSONObj cmdObj = fromjson(
                         "{find: 'testns',"
                         "filter: {a: 1},"
                         "sort: {b: 1},"
                         "projection: {c: 1},"
                         "hint: {d: 1},"
                         "readConcern: {e: 1},"
                         "$queryOptions: {$readPreference: 'secondary'},"
                         "collation: {f: 1},"
                         "limit: 3,"
                         "skip: 5,"
                         "batchSize: 90,"
                         "singleBatch: false, '$db': 'test'}")
                         .addField(rtcObj["runtimeConstants"]);

    unique_ptr<FindCommand> findCommand(query_request_helper::makeFromFindCommandForTests(cmdObj));
    // Check the values inside the QR.
    BSONObj expectedQuery = BSON("a" << 1);
    ASSERT_EQUALS(0, expectedQuery.woCompare(findCommand->getFilter()));
    BSONObj expectedSort = BSON("b" << 1);
    ASSERT_EQUALS(0, expectedSort.woCompare(findCommand->getSort()));
    BSONObj expectedProj = BSON("c" << 1);
    ASSERT_EQUALS(0, expectedProj.woCompare(findCommand->getProjection()));
    BSONObj expectedHint = BSON("d" << 1);
    ASSERT_EQUALS(0, expectedHint.woCompare(findCommand->getHint()));
    BSONObj expectedReadConcern = BSON("e" << 1);
    ASSERT(findCommand->getReadConcern());
    ASSERT_BSONOBJ_EQ(expectedReadConcern, *findCommand->getReadConcern());
    BSONObj expectedUnwrappedReadPref = BSON("$readPreference"
                                             << "secondary");
    ASSERT_EQUALS(0, expectedUnwrappedReadPref.woCompare(findCommand->getUnwrappedReadPref()));
    BSONObj expectedCollation = BSON("f" << 1);
    ASSERT_EQUALS(0, expectedCollation.woCompare(findCommand->getCollation()));
    ASSERT_EQUALS(3, *findCommand->getLimit());
    ASSERT_EQUALS(5, *findCommand->getSkip());
    ASSERT_EQUALS(90, *findCommand->getBatchSize());
    ASSERT(findCommand->getLegacyRuntimeConstants().has_value());
    ASSERT_EQUALS(findCommand->getLegacyRuntimeConstants()->getLocalNow(), rtc.getLocalNow());
    ASSERT_EQUALS(findCommand->getLegacyRuntimeConstants()->getClusterTime(), rtc.getClusterTime());
    ASSERT(!findCommand->getSingleBatch());
}

TEST(QueryRequestTest, ParseFromCommandLargeLimit) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter: {a: 1},"
        "limit: 8000000000, '$db': 'test'}");  // 8 * 1000 * 1000 * 1000

    unique_ptr<FindCommand> findCommand(query_request_helper::makeFromFindCommandForTests(cmdObj));

    ASSERT_EQUALS(8LL * 1000 * 1000 * 1000, *findCommand->getLimit());
}

TEST(QueryRequestTest, ParseFromCommandLargeBatchSize) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter: {a: 1},"
        "batchSize: 8000000000, '$db': 'test'}");  // 8 * 1000 * 1000 * 1000

    unique_ptr<FindCommand> findCommand(query_request_helper::makeFromFindCommandForTests(cmdObj));

    ASSERT_EQUALS(8LL * 1000 * 1000 * 1000, *findCommand->getBatchSize());
}

TEST(QueryRequestTest, ParseFromCommandLargeSkip) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter: {a: 1},"
        "skip: 8000000000, '$db': 'test'}");  // 8 * 1000 * 1000 * 1000

    unique_ptr<FindCommand> findCommand(query_request_helper::makeFromFindCommandForTests(cmdObj));

    ASSERT_EQUALS(8LL * 1000 * 1000 * 1000, *findCommand->getSkip());
}

//
// Parsing errors where a field has the wrong type.
//

TEST(QueryRequestTest, ParseFromCommandQueryWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter: 3, '$db': 'test'}");

    ASSERT_THROWS_CODE(query_request_helper::makeFromFindCommandForTests(cmdObj),
                       DBException,
                       ErrorCodes::TypeMismatch);
}

TEST(QueryRequestTest, ParseFromCommandSortWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "sort: 3, '$db': 'test'}");

    ASSERT_THROWS_CODE(query_request_helper::makeFromFindCommandForTests(cmdObj),
                       DBException,
                       ErrorCodes::TypeMismatch);
}


TEST(QueryRequestTest, ParseFromCommandProjWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "projection: 'foo', '$db': 'test'}");

    ASSERT_THROWS_CODE(query_request_helper::makeFromFindCommandForTests(cmdObj),
                       DBException,
                       ErrorCodes::TypeMismatch);
}


TEST(QueryRequestTest, ParseFromCommandSkipWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "skip: '5',"
        "projection: {a: 1}, '$db': 'test'}");

    ASSERT_THROWS_CODE(query_request_helper::makeFromFindCommandForTests(cmdObj),
                       DBException,
                       ErrorCodes::TypeMismatch);
}


TEST(QueryRequestTest, ParseFromCommandLimitWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "limit: '5',"
        "projection: {a: 1}, '$db': 'test'}");

    ASSERT_THROWS_CODE(query_request_helper::makeFromFindCommandForTests(cmdObj),
                       DBException,
                       ErrorCodes::TypeMismatch);
}


TEST(QueryRequestTest, ParseFromCommandSingleBatchWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "singleBatch: 'false',"
        "projection: {a: 1}, '$db': 'test'}");

    ASSERT_THROWS_CODE(query_request_helper::makeFromFindCommandForTests(cmdObj),
                       DBException,
                       ErrorCodes::TypeMismatch);
}


TEST(QueryRequestTest, ParseFromCommandUnwrappedReadPrefWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "$queryOptions: 1, '$db': 'test'}");

    ASSERT_THROWS_CODE(query_request_helper::makeFromFindCommandForTests(cmdObj),
                       DBException,
                       ErrorCodes::TypeMismatch);
}


TEST(QueryRequestTest, ParseFromCommandMaxTimeMSWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "maxTimeMS: true, '$db': 'test'}");

    ASSERT_THROWS_CODE(query_request_helper::makeFromFindCommandForTests(cmdObj),
                       DBException,
                       ErrorCodes::BadValue);
}


TEST(QueryRequestTest, ParseFromCommandMaxWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "max: 3, '$db': 'test'}");

    ASSERT_THROWS_CODE(query_request_helper::makeFromFindCommandForTests(cmdObj),
                       DBException,
                       ErrorCodes::TypeMismatch);
}


TEST(QueryRequestTest, ParseFromCommandMinWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "min: 3, '$db': 'test'}");

    ASSERT_THROWS_CODE(query_request_helper::makeFromFindCommandForTests(cmdObj),
                       DBException,
                       ErrorCodes::TypeMismatch);
}

TEST(QueryRequestTest, ParseFromCommandReturnKeyWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "returnKey: 3, '$db': 'test'}");

    ASSERT_THROWS_CODE(query_request_helper::makeFromFindCommandForTests(cmdObj),
                       DBException,
                       ErrorCodes::TypeMismatch);
}

TEST(QueryRequestTest, ParseFromCommandShowRecordIdWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "showRecordId: 3, '$db': 'test'}");

    ASSERT_THROWS_CODE(query_request_helper::makeFromFindCommandForTests(cmdObj),
                       DBException,
                       ErrorCodes::TypeMismatch);
}

TEST(QueryRequestTest, ParseFromCommandTailableWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "tailable: 3, '$db': 'test'}");

    ASSERT_THROWS_CODE(query_request_helper::makeFromFindCommandForTests(cmdObj),
                       DBException,
                       ErrorCodes::TypeMismatch);
}

TEST(QueryRequestTest, ParseFromCommandSlaveOkWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "slaveOk: 3, '$db': 'test'}");

    ASSERT_THROWS_CODE(
        query_request_helper::makeFromFindCommandForTests(cmdObj), DBException, 40415);
}

TEST(QueryRequestTest, ParseFromCommandOplogReplayWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "oplogReplay: 3, '$db': 'test'}");

    ASSERT_THROWS_CODE(query_request_helper::makeFromFindCommandForTests(cmdObj),
                       DBException,
                       ErrorCodes::TypeMismatch);
}

TEST(QueryRequestTest, ParseFromCommandNoCursorTimeoutWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "noCursorTimeout: 3, '$db': 'test'}");

    ASSERT_THROWS_CODE(query_request_helper::makeFromFindCommandForTests(cmdObj),
                       DBException,
                       ErrorCodes::TypeMismatch);
}

TEST(QueryRequestTest, ParseFromCommandAwaitDataWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "tailable: true,"
        "awaitData: 3, '$db': 'test'}");

    ASSERT_THROWS_CODE(query_request_helper::makeFromFindCommandForTests(cmdObj),
                       DBException,
                       ErrorCodes::TypeMismatch);
}


TEST(QueryRequestTest, ParseFromCommandExhaustWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "exhaust: 3, '$db': 'test'}");

    ASSERT_THROWS_CODE(
        query_request_helper::makeFromFindCommandForTests(cmdObj), DBException, 40415);
}


TEST(QueryRequestTest, ParseFromCommandPartialWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "allowPartialResults: 3, '$db': 'test'}");

    ASSERT_THROWS_CODE(query_request_helper::makeFromFindCommandForTests(cmdObj),
                       DBException,
                       ErrorCodes::TypeMismatch);
}

TEST(QueryRequestTest, ParseFromCommandReadConcernWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "readConcern: 'foo', '$db': 'test'}");

    ASSERT_THROWS_CODE(query_request_helper::makeFromFindCommandForTests(cmdObj),
                       DBException,
                       ErrorCodes::TypeMismatch);
}

TEST(QueryRequestTest, ParseFromCommandCollationWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "collation: 'foo', '$db': 'test'}");

    ASSERT_THROWS_CODE(query_request_helper::makeFromFindCommandForTests(cmdObj),
                       DBException,
                       ErrorCodes::TypeMismatch);
}

TEST(QueryRequestTest, ParseFromCommandReadOnceWrongType) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "readOnce: 1, '$db': 'test'}");

    ASSERT_THROWS_CODE(query_request_helper::makeFromFindCommandForTests(cmdObj),
                       DBException,
                       ErrorCodes::TypeMismatch);
}

TEST(QueryRequestTest, ParseFromCommandLegacyRuntimeConstantsWrongType) {
    BSONObj cmdObj = BSON("find"
                          << "testns"
                          << "runtimeConstants"
                          << "shouldNotBeString"
                          << "$db"
                          << "test");

    ASSERT_THROWS_CODE(query_request_helper::makeFromFindCommandForTests(cmdObj),
                       DBException,
                       ErrorCodes::TypeMismatch);
}

TEST(QueryRequestTest, ParseFromCommandLegacyRuntimeConstantsSubfieldsWrongType) {
    BSONObj cmdObj = BSON("find"
                          << "testns"
                          << "runtimeConstants"
                          << BSON("localNow"
                                  << "shouldBeDate"
                                  << "clusterTime"
                                  << "shouldBeTimestamp")
                          << "$db"
                          << "test");
    ASSERT_THROWS_CODE(query_request_helper::makeFromFindCommandForTests(cmdObj),
                       AssertionException,
                       ErrorCodes::TypeMismatch);
}

//
// Parsing errors where a field has the right type but a bad value.
//

TEST(QueryRequestTest, ParseFromCommandNegativeSkipError) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "skip: -3,"
        "filter: {a: 3}, '$db': 'test'}");
    ASSERT_THROWS_CODE(
        query_request_helper::makeFromFindCommandForTests(cmdObj), DBException, 51024);
}

TEST(QueryRequestTest, ParseFromCommandSkipIsZero) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "skip: 0,"
        "filter: {a: 3}, '$db': 'test'}");
    unique_ptr<FindCommand> findCommand(query_request_helper::makeFromFindCommandForTests(cmdObj));
    ASSERT_BSONOBJ_EQ(BSON("a" << 3), findCommand->getFilter());
    ASSERT_FALSE(findCommand->getSkip());
}

TEST(QueryRequestTest, ParseFromCommandNegativeLimitError) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "limit: -3,"
        "filter: {a: 3}, '$db': 'test'}");
    ASSERT_THROWS_CODE(
        query_request_helper::makeFromFindCommandForTests(cmdObj), DBException, 51024);
}

TEST(QueryRequestTest, ParseFromCommandLimitIsZero) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "limit: 0,"
        "filter: {a: 3}, '$db': 'test'}");
    unique_ptr<FindCommand> findCommand(query_request_helper::makeFromFindCommandForTests(cmdObj));
    ASSERT_BSONOBJ_EQ(BSON("a" << 3), findCommand->getFilter());
    ASSERT_FALSE(findCommand->getLimit());
}

TEST(QueryRequestTest, ParseFromCommandNegativeBatchSizeError) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "batchSize: -10,"
        "filter: {a: 3}, '$db': 'test'}");
    ASSERT_THROWS_CODE(
        query_request_helper::makeFromFindCommandForTests(cmdObj), DBException, 51024);
}

TEST(QueryRequestTest, ParseFromCommandBatchSizeZero) {
    BSONObj cmdObj = fromjson("{find: 'testns', batchSize: 0, '$db': 'test'}");
    unique_ptr<FindCommand> findCommand(query_request_helper::makeFromFindCommandForTests(cmdObj));

    ASSERT(findCommand->getBatchSize());
    ASSERT_EQ(0, *findCommand->getBatchSize());
    ASSERT(!findCommand->getLimit());
}

TEST(QueryRequestTest, ParseFromCommandDefaultBatchSize) {
    BSONObj cmdObj = fromjson("{find: 'testns', '$db': 'test'}");
    unique_ptr<FindCommand> findCommand(query_request_helper::makeFromFindCommandForTests(cmdObj));

    ASSERT(!findCommand->getBatchSize());
    ASSERT(!findCommand->getLimit());
}

TEST(QueryRequestTest, ParseFromCommandRequestResumeToken) {
    BSONObj cmdObj = BSON("find"
                          << "testns"
                          << "hint" << BSON("$natural" << 1) << "sort" << BSON("$natural" << 1)
                          << "$_requestResumeToken" << true << "$db"
                          << "test");

    unique_ptr<FindCommand> findCommand(query_request_helper::makeFromFindCommandForTests(cmdObj));
    ASSERT(findCommand->getRequestResumeToken());
}

TEST(QueryRequestTest, ParseFromCommandResumeToken) {
    BSONObj cmdObj = BSON("find"
                          << "testns"
                          << "hint" << BSON("$natural" << 1) << "sort" << BSON("$natural" << 1)
                          << "$_requestResumeToken" << true << "$_resumeAfter"
                          << BSON("$recordId" << 1LL) << "$db"
                          << "test");

    unique_ptr<FindCommand> findCommand(query_request_helper::makeFromFindCommandForTests(cmdObj));
    ASSERT(!findCommand->getResumeAfter().isEmpty());
    ASSERT(findCommand->getRequestResumeToken());
}

TEST(QueryRequestTest, ParseFromCommandEmptyResumeToken) {
    BSONObj resumeAfter = fromjson("{}");
    BSONObj cmdObj =
        BSON("find"
             << "testns"
             << "hint" << BSON("$natural" << 1) << "sort" << BSON("$natural" << 1)
             << "$_requestResumeToken" << true << "$_resumeAfter" << resumeAfter << "$db"
             << "test");

    unique_ptr<FindCommand> findCommand(query_request_helper::makeFromFindCommandForTests(cmdObj));
    ASSERT(findCommand->getRequestResumeToken());
    ASSERT(findCommand->getResumeAfter().isEmpty());
}

//
// Test FindCommand object ns and uuid variants.
//

TEST(QueryRequestTest, AsFindCommandAllNonOptionFields) {
    BSONObj storage = BSON("runtimeConstants"
                           << (LegacyRuntimeConstants{Date_t::now(), Timestamp(1, 1)}.toBSON()));
    BSONObj cmdObj = fromjson(
                         "{find: 'testns',"
                         "filter: {a: 1},"
                         "projection: {c: 1},"
                         "sort: {b: 1},"
                         "hint: {d: 1},"
                         "collation: {f: 1},"
                         "skip: 5,"
                         "limit: 3,"
                         "batchSize: 90,"
                         "singleBatch: true, "
                         "readConcern: {e: 1}, '$db': 'test'}")
                         .addField(storage["runtimeConstants"]);

    unique_ptr<FindCommand> findCommand(query_request_helper::makeFromFindCommandForTests(cmdObj));
    ASSERT_BSONOBJ_EQ(cmdObj.removeField("$db"), findCommand->toBSON(BSONObj()));
}

TEST(QueryRequestTest, AsFindCommandWithUuidAllNonOptionFields) {
    BSONObj storage = BSON("runtimeConstants"
                           << (LegacyRuntimeConstants{Date_t::now(), Timestamp(1, 1)}.toBSON()));
    BSONObj cmdObj =
        fromjson(
            // This binary value is UUID("01234567-89ab-cdef-edcb-a98765432101")
            "{find: { \"$binary\" : \"ASNFZ4mrze/ty6mHZUMhAQ==\", \"$type\" : \"04\" },"
            "filter: {a: 1},"
            "projection: {c: 1},"
            "sort: {b: 1},"
            "hint: {d: 1},"
            "collation: {f: 1},"
            "skip: 5,"
            "limit: 3,"
            "batchSize: 90,"
            "singleBatch: true,"
            "readConcern: {e: 1}, '$db': 'test'}")
            .addField(storage["runtimeConstants"]);

    unique_ptr<FindCommand> findCommand(query_request_helper::makeFromFindCommandForTests(cmdObj));
    ASSERT_BSONOBJ_EQ(cmdObj.removeField("$db"), findCommand->toBSON(BSONObj()));
}

TEST(QueryRequestTest, AsFindCommandWithUuidNoAvailableNamespace) {
    BSONObj cmdObj =
        fromjson("{find: { \"$binary\" : \"ASNFZ4mrze/ty6mHZUMhAQ==\", \"$type\" : \"04\" }}");
    FindCommand findCommand(NamespaceStringOrUUID(
        "test", UUID::parse("01234567-89ab-cdef-edcb-a98765432101").getValue()));
    ASSERT_BSONOBJ_EQ(cmdObj.removeField("$db"), findCommand.toBSON(BSONObj()));
}

TEST(QueryRequestTest, AsFindCommandWithResumeToken) {
    BSONObj cmdObj = BSON("find"
                          << "testns"
                          << "sort" << BSON("$natural" << 1) << "hint" << BSON("$natural" << 1)
                          << "$_requestResumeToken" << true << "$_resumeAfter"
                          << BSON("$recordId" << 1LL) << "$db"
                          << "test");

    unique_ptr<FindCommand> findCommand(query_request_helper::makeFromFindCommandForTests(cmdObj));
    ASSERT_BSONOBJ_EQ(cmdObj.removeField("$db"), findCommand->toBSON(BSONObj()));
}

TEST(QueryRequestTest, AsFindCommandWithEmptyResumeToken) {
    BSONObj resumeAfter = fromjson("{}");
    BSONObj cmdObj =
        BSON("find"
             << "testns"
             << "hint" << BSON("$natural" << 1) << "sort" << BSON("$natural" << 1)
             << "$_requestResumeToken" << true << "$_resumeAfter" << resumeAfter << "$db"
             << "test");
    unique_ptr<FindCommand> findCommand(query_request_helper::makeFromFindCommandForTests(cmdObj));
    ASSERT(findCommand->toBSON(BSONObj()).getField("$_resumeAftr").eoo());
}

//
//
// Errors checked in query_request_helper::validateFindCommand().
//

TEST(QueryRequestTest, ParseFromCommandMinMaxDifferentFieldsError) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "min: {a: 3},"
        "max: {b: 4}, '$db': 'test'}");
    ASSERT_THROWS_CODE(
        query_request_helper::makeFromFindCommandForTests(cmdObj), DBException, 51176);
}

TEST(QueryRequestTest, ParseCommandAllowNonMetaSortOnFieldWithMetaProject) {
    BSONObj cmdObj;

    cmdObj = fromjson(
        "{find: 'testns',"
        "projection: {a: {$meta: 'textScore'}},"
        "sort: {a: 1}, '$db': 'test'}");
    query_request_helper::makeFromFindCommandForTests(cmdObj);

    cmdObj = fromjson(
        "{find: 'testns',"
        "projection: {a: {$meta: 'textScore'}},"
        "sort: {b: 1}, '$db': 'test'}");
    query_request_helper::makeFromFindCommandForTests(cmdObj);
}

TEST(QueryRequestTest, ParseCommandAllowMetaSortOnFieldWithoutMetaProject) {
    BSONObj cmdObj;

    cmdObj = fromjson(
        "{find: 'testns',"
        "projection: {a: 1},"
        "sort: {a: {$meta: 'textScore'}}, '$db': 'test'}");

    auto findCommand = query_request_helper::makeFromFindCommandForTests(cmdObj);

    cmdObj = fromjson(
        "{find: 'testns',"
        "projection: {b: 1},"
        "sort: {a: {$meta: 'textScore'}}, '$db': 'test'}");
    findCommand = query_request_helper::makeFromFindCommandForTests(cmdObj);
}

TEST(QueryRequestTest, ParseCommandForbidExhaust) {
    BSONObj cmdObj = fromjson("{find: 'testns', exhaust: true, '$db': 'test'}");

    ASSERT_THROWS_CODE(
        query_request_helper::makeFromFindCommandForTests(cmdObj), DBException, 40415);
}

TEST(QueryRequestTest, ParseCommandIsFromFindCommand) {
    BSONObj cmdObj = fromjson("{find: 'testns', '$db': 'test'}");
    unique_ptr<FindCommand> findCommand(query_request_helper::makeFromFindCommandForTests(cmdObj));

    ASSERT_FALSE(findCommand->getNtoreturn());
}

TEST(QueryRequestTest, ParseCommandAwaitDataButNotTailable) {
    BSONObj cmdObj = fromjson("{find: 'testns', awaitData: true, '$db': 'test'}");
    ASSERT_THROWS_CODE(query_request_helper::makeFromFindCommandForTests(cmdObj),
                       DBException,
                       ErrorCodes::FailedToParse);
}

TEST(QueryRequestTest, ParseCommandFirstFieldNotString) {
    BSONObj cmdObj = fromjson("{find: 1, '$db': 'test'}");
    ASSERT_THROWS_CODE(query_request_helper::makeFromFindCommandForTests(cmdObj),
                       DBException,
                       ErrorCodes::BadValue);
}

TEST(QueryRequestTest, ParseCommandIgnoreShardVersionField) {
    BSONObj cmdObj = fromjson("{find: 'test.testns', shardVersion: 'foo', '$db': 'test'}");
    query_request_helper::makeFromFindCommandForTests(cmdObj);
}

TEST(QueryRequestTest, DefaultQueryParametersCorrect) {
    BSONObj cmdObj = fromjson("{find: 'testns', '$db': 'test'}");

    std::unique_ptr<FindCommand> findCommand(
        query_request_helper::makeFromFindCommandForTests(cmdObj));

    ASSERT_FALSE(findCommand->getSkip());
    ASSERT_FALSE(findCommand->getLimit());

    ASSERT_FALSE(findCommand->getSingleBatch());
    ASSERT_FALSE(findCommand->getNtoreturn());
    ASSERT_EQUALS(0, findCommand->getMaxTimeMS().value_or(0));
    ASSERT_EQUALS(false, findCommand->getReturnKey());
    ASSERT_EQUALS(false, findCommand->getShowRecordId());
    ASSERT_EQUALS(false, findCommand->getTailable());
    ASSERT_EQUALS(false, findCommand->getNoCursorTimeout());
    ASSERT_EQUALS(false, findCommand->getTailable() && findCommand->getAwaitData());
    ASSERT_EQUALS(false, findCommand->getAllowPartialResults());
    ASSERT_EQUALS(false, findCommand->getLegacyRuntimeConstants().has_value());
    ASSERT_EQUALS(false, findCommand->getAllowDiskUse());
}

TEST(QueryRequestTest, ParseCommandAllowDiskUseTrue) {
    BSONObj cmdObj = fromjson("{find: 'testns', allowDiskUse: true, '$db': 'test'}");

    auto result = query_request_helper::makeFromFindCommandForTests(cmdObj);

    ASSERT_EQ(true, result->getAllowDiskUse());
}

TEST(QueryRequestTest, ParseCommandAllowDiskUseFalse) {
    BSONObj cmdObj = fromjson("{find: 'testns', allowDiskUse: false, '$db': 'test'}");

    auto result = query_request_helper::makeFromFindCommandForTests(cmdObj);

    ASSERT_EQ(false, result->getAllowDiskUse());
}

//
// Extra fields cause the parse to fail.
//

TEST(QueryRequestTest, ParseFromCommandForbidExtraField) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "foo: {a: 1}, '$db': 'test'}");

    ASSERT_THROWS_CODE(
        query_request_helper::makeFromFindCommandForTests(cmdObj), DBException, 40415);
}

TEST(QueryRequestTest, ParseFromCommandForbidExtraOption) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "foo: true, '$db': 'test'}");

    ASSERT_THROWS_CODE(
        query_request_helper::makeFromFindCommandForTests(cmdObj), DBException, 40415);
}

TEST(QueryRequestTest, ParseMaxTimeMSStringValueFails) {
    BSONObj maxTimeObj = BSON(query_request_helper::cmdOptionMaxTimeMS << "foo");
    ASSERT_NOT_OK(parseMaxTimeMS(maxTimeObj[query_request_helper::cmdOptionMaxTimeMS]));
}

TEST(QueryRequestTest, ParseMaxTimeMSNonIntegralValueFails) {
    BSONObj maxTimeObj = BSON(query_request_helper::cmdOptionMaxTimeMS << 100.3);
    ASSERT_NOT_OK(parseMaxTimeMS(maxTimeObj[query_request_helper::cmdOptionMaxTimeMS]));
}

TEST(QueryRequestTest, ParseMaxTimeMSOutOfRangeDoubleFails) {
    BSONObj maxTimeObj = BSON(query_request_helper::cmdOptionMaxTimeMS << 1e200);
    ASSERT_NOT_OK(parseMaxTimeMS(maxTimeObj[query_request_helper::cmdOptionMaxTimeMS]));
}

TEST(QueryRequestTest, ParseMaxTimeMSNegativeValueFails) {
    BSONObj maxTimeObj = BSON(query_request_helper::cmdOptionMaxTimeMS << -400);
    ASSERT_NOT_OK(parseMaxTimeMS(maxTimeObj[query_request_helper::cmdOptionMaxTimeMS]));
}

TEST(QueryRequestTest, ParseMaxTimeMSZeroSucceeds) {
    BSONObj maxTimeObj = BSON(query_request_helper::cmdOptionMaxTimeMS << 0);
    auto maxTime = parseMaxTimeMS(maxTimeObj[query_request_helper::cmdOptionMaxTimeMS]);
    ASSERT_OK(maxTime);
    ASSERT_EQ(maxTime.getValue(), 0);
}

TEST(QueryRequestTest, ParseMaxTimeMSPositiveInRangeSucceeds) {
    BSONObj maxTimeObj = BSON(query_request_helper::cmdOptionMaxTimeMS << 300);
    auto maxTime = parseMaxTimeMS(maxTimeObj[query_request_helper::cmdOptionMaxTimeMS]);
    ASSERT_OK(maxTime);
    ASSERT_EQ(maxTime.getValue(), 300);
}

TEST(QueryRequestTest, ConvertToAggregationSucceeds) {
    FindCommand findCommand(testns);
    auto agg = query_request_helper::asAggregationCommand(findCommand);
    ASSERT_OK(agg);

    auto aggCmd = OpMsgRequest::fromDBAndBody(testns.db(), agg.getValue()).body;
    auto ar = aggregation_request_helper::parseFromBSONForTests(testns, aggCmd);
    ASSERT_OK(ar.getStatus());
    ASSERT(!ar.getValue().getExplain());
    ASSERT(ar.getValue().getPipeline().empty());
    ASSERT_EQ(ar.getValue().getCursor().getBatchSize().value_or(
                  aggregation_request_helper::kDefaultBatchSize),
              aggregation_request_helper::kDefaultBatchSize);
    ASSERT_EQ(ar.getValue().getNamespace(), testns);
    ASSERT_BSONOBJ_EQ(ar.getValue().getCollation().value_or(BSONObj()), BSONObj());
}

TEST(QueryRequestTest, ConvertToAggregationOmitsExplain) {
    FindCommand findCommand(testns);
    auto agg = query_request_helper::asAggregationCommand(findCommand);
    ASSERT_OK(agg);

    auto aggCmd = OpMsgRequest::fromDBAndBody(testns.db(), agg.getValue()).body;
    auto ar = aggregation_request_helper::parseFromBSONForTests(testns, aggCmd);
    ASSERT_OK(ar.getStatus());
    ASSERT_FALSE(ar.getValue().getExplain());
    ASSERT(ar.getValue().getPipeline().empty());
    ASSERT_EQ(ar.getValue().getNamespace(), testns);
    ASSERT_BSONOBJ_EQ(ar.getValue().getCollation().value_or(BSONObj()), BSONObj());
}

TEST(QueryRequestTest, ConvertToAggregationWithHintSucceeds) {
    FindCommand findCommand(testns);
    findCommand.setHint(fromjson("{a_1: -1}"));
    const auto agg = query_request_helper::asAggregationCommand(findCommand);
    ASSERT_OK(agg);

    auto aggCmd = OpMsgRequest::fromDBAndBody(testns.db(), agg.getValue()).body;
    auto ar = aggregation_request_helper::parseFromBSONForTests(testns, aggCmd);
    ASSERT_OK(ar.getStatus());
    ASSERT_BSONOBJ_EQ(findCommand.getHint(), ar.getValue().getHint().value_or(BSONObj()));
}

TEST(QueryRequestTest, ConvertToAggregationWithMinFails) {
    FindCommand findCommand(testns);
    findCommand.setMin(fromjson("{a: 1}"));
    ASSERT_NOT_OK(query_request_helper::asAggregationCommand(findCommand));
}

TEST(QueryRequestTest, ConvertToAggregationWithMaxFails) {
    FindCommand findCommand(testns);
    findCommand.setMax(fromjson("{a: 1}"));
    ASSERT_NOT_OK(query_request_helper::asAggregationCommand(findCommand));
}

TEST(QueryRequestTest, ConvertToAggregationWithSingleBatchFieldFails) {
    FindCommand findCommand(testns);
    findCommand.setSingleBatch(true);
    ASSERT_NOT_OK(query_request_helper::asAggregationCommand(findCommand));
}

TEST(QueryRequestTest, ConvertToAggregationWithSingleBatchFieldAndLimitFails) {
    FindCommand findCommand(testns);
    findCommand.setSingleBatch(true);
    findCommand.setLimit(7);
    ASSERT_NOT_OK(query_request_helper::asAggregationCommand(findCommand));
}

TEST(QueryRequestTest, ConvertToAggregationWithSingleBatchFieldLimitOneSucceeds) {
    FindCommand findCommand(testns);
    findCommand.setSingleBatch(true);
    findCommand.setLimit(1);
    ASSERT_OK(query_request_helper::asAggregationCommand(findCommand));
}

TEST(QueryRequestTest, ConvertToAggregationWithReturnKeyFails) {
    FindCommand findCommand(testns);
    findCommand.setReturnKey(true);
    ASSERT_NOT_OK(query_request_helper::asAggregationCommand(findCommand));
}

TEST(QueryRequestTest, ConvertToAggregationWithShowRecordIdFails) {
    FindCommand findCommand(testns);
    findCommand.setShowRecordId(true);
    ASSERT_NOT_OK(query_request_helper::asAggregationCommand(findCommand));
}

TEST(QueryRequestTest, ConvertToAggregationWithTailableFails) {
    FindCommand findCommand(testns);
    query_request_helper::setTailableMode(TailableModeEnum::kTailable, &findCommand);
    ASSERT_NOT_OK(query_request_helper::asAggregationCommand(findCommand));
}

TEST(QueryRequestTest, ConvertToAggregationWithNoCursorTimeoutFails) {
    FindCommand findCommand(testns);
    findCommand.setNoCursorTimeout(true);
    ASSERT_NOT_OK(query_request_helper::asAggregationCommand(findCommand));
}

TEST(QueryRequestTest, ConvertToAggregationWithAwaitDataFails) {
    FindCommand findCommand(testns);
    query_request_helper::setTailableMode(TailableModeEnum::kTailableAndAwaitData, &findCommand);
    ASSERT_NOT_OK(query_request_helper::asAggregationCommand(findCommand));
}

TEST(QueryRequestTest, ConvertToAggregationWithAllowPartialResultsFails) {
    FindCommand findCommand(testns);
    findCommand.setAllowPartialResults(true);
    ASSERT_NOT_OK(query_request_helper::asAggregationCommand(findCommand));
}

TEST(QueryRequestTest, ConvertToAggregationWithNToReturnFails) {
    FindCommand findCommand(testns);
    findCommand.setNtoreturn(7);
    ASSERT_NOT_OK(query_request_helper::asAggregationCommand(findCommand));
}

TEST(QueryRequestTest, ConvertToAggregationWithRequestResumeTokenFails) {
    FindCommand findCommand(testns);
    findCommand.setRequestResumeToken(true);
    ASSERT_NOT_OK(query_request_helper::asAggregationCommand(findCommand));
}

TEST(QueryRequestTest, ConvertToAggregationWithResumeAfterFails) {
    FindCommand findCommand(testns);
    BSONObj resumeAfter = BSON("$recordId" << 1LL);
    findCommand.setResumeAfter(resumeAfter);
    ASSERT_NOT_OK(query_request_helper::asAggregationCommand(findCommand));
}

TEST(QueryRequestTest, ConvertToAggregationWithPipeline) {
    FindCommand findCommand(testns);
    findCommand.setFilter(BSON("x" << 1));
    findCommand.setSort(BSON("y" << -1));
    findCommand.setLimit(3);
    findCommand.setSkip(7);
    findCommand.setProjection(BSON("z" << 0));

    auto agg = query_request_helper::asAggregationCommand(findCommand);
    ASSERT_OK(agg);

    auto aggCmd = OpMsgRequest::fromDBAndBody(testns.db(), agg.getValue()).body;
    auto ar = aggregation_request_helper::parseFromBSONForTests(testns, aggCmd);
    ASSERT_OK(ar.getStatus());
    ASSERT(!ar.getValue().getExplain());
    ASSERT_EQ(ar.getValue().getCursor().getBatchSize().value_or(
                  aggregation_request_helper::kDefaultBatchSize),
              aggregation_request_helper::kDefaultBatchSize);
    ASSERT_EQ(ar.getValue().getNamespace(), testns);
    ASSERT_BSONOBJ_EQ(ar.getValue().getCollation().value_or(BSONObj()), BSONObj());

    std::vector<BSONObj> expectedPipeline{BSON("$match" << BSON("x" << 1)),
                                          BSON("$sort" << BSON("y" << -1)),
                                          BSON("$skip" << 7),
                                          BSON("$limit" << 3),
                                          BSON("$project" << BSON("z" << 0))};
    ASSERT(std::equal(expectedPipeline.begin(),
                      expectedPipeline.end(),
                      ar.getValue().getPipeline().begin(),
                      SimpleBSONObjComparator::kInstance.makeEqualTo()));
}

TEST(QueryRequestTest, ConvertToAggregationWithBatchSize) {
    FindCommand findCommand(testns);
    findCommand.setBatchSize(4);

    auto agg = query_request_helper::asAggregationCommand(findCommand);
    ASSERT_OK(agg);

    auto aggCmd = OpMsgRequest::fromDBAndBody(testns.db(), agg.getValue()).body;
    auto ar = aggregation_request_helper::parseFromBSONForTests(testns, aggCmd);
    ASSERT_OK(ar.getStatus());
    ASSERT(!ar.getValue().getExplain());
    ASSERT_EQ(ar.getValue().getNamespace(), testns);
    ASSERT_EQ(ar.getValue().getCursor().getBatchSize().value_or(
                  aggregation_request_helper::kDefaultBatchSize),
              4LL);
    ASSERT_BSONOBJ_EQ(ar.getValue().getCollation().value_or(BSONObj()), BSONObj());
}

TEST(QueryRequestTest, ConvertToAggregationWithMaxTimeMS) {
    FindCommand findCommand(testns);
    findCommand.setMaxTimeMS(9);

    auto agg = query_request_helper::asAggregationCommand(findCommand);
    ASSERT_OK(agg);

    const BSONObj cmdObj = agg.getValue();
    ASSERT_EQ(cmdObj["maxTimeMS"].Int(), 9);

    auto aggCmd = OpMsgRequest::fromDBAndBody(testns.db(), cmdObj).body;
    auto ar = aggregation_request_helper::parseFromBSONForTests(testns, aggCmd);
    ASSERT_OK(ar.getStatus());
    ASSERT(!ar.getValue().getExplain());
    ASSERT_EQ(ar.getValue().getCursor().getBatchSize().value_or(
                  aggregation_request_helper::kDefaultBatchSize),
              aggregation_request_helper::kDefaultBatchSize);
    ASSERT_EQ(ar.getValue().getNamespace(), testns);
    ASSERT_BSONOBJ_EQ(ar.getValue().getCollation().value_or(BSONObj()), BSONObj());
}

TEST(QueryRequestTest, ConvertToAggregationWithCollationSucceeds) {
    FindCommand findCommand(testns);
    findCommand.setCollation(BSON("f" << 1));
    auto agg = query_request_helper::asAggregationCommand(findCommand);
    ASSERT_OK(agg);

    auto aggCmd = OpMsgRequest::fromDBAndBody(testns.db(), agg.getValue()).body;
    auto ar = aggregation_request_helper::parseFromBSONForTests(testns, aggCmd);
    ASSERT_OK(ar.getStatus());
    ASSERT(!ar.getValue().getExplain());
    ASSERT(ar.getValue().getPipeline().empty());
    ASSERT_EQ(ar.getValue().getCursor().getBatchSize().value_or(
                  aggregation_request_helper::kDefaultBatchSize),
              aggregation_request_helper::kDefaultBatchSize);
    ASSERT_EQ(ar.getValue().getNamespace(), testns);
    ASSERT_BSONOBJ_EQ(ar.getValue().getCollation().value_or(BSONObj()), BSON("f" << 1));
}

TEST(QueryRequestTest, ConvertToAggregationWithReadOnceFails) {
    FindCommand findCommand(testns);
    findCommand.setReadOnce(true);
    const auto aggCmd = query_request_helper::asAggregationCommand(findCommand);
    ASSERT_EQ(ErrorCodes::InvalidPipelineOperator, aggCmd.getStatus().code());
}

TEST(QueryRequestTest, ConvertToAggregationWithAllowSpeculativeMajorityReadFails) {
    FindCommand findCommand(testns);
    findCommand.setAllowSpeculativeMajorityRead(true);
    const auto aggCmd = query_request_helper::asAggregationCommand(findCommand);
    ASSERT_EQ(ErrorCodes::InvalidPipelineOperator, aggCmd.getStatus().code());
}

TEST(QueryRequestTest, ConvertToAggregationWithLegacyRuntimeConstantsSucceeds) {
    LegacyRuntimeConstants rtc{Date_t::now(), Timestamp(1, 1)};
    FindCommand findCommand(testns);
    findCommand.setLegacyRuntimeConstants(rtc);
    auto agg = query_request_helper::asAggregationCommand(findCommand);
    ASSERT_OK(agg);

    auto aggCmd = OpMsgRequest::fromDBAndBody(testns.db(), agg.getValue()).body;
    auto ar = aggregation_request_helper::parseFromBSONForTests(testns, aggCmd);
    ASSERT_OK(ar.getStatus());
    ASSERT(ar.getValue().getLegacyRuntimeConstants().has_value());
    ASSERT_EQ(ar.getValue().getLegacyRuntimeConstants()->getLocalNow(), rtc.getLocalNow());
    ASSERT_EQ(ar.getValue().getLegacyRuntimeConstants()->getClusterTime(), rtc.getClusterTime());
}

TEST(QueryRequestTest, ConvertToAggregationWithAllowDiskUseTrueSucceeds) {
    FindCommand findCommand(testns);
    findCommand.setAllowDiskUse(true);
    const auto agg = query_request_helper::asAggregationCommand(findCommand);
    ASSERT_OK(agg.getStatus());

    auto aggCmd = OpMsgRequest::fromDBAndBody(testns.db(), agg.getValue()).body;
    auto ar = aggregation_request_helper::parseFromBSONForTests(testns, aggCmd);
    ASSERT_OK(ar.getStatus());
    ASSERT_EQ(true, ar.getValue().getAllowDiskUse());
}

TEST(QueryRequestTest, ConvertToAggregationWithAllowDiskUseFalseSucceeds) {
    FindCommand findCommand(testns);
    findCommand.setAllowDiskUse(false);
    const auto agg = query_request_helper::asAggregationCommand(findCommand);
    ASSERT_OK(agg.getStatus());

    auto aggCmd = OpMsgRequest::fromDBAndBody(testns.db(), agg.getValue()).body;
    auto ar = aggregation_request_helper::parseFromBSONForTests(testns, aggCmd);
    ASSERT_OK(ar.getStatus());
    ASSERT_EQ(false, ar.getValue().getAllowDiskUse());
}

TEST(QueryRequestTest, ConvertToFindWithAllowDiskUseTrueSucceeds) {
    FindCommand findCommand(testns);
    findCommand.setAllowDiskUse(true);
    const auto findCmd = findCommand.toBSON(BSONObj());

    BSONElement elem = findCmd[FindCommand::kAllowDiskUseFieldName];
    ASSERT_EQ(true, elem.isBoolean());
    ASSERT_EQ(true, elem.Bool());
}

TEST(QueryRequestTest, ConvertToFindWithAllowDiskUseFalseSucceeds) {
    FindCommand findCommand(testns);
    findCommand.setAllowDiskUse(false);
    const auto findCmd = findCommand.toBSON(BSONObj());

    ASSERT_FALSE(findCmd[FindCommand::kAllowDiskUseFieldName].booleanSafe());
}

TEST(QueryRequestTest, ParseFromLegacyQuery) {
    const auto kSkip = 1;
    const auto kNToReturn = 2;
    const NamespaceString nss("test.testns");
    BSONObj queryObj = fromjson(R"({
            query: {query: 1},
            orderby: {sort: 1},
            $hint: {hint: 1},
            $explain: false,
            $min: {x: 'min'},
            $max: {x: 'max'}
         })");

    bool explain = false;
    unique_ptr<FindCommand> findCommand(assertGet(query_request_helper::fromLegacyQuery(
        nss, queryObj, BSON("proj" << 1), kSkip, kNToReturn, QueryOption_Exhaust, &explain)));

    ASSERT_EQ(*findCommand->getNamespaceOrUUID().nss(), nss);
    ASSERT_EQ(explain, false);
    ASSERT_BSONOBJ_EQ(findCommand->getFilter(), fromjson("{query: 1}"));
    ASSERT_BSONOBJ_EQ(findCommand->getProjection(), fromjson("{proj: 1}"));
    ASSERT_BSONOBJ_EQ(findCommand->getSort(), fromjson("{sort: 1}"));
    ASSERT_BSONOBJ_EQ(findCommand->getHint(), fromjson("{hint: 1}"));
    ASSERT_BSONOBJ_EQ(findCommand->getMin(), fromjson("{x: 'min'}"));
    ASSERT_BSONOBJ_EQ(findCommand->getMax(), fromjson("{x: 'max'}"));
    ASSERT_EQ(findCommand->getSkip(), boost::optional<int64_t>(kSkip));
    ASSERT_EQ(findCommand->getNtoreturn(), boost::optional<int64_t>(kNToReturn));
    ASSERT_EQ(findCommand->getSingleBatch(), false);
    ASSERT_EQ(findCommand->getNoCursorTimeout(), false);
    ASSERT_EQ(findCommand->getTailable(), false);
    ASSERT_EQ(findCommand->getAllowPartialResults(), false);
}

TEST(QueryRequestTest, ParseFromLegacyQueryOplogReplayFlagAllowed) {
    const NamespaceString nss("test.testns");
    auto queryObj = fromjson("{query: {query: 1}, orderby: {sort: 1}}");
    const BSONObj projectionObj{};
    const auto nToSkip = 0;
    const auto nToReturn = 0;

    // Test that parsing succeeds even if the oplog replay bit is set in the OP_QUERY message. This
    // flag may be set by old clients.
    auto options = QueryOption_OplogReplay_DEPRECATED;
    bool explain = false;
    unique_ptr<FindCommand> findCommand(assertGet(query_request_helper::fromLegacyQuery(
        nss, queryObj, projectionObj, nToSkip, nToReturn, options, &explain)));

    // Verify that if we reserialize the find command, the 'oplogReplay' field
    // does not appear.
    BSONObjBuilder bob;
    findCommand->serialize(BSONObj(), &bob);
    auto reserialized = bob.obj();

    ASSERT_BSONOBJ_EQ(reserialized,
                      BSON("find"
                           << "testns"
                           << "filter" << BSON("query" << 1) << "sort" << BSON("sort" << 1)
                           << "readConcern" << BSONObj{}));
}

TEST(QueryRequestTest, ParseFromLegacyQueryUnwrapped) {
    BSONObj queryObj = fromjson(R"({
            foo: 1
         })");
    const NamespaceString nss("test.testns");
    bool explain = false;
    unique_ptr<FindCommand> findCommand(assertGet(query_request_helper::fromLegacyQuery(
        nss, queryObj, BSONObj(), 0, 0, QueryOption_Exhaust, &explain)));

    ASSERT_EQ(*findCommand->getNamespaceOrUUID().nss(), nss);
    ASSERT_BSONOBJ_EQ(findCommand->getFilter(), fromjson("{foo: 1}"));
}

TEST(QueryRequestHelperTest, ValidateResponseMissingFields) {
    BSONObjBuilder builder;
    ASSERT_THROWS_CODE(
        query_request_helper::validateCursorResponse(builder.asTempObj()), DBException, 40414);
}

TEST(QueryRequestHelperTest, ValidateResponseWrongDataType) {
    BSONObjBuilder builder;
    builder.append("cursor", 1);
    ASSERT_THROWS_CODE(query_request_helper::validateCursorResponse(builder.asTempObj()),
                       DBException,
                       ErrorCodes::TypeMismatch);
}

TEST(QueryRequestTest, ParseFromLegacyQueryTooNegativeNToReturn) {
    BSONObj queryObj = fromjson(R"({
            foo: 1
         })");

    const NamespaceString nss("test.testns");
    bool explain = false;
    ASSERT_NOT_OK(query_request_helper::fromLegacyQuery(nss,
                                                        queryObj,
                                                        BSONObj(),
                                                        0,
                                                        std::numeric_limits<int>::min(),
                                                        QueryOption_Exhaust,
                                                        &explain)
                      .getStatus());
}

class QueryRequestTest : public ServiceContextTest {};

TEST_F(QueryRequestTest, ParseFromUUID) {
    const CollectionUUID uuid = UUID::gen();


    NamespaceStringOrUUID nssOrUUID("test", uuid);
    FindCommand findCommand(nssOrUUID);
    const NamespaceString nss("test.testns");
    // Ensure a call to refreshNSS succeeds.
    query_request_helper::refreshNSS(nss, &findCommand);
    ASSERT_EQ(nss, *findCommand.getNamespaceOrUUID().nss());
}

}  // namespace
}  // namespace mongo
