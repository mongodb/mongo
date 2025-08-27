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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/pipeline/legacy_runtime_constants_gen.h"
#include "mongo/db/pipeline/query_request_conversion.h"
#include "mongo/db/query/client_cursor/cursor_id.h"
#include "mongo/db/query/client_cursor/cursor_response_gen.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/find_command_gen.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/db/query/tailable_mode_gen.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/tenant_id.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/serialization_context.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

using std::unique_ptr;

static const NamespaceString testns =
    NamespaceString::createNamespaceString_forTest("testdb.testcoll");

TEST(QueryRequestTest, NegativeSkip) {
    FindCommandRequest findCommand(testns);
    ASSERT_THROWS_CODE(findCommand.setSkip(-1), DBException, ErrorCodes::BadValue);
}

TEST(QueryRequestTest, ZeroSkip) {
    FindCommandRequest findCommand(testns);
    findCommand.setSkip(0);
    ASSERT_OK(query_request_helper::validateFindCommandRequest(findCommand));
}

TEST(QueryRequestTest, PositiveSkip) {
    FindCommandRequest findCommand(testns);
    findCommand.setSkip(1);
    ASSERT_OK(query_request_helper::validateFindCommandRequest(findCommand));
}

TEST(QueryRequestTest, NegativeLimit) {
    FindCommandRequest findCommand(testns);
    ASSERT_THROWS_CODE(findCommand.setLimit(-1), DBException, ErrorCodes::BadValue);
}

TEST(QueryRequestTest, ZeroLimit) {
    FindCommandRequest findCommand(testns);
    findCommand.setLimit(0);
    ASSERT_OK(query_request_helper::validateFindCommandRequest(findCommand));
}

TEST(QueryRequestTest, PositiveLimit) {
    FindCommandRequest findCommand(testns);
    findCommand.setLimit(1);
    ASSERT_OK(query_request_helper::validateFindCommandRequest(findCommand));
}

TEST(QueryRequestTest, NegativeBatchSize) {
    FindCommandRequest findCommand(testns);
    ASSERT_THROWS_CODE(findCommand.setBatchSize(-1), DBException, ErrorCodes::BadValue);
}

TEST(QueryRequestTest, ZeroBatchSize) {
    FindCommandRequest findCommand(testns);
    findCommand.setBatchSize(0);
    ASSERT_OK(query_request_helper::validateFindCommandRequest(findCommand));
}

TEST(QueryRequestTest, PositiveBatchSize) {
    FindCommandRequest findCommand(testns);
    findCommand.setBatchSize(1);
    ASSERT_OK(query_request_helper::validateFindCommandRequest(findCommand));
}

TEST(QueryRequestTest, NegativeMaxTimeMS) {
    FindCommandRequest findCommand(testns);
    ASSERT_THROWS_CODE(findCommand.setMaxTimeMS(-1), DBException, ErrorCodes::BadValue);
}

TEST(QueryRequestTest, ZeroMaxTimeMS) {
    FindCommandRequest findCommand(testns);
    findCommand.setMaxTimeMS(0);
    ASSERT_OK(query_request_helper::validateFindCommandRequest(findCommand));
}

TEST(QueryRequestTest, PositiveMaxTimeMS) {
    FindCommandRequest findCommand(testns);
    findCommand.setMaxTimeMS(1);
    ASSERT_OK(query_request_helper::validateFindCommandRequest(findCommand));
}

TEST(QueryRequestTest, ValidSortOrder) {
    FindCommandRequest findCommand(testns);
    findCommand.setSort(fromjson("{a: 1}"));
    ASSERT_OK(query_request_helper::validateFindCommandRequest(findCommand));
}

TEST(QueryRequestTest, DoesNotErrorOnInvalidSortPattern) {
    FindCommandRequest findCommand(testns);
    findCommand.setSort(fromjson("{a: \"\"}"));
    // FindCommandRequest isn't responsible for validating the sort pattern, so it is considered
    // valid even though the sort pattern {a: ""} is not well-formed.
    ASSERT_OK(query_request_helper::validateFindCommandRequest(findCommand));
}

TEST(QueryRequestTest, MinFieldsNotPrefixOfMax) {
    FindCommandRequest findCommand(testns);
    findCommand.setMin(fromjson("{a: 1}"));
    findCommand.setMax(fromjson("{b: 1}"));
    ASSERT_NOT_OK(query_request_helper::validateFindCommandRequest(findCommand));
}

TEST(QueryRequestTest, MinFieldsMoreThanMax) {
    FindCommandRequest findCommand(testns);
    findCommand.setMin(fromjson("{a: 1, b: 1}"));
    findCommand.setMax(fromjson("{a: 1}"));
    ASSERT_NOT_OK(query_request_helper::validateFindCommandRequest(findCommand));
}

TEST(QueryRequestTest, MinFieldsLessThanMax) {
    FindCommandRequest findCommand(testns);
    findCommand.setMin(fromjson("{a: 1}"));
    findCommand.setMax(fromjson("{a: 1, b: 1}"));
    ASSERT_NOT_OK(query_request_helper::validateFindCommandRequest(findCommand));
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
    FindCommandRequest findCommand(testns);
    findCommand.setProjection(fromjson("{a: 1}"));
    findCommand.setSort(fromjson("{a: 1}"));
    ASSERT_OK(query_request_helper::validateFindCommandRequest(findCommand));

    FindCommandRequest metaFC(testns);
    metaFC.setProjection(fromjson("{a: {$meta: \"textScore\"}}"));
    metaFC.setSort(fromjson("{a: {$meta: \"textScore\"}}"));
    ASSERT_OK(query_request_helper::validateFindCommandRequest(metaFC));
}

TEST(QueryRequestTest, TextScoreMetaSortOnFieldDoesNotRequireMetaProjection) {
    FindCommandRequest findCommand(testns);
    findCommand.setProjection(fromjson("{b: 1}"));
    findCommand.setSort(fromjson("{a: {$meta: 'textScore'}}"));
    ASSERT_OK(query_request_helper::validateFindCommandRequest(findCommand));
}

TEST(QueryRequestTest, TextScoreMetaProjectionDoesNotRequireTextScoreMetaSort) {
    FindCommandRequest findCommand(testns);
    findCommand.setProjection(fromjson("{a: {$meta: \"textScore\"}}"));
    findCommand.setSort(fromjson("{b: 1}"));
    ASSERT_OK(query_request_helper::validateFindCommandRequest(findCommand));
}

TEST(QueryRequestTest, RequestResumeTokenWithHint) {
    FindCommandRequest findCommand(testns);
    findCommand.setRequestResumeToken(true);
    ASSERT_NOT_OK(query_request_helper::validateFindCommandRequest(findCommand));
    findCommand.setHint(fromjson("{a: 1}"));
    ASSERT_NOT_OK(query_request_helper::validateFindCommandRequest(findCommand));
    findCommand.setHint(fromjson("{$natural: 1}"));
    ASSERT_OK(query_request_helper::validateFindCommandRequest(findCommand));
}

