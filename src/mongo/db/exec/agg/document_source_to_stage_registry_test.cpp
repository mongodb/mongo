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

#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_test_optimizations.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace exec {
namespace agg {

namespace {

constexpr auto overridenStagePtr = nullptr;

class DocumentSourceUniqueForThisTest : public DocumentSourceTestOptimizations {
public:
    DocumentSourceUniqueForThisTest(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSourceTestOptimizations(expCtx) {}
    static const Id& id;
    Id getId() const override {
        return id;
    }
};

// Allocate a unique id for 'DocumentSourceUniqueForThisTest'.
ALLOCATE_DOCUMENT_SOURCE_ID(uniqueForThisTest, DocumentSourceUniqueForThisTest::id)

int mappingFnCallCount = 0;

boost::intrusive_ptr<exec::agg::Stage> documentSourceUniqueForThisTestMappingFn(
    const boost::intrusive_ptr<const DocumentSource>& ds) {
    mappingFnCallCount++;
    return overridenStagePtr;
}

/**
 * Test that for the DocumentSources that do not have a registered mapping function, we hit the
 * tassert in the mapping function.
 */
DEATH_TEST(DocumentSourceToStageRegistryTest,
           NonexistentMapper,
           "Missing 'DocumentSource' to 'agg::Stage' mapping function") {
    const auto expCtx = make_intrusive<ExpressionContextForTest>();

    // Create a DocumentSources that do not have a registered mapping function.
    const boost::intrusive_ptr<DocumentSource> ds =
        make_intrusive<DocumentSourceTestOptimizations>(expCtx);

    ASSERT_THROWS_CODE(buildStage(ds), DBException, 10395401);
}

REGISTER_AGG_STAGE_MAPPING(uniqueForThisTest,
                           DocumentSourceUniqueForThisTest::id,
                           documentSourceUniqueForThisTestMappingFn)

/**
 * Verify that REGISTER_AGG_STAGE_MAPPING macro overrides the default
 * DocumentSource -> Stage function. The default is to return
 * static_cast<Stage*>(documentSourcePtr);
 */
TEST(DocumentSourceToStageRegistryTest, OverriddenMapper) {
    auto uniqueForThisTestDS =
        make_intrusive<DocumentSourceUniqueForThisTest>(make_intrusive<ExpressionContextForTest>());
    mappingFnCallCount = 0;

    auto uniqueForThisTestStage = buildStage(uniqueForThisTestDS);

    ASSERT_EQ(overridenStagePtr, uniqueForThisTestStage.get());
    ASSERT_GT(mappingFnCallCount, 0);
}

}  // namespace

}  // namespace agg
}  // namespace exec
}  // namespace mongo
