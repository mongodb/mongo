/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/unittest.h"

#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_sort.h"

namespace mongo {
namespace exec {
namespace agg {

namespace {

constexpr auto overridenStagePtr = nullptr;
int mappingFnCallCount = 0;
boost::intrusive_ptr<exec::agg::Stage> documentSourceSortMappingFnForTest(
    const boost::intrusive_ptr<const DocumentSource>& ds) {
    mappingFnCallCount++;
    return overridenStagePtr;
}

// This provides access to getExpCtx(), but we'll use a different name for this test suite.
using DocumentSourceToStageRegistryTest = AggregationContextFixture;

/**
 * Use default mapper for all unregistered document sources.
 * The default is to return static_cast<Stage*>(documentSourcePtr);
 */
TEST_F(DocumentSourceToStageRegistryTest, DefaultMapper) {
    auto matchDS =
        DocumentSourceMatch::create(fromjson("{$text: {$search: 'hello'} }"), getExpCtx());
    auto matchStage = buildStage(matchDS);

    ASSERT_EQ(matchDS.get(), matchStage.get());
}

REGISTER_AGG_STAGE_MAPPING(sort, DocumentSourceSort::id, documentSourceSortMappingFnForTest)

/**
 * Verify that REGISTER_AGG_STAGE_MAPPING macro overrides the default
 * DocumentSource -> Stage function. The default is to return
 * static_cast<Stage*>(documentSourcePtr);
 */
TEST_F(DocumentSourceToStageRegistryTest, OverriddenMapper) {
    BSONObj spec = fromjson("{$sort: {a: 1}}");
    auto sortDS = DocumentSourceSort::createFromBson(spec.firstElement(), getExpCtx());
    mappingFnCallCount = 0;

    auto sortStage = buildStage(sortDS);

    ASSERT_EQ(overridenStagePtr, sortStage.get());
    ASSERT_GT(mappingFnCallCount, 0);
}

}  // namespace

}  // namespace agg
}  // namespace exec
}  // namespace mongo