TEST(QueryRequestTest, RequestResumeTokenWithSort) {
    FindCommandRequest findCommand(testns);
    findCommand.setRequestResumeToken(true);
    // Hint must be explicitly set for the query request to validate.
    findCommand.setHint(fromjson("{$natural: 1}"));
    ASSERT_OK(query_request_helper::validateFindCommandRequest(findCommand));
    findCommand.setSort(fromjson("{a: 1}"));
    ASSERT_NOT_OK(query_request_helper::validateFindCommandRequest(findCommand));
    findCommand.setSort(fromjson("{$natural: 1}"));
    ASSERT_OK(query_request_helper::validateFindCommandRequest(findCommand));
}

TEST(QueryRequestTest, InvalidResumeAfterStartAtTogether) {
    QueryTestServiceContext serviceContext;
    auto uniqueTxn = serviceContext.makeOperationContext();
    OperationContext* opCtx = uniqueTxn.get();
    FindCommandRequest findCommand(testns);

    findCommand.setResumeAfter(fromjson("{$recordId: NumberLong(1)}"));
    findCommand.setStartAt(fromjson("{$recordId: NumberLong(2)}"));
    findCommand.setRequestResumeToken(true);
    findCommand.setHint(fromjson("{$natural: 1}"));

    ASSERT_NOT_OK(query_request_helper::validateResumeInput(
        opCtx, findCommand.getResumeAfter(), findCommand.getStartAt(), false));
}

TEST(QueryRequestTest, InvalidResumeAfterWrongRecordIdType) {
    QueryTestServiceContext serviceContext;
    auto uniqueTxn = serviceContext.makeOperationContext();
    OperationContext* opCtx = uniqueTxn.get();
    FindCommandRequest findCommand(testns);
    BSONObj resumeAfter = BSON("$recordId" << 1);
    findCommand.setResumeAfter(resumeAfter);
    findCommand.setRequestResumeToken(true);
    // Hint must be explicitly set for the query request to validate.
    findCommand.setHint(fromjson("{$natural: 1}"));
    ASSERT_NOT_OK(query_request_helper::validateResumeInput(
        opCtx, findCommand.getResumeAfter(), {} /* startAt */, false /* isClusteredCollection */));
    resumeAfter = BSON("$recordId" << 1LL);
    findCommand.setResumeAfter(resumeAfter);
    ASSERT_OK(query_request_helper::validateFindCommandRequest(findCommand));
    ASSERT_OK(query_request_helper::validateResumeInput(
        opCtx, findCommand.getResumeAfter(), {} /* startAt */, false /* isClusteredCollection */));
}

TEST(QueryRequestTest, InvalidStartAtWrongRecordIdType) {
    QueryTestServiceContext serviceContext;
    auto uniqueTxn = serviceContext.makeOperationContext();
    OperationContext* opCtx = uniqueTxn.get();
    FindCommandRequest findCommand(testns);
    findCommand.setStartAt(fromjson("{$recordId: NumberInt(1)}"));
    findCommand.setRequestResumeToken(true);
    // Hint must be explicitly set for the query request to validate.
    findCommand.setHint(fromjson("{$natural: 1}"));
    ASSERT_NOT_OK(query_request_helper::validateResumeInput(
        opCtx, {} /* resumeAfter */, findCommand.getStartAt(), false /* isClusteredCollection */));
    findCommand.setStartAt(fromjson("{$recordId: NumberLong(1)}"));
    ASSERT_OK(query_request_helper::validateFindCommandRequest(findCommand));
    ASSERT_OK(query_request_helper::validateResumeInput(
        opCtx, {} /* resumeAfter */, findCommand.getStartAt(), false /* isClusteredCollection */));
}

TEST(QueryRequestTest, InvalidResumeAfterExtraField) {
    QueryTestServiceContext serviceContext;
    auto uniqueTxn = serviceContext.makeOperationContext();
    OperationContext* opCtx = uniqueTxn.get();
    FindCommandRequest findCommand(testns);
    BSONObj resumeAfter = BSON("$recordId" << 1LL << "$extra" << 1);
    findCommand.setResumeAfter(resumeAfter);
    findCommand.setRequestResumeToken(true);
    // Hint must be explicitly set for the query request to validate.
    findCommand.setHint(fromjson("{$natural: 1}"));
    ASSERT_NOT_OK(query_request_helper::validateResumeInput(
        opCtx, findCommand.getResumeAfter(), {} /* startAt */, false /* isClusteredCollection */));
}

TEST(QueryRequestTest, InvalidStartAtExtraField) {
    QueryTestServiceContext serviceContext;
    auto uniqueTxn = serviceContext.makeOperationContext();
    OperationContext* opCtx = uniqueTxn.get();
    FindCommandRequest findCommand(testns);
    findCommand.setStartAt(fromjson("{$recordId: NumberLong(1), $extra: 1}"));
    findCommand.setRequestResumeToken(true);
    // Hint must be explicitly set for the query request to validate.
    findCommand.setHint(fromjson("{$natural: 1}"));
    ASSERT_NOT_OK(query_request_helper::validateResumeInput(
        opCtx, {} /* resumeAfter */, findCommand.getStartAt(), false /* isClusteredCollection */));
}

TEST(QueryRequestTest, ResumeAfterWithHint) {
    FindCommandRequest findCommand(testns);
    BSONObj resumeAfter = BSON("$recordId" << 1LL);
    findCommand.setResumeAfter(resumeAfter);
    findCommand.setRequestResumeToken(true);
    ASSERT_NOT_OK(query_request_helper::validateFindCommandRequest(findCommand));
    findCommand.setHint(fromjson("{a: 1}"));
    ASSERT_NOT_OK(query_request_helper::validateFindCommandRequest(findCommand));
    findCommand.setHint(fromjson("{$natural: 1}"));
    ASSERT_OK(query_request_helper::validateFindCommandRequest(findCommand));
}

TEST(QueryRequestTest, StartAtWithHint) {
    FindCommandRequest findCommand(testns);
    findCommand.setStartAt(fromjson("{$recordId: NumberLong(1)}"));
    findCommand.setRequestResumeToken(true);
    ASSERT_NOT_OK(query_request_helper::validateFindCommandRequest(findCommand));
    findCommand.setHint(fromjson("{a: 1}"));
    ASSERT_NOT_OK(query_request_helper::validateFindCommandRequest(findCommand));
    findCommand.setHint(fromjson("{$natural: 1}"));
    ASSERT_OK(query_request_helper::validateFindCommandRequest(findCommand));
}

