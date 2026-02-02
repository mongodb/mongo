/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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


#include "mongo/db/exec/agg/lookup_stage.h"

#include "mongo/bson/json.h"
#include "mongo/bson/unordered_fields_bsonobj_comparator.h"
#include "mongo/db/database_name.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_lookup_test_util.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/str.h"

#include <deque>
#include <list>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

using namespace test;

class LookupStageTest : public AggregationContextFixture {
protected:
    LookupStageTest() {
        ShardingState::create(getServiceContext());
        // By default, make a mock mongo interface without any results from the foreign collection.
        // Individual tests will make their own interface if they need mock results.
        getExpCtx()->setMongoProcessInterface(
            std::make_shared<DocumentSourceLookupMockMongoInterface>(
                std::deque<DocumentSource::GetNextResult>{}));
    }
};
const long long kDefaultMaxCacheSize =
    loadMemoryLimit(StageMemoryLimit::DocumentSourceLookupCacheSizeBytes);

const auto kExplain = SerializationOptions{
    .verbosity = boost::make_optional(ExplainOptions::Verbosity::kQueryPlanner)};


TEST_F(LookupStageTest, ShouldPropagatePauses) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "foreign");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});

    // Mock the input of a foreign namespace, pausing every other result.
    auto mockLocalStage =
        exec::agg::MockStage::createForTest({Document{{"foreignId", 0}},
                                             DocumentSource::GetNextResult::makePauseExecution(),
                                             Document{{"foreignId", 1}},
                                             DocumentSource::GetNextResult::makePauseExecution()},
                                            expCtx);

    // Mock out the foreign collection.
    std::deque<DocumentSource::GetNextResult> mockForeignContents{Document{{"_id", 0}},
                                                                  Document{{"_id", 1}}};
    expCtx->setMongoProcessInterface(
        std::make_shared<DocumentSourceLookupMockMongoInterface>(std::move(mockForeignContents)));

    // Set up the $lookup stage.
    auto lookupSpec = Document{{"$lookup",
                                Document{{"from", fromNs.coll()},
                                         {"localField", "foreignId"_sd},
                                         {"foreignField", "_id"_sd},
                                         {"as", "foreignDocs"_sd}}}}
                          .toBson();
    auto lookup = makeLookUpFromBson(lookupSpec.firstElement(), expCtx);
    auto lookupStage = exec::agg::buildStage(lookup);
    lookupStage->setSource(mockLocalStage.get());

    auto next = lookupStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(),
                       (Document{{"foreignId", 0}, {"foreignDocs", {Document{{"_id", 0}}}}}));

    ASSERT_TRUE(lookupStage->getNext().isPaused());

    next = lookupStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(),
                       (Document{{"foreignId", 1}, {"foreignDocs", {Document{{"_id", 1}}}}}));

    ASSERT_TRUE(lookupStage->getNext().isPaused());

    ASSERT_TRUE(lookupStage->getNext().isEOF());
    ASSERT_TRUE(lookupStage->getNext().isEOF());
}

TEST_F(LookupStageTest, ShouldPropagatePausesWhileUnwinding) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "foreign");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});

    // Mock out the foreign collection.
    std::deque<DocumentSource::GetNextResult> mockForeignContents{Document{{"_id", 0}},
                                                                  Document{{"_id", 1}}};
    expCtx->setMongoProcessInterface(
        std::make_shared<DocumentSourceLookupMockMongoInterface>(std::move(mockForeignContents)));

    // Mock its input, pausing every other result.
    auto mockLocalStage =
        exec::agg::MockStage::createForTest({Document{{"foreignId", 0}},
                                             DocumentSource::GetNextResult::makePauseExecution(),
                                             Document{{"foreignId", 1}},
                                             DocumentSource::GetNextResult::makePauseExecution()},
                                            expCtx);

    // Set up the $lookup stage.
    auto lookupSpec = Document{{"$lookup",
                                Document{{"from", fromNs.coll()},
                                         {"localField", "foreignId"_sd},
                                         {"foreignField", "_id"_sd},
                                         {"as", "foreignDoc"_sd}}}}
                          .toBson();
    auto lookup = makeLookUpFromBson(lookupSpec.firstElement(), expCtx);

    const bool preserveNullAndEmptyArrays = false;
    const boost::optional<std::string> includeArrayIndex = boost::none;
    lookup->setUnwindStage_forTest(DocumentSourceUnwind::create(
        expCtx, "foreignDoc", preserveNullAndEmptyArrays, includeArrayIndex));

    auto lookupStage = exec::agg::buildStage(lookup.get());
    lookupStage->setSource(mockLocalStage.get());

    auto next = lookupStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(),
                       (Document{{"foreignId", 0}, {"foreignDoc", Document{{"_id", 0}}}}));

    ASSERT_TRUE(lookupStage->getNext().isPaused());

    next = lookupStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(),
                       (Document{{"foreignId", 1}, {"foreignDoc", Document{{"_id", 1}}}}));

    ASSERT_TRUE(lookupStage->getNext().isPaused());

    ASSERT_TRUE(lookupStage->getNext().isEOF());
    ASSERT_TRUE(lookupStage->getNext().isEOF());
}


