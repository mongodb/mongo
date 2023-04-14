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

#include <boost/intrusive_ptr.hpp>

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_out.h"
#include "mongo/idl/server_parameter_test_util.h"

namespace mongo {
namespace {

using boost::intrusive_ptr;

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
        OperationContext* opCtx, const NamespaceString& nss) const override {
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
        getExpCtx()->mongoProcessInterface = std::make_shared<MongoProcessInterfaceForTest>();
    }

    intrusive_ptr<DocumentSourceOut> createOutStage(BSONObj spec) {
        auto specElem = spec.firstElement();
        intrusive_ptr<DocumentSourceOut> outStage = dynamic_cast<DocumentSourceOut*>(
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
    ASSERT_THROWS_CODE(createOutStage(spec), AssertionException, 16994);
}

TEST_F(DocumentSourceOutTest, AcceptsStringArgument) {
    BSONObj spec = BSON("$out"
                        << "some_collection");
    auto outStage = createOutStage(spec);
    ASSERT_EQ(outStage->getOutputNs().coll(), "some_collection");
}

TEST_F(DocumentSourceOutTest, SerializeToString) {
    BSONObj spec = BSON("$out"
                        << "some_collection");
    auto outStage = createOutStage(spec);
    auto serialized = outStage->serialize().getDocument();
    ASSERT_EQ(serialized["$out"]["coll"].getStringData(), "some_collection");

    // Make sure we can reparse the serialized BSON.
    auto reparsedOutStage = createOutStage(serialized.toBson());
    auto reSerialized = reparsedOutStage->serialize().getDocument();
    ASSERT_EQ(reSerialized["$out"]["coll"].getStringData(), "some_collection");
}

TEST_F(DocumentSourceOutTest, Redaction) {
    // TODO SERVER-75138 test support for redaction with timeseries options
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
                db: "HASH<foo>",
                coll: "HASH<bar>"
            }
        })",
        redact(*docSource));
}

using DocumentSourceOutServerlessTest = ServerlessAggregationContextFixture;

TEST_F(DocumentSourceOutServerlessTest,
       LiteParsedDocumentSourceLookupContainsExpectedNamespacesInServerless) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);

    auto tenantId = TenantId(OID::gen());
    NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(tenantId, "test", "testColl");
    std::vector<BSONObj> pipeline;

    auto stageSpec = BSON("$out"
                          << "some_collection");
    auto liteParsedLookup = DocumentSourceOut::LiteParsed::parse(nss, stageSpec.firstElement());
    auto namespaceSet = liteParsedLookup->getInvolvedNamespaces();
    ASSERT_EQ(1, namespaceSet.size());
    ASSERT_EQ(1ul,
              namespaceSet.count(NamespaceString::createNamespaceString_forTest(
                  tenantId, "test", "some_collection")));

    // The tenantId for the outputNs should be the same as that on the expCtx despite outputting
    // into different dbs.
    stageSpec = BSON("$out" << BSON("db"
                                    << "target_db"
                                    << "coll"
                                    << "some_collection"));
    liteParsedLookup = DocumentSourceOut::LiteParsed::parse(nss, stageSpec.firstElement());
    namespaceSet = liteParsedLookup->getInvolvedNamespaces();
    ASSERT_EQ(1, namespaceSet.size());
    ASSERT_EQ(1ul,
              namespaceSet.count(NamespaceString::createNamespaceString_forTest(
                  tenantId, "target_db", "some_collection")));
}

TEST_F(DocumentSourceOutServerlessTest, CreateFromBSONContainsExpectedNamespacesInServerless) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);

    auto expCtx = getExpCtx();
    ASSERT(expCtx->ns.tenantId());
    auto defaultDb = expCtx->ns.dbName();

    const std::string targetColl = "target_collection";
    auto spec = BSON("$out" << targetColl);
    auto outStage = DocumentSourceOut::createFromBson(spec.firstElement(), expCtx);
    auto outSource = static_cast<DocumentSourceOut*>(outStage.get());
    ASSERT(outSource);
    ASSERT_EQ(outSource->getOutputNs(),
              NamespaceString::createNamespaceString_forTest(defaultDb, targetColl));

    // Assert the tenantId is not included in the serialized namespace.
    auto serialized = outSource->serialize().getDocument();
    auto expectedDoc = Document{{"db", expCtx->ns.dbName().db()}, {"coll", targetColl}};
    ASSERT_DOCUMENT_EQ(serialized["$out"].getDocument(), expectedDoc);

    // The tenantId for the outputNs should be the same as that on the expCtx despite outputting
    // into different dbs.
    const std::string targetDb = "target_db";
    spec = BSON("$out" << BSON("db" << targetDb << "coll" << targetColl));
    outStage = DocumentSourceOut::createFromBson(spec.firstElement(), expCtx);
    outSource = static_cast<DocumentSourceOut*>(outStage.get());
    ASSERT(outSource);
    ASSERT(outSource->getOutputNs().tenantId());
    ASSERT_EQ(*outSource->getOutputNs().tenantId(), *expCtx->ns.tenantId());
    ASSERT_EQ(outSource->getOutputNs().dbName().db(), targetDb);

    // Assert the tenantId is not included in the serialized namespace.
    serialized = outSource->serialize().getDocument();
    expectedDoc = Document{{"db", targetDb}, {"coll", targetColl}};
    ASSERT_DOCUMENT_EQ(serialized["$out"].getDocument(), expectedDoc);
}

}  // namespace
}  // namespace mongo
