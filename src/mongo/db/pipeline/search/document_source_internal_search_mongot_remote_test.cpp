// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/search/document_source_internal_search_mongot_remote.h"

#include "mongo/bson/json.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/search/mongot_options.h"

namespace mongo {
namespace {

class InternalSearchMongotRemoteTest : service_context_test::WithSetupTransportLayer,
                                       public AggregationContextFixture {
    void setUp() override {
        executor::startupSearchExecutorsIfNeeded(getServiceContext());
    }

    void tearDown() override {
        executor::shutdownSearchExecutorsIfNeeded(getServiceContext());
    }
};

boost::intrusive_ptr<DocumentSource> createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    auto specObj = elem.embeddedObject();
    auto serviceContext = expCtx->getOperationContext()->getServiceContext();
    auto executor = uassertStatusOK(executor::getMongotTaskExecutor(serviceContext));
    // The serialization for unsharded search does not contain a 'mongotQuery' field.
    InternalSearchMongotRemoteSpec spec = InternalSearchMongotRemoteSpec::parse(
        specObj, IDLParserContext(DocumentSourceInternalSearchMongotRemote::kStageName));
    return new DocumentSourceInternalSearchMongotRemote(std::move(spec), expCtx, executor);
}

TEST_F(InternalSearchMongotRemoteTest, SearchMongotRemoteNotAllowedInTransaction) {
    auto expCtx = getExpCtx();
    expCtx->setUUID(UUID::gen());
    expCtx->getOperationContext()->setInMultiDocumentTransaction();
    globalMongotParams.host = "localhost:27027";
    globalMongotParams.enabled = true;

    auto specObj = BSON("$_internalSearchMongotRemote"
                        << BSON("mongotQuery" << BSONObj() << "metadataMergeProtocolVersion" << 1));
    auto spec = specObj.firstElement();

    // Set up the mongotRemote stage.
    auto mongotRemoteStage = createFromBson(spec, expCtx);
    ASSERT_THROWS_CODE(Pipeline::create({mongotRemoteStage}, expCtx),
                       AssertionException,
                       ErrorCodes::OperationNotSupportedInTransaction);
}

TEST_F(InternalSearchMongotRemoteTest, SearchMongotRemoteAllowsUnknownFields) {
    auto expCtx = getExpCtx();
    globalMongotParams.host = "localhost:27027";
    globalMongotParams.enabled = true;
    auto specObj = BSON("$_internalSearchMongotRemote"
                        << BSON("mongotQuery" << BSONObj() << "metadataMergeProtocolVersion" << 1
                                              << "unknownField" << BSONObj()));
    auto spec = specObj.firstElement();

    // Because internalSearchMongotRemoteSpec is {strict: false}, the superfluous fields on the
    // request should be ignored and the DocumentSourceInternalSearchMongotRemote stage should be
    // serialized successfully.
    auto mongotRemote = createFromBson(spec, expCtx);
    auto mongotRemoteStage = exec::agg::buildStage(mongotRemote);
    ASSERT_TRUE(mongotRemoteStage->getNext().isEOF());
}

TEST_F(InternalSearchMongotRemoteTest, SearchMongotRemoteReturnsEOFWhenCollDoesNotExist) {
    auto expCtx = getExpCtx();
    globalMongotParams.host = "localhost:27027";
    globalMongotParams.enabled = true;

    auto specObj = BSON("$_internalSearchMongotRemote"
                        << BSON("mongotQuery" << BSONObj() << "metadataMergeProtocolVersion" << 1));
    auto spec = specObj.firstElement();

    // Set up the mongotRemote stage.
    auto mongotRemote = createFromBson(spec, expCtx);
    auto mongotRemoteStage = exec::agg::buildStage(mongotRemote);
    ASSERT_TRUE(mongotRemoteStage->getNext().isEOF());
}

TEST_F(InternalSearchMongotRemoteTest, RedactsCorrectly) {
    auto spec = BSON("$_internalSearchMongotRemote"
                     << BSON("mongotQuery" << BSONObj() << "metadataMergeProtocolVersion" << 1));

    auto mongotRemoteStage = createFromBson(spec.firstElement(), getExpCtx());

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

    auto mongotRemoteStage = createFromBson(spec.firstElement(), getExpCtx());

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
