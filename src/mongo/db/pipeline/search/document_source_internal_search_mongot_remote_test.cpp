/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/search/document_source_internal_search_mongot_remote.h"
#include "mongo/db/query/search/mongot_options.h"
#include "mongo/unittest/death_test.h"

namespace mongo {
namespace {

using InternalSearchMongotRemoteTest = AggregationContextFixture;

TEST_F(InternalSearchMongotRemoteTest, SearchMongotRemoteNotAllowedInTransaction) {
    auto expCtx = getExpCtx();
    expCtx->uuid = UUID::gen();
    expCtx->opCtx->setInMultiDocumentTransaction();
    globalMongotParams.host = "localhost:27027";
    globalMongotParams.enabled = true;

    auto specObj = BSON("$_internalSearchMongotRemote"
                        << BSON("mongotQuery" << BSONObj() << "metadataMergeProtocolVersion" << 1));
    auto spec = specObj.firstElement();

    // Set up the mongotRemote stage.
    auto mongotRemoteStage = DocumentSourceInternalSearchMongotRemote::createFromBson(spec, expCtx);
    ASSERT_THROWS_CODE(Pipeline::create({mongotRemoteStage}, expCtx),
                       AssertionException,
                       ErrorCodes::OperationNotSupportedInTransaction);
}

TEST_F(InternalSearchMongotRemoteTest, SearchMongotRemoteReturnsEOFWhenCollDoesNotExist) {
    auto expCtx = getExpCtx();
    globalMongotParams.host = "localhost:27027";
    globalMongotParams.enabled = true;

    auto specObj = BSON("$_internalSearchMongotRemote"
                        << BSON("mongotQuery" << BSONObj() << "metadataMergeProtocolVersion" << 1));
    auto spec = specObj.firstElement();

    // Set up the mongotRemote stage.
    auto mongotRemoteStage = DocumentSourceInternalSearchMongotRemote::createFromBson(spec, expCtx);
    ASSERT_TRUE(mongotRemoteStage->getNext().isEOF());
}

TEST_F(InternalSearchMongotRemoteTest, RedactsCorrectly) {
    auto spec = BSON("$_internalSearchMongotRemote"
                     << BSON("mongotQuery" << BSONObj() << "metadataMergeProtocolVersion" << 1));

    auto mongotRemoteStage =
        DocumentSourceInternalSearchMongotRemote::createFromBson(spec.firstElement(), getExpCtx());

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$_internalSearchMongotRemote": {
                "mongotQuery": "?object",
                "metadataMergeProtocolVersion": "?number",
                "limit": "?number",
                "requiresSearchMetaCursor": "?bool"
            }
        })",
        redact(*mongotRemoteStage));
}

TEST_F(InternalSearchMongotRemoteTest, RedactsCorrectlyWithMergingPipeline) {
    auto spec = fromjson(R"({
        $_internalSearchMongotRemote: {
            mongotQuery: { },
            metadataMergeProtocolVersion: 1,
            mergingPipeline: [
                {
                    $group: {
                        _id: "$x",
                        count: {
                            "$sum": 1
                        }
                    }
                }
            ]
        }
    })");

    auto mongotRemoteStage =
        DocumentSourceInternalSearchMongotRemote::createFromBson(spec.firstElement(), getExpCtx());

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$_internalSearchMongotRemote": {
                "mongotQuery": "?object",
                "metadataMergeProtocolVersion": "?number",
                "limit": "?number",
                "requiresSearchMetaCursor": "?bool",
                "mergingPipeline": [
                    {
                        "$group": {
                            "_id": "$HASH<x>",
                            "HASH<count>": {
                                "$sum": "?number"
                            }
                        }
                    }
                ]
            }
        })",
        redact(*mongotRemoteStage));
}

}  // namespace
}  // namespace mongo
