// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/db/pipeline/document_source_documents.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_documents.h"
#include "mongo/db/pipeline/document_source_unwind.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"

#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

using DocumentSourceDocumentsTest = AggregationContextFixture;

TEST_F(DocumentSourceDocumentsTest, DocumentsStageRedactsCorrectly) {
    auto spec = fromjson(R"({
        $documents: [
            { x: 10 }, { x: 2 }, { x: 5 }
        ]
    })");
    auto docSourcesList = DocumentSourceDocuments::createFromBson(spec.firstElement(), getExpCtx());
    ASSERT_EQ(4, docSourcesList.size());

    // We must retrieve the internally-generated field name shared across these stages in order to
    // make sure they're serialized properly.
    std::vector<boost::intrusive_ptr<DocumentSource>> docSourcesVec(docSourcesList.begin(),
                                                                    docSourcesList.end());
    auto unwindStage = static_cast<DocumentSourceUnwind*>(docSourcesVec[2].get());
    ASSERT(unwindStage);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"$queue":"?array<?object>"})",
        redact(*docSourcesVec[0]));


    ASSERT_BSONOBJ_EQ(  // NOLINT
        fromjson(R"({
            "$project": {
                "HASH<_id>": true,
                "HASH<)" +
                 // SERVER-87666 ensure that the generated field is consistent.
                 DocumentSourceDocuments::kGenFieldName +
                 R"(>": "?array<?object>"
            }
        })"),
        redact(*docSourcesVec[1]));


    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        "{'$unwind': {'path' : '$HASH<" + DocumentSourceDocuments::kGenFieldName + ">' } }",
        redact(*docSourcesVec[2]));


    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        "{$replaceRoot: {newRoot: '$HASH<" + DocumentSourceDocuments::kGenFieldName + ">'}}",
        redact(*docSourcesVec[3]));
}

TEST_F(DocumentSourceDocumentsTest, ReturnsDesugaredStagesProperly) {
    auto expCtx = getExpCtx();
    expCtx->setNamespaceString(NamespaceString::makeCollectionlessAggregateNSS(
        DatabaseName::createDatabaseName_forTest(boost::none, "unittests")));

    auto spec = fromjson(R"({
        $documents: [
            { x: 10 }, { x: 2 }, { x: 5 }
        ]
    })");

    // Obtain desugared stages using helper.
    auto pipeline = pipeline_factory::makePipeline(
        std::vector<BSONObj>({spec}), expCtx, pipeline_factory::kOptionsMinimal);
    auto serializedPipeline = pipeline->serializeToBson();
    ASSERT_EQ(4, serializedPipeline.size());
    auto desugaredStages =
        DocumentSourceDocuments::extractDesugaredStagesFromPipeline(serializedPipeline);
    ASSERT(desugaredStages.has_value());

    // Obtain desugared stages directly.
    auto docSourcesList = DocumentSourceDocuments::createFromBson(spec.firstElement(), getExpCtx());
    ASSERT_EQ(4, docSourcesList.size());
    std::vector<boost::intrusive_ptr<DocumentSource>> docSourcesVec(docSourcesList.begin(),
                                                                    docSourcesList.end());

    // Make sure the stages returned from extractDesugaredStagesFromPipeline() function match those
    // created from $documents itself.
    for (size_t i = 0; i < serializedPipeline.size(); ++i) {
        std::vector<Value> serialized;
        (*docSourcesVec[i]).serializeToArray(serialized);
        ASSERT_EQ(1, serialized.size());
        ASSERT_BSONOBJ_EQ(serialized[0].getDocument().toBson(), desugaredStages.value()[i]);
    }
}

TEST_F(DocumentSourceDocumentsTest, ReturnsNoneIfNotDesugaredDocuments) {
    auto expCtx = getExpCtx();
    expCtx->setNamespaceString(NamespaceString::makeCollectionlessAggregateNSS(
        DatabaseName::createDatabaseName_forTest(boost::none, "unittests")));

    // Create pipeline with same stages as desugared $documents.
    auto queueStage = BSON("$queue" << BSON_ARRAY(BSON("a" << 1)));
    auto projectStage = BSON("$project" << BSON("a" << false));
    auto unwindStage = fromjson(R"({
        $unwind: {
            path: "$foo.bar",
            includeArrayIndex: "foo.baz",
            preserveNullAndEmptyArrays: true
        }
    })");
    auto replaceRootStage = BSON("$replaceRoot" << BSON("newRoot" << "$fullDocument"));

    auto pipeline = pipeline_factory::makePipeline(
        std::vector<BSONObj>({queueStage, projectStage, unwindStage, replaceRootStage}),
        expCtx,
        pipeline_factory::kOptionsMinimal);
    auto serializedPipeline = pipeline->serializeToBson();
    auto desugaredStages =
        DocumentSourceDocuments::extractDesugaredStagesFromPipeline(serializedPipeline);
    ASSERT_FALSE(desugaredStages.has_value());
}

}  // namespace mongo