TEST(QueryRequestTest, ResumeAfterWithSort) {
    QueryTestServiceContext serviceContext;
    auto uniqueTxn = serviceContext.makeOperationContext();
    OperationContext* opCtx = uniqueTxn.get();
    FindCommandRequest findCommand(testns);
    BSONObj resumeAfter = BSON("$recordId" << 1LL);
    findCommand.setResumeAfter(resumeAfter);
    findCommand.setRequestResumeToken(true);
    // Hint must be explicitly set for the query request to validate.
    findCommand.setHint(fromjson("{$natural: 1}"));
    ASSERT_OK(query_request_helper::validateFindCommandRequest(findCommand));
    ASSERT_OK(query_request_helper::validateResumeInput(
        opCtx, findCommand.getResumeAfter(), {} /* startAt */, false /* isClusteredCollection */));
    findCommand.setSort(fromjson("{a: 1}"));
    ASSERT_NOT_OK(query_request_helper::validateFindCommandRequest(findCommand));
    findCommand.setSort(fromjson("{$natural: 1}"));
    ASSERT_OK(query_request_helper::validateFindCommandRequest(findCommand));
}

TEST(QueryRequestTest, StartAtWithSort) {
    QueryTestServiceContext serviceContext;
    auto uniqueTxn = serviceContext.makeOperationContext();
    OperationContext* opCtx = uniqueTxn.get();
    FindCommandRequest findCommand(testns);
    findCommand.setStartAt(fromjson("{$recordId: NumberLong(1)}"));
    findCommand.setRequestResumeToken(true);
    findCommand.setHint(fromjson("{$natural: 1}"));
    ASSERT_OK(query_request_helper::validateFindCommandRequest(findCommand));
    ASSERT_OK(query_request_helper::validateResumeInput(
        opCtx, {} /* resumeAfter */, findCommand.getStartAt(), false));
    findCommand.setSort(fromjson("{a: 1}"));
    ASSERT_NOT_OK(query_request_helper::validateFindCommandRequest(findCommand));
    findCommand.setSort(fromjson("{$natural: 1}"));
    ASSERT_OK(query_request_helper::validateFindCommandRequest(findCommand));
}

TEST(QueryRequestTest, ResumeNoSpecifiedRequestResumeToken) {
    QueryTestServiceContext serviceContext;
    auto uniqueTxn = serviceContext.makeOperationContext();
    OperationContext* opCtx = uniqueTxn.get();
    FindCommandRequest findCommand(testns);
    BSONObj resumeAfter = BSON("$recordId" << 1LL);
    findCommand.setResumeAfter(resumeAfter);
    // Hint must be explicitly set for the query request to validate.
    findCommand.setHint(fromjson("{$natural: 1}"));
    ASSERT_NOT_OK(query_request_helper::validateFindCommandRequest(findCommand));
    findCommand.setRequestResumeToken(true);
    ASSERT_OK(query_request_helper::validateFindCommandRequest(findCommand));
    ASSERT_OK(query_request_helper::validateResumeInput(
        opCtx, findCommand.getResumeAfter(), {} /* startAt */, false /* isClusteredCollection */));
}

TEST(QueryRequestTest, StartAtNoSpecifiedRequestResumeToken) {
    QueryTestServiceContext serviceContext;
    auto uniqueTxn = serviceContext.makeOperationContext();
    OperationContext* opCtx = uniqueTxn.get();
    FindCommandRequest findCommand(testns);
    findCommand.setStartAt(fromjson("{$recordId: NumberLong(1)}"));
    findCommand.setHint(fromjson("{$natural: 1}"));
    ASSERT_NOT_OK(query_request_helper::validateFindCommandRequest(findCommand));
    findCommand.setRequestResumeToken(true);
    ASSERT_OK(query_request_helper::validateFindCommandRequest(findCommand));
    ASSERT_OK(query_request_helper::validateResumeInput(
        opCtx, {} /* resumeAfter */, findCommand.getStartAt(), false));
}

TEST(QueryRequestTest, ExplicitEmptyResumeAfter) {
    QueryTestServiceContext serviceContext;
    auto uniqueTxn = serviceContext.makeOperationContext();
    OperationContext* opCtx = uniqueTxn.get();
    FindCommandRequest findCommand(NamespaceString::kRsOplogNamespace);
    BSONObj resumeAfter = fromjson("{}");
    // Hint must be explicitly set for the query request to validate.
    findCommand.setHint(fromjson("{$natural: 1}"));
    findCommand.setResumeAfter(resumeAfter);
    ASSERT_OK(query_request_helper::validateFindCommandRequest(findCommand));
    findCommand.setRequestResumeToken(true);
    ASSERT_OK(query_request_helper::validateFindCommandRequest(findCommand));
    ASSERT_OK(query_request_helper::validateResumeInput(
        opCtx, findCommand.getResumeAfter(), {} /* startAt */, false /* isClusteredCollection */));
}

TEST(QueryRequestTest, ExplicitEmptyStartAt) {
    QueryTestServiceContext serviceContext;
    auto uniqueTxn = serviceContext.makeOperationContext();
    OperationContext* opCtx = uniqueTxn.get();
    FindCommandRequest findCommand(NamespaceString::kRsOplogNamespace);
    findCommand.setHint(fromjson("{$natural: 1}"));
    findCommand.setStartAt(fromjson("{}"));
    ASSERT_OK(query_request_helper::validateFindCommandRequest(findCommand));
    findCommand.setRequestResumeToken(true);
    ASSERT_OK(query_request_helper::validateFindCommandRequest(findCommand));
    ASSERT_OK(query_request_helper::validateResumeInput(
        opCtx, {} /* resumeAfter */, findCommand.getStartAt(), false));
}

TEST(QueryRequestTest, ResumeAfterMismatchInitialSyncId) {
    QueryTestServiceContext serviceContext;
    auto uniqueTxn = serviceContext.makeOperationContext();
    OperationContext* opCtx = uniqueTxn.get();
    auto replCoord =
        std::make_unique<repl::ReplicationCoordinatorMock>(serviceContext.getServiceContext());
    repl::ReplicationCoordinator::set(serviceContext.getServiceContext(), std::move(replCoord));

    FindCommandRequest findCommand(testns);
    BSONObj resumeAfter =
        BSON("$recordId" << 1LL << "$initialSyncId"
                         << uassertStatusOK(UUID::parse("12345678-1234-9876-1234-000000000000")));
    findCommand.setResumeAfter(resumeAfter);
    // Hint must be explicitly set for the query request to validate.
    findCommand.setHint(fromjson("{$natural: 1}"));
    ASSERT_NOT_OK(query_request_helper::validateFindCommandRequest(findCommand));
    findCommand.setRequestResumeToken(true);
    ASSERT_OK(query_request_helper::validateFindCommandRequest(findCommand));
    ASSERT_NOT_OK(query_request_helper::validateResumeInput(
        opCtx, findCommand.getResumeAfter(), {} /* startAt */, false /* isClusteredCollection */));
}

