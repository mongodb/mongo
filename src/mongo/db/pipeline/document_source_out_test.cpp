// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_out.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/process_interface/stub_mongo_process_interface.h"
#include "mongo/db/pipeline/serverless_aggregation_context_fixture.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

#include <string>
#include <vector>

#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

/**
 * For the purpsoses of this test, assume every collection is unsharded. Stages may ask this during
 * setup. For example, to compute its constraints, the $out stage needs to know if the output
 * collection is sharded.
 */
class MongoProcessInterfaceForTest : public StubMongoProcessInterface {
public:
    bool isSharded(OperationContext* opCtx, const NamespaceString& ns) override {
        return false;
    }

    /**
     * For the purposes of these tests, pretend each collection is unsharded and has a document key
     * of just "_id".
     */
    std::vector<FieldPath> collectDocumentKeyFieldsActingAsRouter(
        OperationContext* opCtx,
        const NamespaceString& nss,
        RoutingContext* routingCtx = nullptr) const override {
        return {"_id"};
    }

    void checkRoutingInfoEpochOrThrow(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                      const NamespaceString&,
                                      ChunkVersion) const override {
        return;  // Pretend it always matches for our tests here.
    }
};

class DocumentSourceOutTest : public AggregationContextFixture {
public:
    DocumentSourceOutTest() : AggregationContextFixture() {
        getExpCtx()->setMongoProcessInterface(std::make_shared<MongoProcessInterfaceForTest>());
    }

    boost::intrusive_ptr<DocumentSourceOut> createOutStage(BSONObj spec) {
        auto specElem = spec.firstElement();
        boost::intrusive_ptr<DocumentSourceOut> outStage = dynamic_cast<DocumentSourceOut*>(
            DocumentSourceOut::createFromBson(specElem, getExpCtx()).get());
        ASSERT_TRUE(outStage);
        return outStage;
    }
};

TEST_F(DocumentSourceOutTest, FailsToParseIncorrectType) {
    BSONObj spec = BSON("$out" << 1);
    ASSERT_THROWS_CODE(createOutStage(spec), AssertionException, 16990);

    spec = BSON("$out" << BSONArray());
    ASSERT_THROWS_CODE(createOutStage(spec), AssertionException, 16990);

    spec = BSON("$out" << BSONObj());
    ASSERT_THROWS_CODE(createOutStage(spec), AssertionException, ErrorCodes::IDLFailedToParse);
}

TEST_F(DocumentSourceOutTest, AcceptsStringArgument) {
    BSONObj spec = BSON("$out" << "some_collection");
    auto outStage = createOutStage(spec);
    ASSERT_EQ(outStage->getOutputNs().coll(), "some_collection");
}

TEST_F(DocumentSourceOutTest, SerializeToString) {
    BSONObj spec = BSON("$out" << "some_collection");
    auto outStage = createOutStage(spec);
    auto serialized = outStage->serialize().getDocument();
    ASSERT_EQ(serialized["$out"]["coll"].getStringData(), "some_collection");

    // Make sure we can reparse the serialized BSON.
    auto reparsedOutStage = createOutStage(serialized.toBson());
    auto reSerialized = reparsedOutStage->serialize().getDocument();
    ASSERT_EQ(reSerialized["$out"]["coll"].getStringData(), "some_collection");
}

TEST_F(DocumentSourceOutTest, RedactionNoTimeseries) {
    auto spec = fromjson(R"({
            $out: {
                db: "foo",
                coll: "bar"
            }
        })");
    auto docSource = DocumentSourceOut::createFromBson(spec.firstElement(), getExpCtx());

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            $out: {
                coll: "HASH<bar>",
                db: "HASH<foo>"
            }
        })",
        redact(*docSource));
}

TEST_F(DocumentSourceOutTest, RedactionTimeseries) {
    auto spec = fromjson(R"({
            $out: {
                db: "foo",
                coll: "bar",
                timeseries: {
                    timeField: "time",
                    metaField: "meta",
                    granularity: "minutes",
                    bucketRoundingSeconds: 300,
                    bucketMaxSpanSeconds: 300
                }
            }
        })");
    auto docSource = DocumentSourceOut::createFromBson(spec.firstElement(), getExpCtx());
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$out": {
                "coll": "HASH<bar>",
                "db": "HASH<foo>",
                "timeseries": {
                    "timeField": "HASH<time>",
                    "metaField": "HASH<meta>",
                    "granularity": "minutes",
                    "bucketRoundingSeconds": "?number",
                    "bucketMaxSpanSeconds": "?number"
                }
            }
        })",
        redact(*docSource));
}

