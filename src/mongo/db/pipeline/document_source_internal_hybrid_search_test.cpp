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

#include "mongo/db/pipeline/document_source_internal_hybrid_search.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/optimization/optimize.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"

namespace mongo {
namespace {

using DocumentSourceInternalHybridSearchTest = AggregationContextFixture;

TEST_F(DocumentSourceInternalHybridSearchTest, RejectsNonObjectSpec) {
    BSONObj spec = BSON("$_internalHybridSearch" << 1);
    ASSERT_THROWS_CODE(
        DocumentSourceInternalHybridSearch::createFromBson(spec.firstElement(), getExpCtx()),
        AssertionException,
        ErrorCodes::TypeMismatch);
}

TEST_F(DocumentSourceInternalHybridSearchTest, RejectsNonEmptyObjectSpec) {
    BSONObj spec = BSON("$_internalHybridSearch" << BSON("unexpected" << 1));
    ASSERT_THROWS_CODE(
        DocumentSourceInternalHybridSearch::createFromBson(spec.firstElement(), getExpCtx()),
        AssertionException,
        ErrorCodes::FailedToParse);
}

TEST_F(DocumentSourceInternalHybridSearchTest, SerializeRoundTrips) {
    BSONObj spec = BSON("$_internalHybridSearch" << BSONObj());
    auto stage =
        DocumentSourceInternalHybridSearch::createFromBson(spec.firstElement(), getExpCtx());

    std::vector<Value> serialized;
    stage->serializeToArray(serialized);
    ASSERT_EQ(serialized.size(), 1U);
    BSONObj roundTripped = serialized[0].getDocument().toBson();
    ASSERT_BSONOBJ_EQ(roundTripped, spec);

    // The round-tripped form parses back to the same stage.
    auto reparsed = DocumentSourceInternalHybridSearch::createFromBson(roundTripped.firstElement(),
                                                                       getExpCtx());
    ASSERT_EQ(reparsed->getSourceName(), DocumentSourceInternalHybridSearch::kStageName);
}

TEST_F(DocumentSourceInternalHybridSearchTest, ExecStageIsPassthrough) {
    BSONObj spec = BSON("$_internalHybridSearch" << BSONObj());
    auto ds = DocumentSourceInternalHybridSearch::createFromBson(spec.firstElement(), getExpCtx());

    auto mock = exec::agg::MockStage::createForTest({DOC("a" << 1), DOC("a" << 2)}, getExpCtx());
    auto stage = exec::agg::buildStageAndStitch(ds, mock);

    auto first = stage->getNext();
    ASSERT_TRUE(first.isAdvanced());
    ASSERT_DOCUMENT_EQ(first.getDocument(), (DOC("a" << 1)));
    auto second = stage->getNext();
    ASSERT_TRUE(second.isAdvanced());
    ASSERT_DOCUMENT_EQ(second.getDocument(), (DOC("a" << 2)));
    ASSERT_TRUE(stage->getNext().isEOF());
    ASSERT_TRUE(stage->isEOF());
}

TEST_F(DocumentSourceInternalHybridSearchTest, DoesNotBlockSortLimitAbsorption) {
    // The desugared pipeline ends [..., $sort, marker] and a trailing user $limit must still
    // reach the $sort for bounded top-k, so the marker must allow $skip/$limit to swap past it.
    auto sort = DocumentSourceSort::create(getExpCtx(), BSON("a" << 1));
    auto marker = make_intrusive<DocumentSourceInternalHybridSearch>(getExpCtx());
    auto limit = DocumentSourceLimit::create(getExpCtx(), 5);
    auto pipeline = Pipeline::create({sort, marker, limit}, getExpCtx());

    pipeline_optimization::optimizePipeline(*pipeline);

    const auto& sources = pipeline->getSources();
    ASSERT_EQ(sources.size(), 2U);
    auto* optimizedSort = dynamic_cast<DocumentSourceSort*>(sources.front().get());
    ASSERT(optimizedSort);
    ASSERT_EQ(optimizedSort->getLimit(), boost::optional<long long>(5));
    ASSERT_EQ(sources.back()->getSourceName(), DocumentSourceInternalHybridSearch::kStageName);
}

}  // namespace
}  // namespace mongo