TEST(QueryRequestTest, StartAtMismatchInitialSyncId) {
    QueryTestServiceContext serviceContext;
    auto uniqueTxn = serviceContext.makeOperationContext();
    OperationContext* opCtx = uniqueTxn.get();
    auto replCoord =
        std::make_unique<repl::ReplicationCoordinatorMock>(serviceContext.getServiceContext());
    repl::ReplicationCoordinator::set(serviceContext.getServiceContext(), std::move(replCoord));

    FindCommandRequest findCommand(testns);
    BSONObj startAt =
        BSON("$recordId" << 1LL << "$initialSyncId"
                         << uassertStatusOK(UUID::parse("12345678-1234-9876-1234-000000000000")));
    findCommand.setStartAt(startAt);
    findCommand.setHint(fromjson("{$natural: 1}"));
    ASSERT_NOT_OK(query_request_helper::validateFindCommandRequest(findCommand));
    findCommand.setRequestResumeToken(true);
    ASSERT_OK(query_request_helper::validateFindCommandRequest(findCommand));
    ASSERT_NOT_OK(query_request_helper::validateResumeInput(
        opCtx, {} /* resumeAfter */, findCommand.getStartAt(), false));
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

    unique_ptr<FindCommandRequest> findCommand(
        query_request_helper::makeFromFindCommandForTests(cmdObj));

    // Make sure the values from the command BSON are reflected in the QR.
    ASSERT(findCommand->getShowRecordId());
}

TEST(QueryRequestTest, ParseFromCommandHintAsString) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter:  {a: 1},"
        "hint: 'foo_1', '$db': 'test'}");

    unique_ptr<FindCommandRequest> findCommand(
        query_request_helper::makeFromFindCommandForTests(cmdObj));

    BSONObj hintObj = findCommand->getHint();
    ASSERT_BSONOBJ_EQ(BSON("$hint" << "foo_1"), hintObj);
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
        "includeQueryStatsMetrics: true,"
        "'$db': 'test'}");

    unique_ptr<FindCommandRequest> findCommand(
        query_request_helper::makeFromFindCommandForTests(cmdObj));

    // Test that all the flags got set to true.
    ASSERT(findCommand->getTailable());
    ASSERT(findCommand->getNoCursorTimeout());
    ASSERT(findCommand->getTailable() && findCommand->getAwaitData());
    ASSERT(findCommand->getAllowPartialResults());
    ASSERT(findCommand->getReadOnce());
    ASSERT(findCommand->getIncludeQueryStatsMetrics());
}

TEST(QueryRequestTest, OplogReplayFlagIsAllowedButIgnored) {
    auto cmdObj = BSON("find" << "testns"
                              << "oplogReplay" << true << "tailable" << true << "$db"
                              << "test");
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.testns");
    auto findCommand = query_request_helper::makeFromFindCommandForTests(cmdObj);

    // Verify that the 'oplogReplay' flag does not appear if we reserialize the request.
    auto reserialized = findCommand->toBSON();
    ASSERT_BSONOBJ_EQ(reserialized,
                      BSON("find" << "testns"
                                  << "tailable" << true));
}

TEST(QueryRequestTest, ParseFromCommandReadOnceDefaultsToFalse) {
    BSONObj cmdObj = fromjson("{find: 'testns', '$db': 'test'}");

    unique_ptr<FindCommandRequest> findCommand(
        query_request_helper::makeFromFindCommandForTests(cmdObj));
    ASSERT(!findCommand->getReadOnce());
}

TEST(QueryRequestTest, ParseFromCommandIncludeQueryStatsMetricsDefaultsToFalse) {
    BSONObj cmdObj = fromjson("{find: 'testns', '$db': 'test'}");

    unique_ptr<FindCommandRequest> findCommand(
        query_request_helper::makeFromFindCommandForTests(cmdObj));
    ASSERT(!findCommand->getIncludeQueryStatsMetrics());
}

TEST(QueryRequestTest, ParseFromCommandValidMinMax) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "comment: 'the comment',"
        "min: {a: 1},"
        "max: {a: 2}, '$db': 'test'}");

    unique_ptr<FindCommandRequest> findCommand(
        query_request_helper::makeFromFindCommandForTests(cmdObj));
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
                         "readConcern: {level: 'local'},"
                         "$queryOptions: {$readPreference: 'secondary'},"
                         "collation: {f: 1},"
                         "limit: 3,"
                         "skip: 5,"
                         "batchSize: 90,"
                         "singleBatch: false, '$db': 'test'}")
                         .addField(rtcObj["runtimeConstants"]);

    unique_ptr<FindCommandRequest> findCommand(
        query_request_helper::makeFromFindCommandForTests(cmdObj));
    // Check the values inside the QR.
    BSONObj expectedQuery = BSON("a" << 1);
    ASSERT_EQUALS(0, expectedQuery.woCompare(findCommand->getFilter()));
    BSONObj expectedSort = BSON("b" << 1);
    ASSERT_EQUALS(0, expectedSort.woCompare(findCommand->getSort()));
    BSONObj expectedProj = BSON("c" << 1);
    ASSERT_EQUALS(0, expectedProj.woCompare(findCommand->getProjection()));
    BSONObj expectedHint = BSON("d" << 1);
    ASSERT_EQUALS(0, expectedHint.woCompare(findCommand->getHint()));
    ASSERT(findCommand->getReadConcern());
    ASSERT_BSONOBJ_EQ(repl::ReadConcernArgs::kLocal.toBSONInner(),
                      findCommand->getReadConcern()->toBSONInner());
    BSONObj expectedUnwrappedReadPref = BSON("$readPreference" << "secondary");
    ASSERT_BSONOBJ_EQ(expectedUnwrappedReadPref,
                      findCommand->getUnwrappedReadPref().value_or(BSONObj()));
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

    unique_ptr<FindCommandRequest> findCommand(
        query_request_helper::makeFromFindCommandForTests(cmdObj));

    ASSERT_EQUALS(8LL * 1000 * 1000 * 1000, *findCommand->getLimit());
}

TEST(QueryRequestTest, ParseFromCommandLargeBatchSize) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter: {a: 1},"
        "batchSize: 8000000000, '$db': 'test'}");  // 8 * 1000 * 1000 * 1000

    unique_ptr<FindCommandRequest> findCommand(
        query_request_helper::makeFromFindCommandForTests(cmdObj));

    ASSERT_EQUALS(8LL * 1000 * 1000 * 1000, *findCommand->getBatchSize());
}