using DocumentSourceOutServerlessTest = ServerlessAggregationContextFixture;

TEST_F(DocumentSourceOutServerlessTest,
       LiteParsedDocumentSourceLookupContainsExpectedNamespacesInServerless) {
    unittest::ServerParameterGuard multitenancyController("multitenancySupport", true);

    auto tenantId = TenantId(OID::gen());
    NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(tenantId, "test", "testColl");
    std::vector<BSONObj> pipeline;

    auto stageSpec = BSON("$out" << "some_collection");
    auto liteParsedLookup =
        DocumentSourceOut::LiteParsed::parse(nss, stageSpec.firstElement(), LiteParserOptions{});
    auto namespaceSet = liteParsedLookup->getInvolvedNamespaces();
    ASSERT_EQ(1, namespaceSet.size());
    ASSERT_EQ(1ul,
              namespaceSet.count(NamespaceString::createNamespaceString_forTest(
                  tenantId, "test", "some_collection")));

    // The tenantId for the outputNs should be the same as that on the expCtx despite outputting
    // into different dbs.
    stageSpec = BSON("$out" << BSON("db" << "target_db"
                                         << "coll"
                                         << "some_collection"));
    liteParsedLookup =
        DocumentSourceOut::LiteParsed::parse(nss, stageSpec.firstElement(), LiteParserOptions{});
    namespaceSet = liteParsedLookup->getInvolvedNamespaces();
    ASSERT_EQ(1, namespaceSet.size());
    ASSERT_EQ(1ul,
              namespaceSet.count(NamespaceString::createNamespaceString_forTest(
                  tenantId, "target_db", "some_collection")));
}

TEST_F(DocumentSourceOutServerlessTest, CreateFromBSONContainsExpectedNamespacesInServerless) {
    unittest::ServerParameterGuard multitenancyController("multitenancySupport", true);

    auto expCtx = getExpCtx();
    ASSERT(expCtx->getNamespaceString().tenantId());
    auto defaultDb = expCtx->getNamespaceString().dbName();

    const std::string targetColl = "target_collection";
    auto spec = BSON("$out" << targetColl);
    auto outStage = DocumentSourceOut::createFromBson(spec.firstElement(), expCtx);
    auto outSource = static_cast<DocumentSourceOut*>(outStage.get());
    ASSERT(outSource);
    ASSERT_EQ(outSource->getOutputNs(),
              NamespaceString::createNamespaceString_forTest(defaultDb, targetColl));

    // TODO SERVER-77000: update this test once the serialize function has been updated to use
    // DatabaseNameUtil::serialize() instead.  We need to set the serialization context objs on the
    // expCtx, and manipulate before calling outSource->serialize().
    // Assert the tenantId is not included in the serialized namespace.
    auto serialized = outSource->serialize().getDocument();
    auto expectedDoc = Document{{"coll", targetColl},
                                {"db", expCtx->getNamespaceString().dbName().toString_forTest()}};
    ASSERT_DOCUMENT_EQ(serialized["$out"].getDocument(), expectedDoc);

    // TODO SERVER-77000: uncomment the below
    // expCtx->getSerializationContext().setPrefixState(true);
    // std::string targetDb = str::stream()
    //     << expCtx->getNamespaceString().tenantId()->toString() << "_" <<
    //     expCtx->getNamespaceString().dbName().toString_forTest();
    // serialized = outSource->serialize().getDocument();
    // expectedDoc = Document{{"coll", targetColl}, {"db", targetDb}};
    // ASSERT_DOCUMENT_EQ(serialized["$out"].getDocument(), expectedDoc);

    // The tenantId for the outputNs should be the same as that on the expCtx despite outputting
    // into different dbs.
    const std::string targetDb = "target_db";
    spec = BSON("$out" << BSON("db" << targetDb << "coll" << targetColl));
    outStage = DocumentSourceOut::createFromBson(spec.firstElement(), expCtx);
    outSource = static_cast<DocumentSourceOut*>(outStage.get());
    ASSERT(outSource);
    ASSERT(outSource->getOutputNs().tenantId());
    ASSERT_EQ(*outSource->getOutputNs().tenantId(), *expCtx->getNamespaceString().tenantId());
    ASSERT_EQ(outSource->getOutputNs().dbName().toString_forTest(), targetDb);

    // Assert the tenantId is not included in the serialized namespace.
    serialized = outSource->serialize().getDocument();
    expectedDoc = Document{{"coll", targetColl}, {"db", targetDb}};
    ASSERT_DOCUMENT_EQ(serialized["$out"].getDocument(), expectedDoc);
}

}  // namespace
}  // namespace mongo