TEST_F(LookupStageTest, ShouldReplaceNonCorrelatedPrefixWithCacheAfterFirstSubPipelineIteration) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});

    std::deque<DocumentSource::GetNextResult> mockForeignContents{
        Document{{"x", 0}}, Document{{"x", 1}}, Document{{"x", 2}}};
    expCtx->setMongoProcessInterface(
        std::make_shared<DocumentSourceLookupMockMongoInterface>(mockForeignContents));

    auto lookupDS = makeLookUpFromJson(
        "{$lookup: {let: {var1: '$_id'}, pipeline: [{$match: {x: {$gte: 0}}}, {$sort: {x: "
        "1}}, {$addFields: {varField: {$sum: ['$x', '$$var1']}}}], from: 'coll', as: "
        "'as'}}",
        expCtx);

    // Prepare the mocked local stage.
    auto mockLocalStage = exec::agg::MockStage::createForTest(
        {Document{{"_id", 0}}, Document{{"_id", 1}}, Document{{"_id", 2}}}, expCtx);

    auto lookupStage = buildLookUpStage(lookupDS);
    lookupStage->setSource(mockLocalStage.get());

    // Confirm that the empty 'kBuilding' cache is placed just before the correlated $addFields.
    auto subPipeline =
        lookupStage->buildPipeline(lookupDS->getSubpipelineExpCtx(), DOC("_id" << 0));
    ASSERT(subPipeline);

    auto expectedPipe = fromjson(
        str::stream() << "[{$mock: {}}, {$match: {x: {$gte: 0}}}, {$sort: {sortKey: {x: 1}}}, "
                      << sequentialCacheStageObj("kBuilding")
                      << ", {$addFields: {varField: {$sum: ['$x', {$const: 0}]}}}]");

    ASSERT_VALUE_EQ(Value(subPipeline->writeExplainOps(kExplain)), Value(BSONArray(expectedPipe)));

    // Verify the first result (non-cached) from the $lookup, for local document {_id: 0}.
    auto nonCachedResult = lookupStage->getNext();
    ASSERT(nonCachedResult.isAdvanced());
    ASSERT_DOCUMENT_EQ(
        Document{fromjson(
            "{_id: 0, as: [{x: 0, varField: 0}, {x: 1, varField: 1}, {x: 2, varField: 2}]}")},
        nonCachedResult.getDocument());

    // Preview the subpipeline that will be used to process the second local document {_id: 1}. The
    // sub-pipeline cache has been built on the first iteration, and is now serving in place of the
    // mocked foreign input source and the non-correlated stages at the start of the pipeline.
    subPipeline = lookupStage->buildPipeline(lookupDS->getSubpipelineExpCtx(), DOC("_id" << 1));
    ASSERT(subPipeline);

    expectedPipe =
        fromjson(str::stream() << "[" << sequentialCacheStageObj("kServing")
                               << ", {$addFields: {varField: {$sum: ['$x', {$const: 1}]}}}]");

    ASSERT_VALUE_EQ(Value(subPipeline->writeExplainOps(kExplain)), Value(BSONArray(expectedPipe)));

    // Verify that the rest of the results are correctly constructed from the cache.
    auto cachedResult = lookupStage->getNext();
    ASSERT(cachedResult.isAdvanced());
    ASSERT_DOCUMENT_EQ(
        Document{fromjson(
            "{_id: 1, as: [{x: 0, varField: 1}, {x: 1, varField: 2}, {x: 2, varField: 3}]}")},
        cachedResult.getDocument());

    cachedResult = lookupStage->getNext();
    ASSERT(cachedResult.isAdvanced());
    ASSERT_DOCUMENT_EQ(
        Document{fromjson(
            "{_id: 2, as: [{x: 0, varField: 2}, {x: 1, varField: 3}, {x: 2, varField: 4}]}")},
        cachedResult.getDocument());
}