TEST(QueryRequestTest, ParseFromCommandLargeSkip) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "filter: {a: 1},"
        "skip: 8000000000, '$db': 'test'}");  // 8 * 1000 * 1000 * 1000

    unique_ptr<FindCommandRequest> findCommand(
        query_request_helper::makeFromFindCommandForTests(cmdObj));

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
                       ErrorCodes::TypeMismatch);
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

    ASSERT_THROWS_CODE(query_request_helper::makeFromFindCommandForTests(cmdObj),
                       DBException,
                       ErrorCodes::IDLUnknownField);
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

    ASSERT_THROWS_CODE(query_request_helper::makeFromFindCommandForTests(cmdObj),
                       DBException,
                       ErrorCodes::IDLUnknownField);
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
    BSONObj cmdObj = BSON("find" << "testns"
                                 << "runtimeConstants"
                                 << "shouldNotBeString"
                                 << "$db"
                                 << "test");

    ASSERT_THROWS_CODE(query_request_helper::makeFromFindCommandForTests(cmdObj),
                       DBException,
                       ErrorCodes::TypeMismatch);
}

TEST(QueryRequestTest, ParseFromCommandLegacyRuntimeConstantsSubfieldsWrongType) {
    BSONObj cmdObj = BSON("find" << "testns"
                                 << "runtimeConstants"
                                 << BSON("localNow" << "shouldBeDate"
                                                    << "clusterTime"
                                                    << "shouldBeTimestamp")
                                 << "$db"
                                 << "test");
    ASSERT_THROWS_CODE(query_request_helper::makeFromFindCommandForTests(cmdObj),
                       AssertionException,
                       ErrorCodes::TypeMismatch);
}

TEST(QueryRequestTest, ParseFromCommandIncludeQueryStatsMetricsWrongType) {
    BSONObj cmdObj = fromjson("{find: 'testns', '$db': 'test', 'includeQueryStatsMetrics': 42}");

    ASSERT_THROWS_CODE(query_request_helper::makeFromFindCommandForTests(cmdObj),
                       DBException,
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
    ASSERT_THROWS_CODE(query_request_helper::makeFromFindCommandForTests(cmdObj),
                       DBException,
                       ErrorCodes::BadValue);
}

TEST(QueryRequestTest, ParseFromCommandSkipIsZero) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "skip: 0,"
        "filter: {a: 3}, '$db': 'test'}");
    unique_ptr<FindCommandRequest> findCommand(
        query_request_helper::makeFromFindCommandForTests(cmdObj));
    ASSERT_BSONOBJ_EQ(BSON("a" << 3), findCommand->getFilter());
    ASSERT_FALSE(findCommand->getSkip());
}

TEST(QueryRequestTest, ParseFromCommandNegativeLimitError) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "limit: -3,"
        "filter: {a: 3}, '$db': 'test'}");
    ASSERT_THROWS_CODE(query_request_helper::makeFromFindCommandForTests(cmdObj),
                       DBException,
                       ErrorCodes::BadValue);
}

TEST(QueryRequestTest, ParseFromCommandLimitIsZero) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "limit: 0,"
        "filter: {a: 3}, '$db': 'test'}");
    unique_ptr<FindCommandRequest> findCommand(
        query_request_helper::makeFromFindCommandForTests(cmdObj));
    ASSERT_BSONOBJ_EQ(BSON("a" << 3), findCommand->getFilter());
    ASSERT_FALSE(findCommand->getLimit());
}

TEST(QueryRequestTest, ParseFromCommandNegativeBatchSizeError) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "batchSize: -10,"
        "filter: {a: 3}, '$db': 'test'}");
    ASSERT_THROWS_CODE(query_request_helper::makeFromFindCommandForTests(cmdObj),
                       DBException,
                       ErrorCodes::BadValue);
}

TEST(QueryRequestTest, ParseFromCommandBatchSizeZero) {
    BSONObj cmdObj = fromjson("{find: 'testns', batchSize: 0, '$db': 'test'}");
    unique_ptr<FindCommandRequest> findCommand(
        query_request_helper::makeFromFindCommandForTests(cmdObj));

    ASSERT(findCommand->getBatchSize());
    ASSERT_EQ(0, *findCommand->getBatchSize());
    ASSERT(!findCommand->getLimit());
}

TEST(QueryRequestTest, ParseFromCommandDefaultBatchSize) {
    BSONObj cmdObj = fromjson("{find: 'testns', '$db': 'test'}");
    unique_ptr<FindCommandRequest> findCommand(
        query_request_helper::makeFromFindCommandForTests(cmdObj));

    ASSERT(!findCommand->getBatchSize());
    ASSERT(!findCommand->getLimit());
}

TEST(QueryRequestTest, ParseFromCommandRequestResumeToken) {
    BSONObj cmdObj = BSON("find" << "testns"
                                 << "hint" << BSON("$natural" << 1) << "sort"
                                 << BSON("$natural" << 1) << "$_requestResumeToken" << true << "$db"
                                 << "test");

    unique_ptr<FindCommandRequest> findCommand(
        query_request_helper::makeFromFindCommandForTests(cmdObj));
    ASSERT(findCommand->getRequestResumeToken());
}

TEST(QueryRequestTest, ParseFromCommandResumeToken) {
    BSONObj cmdObj = BSON("find" << "testns"
                                 << "hint" << BSON("$natural" << 1) << "sort"
                                 << BSON("$natural" << 1) << "$_requestResumeToken" << true
                                 << "$_resumeAfter" << BSON("$recordId" << 1LL) << "$db"
                                 << "test");

    unique_ptr<FindCommandRequest> findCommand(
        query_request_helper::makeFromFindCommandForTests(cmdObj));
    ASSERT(!findCommand->getResumeAfter().isEmpty());
    ASSERT(findCommand->getRequestResumeToken());
}

TEST(QueryRequestTest, ParseFromCommandEmptyResumeToken) {
    BSONObj resumeAfter = fromjson("{}");
    BSONObj cmdObj =
        BSON("find" << "testns"
                    << "hint" << BSON("$natural" << 1) << "sort" << BSON("$natural" << 1)
                    << "$_requestResumeToken" << true << "$_resumeAfter" << resumeAfter << "$db"
                    << "test");

    unique_ptr<FindCommandRequest> findCommand(
        query_request_helper::makeFromFindCommandForTests(cmdObj));
    ASSERT(findCommand->getRequestResumeToken());
    ASSERT(findCommand->getResumeAfter().isEmpty());
}

//
// Test FindCommandRequest object ns and uuid variants.
//

TEST(QueryRequestTest, AsFindCommandRequestAllNonOptionFields) {
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
                         "readConcern: {level: 'local'}, '$db': 'test'}")
                         .addField(storage["runtimeConstants"]);

    unique_ptr<FindCommandRequest> findCommand(
        query_request_helper::makeFromFindCommandForTests(cmdObj));
    ASSERT_BSONOBJ_EQ_UNORDERED(cmdObj.removeField("$db"), findCommand->toBSON());
}

TEST(QueryRequestTest, AsFindCommandRequestWithUuidAllNonOptionFields) {
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
            "readConcern: {level: 'local'}, '$db': 'test'}")
            .addField(storage["runtimeConstants"]);

    unique_ptr<FindCommandRequest> findCommand(
        query_request_helper::makeFromFindCommandForTests(cmdObj));
    ASSERT_BSONOBJ_EQ_UNORDERED(cmdObj.removeField("$db"), findCommand->toBSON());
}

