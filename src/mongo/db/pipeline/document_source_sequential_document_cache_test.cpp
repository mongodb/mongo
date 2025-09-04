/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/pipeline/document_source_sequential_document_cache.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/explain_verbosity_gen.h"
#include "mongo/db/query/stage_memory_limit_knobs/knobs.h"
#include "mongo/unittest/unittest.h"

#include <string>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

// This provides access to getExpCtx(), but we'll use a different name for this test suite.
using DocumentSourceSequentialDocumentCacheTest = AggregationContextFixture;

const long long kDefaultMaxCacheSize =
    loadMemoryLimit(StageMemoryLimit::DocumentSourceLookupCacheSizeBytes);

TEST_F(DocumentSourceSequentialDocumentCacheTest, ReturnsEOFOnSubsequentCallsAfterSourceExhausted) {
    auto cache = std::make_shared<SequentialDocumentCache>(kDefaultMaxCacheSize);
    auto documentSourceSequentialDocumentCache =
        DocumentSourceSequentialDocumentCache::create(getExpCtx(), cache);
    auto sequentialDocumentCacheStage =
        exec::agg::buildStage(documentSourceSequentialDocumentCache);

    auto mockStage =
        exec::agg::MockStage::createForTest({"{a: 1, b: 2}", "{a: 3, b: 4}"}, getExpCtx());
    sequentialDocumentCacheStage->setSource(mockStage.get());

    ASSERT(sequentialDocumentCacheStage->getNext().isAdvanced());
    ASSERT(sequentialDocumentCacheStage->getNext().isAdvanced());
    ASSERT(sequentialDocumentCacheStage->getNext().isEOF());
    ASSERT(sequentialDocumentCacheStage->getNext().isEOF());
}

TEST_F(DocumentSourceSequentialDocumentCacheTest, ReturnsEOFAfterCacheExhausted) {
    auto cache = std::make_shared<SequentialDocumentCache>(kDefaultMaxCacheSize);
    cache->add(DOC("_id" << 0));
    cache->add(DOC("_id" << 1));
    cache->freeze();

    auto documentSourceSequentialDocumentCache =
        DocumentSourceSequentialDocumentCache::create(getExpCtx(), cache);
    auto sequentialDocumentCacheStage =
        exec::agg::buildStage(documentSourceSequentialDocumentCache);

    ASSERT(cache->isServing());
    ASSERT(sequentialDocumentCacheStage->getNext().isAdvanced());
    ASSERT(sequentialDocumentCacheStage->getNext().isAdvanced());
    ASSERT(sequentialDocumentCacheStage->getNext().isEOF());
    ASSERT(sequentialDocumentCacheStage->getNext().isEOF());
}

TEST_F(DocumentSourceSequentialDocumentCacheTest, Redaction) {
    auto cache = std::make_shared<SequentialDocumentCache>(kDefaultMaxCacheSize);
    cache->add(DOC("_id" << 0));
    cache->add(DOC("_id" << 1));
    auto documentCache = DocumentSourceSequentialDocumentCache::create(getExpCtx(), cache);
    std::vector<Value> vals;

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"$sequentialCache":{"maxSizeBytes":"?number","status":"kBuilding"}})",
        redact(*documentCache, true, ExplainOptions::Verbosity::kQueryPlanner));

    cache->freeze();
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"$sequentialCache":{"maxSizeBytes":"?number","status":"kServing"}})",
        redact(*documentCache, true, ExplainOptions::Verbosity::kQueryPlanner));

    cache->abandon();
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"$sequentialCache":{"maxSizeBytes":"?number","status":"kAbandoned"}})",
        redact(*documentCache, true, ExplainOptions::Verbosity::kQueryPlanner));
}
}  // namespace
}  // namespace mongo
