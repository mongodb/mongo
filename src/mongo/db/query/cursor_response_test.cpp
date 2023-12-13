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

#include <initializer_list>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/oid.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/op_msg_rpc_impls.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/namespace_string_util.h"

namespace mongo {

namespace {

static const BSONObj basicMetricsObj = fromjson(R"({
    keysExamined: {"$numberLong": "1"},
    docsExamined: {"$numberLong": "2"},
    hasSortStage: true,
    usedDisk: true,
    fromMultiPlanner: true,
    fromPlanCache: true
})");

BSONObj makeResponseBSON(const BSONObj& cursor) {
    return BSON("ok" << 1 << "cursor" << cursor);
}

BSONObj makeCursorBSON() {
    return BSON("id" << CursorId(123) << "ns"
                     << "db.coll"
                     << "firstBatch" << BSONArray());
}

BSONObj makeCursorBSON(const BSONObj& metrics) {
    auto cursor = makeCursorBSON();
    BSONObjBuilder cursorBuilder(std::move(cursor));
    cursorBuilder << "metrics" << metrics;
    return cursorBuilder.obj();
}

TEST(CursorResponseTest, parseFromBSONFirstBatch) {
    StatusWith<CursorResponse> result = CursorResponse::parseFromBSON(BSON(
        "cursor" << BSON("id" << CursorId(123) << "ns"
                              << "db.coll"
                              << "firstBatch" << BSON_ARRAY(BSON("_id" << 1) << BSON("_id" << 2)))
                 << "ok" << 1));
    ASSERT_OK(result.getStatus());

    CursorResponse response = std::move(result.getValue());
    ASSERT_EQ(response.getCursorId(), CursorId(123));
    ASSERT_EQ(response.getNSS().ns_forTest(), "db.coll");
    ASSERT_EQ(response.getBatch().size(), 2U);
    ASSERT_EQ(response.getCursorMetrics(), boost::none);
    ASSERT_BSONOBJ_EQ(response.getBatch()[0], BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(response.getBatch()[1], BSON("_id" << 2));
}

TEST(CursorResponseTest, parseFromBSONNextBatch) {
    StatusWith<CursorResponse> result = CursorResponse::parseFromBSON(BSON(
        "cursor" << BSON("id" << CursorId(123) << "ns"
                              << "db.coll"
                              << "nextBatch" << BSON_ARRAY(BSON("_id" << 1) << BSON("_id" << 2)))
                 << "ok" << 1));
    ASSERT_OK(result.getStatus());

    CursorResponse response = std::move(result.getValue());
    ASSERT_EQ(response.getCursorId(), CursorId(123));
    ASSERT_EQ(response.getNSS().ns_forTest(), "db.coll");
    ASSERT_EQ(response.getBatch().size(), 2U);
    ASSERT_EQ(response.getCursorMetrics(), boost::none);
    ASSERT_BSONOBJ_EQ(response.getBatch()[0], BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(response.getBatch()[1], BSON("_id" << 2));
}

TEST(CursorResponseTest, parseFromBSONCursorIdZero) {
    StatusWith<CursorResponse> result = CursorResponse::parseFromBSON(BSON(
        "cursor" << BSON("id" << CursorId(0) << "ns"
                              << "db.coll"
                              << "nextBatch" << BSON_ARRAY(BSON("_id" << 1) << BSON("_id" << 2)))
                 << "ok" << 1));
    ASSERT_OK(result.getStatus());

    CursorResponse response = std::move(result.getValue());
    ASSERT_EQ(response.getCursorId(), CursorId(0));
    ASSERT_EQ(response.getNSS().ns_forTest(), "db.coll");
    ASSERT_EQ(response.getBatch().size(), 2U);
    ASSERT_BSONOBJ_EQ(response.getBatch()[0], BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(response.getBatch()[1], BSON("_id" << 2));
}

TEST(CursorResponseTest, parseFromBSONEmptyBatch) {
    StatusWith<CursorResponse> result = CursorResponse::parseFromBSON(
        BSON("cursor" << BSON("id" << CursorId(123) << "ns"
                                   << "db.coll"
                                   << "nextBatch" << BSONArrayBuilder().arr())
                      << "ok" << 1));
    ASSERT_OK(result.getStatus());

    CursorResponse response = std::move(result.getValue());
    ASSERT_EQ(response.getCursorId(), CursorId(123));
    ASSERT_EQ(response.getNSS().ns_forTest(), "db.coll");
    ASSERT_EQ(response.getBatch().size(), 0U);
}

TEST(CursorResponseTest, parseFromBSONMissingCursorField) {
    StatusWith<CursorResponse> result = CursorResponse::parseFromBSON(BSON("ok" << 1));
    ASSERT_NOT_OK(result.getStatus());
}

TEST(CursorResponseTest, parseFromBSONCursorFieldWrongType) {
    StatusWith<CursorResponse> result =
        CursorResponse::parseFromBSON(BSON("cursor" << 3 << "ok" << 1));
    ASSERT_NOT_OK(result.getStatus());
}

TEST(CursorResponseTest, parseFromBSONNsFieldMissing) {
    StatusWith<CursorResponse> result = CursorResponse::parseFromBSON(
        BSON("cursor" << BSON("id" << CursorId(123) << "firstBatch"
                                   << BSON_ARRAY(BSON("_id" << 1) << BSON("_id" << 2)))
                      << "ok" << 1));
    ASSERT_NOT_OK(result.getStatus());
}

TEST(CursorResponseTest, parseFromBSONNsFieldWrongType) {
    StatusWith<CursorResponse> result = CursorResponse::parseFromBSON(
        BSON("cursor" << BSON("id" << CursorId(123) << "ns" << 456 << "firstBatch"
                                   << BSON_ARRAY(BSON("_id" << 1) << BSON("_id" << 2)))
                      << "ok" << 1));
    ASSERT_NOT_OK(result.getStatus());
}

TEST(CursorResponseTest, parseFromBSONIdFieldMissing) {
    StatusWith<CursorResponse> result = CursorResponse::parseFromBSON(
        BSON("cursor" << BSON("ns"
                              << "db.coll"
                              << "nextBatch" << BSON_ARRAY(BSON("_id" << 1) << BSON("_id" << 2)))
                      << "ok" << 1));
    ASSERT_NOT_OK(result.getStatus());
}

TEST(CursorResponseTest, parseFromBSONIdFieldWrongType) {
    StatusWith<CursorResponse> result = CursorResponse::parseFromBSON(
        BSON("cursor" << BSON("id"
                              << "123"
                              << "ns"
                              << "db.coll"
                              << "nextBatch" << BSON_ARRAY(BSON("_id" << 1) << BSON("_id" << 2)))
                      << "ok" << 1));
    ASSERT_NOT_OK(result.getStatus());
}

TEST(CursorResponseTest, parseFromBSONBatchFieldMissing) {
    StatusWith<CursorResponse> result =
        CursorResponse::parseFromBSON(BSON("cursor" << BSON("id" << CursorId(123) << "ns"
                                                                 << "db.coll")
                                                    << "ok" << 1));
    ASSERT_NOT_OK(result.getStatus());
}

TEST(CursorResponseTest, parseFromBSONFirstBatchFieldWrongType) {
    StatusWith<CursorResponse> result = CursorResponse::parseFromBSON(
        BSON("cursor" << BSON("id" << CursorId(123) << "ns"
                                   << "db.coll"
                                   << "firstBatch" << BSON("_id" << 1))
                      << "ok" << 1));
    ASSERT_NOT_OK(result.getStatus());
}

TEST(CursorResponseTest, parseFromBSONNextBatchFieldWrongType) {
    StatusWith<CursorResponse> result =
        CursorResponse::parseFromBSON(BSON("cursor" << BSON("id" << CursorId(123) << "ns"
                                                                 << "db.coll"
                                                                 << "nextBatch" << BSON("_id" << 1))
                                                    << "ok" << 1));
    ASSERT_NOT_OK(result.getStatus());
}

TEST(CursorResponseTest, parseFromBSONOkFieldMissing) {
    StatusWith<CursorResponse> result = CursorResponse::parseFromBSON(BSON(
        "cursor" << BSON("id" << CursorId(123) << "ns"
                              << "db.coll"
                              << "nextBatch" << BSON_ARRAY(BSON("_id" << 1) << BSON("_id" << 2)))));
    ASSERT_NOT_OK(result.getStatus());
}

TEST(CursorResponseTest, parseFromBSONPartialResultsReturnedField) {
    StatusWith<CursorResponse> result = CursorResponse::parseFromBSON(BSON(
        "cursor" << BSON("id" << CursorId(123) << "ns"
                              << "db.coll"
                              << "firstBatch" << BSON_ARRAY(BSON("_id" << 1) << BSON("_id" << 2))
                              << "partialResultsReturned" << true)
                 << "ok" << 1));
    ASSERT_OK(result.getStatus());

    CursorResponse response = std::move(result.getValue());
    ASSERT_EQ(response.getCursorId(), CursorId(123));
    ASSERT_EQ(response.getNSS().ns_forTest(), "db.coll");
    ASSERT_EQ(response.getBatch().size(), 2U);
    ASSERT_BSONOBJ_EQ(response.getBatch()[0], BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(response.getBatch()[1], BSON("_id" << 2));
    ASSERT_EQ(response.getPartialResultsReturned(), true);
}

TEST(CursorResponseTest, parseFromBSONPartialResultsReturnedFieldWrongType) {
    StatusWith<CursorResponse> result = CursorResponse::parseFromBSON(BSON(
        "cursor" << BSON("id" << CursorId(123) << "ns"
                              << "db.coll"
                              << "firstBatch" << BSON_ARRAY(BSON("_id" << 1) << BSON("_id" << 2))
                              << "partialResultsReturned" << 1)
                 << "ok" << 1));
    ASSERT_NOT_OK(result.getStatus());
}

TEST(CursorResponseTest, parseFromBSONVarsFieldCorrect) {
    BSONObj varsContents = BSON("randomVar" << 7);
    StatusWith<CursorResponse> result = CursorResponse::parseFromBSON(BSON(
        "cursor" << BSON("id" << CursorId(123) << "ns"
                              << "db.coll"
                              << "firstBatch" << BSON_ARRAY(BSON("_id" << 1) << BSON("_id" << 2)))
                 << "vars" << varsContents << "ok" << 1));
    ASSERT_OK(result.getStatus());

    CursorResponse response = std::move(result.getValue());
    ASSERT_EQ(response.getCursorId(), CursorId(123));
    ASSERT_EQ(response.getNSS().ns_forTest(), "db.coll");
    ASSERT_EQ(response.getBatch().size(), 2U);
    ASSERT_BSONOBJ_EQ(response.getBatch()[0], BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(response.getBatch()[1], BSON("_id" << 2));
    ASSERT_TRUE(response.getVarsField());
    ASSERT_BSONOBJ_EQ(response.getVarsField().value(), varsContents);
}

TEST(CursorResponseTest, parseFromBSONVarsFieldWrongType) {
    StatusWith<CursorResponse> result = CursorResponse::parseFromBSON(BSON(
        "cursor" << BSON("id" << CursorId(123) << "ns"
                              << "db.coll"
                              << "firstBatch" << BSON_ARRAY(BSON("_id" << 1) << BSON("_id" << 2)))
                 << "vars" << 2 << "ok" << 1));
    ASSERT_NOT_OK(result.getStatus());
}

TEST(CursorResponseTest, parseFromBSONMultipleVars) {
    BSONObj varsContents = BSON("randomVar" << 7 << "otherVar" << BSON("nested" << 2));
    StatusWith<CursorResponse> result = CursorResponse::parseFromBSON(BSON(
        "cursor" << BSON("id" << CursorId(123) << "ns"
                              << "db.coll"
                              << "firstBatch" << BSON_ARRAY(BSON("_id" << 1) << BSON("_id" << 2)))
                 << "vars" << varsContents << "ok" << 1));
    ASSERT_OK(result.getStatus());

    CursorResponse response = std::move(result.getValue());
    ASSERT_EQ(response.getCursorId(), CursorId(123));
    ASSERT_EQ(response.getNSS().ns_forTest(), "db.coll");
    ASSERT_EQ(response.getBatch().size(), 2U);
    ASSERT_BSONOBJ_EQ(response.getBatch()[0], BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(response.getBatch()[1], BSON("_id" << 2));
    ASSERT_TRUE(response.getVarsField());
    ASSERT_BSONOBJ_EQ(response.getVarsField().value(), varsContents);
}

TEST(CursorResponseTest, parseFromBSONCursorMetrics) {
    // Start with a valid cursor and add a valid metrics field.
    auto cursor = makeCursorBSON();

    BSONObjBuilder cursorBuilder(cursor);
    cursorBuilder << "metrics" << fromjson(R"({
        keysExamined: {"$numberLong": "1"},
        docsExamined: {"$numberLong": "2"},
        hasSortStage: true,
        usedDisk: true,
        fromMultiPlanner: true,
        fromPlanCache: true
    })");

    // Check that it parses correctly.
    StatusWith<CursorResponse> result =
        CursorResponse::parseFromBSON(makeResponseBSON(cursorBuilder.obj()));
    ASSERT_OK(result.getStatus());

    CursorResponse response = std::move(result.getValue());
    ASSERT_TRUE(response.getCursorMetrics());

    auto& metrics = *response.getCursorMetrics();

    ASSERT_EQ(metrics.getKeysExamined(), 1);
    ASSERT_EQ(metrics.getDocsExamined(), 2);
    ASSERT_TRUE(metrics.getHasSortStage());
    ASSERT_TRUE(metrics.getUsedDisk());
    ASSERT_TRUE(metrics.getFromMultiPlanner());
    ASSERT_TRUE(metrics.getFromPlanCache());
}

TEST(CursorResponseTest, parseFromBSONCursorMetricsWrongType) {
    // Start with a valid cursor
    auto cursor = makeCursorBSON();

    // Check that the baseline is valid
    ASSERT_OK(CursorResponse::parseFromBSON(makeResponseBSON(cursor)));

    // Add an invalid metrics field, check the result is an error
    auto cursorBuilder = BSONObjBuilder(std::move(cursor));
    cursorBuilder << "metrics"
                  << "should be an object";
    auto badCursor = cursorBuilder.obj();

    ASSERT_NOT_OK(CursorResponse::parseFromBSON(makeResponseBSON(badCursor)));
}

TEST(CursorResponseTest, parseFromBSONCursorMetricsIncomplete) {
    // Start with a valid cursor
    auto metrics = basicMetricsObj;
    auto cursor = makeCursorBSON(metrics);

    // Check that the baseline is valid
    ASSERT_OK(CursorResponse::parseFromBSON(makeResponseBSON(cursor)));

    // Remove each mandatory field and then check that the result is invalid.
    std::vector<StringData> fields{CursorMetrics::kKeysExaminedFieldName,
                                   CursorMetrics::kDocsExaminedFieldName,
                                   CursorMetrics::kHasSortStageFieldName,
                                   CursorMetrics::kUsedDiskFieldName,
                                   CursorMetrics::kFromMultiPlannerFieldName,
                                   CursorMetrics::kFromPlanCacheFieldName};
    for (auto fieldName : fields) {
        auto badMetrics = metrics.copy().removeField(fieldName);
        auto badCursor = makeCursorBSON(badMetrics);
        auto badResponse = makeResponseBSON(badCursor);
        ASSERT_NOT_OK(CursorResponse::parseFromBSON(badResponse));
    }
}

TEST(CursorResponseTest, parseFromBSONCursorMetricsToleratesUnknownFields) {
    // Start with a valid cursor
    auto metrics = basicMetricsObj.copy();
    auto cursor = makeCursorBSON(metrics);

    // Check that the baseline is valid
    ASSERT_OK(CursorResponse::parseFromBSON(makeResponseBSON(cursor)));

    // Add a field to the cursor
    auto badMetrics = (BSONObjBuilder(std::move(metrics)) << "notAField" << 7).obj();
    auto badResponse = makeResponseBSON(makeCursorBSON(badMetrics));
    ASSERT_OK(CursorResponse::parseFromBSON(badResponse));
}

TEST(CursorResponseTest, parseFromBSONToleratesUnknownFields) {
    StatusWith<CursorResponse> result =
        CursorResponse::parseFromBSON(BSON("cursor" << BSON("id" << CursorId(123) << "ns"
                                                                 << "db.coll"
                                                                 << "firstBatch" << BSONArray())
                                                    << "ok" << 1 << "otherField"
                                                    << "value"));
    ASSERT_OK(result.getStatus());

    CursorResponse response = std::move(result.getValue());
    ASSERT_EQ(response.getCursorId(), CursorId(123));
    ASSERT_EQ(response.getNSS().ns_forTest(), "db.coll");
    ASSERT_EQ(response.getBatch().size(), 0U);
}

TEST(CursorResponseTest, roundTripThroughCursorResponseBuilderWithPartialResultsReturned) {
    CursorResponseBuilder::Options options;
    options.isInitialResponse = true;
    rpc::OpMsgReplyBuilder builder;
    BSONObj okStatus = BSON("ok" << 1);
    BSONObj testDoc = BSON("_id" << 1);
    BSONObj expectedBody =
        BSON("cursor" << BSON("firstBatch" << BSON_ARRAY(testDoc) << "partialResultsReturned"
                                           << true << "id" << CursorId(123) << "ns"
                                           << "db.coll"));

    // Use CursorResponseBuilder to serialize the cursor response to OpMsgReplyBuilder.
    CursorResponseBuilder crb(&builder, options);
    crb.append(testDoc);
    crb.setPartialResultsReturned(true);
    crb.done(CursorId(123),
             NamespaceString::createNamespaceString_forTest(boost::none, "db.coll"),
             SerializationContext::stateCommandReply());

    // Confirm that the resulting BSONObj response matches the expected body.
    auto msg = builder.done();
    auto opMsg = OpMsg::parse(msg);
    ASSERT_BSONOBJ_EQ(expectedBody, opMsg.body);

    // Append {"ok": 1} to the opMsg body so that it can be parsed by CursorResponse.
    auto swCursorResponse = CursorResponse::parseFromBSON(opMsg.body.addField(okStatus["ok"]));
    ASSERT_OK(swCursorResponse.getStatus());

    // Confirm the CursorReponse parsed from CursorResponseBuilder output has the correct content.
    CursorResponse response = std::move(swCursorResponse.getValue());
    ASSERT_EQ(response.getCursorId(), CursorId(123));
    ASSERT_EQ(response.getNSS().ns_forTest(), "db.coll");
    ASSERT_EQ(response.getBatch().size(), 1U);
    ASSERT_BSONOBJ_EQ(response.getBatch()[0], testDoc);
    ASSERT_EQ(response.getPartialResultsReturned(), true);

    // Re-serialize a BSONObj response from the CursorResponse.
    auto cursorResBSON = response.toBSONAsInitialResponse();

    // Confirm that the BSON serialized by the CursorResponse is the same as that serialized by the
    // CursorResponseBuilder. Field ordering differs between the two, so compare per-element.
    BSONObjIteratorSorted cursorResIt(cursorResBSON["cursor"].Obj());
    BSONObjIteratorSorted cursorBuilderIt(opMsg.body["cursor"].Obj());
    while (cursorResIt.more()) {
        ASSERT(cursorBuilderIt.more());
        ASSERT_EQ(cursorResIt.next().woCompare(cursorBuilderIt.next()), 0);
    }
    ASSERT(!cursorBuilderIt.more());
}

TEST(CursorResponseTest,
     roundTripThroughCursorResponseBuilderWithPartialResultsReturnedWithTenantId) {
    CursorResponseBuilder::Options options;
    options.isInitialResponse = true;
    TenantId tid(OID::gen());
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(tid, "db.coll");

    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);

    for (bool flagStatus : {false, true}) {
        rpc::OpMsgReplyBuilder builder;
        BSONObj okStatus = BSON("ok" << 1);
        BSONObj testDoc = BSON("_id" << 1);
        auto scReply = SerializationContext::stateCommandReply();
        scReply.setTenantIdSource(flagStatus /*nonPrefixedTenantId*/);

        BSONObj expectedBody =
            BSON("cursor" << BSON("firstBatch" << BSON_ARRAY(testDoc) << "partialResultsReturned"
                                               << true << "id" << CursorId(123) << "ns"
                                               << NamespaceStringUtil::serialize(nss, scReply)));

        // Use CursorResponseBuilder to serialize the cursor response to OpMsgReplyBuilder.
        CursorResponseBuilder crb(&builder, options);
        crb.append(testDoc);
        crb.setPartialResultsReturned(true);
        crb.done(CursorId(123), nss, scReply);

        // Confirm that the resulting BSONObj response matches the expected body.
        auto msg = builder.done();
        auto opMsg = OpMsg::parse(msg);
        ASSERT_BSONOBJ_EQ(expectedBody, opMsg.body);

        // Append {"ok": 1} to the opMsg body so that it can be parsed by CursorResponse.
        auto swCursorResponse =
            CursorResponse::parseFromBSON(opMsg.body.addField(okStatus["ok"]), nullptr, tid);
        ASSERT_OK(swCursorResponse.getStatus());

        // Confirm the CursorReponse parsed from CursorResponseBuilder output has the correct
        // content.
        CursorResponse response = std::move(swCursorResponse.getValue());
        ASSERT_EQ(response.getCursorId(), CursorId(123));

        ASSERT_EQ(response.getNSS(), nss);
        ASSERT_EQ(response.getBatch().size(), 1U);
        ASSERT_BSONOBJ_EQ(response.getBatch()[0], testDoc);
        ASSERT_EQ(response.getPartialResultsReturned(), true);

        // Re-serialize a BSONObj response from the CursorResponse.
        auto cursorResBSON = response.toBSONAsInitialResponse(scReply);

        // Confirm that the BSON serialized by the CursorResponse is the same as that serialized by
        // the CursorResponseBuilder. Field ordering differs between the two, so compare
        // per-element.
        BSONObjIteratorSorted cursorResIt(cursorResBSON["cursor"].Obj());
        BSONObjIteratorSorted cursorBuilderIt(opMsg.body["cursor"].Obj());
        while (cursorResIt.more()) {
            ASSERT(cursorBuilderIt.more());
            ASSERT_EQ(cursorResIt.next().woCompare(cursorBuilderIt.next()), 0);
        }
        ASSERT(!cursorBuilderIt.more());
    }
}


TEST(CursorResponseTest, parseFromBSONHandleErrorResponse) {
    StatusWith<CursorResponse> result =
        CursorResponse::parseFromBSON(BSON("ok" << 0 << "code" << 123 << "errmsg"
                                                << "does not work"));
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQ(result.getStatus().code(), 123);
    ASSERT_EQ(result.getStatus().reason(), "does not work");
}

TEST(CursorResponseTest, toBSONInitialResponse) {
    std::vector<BSONObj> batch = {BSON("_id" << 1), BSON("_id" << 2)};
    CursorResponse response(
        NamespaceString::createNamespaceString_forTest("testdb.testcoll"), CursorId(123), batch);
    BSONObj responseObj = response.toBSON(CursorResponse::ResponseType::InitialResponse);
    BSONObj expectedResponse = BSON(
        "cursor" << BSON("id" << CursorId(123) << "ns"
                              << "testdb.testcoll"
                              << "firstBatch" << BSON_ARRAY(BSON("_id" << 1) << BSON("_id" << 2)))
                 << "ok" << 1.0);
    ASSERT_BSONOBJ_EQ(responseObj, expectedResponse);
}

TEST(CursorResponseTest, toBSONSubsequentResponse) {
    std::vector<BSONObj> batch = {BSON("_id" << 1), BSON("_id" << 2)};
    CursorResponse response(
        NamespaceString::createNamespaceString_forTest("testdb.testcoll"), CursorId(123), batch);
    BSONObj responseObj = response.toBSON(CursorResponse::ResponseType::SubsequentResponse);
    BSONObj expectedResponse = BSON(
        "cursor" << BSON("id" << CursorId(123) << "ns"
                              << "testdb.testcoll"
                              << "nextBatch" << BSON_ARRAY(BSON("_id" << 1) << BSON("_id" << 2)))
                 << "ok" << 1.0);
    ASSERT_BSONOBJ_EQ(responseObj, expectedResponse);
}

TEST(CursorResponseTest, toBSONPartialResultsReturned) {
    std::vector<BSONObj> batch = {BSON("_id" << 1), BSON("_id" << 2)};
    CursorResponse response(NamespaceString::createNamespaceString_forTest("testdb.testcoll"),
                            CursorId(123),
                            batch,
                            boost::none,
                            boost::none,
                            boost::none,
                            boost::none,
                            boost::none,
                            boost::none,
                            true);
    BSONObj responseObj = response.toBSON(CursorResponse::ResponseType::InitialResponse);
    BSONObj expectedResponse = BSON(
        "cursor" << BSON("id" << CursorId(123) << "ns"
                              << "testdb.testcoll"
                              << "firstBatch" << BSON_ARRAY(BSON("_id" << 1) << BSON("_id" << 2))
                              << "partialResultsReturned" << true)
                 << "ok" << 1.0);
    ASSERT_BSONOBJ_EQ(responseObj, expectedResponse);
}

TEST(CursorResponseTest, addToBSONInitialResponse) {
    std::vector<BSONObj> batch = {BSON("_id" << 1), BSON("_id" << 2)};
    CursorResponse response(
        NamespaceString::createNamespaceString_forTest("testdb.testcoll"), CursorId(123), batch);

    BSONObjBuilder builder;
    response.addToBSON(CursorResponse::ResponseType::InitialResponse, &builder);
    BSONObj responseObj = builder.obj();

    BSONObj expectedResponse = BSON(
        "cursor" << BSON("id" << CursorId(123) << "ns"
                              << "testdb.testcoll"
                              << "firstBatch" << BSON_ARRAY(BSON("_id" << 1) << BSON("_id" << 2)))
                 << "ok" << 1.0);
    ASSERT_BSONOBJ_EQ(responseObj, expectedResponse);
}

TEST(CursorResponseTest, addToBSONSubsequentResponse) {
    std::vector<BSONObj> batch = {BSON("_id" << 1), BSON("_id" << 2)};
    CursorResponse response(
        NamespaceString::createNamespaceString_forTest("testdb.testcoll"), CursorId(123), batch);

    BSONObjBuilder builder;
    response.addToBSON(CursorResponse::ResponseType::SubsequentResponse, &builder);
    BSONObj responseObj = builder.obj();

    BSONObj expectedResponse = BSON(
        "cursor" << BSON("id" << CursorId(123) << "ns"
                              << "testdb.testcoll"
                              << "nextBatch" << BSON_ARRAY(BSON("_id" << 1) << BSON("_id" << 2)))
                 << "ok" << 1.0);
    ASSERT_BSONOBJ_EQ(responseObj, expectedResponse);
}

TEST(CursorResponseTest, addToBSONInitialResponseWithTenantId) {
    TenantId tid(OID::gen());
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(tid, "testdb.testcoll");

    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);

    for (bool flagStatus : {false, true}) {
        RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID",
                                                                   flagStatus);

        std::vector<BSONObj> batch = {BSON("_id" << 1), BSON("_id" << 2)};
        CursorResponse response(nss, CursorId(123), batch);

        BSONObjBuilder builder;
        response.addToBSON(CursorResponse::ResponseType::InitialResponse, &builder);
        BSONObj responseObj = builder.obj();

        BSONObj expectedResponse =
            BSON("cursor" << BSON("id" << CursorId(123) << "ns"
                                       << NamespaceStringUtil::serialize(
                                              nss, SerializationContext::stateDefault())
                                       << "firstBatch"
                                       << BSON_ARRAY(BSON("_id" << 1) << BSON("_id" << 2)))
                          << "ok" << 1.0);
        ASSERT_BSONOBJ_EQ(responseObj, expectedResponse);
    }
}

TEST(CursorResponseTest, serializePostBatchResumeToken) {
    std::vector<BSONObj> batch = {BSON("_id" << 1), BSON("_id" << 2)};
    auto postBatchResumeToken =
        ResumeToken::makeHighWaterMarkToken(Timestamp(1, 2), ResumeTokenData::kDefaultTokenVersion)
            .toDocument()
            .toBson();
    CursorResponse response(NamespaceString::createNamespaceString_forTest("db.coll"),
                            CursorId(123),
                            batch,
                            boost::none,
                            postBatchResumeToken);
    auto serialized = response.toBSON(CursorResponse::ResponseType::SubsequentResponse);
    ASSERT_BSONOBJ_EQ(serialized,
                      BSON("cursor" << BSON("id" << CursorId(123) << "ns"
                                                 << "db.coll"
                                                 << "nextBatch"
                                                 << BSON_ARRAY(BSON("_id" << 1) << BSON("_id" << 2))
                                                 << "postBatchResumeToken" << postBatchResumeToken)
                                    << "ok" << 1));
    auto reparsed = CursorResponse::parseFromBSON(serialized);
    ASSERT_OK(reparsed.getStatus());
    CursorResponse reparsedResponse = std::move(reparsed.getValue());
    ASSERT_EQ(reparsedResponse.getCursorId(), CursorId(123));
    ASSERT_EQ(reparsedResponse.getNSS().ns_forTest(), "db.coll");
    ASSERT_EQ(reparsedResponse.getBatch().size(), 2U);
    ASSERT_BSONOBJ_EQ(*reparsedResponse.getPostBatchResumeToken(), postBatchResumeToken);
}

TEST(CursorResponseTest, parseFromBSONManyBasic) {
    BSONObj cursor1 =
        BSON("id" << CursorId(123) << "ns"
                  << "db.coll"
                  << "firstBatch" << BSON_ARRAY(BSON("_id" << 1) << BSON("_id" << 2)));
    BSONObj cursor2 =
        BSON("id" << CursorId(234) << "ns"
                  << "db.coll"
                  << "firstBatch" << BSON_ARRAY(BSON("_id" << 3) << BSON("_id" << 4)));
    std::vector<StatusWith<CursorResponse>> results = CursorResponse::parseFromBSONMany(
        BSON("cursors" << BSON_ARRAY(BSON("cursor" << cursor1 << "ok" << 1)
                                     << BSON("cursor" << cursor2 << "ok" << 1))));

    for (auto& result : results) {
        ASSERT_OK(result);
    }

    ASSERT_EQ(results.size(), 2);

    CursorResponse response1 = std::move(results[0].getValue());
    ASSERT_EQ(response1.getCursorId(), CursorId(123));
    ASSERT_EQ(response1.getNSS().ns_forTest(), "db.coll");
    ASSERT_EQ(response1.getBatch().size(), 2U);
    ASSERT_BSONOBJ_EQ(response1.getBatch()[0], BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(response1.getBatch()[1], BSON("_id" << 2));

    CursorResponse response2 = std::move(results[1].getValue());
    ASSERT_EQ(response2.getCursorId(), CursorId(234));
    ASSERT_EQ(response2.getNSS().ns_forTest(), "db.coll");
    ASSERT_EQ(response2.getBatch().size(), 2U);
    ASSERT_BSONOBJ_EQ(response2.getBatch()[0], BSON("_id" << 3));
    ASSERT_BSONOBJ_EQ(response2.getBatch()[1], BSON("_id" << 4));
}

TEST(CursorResponseTest, parseFromBSONManySingleCursor) {
    // In this case, parseFromBSONMany falls back to parseFromBSON
    std::vector<StatusWith<CursorResponse>> results = CursorResponse::parseFromBSONMany(BSON(
        "cursor" << BSON("id" << CursorId(123) << "ns"
                              << "db.coll"
                              << "firstBatch" << BSON_ARRAY(BSON("_id" << 1) << BSON("_id" << 2)))
                 << "ok" << 1));

    ASSERT_EQ(results.size(), 1);

    auto& result = results[0];

    ASSERT_OK(result.getStatus());

    CursorResponse response = std::move(result.getValue());
    ASSERT_EQ(response.getCursorId(), CursorId(123));
    ASSERT_EQ(response.getNSS().ns_forTest(), "db.coll");
    ASSERT_EQ(response.getBatch().size(), 2U);
    ASSERT_BSONOBJ_EQ(response.getBatch()[0], BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(response.getBatch()[1], BSON("_id" << 2));
}

TEST(CursorResponseTest, parseFromBSONManyEmptyCursors) {
    std::vector<StatusWith<CursorResponse>> results =
        CursorResponse::parseFromBSONMany(BSON("cursors" << BSONArray() << "ok" << 1));

    ASSERT_EQ(results.size(), 0);
}

}  // namespace
}  // namespace mongo