TEST(QueryRequestTest, AsFindCommandRequestWithUuidNoAvailableNamespace) {
    BSONObj cmdObj =
        fromjson("{find: { \"$binary\" : \"ASNFZ4mrze/ty6mHZUMhAQ==\", \"$type\" : \"04\" }}");
    FindCommandRequest findCommand(
        NamespaceStringOrUUID(DatabaseName::createDatabaseName_forTest(boost::none, "test"),
                              UUID::parse("01234567-89ab-cdef-edcb-a98765432101").getValue()));
    ASSERT_BSONOBJ_EQ(cmdObj.removeField("$db"), findCommand.toBSON());
}

TEST(QueryRequestTest, AsFindCommandRequestWithResumeToken) {
    BSONObj cmdObj = BSON("find" << "testns"
                                 << "sort" << BSON("$natural" << 1) << "hint"
                                 << BSON("$natural" << 1) << "$_requestResumeToken" << true
                                 << "$_resumeAfter" << BSON("$recordId" << 1LL) << "$db"
                                 << "test");

    unique_ptr<FindCommandRequest> findCommand(
        query_request_helper::makeFromFindCommandForTests(cmdObj));
    ASSERT_BSONOBJ_EQ(cmdObj.removeField("$db"), findCommand->toBSON());
}

TEST(QueryRequestTest, AsFindCommandRequestWithEmptyResumeToken) {
    BSONObj resumeAfter = fromjson("{}");
    BSONObj cmdObj =
        BSON("find" << "testns"
                    << "hint" << BSON("$natural" << 1) << "sort" << BSON("$natural" << 1)
                    << "$_requestResumeToken" << true << "$_resumeAfter" << resumeAfter << "$db"
                    << "test");
    unique_ptr<FindCommandRequest> findCommand(
        query_request_helper::makeFromFindCommandForTests(cmdObj));
    ASSERT(findCommand->toBSON().getField("$_resumeAftr").eoo());
}

//
//
// Errors checked in query_request_helper::validateFindCommandRequest().
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

    ASSERT_THROWS_CODE(query_request_helper::makeFromFindCommandForTests(cmdObj),
                       DBException,
                       ErrorCodes::IDLUnknownField);
}

// Older versions of the server supported an "ntoreturn" parameter to the find command, which was
// used for upconversion of legacy OP_QUERY to a find command. Now that OP_QUERY is no longer
// supported, the "ntoreturn" parameter is not supported either, and should be rejected during
// parsing.
TEST(QueryRequestTest, NToReturnParamFailsToParse) {
    BSONObj cmdObj = fromjson("{find: 'testns', '$db': 'test', ntoreturn: 5}");
    ASSERT_THROWS_CODE_AND_WHAT(query_request_helper::makeFromFindCommandForTests(cmdObj),
                                DBException,
                                ErrorCodes::IDLUnknownField,
                                "BSON field 'FindCommandRequest.ntoreturn' is an unknown field.");
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
                       ErrorCodes::InvalidNamespace);
}

TEST(QueryRequestTest, ParseCommandFailsWithInvalidShardVersionField) {
    BSONObj cmdObj = fromjson("{find: 'test.testns', shardVersion: 'foo', '$db': 'test'}");
    ASSERT_THROWS(query_request_helper::makeFromFindCommandForTests(cmdObj), DBException);
}

TEST(QueryRequestTest, DefaultQueryParametersCorrect) {
    BSONObj cmdObj = fromjson("{find: 'testns', '$db': 'test'}");

    std::unique_ptr<FindCommandRequest> findCommand(
        query_request_helper::makeFromFindCommandForTests(cmdObj));

    ASSERT_FALSE(findCommand->getSkip());
    ASSERT_FALSE(findCommand->getLimit());

    ASSERT_FALSE(findCommand->getSingleBatch());
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

    ASSERT_THROWS_CODE(query_request_helper::makeFromFindCommandForTests(cmdObj),
                       DBException,
                       ErrorCodes::IDLUnknownField);
}

TEST(QueryRequestTest, ParseFromCommandForbidExtraOption) {
    BSONObj cmdObj = fromjson(
        "{find: 'testns',"
        "foo: true, '$db': 'test'}");

    ASSERT_THROWS_CODE(query_request_helper::makeFromFindCommandForTests(cmdObj),
                       DBException,
                       ErrorCodes::IDLUnknownField);
}

TEST(QueryRequestTest, ConvertToAggregationSucceeds) {
    FindCommandRequest findCommand(testns);
    auto ar = query_request_conversion::asAggregateCommandRequest(findCommand);

    ASSERT(!ar.getExplain());
    ASSERT(!ar.getAllowDiskUse().has_value());
    ASSERT(ar.getPipeline().empty());
    ASSERT_EQ(ar.getCursor().getBatchSize().value_or(aggregation_request_helper::kDefaultBatchSize),
              aggregation_request_helper::kDefaultBatchSize);
    ASSERT_EQ(ar.getNamespace(), testns);
    ASSERT_BSONOBJ_EQ(ar.getCollation().value_or(BSONObj()), BSONObj());
}

TEST(QueryRequestTest, ConvertToAggregationOmitsExplain) {
    FindCommandRequest findCommand(testns);
    auto ar = query_request_conversion::asAggregateCommandRequest(findCommand);

    ASSERT_FALSE(ar.getExplain());
    ASSERT(ar.getPipeline().empty());
    ASSERT_EQ(ar.getNamespace(), testns);
    ASSERT_BSONOBJ_EQ(ar.getCollation().value_or(BSONObj()), BSONObj());
}

TEST(QueryRequestTest, ConvertToAggregationWithHintSucceeds) {
    FindCommandRequest findCommand(testns);
    findCommand.setHint(fromjson("{a_1: -1}"));
    auto ar = query_request_conversion::asAggregateCommandRequest(findCommand);

    ASSERT_BSONOBJ_EQ(findCommand.getHint(), ar.getHint().value_or(BSONObj()));
}

TEST(QueryRequestTest, ConvertToAggregationWithMinFails) {
    FindCommandRequest findCommand(testns);
    findCommand.setMin(fromjson("{a: 1}"));
    ASSERT_THROWS_CODE(query_request_conversion::asAggregateCommandRequest(findCommand),
                       DBException,
                       ErrorCodes::InvalidPipelineOperator);
}

TEST(QueryRequestTest, ConvertToAggregationWithMaxFails) {
    FindCommandRequest findCommand(testns);
    findCommand.setMax(fromjson("{a: 1}"));
    ASSERT_THROWS_CODE(query_request_conversion::asAggregateCommandRequest(findCommand),
                       DBException,
                       ErrorCodes::InvalidPipelineOperator);
}

