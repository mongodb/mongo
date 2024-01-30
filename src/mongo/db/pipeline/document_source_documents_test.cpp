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
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_documents.h"
#include "mongo/db/pipeline/document_source_unwind.h"

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
    auto generatedField = unwindStage->getUnwindPath();
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        "{$queue: '?'}",
        redact(*docSourcesVec[0]));


    ASSERT_BSONOBJ_EQ(  // NOLINT
        fromjson(R"({
            "$project": {
                "HASH<_id>": true,
                "HASH<)" +
                 generatedField +
                 R"(>": "?array<?object>"
            }
        })"),
        redact(*docSourcesVec[1]));


    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        "{'$unwind': {'path' : '$HASH<" + generatedField + ">' } }",
        redact(*docSourcesVec[2]));


    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        "{$replaceRoot: {newRoot: '$HASH<" + generatedField + ">'}}",
        redact(*docSourcesVec[3]));
}

}  // namespace mongo