TEST_F(LookupStageTest, ShouldAbandonCacheIfMaxSizeIsExceededAfterFirstSubPipelineIteration) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});

    std::deque<DocumentSource::GetNextResult> mockForeignContents{Document{{"x", 0}},
                                                                  Document{{"x", 1}}};
    expCtx->setMongoProcessInterface(
        std::make_shared<DocumentSourceLookupMockMongoInterface>(mockForeignContents));

    auto lookupDS = makeLookUpFromJson(
        "{$lookup: {let: {var1: '$_id'}, pipeline: [{$match: {x: {$gte: 0}}}, {$sort: {x: "
        "1}}, {$addFields: {varField: {$sum: ['$x', '$$var1']}}}], from: 'coll', as: "
        "'as'}}",
        expCtx);

    // Prepare the mocked local and foreign sources.
    auto mockLocalStage =
        exec::agg::MockStage::createForTest({Document{{"_id", 0}}, Document{{"_id", 1}}}, expCtx);

    auto lookupStage = buildLookUpStage(lookupDS);
    lookupStage->setSource(mockLocalStage.get());

    // Ensure the cache is abandoned after the first iteration by setting its max size to 0.
    size_t maxCacheSizeBytes = 0;
    lookupStage->reInitializeCache_forTest(maxCacheSizeBytes);

    // Confirm that the empty 'kBuilding' cache is placed just before the correlated $addFields.
    auto subPipeline =
        lookupStage->buildPipeline(lookupDS->getSubpipelineExpCtx(), DOC("_id" << 0));
    ASSERT(subPipeline);

    auto expectedPipe = fromjson(
        str::stream() << "[{$mock: {}}, {$match: {x: {$gte: 0}}}, {$sort: {sortKey: {x: 1}}}, "
                      << sequentialCacheStageObj("kBuilding", 0ll)
                      << ", {$addFields: {varField: {$sum: ['$x', {$const: 0}]}}}]");

    ASSERT_VALUE_EQ(Value(subPipeline->writeExplainOps(kExplain)), Value(BSONArray(expectedPipe)));

    // Get the first result from the stage, for local document {_id: 0}.
    auto firstResult = lookupStage->getNext();
    ASSERT_DOCUMENT_EQ(
        Document{fromjson("{_id: 0, as: [{x: 0, varField: 0}, {x: 1, varField: 1}]}")},
        firstResult.getDocument());

    // Preview the subpipeline that will be used to process the second local document {_id: 1}. The
    // sub-pipeline cache exceeded its max size on the first iteration, was abandoned, and is now
    // absent from the pipeline.
    subPipeline = lookupStage->buildPipeline(lookupDS->getSubpipelineExpCtx(), DOC("_id" << 1));
    ASSERT(subPipeline);

    expectedPipe = fromjson(
        str::stream() << "[{$mock: {}}, {$match: {x: {$gte: 0}}}, {$sort: {sortKey: {x: 1}}}, "
                         "{$addFields: {varField: {$sum: ['$x', {$const: 1}]}}}]");

    ASSERT_VALUE_EQ(Value(subPipeline->writeExplainOps(kExplain)), Value(BSONArray(expectedPipe)));

    // Verify that the second document is constructed correctly without the cache.
    auto secondResult = lookupStage->getNext();

    ASSERT_DOCUMENT_EQ(
        Document{fromjson("{_id: 1, as: [{x: 0, varField: 1}, {x: 1, varField: 2}]}")},
        secondResult.getDocument());
}

}  // namespace
}  // namespace mongo