TEST(QueryRequestTest, ConvertToAggregationWithSingleBatchFieldFails) {
    FindCommandRequest findCommand(testns);
    findCommand.setSingleBatch(true);
    ASSERT_THROWS_CODE(query_request_conversion::asAggregateCommandRequest(findCommand),
                       DBException,
                       ErrorCodes::InvalidPipelineOperator);
}

TEST(QueryRequestTest, ConvertToAggregationWithSingleBatchFieldAndLimitFails) {
    FindCommandRequest findCommand(testns);
    findCommand.setSingleBatch(true);
    findCommand.setLimit(7);
    ASSERT_THROWS_CODE(query_request_conversion::asAggregateCommandRequest(findCommand),
                       DBException,
                       ErrorCodes::InvalidPipelineOperator);
}

TEST(QueryRequestTest, ConvertToAggregationWithSingleBatchFieldLimitOneSucceeds) {
    FindCommandRequest findCommand(testns);
    findCommand.setSingleBatch(true);
    findCommand.setLimit(1);

    ASSERT_DOES_NOT_THROW(query_request_conversion::asAggregateCommandRequest(findCommand));
}

TEST(QueryRequestTest, ConvertToAggregationWithReturnKeyFails) {
    FindCommandRequest findCommand(testns);
    findCommand.setReturnKey(true);
    ASSERT_THROWS_CODE(query_request_conversion::asAggregateCommandRequest(findCommand),
                       DBException,
                       ErrorCodes::InvalidPipelineOperator);
}

TEST(QueryRequestTest, ConvertToAggregationWithShowRecordIdFails) {
    FindCommandRequest findCommand(testns);
    findCommand.setShowRecordId(true);
    ASSERT_THROWS_CODE(query_request_conversion::asAggregateCommandRequest(findCommand),
                       DBException,
                       ErrorCodes::InvalidPipelineOperator);
}

TEST(QueryRequestTest, ConvertToAggregationWithTailableFails) {
    FindCommandRequest findCommand(testns);
    query_request_helper::setTailableMode(TailableModeEnum::kTailable, &findCommand);
    ASSERT_THROWS_CODE(query_request_conversion::asAggregateCommandRequest(findCommand),
                       DBException,
                       ErrorCodes::InvalidPipelineOperator);
}

TEST(QueryRequestTest, ConvertToAggregationWithNoCursorTimeoutFails) {
    FindCommandRequest findCommand(testns);
    findCommand.setNoCursorTimeout(true);
    ASSERT_THROWS_CODE(query_request_conversion::asAggregateCommandRequest(findCommand),
                       DBException,
                       ErrorCodes::InvalidPipelineOperator);
}

TEST(QueryRequestTest, ConvertToAggregationWithAwaitDataFails) {
    FindCommandRequest findCommand(testns);
    query_request_helper::setTailableMode(TailableModeEnum::kTailableAndAwaitData, &findCommand);
    ASSERT_THROWS_CODE(query_request_conversion::asAggregateCommandRequest(findCommand),
                       DBException,
                       ErrorCodes::InvalidPipelineOperator);
}

TEST(QueryRequestTest, ConvertToAggregationWithAllowPartialResultsFails) {
    FindCommandRequest findCommand(testns);
    findCommand.setAllowPartialResults(true);
    ASSERT_THROWS_CODE(query_request_conversion::asAggregateCommandRequest(findCommand),
                       DBException,
                       ErrorCodes::InvalidPipelineOperator);
}

TEST(QueryRequestTest, ConvertToAggregationWithRequestResumeTokenSucceeds) {
    FindCommandRequest findCommand(testns);
    findCommand.setRequestResumeToken(true);
    findCommand.setHint(BSON("$natural" << 1));

    auto ar = query_request_conversion::asAggregateCommandRequest(findCommand);

    ASSERT(ar.getRequestResumeToken());
}

TEST(QueryRequestTest, ConvertToAggregationWithPipeline) {
    FindCommandRequest findCommand(testns);
    findCommand.setFilter(BSON("x" << 1));
    findCommand.setSort(BSON("y" << -1));
    findCommand.setLimit(3);
    findCommand.setSkip(7);
    findCommand.setProjection(BSON("z" << 0));

    auto ar = query_request_conversion::asAggregateCommandRequest(findCommand);

    ASSERT(!ar.getExplain());
    ASSERT_EQ(ar.getCursor().getBatchSize().value_or(aggregation_request_helper::kDefaultBatchSize),
              aggregation_request_helper::kDefaultBatchSize);
    ASSERT_EQ(ar.getNamespace(), testns);
    ASSERT_BSONOBJ_EQ(ar.getCollation().value_or(BSONObj()), BSONObj());

    std::vector<BSONObj> expectedPipeline{BSON("$match" << BSON("x" << 1)),
                                          BSON("$sort" << BSON("y" << -1)),
                                          BSON("$skip" << 7),
                                          BSON("$limit" << 3),
                                          BSON("$project" << BSON("z" << 0))};
    ASSERT(std::equal(expectedPipeline.begin(),
                      expectedPipeline.end(),
                      ar.getPipeline().begin(),
                      SimpleBSONObjComparator::kInstance.makeEqualTo()));
}

TEST(QueryRequestTest, ConvertToAggregationWithBatchSize) {
    FindCommandRequest findCommand(testns);
    findCommand.setBatchSize(4);

    auto ar = query_request_conversion::asAggregateCommandRequest(findCommand);

    ASSERT(!ar.getExplain());
    ASSERT_EQ(ar.getNamespace(), testns);
    ASSERT_EQ(ar.getCursor().getBatchSize().value_or(aggregation_request_helper::kDefaultBatchSize),
              4LL);
    ASSERT_BSONOBJ_EQ(ar.getCollation().value_or(BSONObj()), BSONObj());
}

TEST(QueryRequestTest, ConvertToAggregationWithMaxTimeMS) {
    FindCommandRequest findCommand(testns);
    findCommand.setMaxTimeMS(9);

    auto ar = query_request_conversion::asAggregateCommandRequest(findCommand);

    ASSERT_TRUE(ar.getMaxTimeMS().has_value());
    ASSERT_EQ(ar.getMaxTimeMS().get(), 9);

    ASSERT(!ar.getExplain());
    ASSERT_EQ(ar.getCursor().getBatchSize().value_or(aggregation_request_helper::kDefaultBatchSize),
              aggregation_request_helper::kDefaultBatchSize);
    ASSERT_EQ(ar.getNamespace(), testns);
    ASSERT_BSONOBJ_EQ(ar.getCollation().value_or(BSONObj()), BSONObj());
}

