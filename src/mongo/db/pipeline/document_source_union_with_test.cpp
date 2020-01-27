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

#include <array>
#include <deque>
#include <list>
#include <set>
#include <vector>

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_comparator.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_add_fields.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/document_source_replace_root.h"
#include "mongo/db/pipeline/document_source_union_with.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/stub_mongo_process_interface_lookup_single_document.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"

namespace mongo {
namespace {

using MockMongoInterface = StubMongoProcessInterfaceLookupSingleDocument;

// This provides access to getExpCtx(), but we'll use a different name for this test suite.
using DocumentSourceUnionWithTest = AggregationContextFixture;

TEST_F(DocumentSourceUnionWithTest, BasicSerialUnions) {
    const auto docs = std::array{Document{{"a", 1}}, Document{{"b", 1}}, Document{{"c", 1}}};
    const auto mock = DocumentSourceMock::createForTest(docs[0]);
    const auto mockDequeOne = std::deque<DocumentSource::GetNextResult>{Document{docs[1]}};
    const auto mockDequeTwo = std::deque<DocumentSource::GetNextResult>{Document{docs[2]}};
    const auto mockCtxOne = getExpCtx()->copyWith({});
    mockCtxOne->mongoProcessInterface = std::make_unique<MockMongoInterface>(mockDequeOne);
    const auto mockCtxTwo = getExpCtx()->copyWith({});
    mockCtxTwo->mongoProcessInterface = std::make_unique<MockMongoInterface>(mockDequeTwo);
    auto unionWithOne = DocumentSourceUnionWith(
        mockCtxOne,
        uassertStatusOK(
            Pipeline::create(std::list<boost::intrusive_ptr<DocumentSource>>{}, getExpCtx())));
    auto unionWithTwo = DocumentSourceUnionWith(
        mockCtxTwo,
        uassertStatusOK(
            Pipeline::create(std::list<boost::intrusive_ptr<DocumentSource>>{}, getExpCtx())));
    unionWithOne.setSource(mock.get());
    unionWithTwo.setSource(&unionWithOne);

    auto comparator = DocumentComparator();
    auto results = comparator.makeUnorderedDocumentSet();
    for (auto& doc [[maybe_unused]] : docs) {
        auto next = unionWithTwo.getNext();
        ASSERT_TRUE(next.isAdvanced());
        const auto [ignored, inserted] = results.insert(next.releaseDocument());
        ASSERT_TRUE(inserted);
    }
    for (const auto& doc : docs)
        ASSERT_TRUE(results.find(doc) != results.end());

    ASSERT_TRUE(unionWithTwo.getNext().isEOF());
    ASSERT_TRUE(unionWithTwo.getNext().isEOF());
    ASSERT_TRUE(unionWithTwo.getNext().isEOF());
}

TEST_F(DocumentSourceUnionWithTest, BasicNestedUnions) {
    const auto docs = std::array{Document{{"a", 1}}, Document{{"b", 1}}, Document{{"c", 1}}};
    const auto mock = DocumentSourceMock::createForTest(docs[0]);
    const auto mockDequeOne = std::deque<DocumentSource::GetNextResult>{Document{docs[1]}};
    const auto mockDequeTwo = std::deque<DocumentSource::GetNextResult>{Document{docs[2]}};
    const auto mockCtxOne = getExpCtx()->copyWith({});
    mockCtxOne->mongoProcessInterface = std::make_unique<MockMongoInterface>(mockDequeOne);
    const auto mockCtxTwo = getExpCtx()->copyWith({});
    mockCtxTwo->mongoProcessInterface = std::make_unique<MockMongoInterface>(mockDequeTwo);
    auto unionWithOne = make_intrusive<DocumentSourceUnionWith>(
        mockCtxOne,
        uassertStatusOK(
            Pipeline::create(std::list<boost::intrusive_ptr<DocumentSource>>{}, getExpCtx())));
    auto unionWithTwo = DocumentSourceUnionWith(
        mockCtxTwo,
        uassertStatusOK(Pipeline::create(
            std::list<boost::intrusive_ptr<DocumentSource>>{unionWithOne}, getExpCtx())));
    unionWithTwo.setSource(mock.get());

    auto comparator = DocumentComparator();
    auto results = comparator.makeUnorderedDocumentSet();
    for (auto& doc [[maybe_unused]] : docs) {
        auto next = unionWithTwo.getNext();
        ASSERT_TRUE(next.isAdvanced());
        const auto [ignored, inserted] = results.insert(next.releaseDocument());
        ASSERT_TRUE(inserted);
    }
    for (const auto& doc : docs)
        ASSERT_TRUE(results.find(doc) != results.end());

    ASSERT_TRUE(unionWithTwo.getNext().isEOF());
    ASSERT_TRUE(unionWithTwo.getNext().isEOF());
    ASSERT_TRUE(unionWithTwo.getNext().isEOF());
}

TEST_F(DocumentSourceUnionWithTest, UnionsWithNonEmptySubPipelines) {
    const auto inputDocs = std::array{Document{{"a", 1}}, Document{{"b", 1}}, Document{{"c", 1}}};
    const auto outputDocs = std::array{Document{{"a", 1}}, Document{{"c", 1}, {"d", 1}}};
    const auto mock = DocumentSourceMock::createForTest(inputDocs[0]);
    const auto mockDequeOne = std::deque<DocumentSource::GetNextResult>{Document{inputDocs[1]}};
    const auto mockDequeTwo = std::deque<DocumentSource::GetNextResult>{Document{inputDocs[2]}};
    const auto mockCtxOne = getExpCtx()->copyWith({});
    mockCtxOne->mongoProcessInterface = std::make_unique<MockMongoInterface>(mockDequeOne);
    const auto mockCtxTwo = getExpCtx()->copyWith({});
    mockCtxTwo->mongoProcessInterface = std::make_unique<MockMongoInterface>(mockDequeTwo);
    const auto filter = DocumentSourceMatch::create(BSON("d" << 1), mockCtxOne);
    const auto proj = DocumentSourceAddFields::create(BSON("d" << 1), mockCtxTwo);
    auto unionWithOne = DocumentSourceUnionWith(
        mockCtxOne,
        uassertStatusOK(Pipeline::create(std::list<boost::intrusive_ptr<DocumentSource>>{filter},
                                         getExpCtx())));
    auto unionWithTwo = DocumentSourceUnionWith(
        mockCtxTwo,
        uassertStatusOK(
            Pipeline::create(std::list<boost::intrusive_ptr<DocumentSource>>{proj}, getExpCtx())));
    unionWithOne.setSource(mock.get());
    unionWithTwo.setSource(&unionWithOne);

    auto comparator = DocumentComparator();
    auto results = comparator.makeUnorderedDocumentSet();
    for (auto& doc [[maybe_unused]] : outputDocs) {
        auto next = unionWithTwo.getNext();
        ASSERT_TRUE(next.isAdvanced());
        const auto [ignored, inserted] = results.insert(next.releaseDocument());
        ASSERT_TRUE(inserted);
    }
    for (const auto& doc : outputDocs)
        ASSERT_TRUE(results.find(doc) != results.end());

    ASSERT_TRUE(unionWithTwo.getNext().isEOF());
    ASSERT_TRUE(unionWithTwo.getNext().isEOF());
    ASSERT_TRUE(unionWithTwo.getNext().isEOF());
}

TEST_F(DocumentSourceUnionWithTest, SerializeAndParseWithPipeline) {
    auto bson =
        BSON("$unionWith" << BSON("coll"
                                  << "foo"
                                  << "pipeline"
                                  << BSON_ARRAY(
                                         BSON("$addFields" << BSON("a" << BSON("$const" << 3))))));
    auto unionWith = DocumentSourceUnionWith::createFromBson(bson.firstElement(), getExpCtx());
    ASSERT(unionWith->getSourceName() == DocumentSourceUnionWith::kStageName);
    std::vector<Value> serializedArray;
    unionWith->serializeToArray(serializedArray);
    auto serializedBson = serializedArray[0].getDocument().toBson();
    ASSERT_BSONOBJ_EQ(serializedBson, bson);
    unionWith = DocumentSourceUnionWith::createFromBson(serializedBson.firstElement(), getExpCtx());
    ASSERT(unionWith != nullptr);
    ASSERT(unionWith->getSourceName() == DocumentSourceUnionWith::kStageName);
}

TEST_F(DocumentSourceUnionWithTest, SerializeAndParseWithoutPipeline) {
    auto bson = BSON("$unionWith"
                     << "foo");
    auto desugaredBson = BSON("$unionWith" << BSON("coll"
                                                   << "foo"
                                                   << "pipeline" << BSONArray()));
    auto unionWith = DocumentSourceUnionWith::createFromBson(bson.firstElement(), getExpCtx());
    ASSERT(unionWith->getSourceName() == DocumentSourceUnionWith::kStageName);
    std::vector<Value> serializedArray;
    unionWith->serializeToArray(serializedArray);
    auto serializedBson = serializedArray[0].getDocument().toBson();
    ASSERT_BSONOBJ_EQ(serializedBson, desugaredBson);
    unionWith = DocumentSourceUnionWith::createFromBson(serializedBson.firstElement(), getExpCtx());
    ASSERT(unionWith != nullptr);
    ASSERT(unionWith->getSourceName() == DocumentSourceUnionWith::kStageName);
}

TEST_F(DocumentSourceUnionWithTest, SerializeAndParseWithoutPipelineExtraSubobject) {
    auto bson = BSON("$unionWith" << BSON("coll"
                                          << "foo"));
    auto desugaredBson = BSON("$unionWith" << BSON("coll"
                                                   << "foo"
                                                   << "pipeline" << BSONArray()));
    auto unionWith = DocumentSourceUnionWith::createFromBson(bson.firstElement(), getExpCtx());
    ASSERT(unionWith->getSourceName() == DocumentSourceUnionWith::kStageName);
    std::vector<Value> serializedArray;
    unionWith->serializeToArray(serializedArray);
    auto serializedBson = serializedArray[0].getDocument().toBson();
    ASSERT_BSONOBJ_EQ(serializedBson, desugaredBson);
    unionWith = DocumentSourceUnionWith::createFromBson(serializedBson.firstElement(), getExpCtx());
    ASSERT(unionWith != nullptr);
    ASSERT(unionWith->getSourceName() == DocumentSourceUnionWith::kStageName);
}

TEST_F(DocumentSourceUnionWithTest, ParseErrors) {
    ASSERT_THROWS_CODE(DocumentSourceUnionWith::createFromBson(
                           BSON("$unionWith" << false).firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::FailedToParse);
    ASSERT_THROWS_CODE(DocumentSourceUnionWith::createFromBson(BSON("$unionWith" << BSON("coll"
                                                                                         << "foo"
                                                                                         << "coll"
                                                                                         << "bar"))
                                                                   .firstElement(),
                                                               getExpCtx()),
                       AssertionException,
                       40413);
    ASSERT_THROWS_CODE(
        DocumentSourceUnionWith::createFromBson(
            BSON("$unionWith" << BSON("coll"
                                      << "foo"
                                      << "pipeline"
                                      << BSON_ARRAY(BSON("$addFields" << BSON("a" << 3))) << "coll"
                                      << "bar"))
                .firstElement(),
            getExpCtx()),
        AssertionException,
        40413);
    ASSERT_THROWS_CODE(
        DocumentSourceUnionWith::createFromBson(
            BSON("$unionWith" << BSON("coll"
                                      << "foo"
                                      << "pipeline"
                                      << BSON_ARRAY(BSON("$addFields" << BSON("a" << 3))) << "myDog"
                                      << "bar"))
                .firstElement(),
            getExpCtx()),
        AssertionException,
        40415);
    ASSERT_THROWS_CODE(
        DocumentSourceUnionWith::createFromBson(
            BSON("$unionWith" << BSON("coll"
                                      << "foo"
                                      << "pipeline"
                                      << BSON_ARRAY(BSON("$petMyDog" << BSON("myDog" << 3)))))
                .firstElement(),
            getExpCtx()),
        AssertionException,
        16436);
    ASSERT_THROWS_CODE(
        DocumentSourceUnionWith::createFromBson(
            BSON("$unionWith" << BSON("coll" << BSON("not"
                                                     << "string")
                                             << "pipeline"
                                             << BSON_ARRAY(BSON("$addFields" << BSON("a" << 3)))))
                .firstElement(),
            getExpCtx()),
        AssertionException,
        ErrorCodes::TypeMismatch);
    ASSERT_THROWS_CODE(
        DocumentSourceUnionWith::createFromBson(BSON("$unionWith" << BSON("coll"
                                                                          << "foo"
                                                                          << "pipeline"
                                                                          << "string"))
                                                    .firstElement(),
                                                getExpCtx()),
        AssertionException,
        10065);
    ASSERT_THROWS_CODE(
        DocumentSourceUnionWith::createFromBson(BSON("$unionWith" << BSON("coll"
                                                                          << "foo"
                                                                          << "pipeline"
                                                                          << BSON("not"
                                                                                  << "string")))
                                                    .firstElement(),
                                                getExpCtx()),
        AssertionException,
        40422);
}

TEST_F(DocumentSourceUnionWithTest, PropagatePauses) {
    const auto mock =
        DocumentSourceMock::createForTest({Document(),
                                           DocumentSource::GetNextResult::makePauseExecution(),
                                           Document(),
                                           DocumentSource::GetNextResult::makePauseExecution()});
    const auto mockDequeOne = std::deque<DocumentSource::GetNextResult>{};
    const auto mockDequeTwo = std::deque<DocumentSource::GetNextResult>{};
    const auto mockCtxOne = getExpCtx()->copyWith({});
    mockCtxOne->mongoProcessInterface = std::make_unique<MockMongoInterface>(mockDequeOne);
    const auto mockCtxTwo = getExpCtx()->copyWith({});
    mockCtxTwo->mongoProcessInterface = std::make_unique<MockMongoInterface>(mockDequeTwo);
    auto unionWithOne = DocumentSourceUnionWith(
        mockCtxOne,
        uassertStatusOK(
            Pipeline::create(std::list<boost::intrusive_ptr<DocumentSource>>{}, getExpCtx())));
    auto unionWithTwo = DocumentSourceUnionWith(
        mockCtxTwo,
        uassertStatusOK(
            Pipeline::create(std::list<boost::intrusive_ptr<DocumentSource>>{}, getExpCtx())));
    unionWithOne.setSource(mock.get());
    unionWithTwo.setSource(&unionWithOne);

    ASSERT_TRUE(unionWithTwo.getNext().isAdvanced());
    ASSERT_TRUE(unionWithTwo.getNext().isPaused());
    ASSERT_TRUE(unionWithTwo.getNext().isAdvanced());
    ASSERT_TRUE(unionWithTwo.getNext().isPaused());

    ASSERT_TRUE(unionWithTwo.getNext().isEOF());
    ASSERT_TRUE(unionWithTwo.getNext().isEOF());
    ASSERT_TRUE(unionWithTwo.getNext().isEOF());
}

TEST_F(DocumentSourceUnionWithTest, DependencyAnalysisReportsFullDoc) {
    auto expCtx = getExpCtx();
    const auto replaceRoot =
        DocumentSourceReplaceRoot::createFromBson(BSON("$replaceRoot" << BSON("newRoot"
                                                                              << "$b"))
                                                      .firstElement(),
                                                  expCtx);
    const auto unionWith = make_intrusive<DocumentSourceUnionWith>(
        expCtx,
        uassertStatusOK(
            Pipeline::create(std::list<boost::intrusive_ptr<DocumentSource>>{}, expCtx)));

    // With the $unionWith *before* the $replaceRoot, the dependency analysis will report that all
    // fields are needed.
    auto pipeline = uassertStatusOK(Pipeline::create({unionWith, replaceRoot}, expCtx));

    auto deps = pipeline->getDependencies(DepsTracker::kNoMetadata);
    ASSERT_BSONOBJ_EQ(deps.toProjectionWithoutMetadata(), BSONObj());
    ASSERT_TRUE(deps.needWholeDocument);
}

TEST_F(DocumentSourceUnionWithTest, DependencyAnalysisReportsReferencedFieldsBeforeUnion) {
    auto expCtx = getExpCtx();

    const auto replaceRoot =
        DocumentSourceReplaceRoot::createFromBson(BSON("$replaceRoot" << BSON("newRoot"
                                                                              << "$b"))
                                                      .firstElement(),
                                                  expCtx);
    const auto unionWith = make_intrusive<DocumentSourceUnionWith>(
        expCtx,
        uassertStatusOK(
            Pipeline::create(std::list<boost::intrusive_ptr<DocumentSource>>{}, expCtx)));

    // With the $unionWith *after* the $replaceRoot, the dependency analysis will now report only
    // the referenced fields.
    auto pipeline = uassertStatusOK(Pipeline::create({replaceRoot, unionWith}, expCtx));

    auto deps = pipeline->getDependencies(DepsTracker::kNoMetadata);
    ASSERT_BSONOBJ_EQ(deps.toProjectionWithoutMetadata(), BSON("b" << 1 << "_id" << 0));
    ASSERT_FALSE(deps.needWholeDocument);
}

}  // namespace
}  // namespace mongo