TEST(QueryRequestTest, ConvertToAggregationWithCollationSucceeds) {
    FindCommandRequest findCommand(testns);
    findCommand.setCollation(BSON("f" << 1));
    auto ar = query_request_conversion::asAggregateCommandRequest(findCommand);

    ASSERT(!ar.getExplain());
    ASSERT(ar.getPipeline().empty());
    ASSERT_EQ(ar.getCursor().getBatchSize().value_or(aggregation_request_helper::kDefaultBatchSize),
              aggregation_request_helper::kDefaultBatchSize);
    ASSERT_EQ(ar.getNamespace(), testns);
    ASSERT_BSONOBJ_EQ(ar.getCollation().value_or(BSONObj()), BSON("f" << 1));
}

TEST(QueryRequestTest, ConvertToAggregationWithReadOnceFails) {
    FindCommandRequest findCommand(testns);
    findCommand.setReadOnce(true);
    ASSERT_THROWS_CODE(query_request_conversion::asAggregateCommandRequest(findCommand),
                       DBException,
                       ErrorCodes::InvalidPipelineOperator);
}

TEST(QueryRequestTest, ConvertToAggregationWithLegacyRuntimeConstantsSucceeds) {
    LegacyRuntimeConstants rtc{Date_t::now(), Timestamp(1, 1)};
    FindCommandRequest findCommand(testns);
    findCommand.setLegacyRuntimeConstants(rtc);
    auto ar = query_request_conversion::asAggregateCommandRequest(findCommand);

    ASSERT(ar.getLegacyRuntimeConstants().has_value());
    ASSERT_EQ(ar.getLegacyRuntimeConstants()->getLocalNow(), rtc.getLocalNow());
    ASSERT_EQ(ar.getLegacyRuntimeConstants()->getClusterTime(), rtc.getClusterTime());
}

TEST(QueryRequestTest, ConvertToAggregationWithAllowDiskUseTrueSucceeds) {
    FindCommandRequest findCommand(testns);
    findCommand.setAllowDiskUse(true);
    auto ar = query_request_conversion::asAggregateCommandRequest(findCommand);

    ASSERT(ar.getAllowDiskUse().has_value());
    ASSERT_EQ(true, ar.getAllowDiskUse());
}

TEST(QueryRequestTest, ConvertToAggregationWithAllowDiskUseFalseSucceeds) {
    FindCommandRequest findCommand(testns);
    findCommand.setAllowDiskUse(false);
    auto ar = query_request_conversion::asAggregateCommandRequest(findCommand);

    ASSERT(ar.getAllowDiskUse().has_value());
    ASSERT_EQ(false, ar.getAllowDiskUse());
}

TEST(QueryRequestTest, ConvertToAggregationWithIncludeQueryStatsMetricsTrueSucceeds) {
    FindCommandRequest findCommand(testns);
    findCommand.setIncludeQueryStatsMetrics(true);
    auto ar = query_request_conversion::asAggregateCommandRequest(findCommand);

    ASSERT_TRUE(ar.getIncludeQueryStatsMetrics());
}

TEST(QueryRequestTest, ConvertToAggregationWithIncludeQueryStatsMetricsFalseSucceeds) {
    FindCommandRequest findCommand(testns);
    findCommand.setIncludeQueryStatsMetrics(false);
    auto ar = query_request_conversion::asAggregateCommandRequest(findCommand);

    ASSERT_FALSE(ar.getIncludeQueryStatsMetrics());
}

TEST(QueryRequestTest, ConvertToFindWithAllowDiskUseTrueSucceeds) {
    FindCommandRequest findCommand(testns);
    findCommand.setAllowDiskUse(true);
    const auto findCmd = findCommand.toBSON();

    BSONElement elem = findCmd[FindCommandRequest::kAllowDiskUseFieldName];
    ASSERT_EQ(true, elem.isBoolean());
    ASSERT_EQ(true, elem.Bool());
}

TEST(QueryRequestTest, ConvertToFindWithAllowDiskUseFalseSucceeds) {
    FindCommandRequest findCommand(testns);
    findCommand.setAllowDiskUse(false);
    const auto findCmd = findCommand.toBSON();

    ASSERT_FALSE(findCmd[FindCommandRequest::kAllowDiskUseFieldName].booleanSafe());
}

TEST(QueryRequestHelperTest, ValidateResponseMissingFields) {
    BSONObjBuilder builder;
    QueryTestServiceContext serviceContext;
    auto uniqueTxn = serviceContext.makeOperationContext();
    OperationContext* opCtx = uniqueTxn.get();
    ASSERT_THROWS_CODE(
        query_request_helper::validateCursorResponse(builder.asTempObj(),
                                                     auth::ValidatedTenancyScope::get(opCtx),
                                                     boost::none,
                                                     SerializationContext()),
        DBException,
        6253507);
}

TEST(QueryRequestHelperTest, ValidateResponseWrongDataType) {
    BSONObjBuilder builder;
    builder.append("cursor", 1);
    QueryTestServiceContext serviceContext;
    auto uniqueTxn = serviceContext.makeOperationContext();
    OperationContext* opCtx = uniqueTxn.get();
    ASSERT_THROWS_CODE(
        query_request_helper::validateCursorResponse(builder.asTempObj(),
                                                     auth::ValidatedTenancyScope::get(opCtx),
                                                     boost::none,
                                                     SerializationContext()),
        DBException,
        ErrorCodes::TypeMismatch);
}

TEST(QueryRequestHelperTest, ParsedCursorRemainsValidAfterBSONDestroyed) {
    std::vector<BSONObj> batch = {BSON("_id" << 1), BSON("_id" << 2)};
    CursorInitialReply cir;
    {
        BSONObj cursorObj =
            BSON("cursor" << BSON("id" << CursorId(123) << "ns"
                                       << "testdb.testcoll"
                                       << "firstBatch"
                                       << BSON_ARRAY(BSON("_id" << 1) << BSON("_id" << 2))));
        cir = CursorInitialReply::parseOwned(
            std::move(cursorObj),
            IDLParserContext("QueryRequestHelperTest::ParsedCursorRemainsValidAFterBSONDestroyed"));
        cursorObj = BSONObj();
    }
    ASSERT_EQ(cir.getCursor()->getFirstBatch().size(), batch.size());
    for (std::vector<BSONObj>::size_type i = 0; i < batch.size(); ++i) {
        ASSERT_BSONOBJ_EQ(batch[i], cir.getCursor()->getFirstBatch()[i]);
    }
}

class ServiceQueryRequestTest : public ServiceContextTest {};

TEST_F(ServiceQueryRequestTest, ParseFromUUID) {
    const UUID uuid = UUID::gen();
    NamespaceStringOrUUID nssOrUUID(DatabaseName::createDatabaseName_forTest(boost::none, "test"),
                                    uuid);
    FindCommandRequest findCommand(nssOrUUID);
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.testns");
    findCommand.setNss(nss);
    ASSERT_EQ(nss, findCommand.getNamespaceOrUUID().nss());
}

}  // namespace
}  // namespace mongo
