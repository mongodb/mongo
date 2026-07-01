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

#include "mongo/db/pipeline/optimization/graph_validation_rules.h"

#include "mongo/bson/json.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_internal_assert_data_assumptions.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/db/query/compiler/metadata/path_arrayness.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <string>
#include <vector>

namespace mongo {
namespace {

class GraphValidationRulesTest : public AggregationContextFixture {
protected:
    /**
     * Registers path-arrayness metadata for the test namespace that marks 'nonArrayField' as
     * definitely not an array (i.e. an indexed, non-multikey path). This causes the dependency
     * graph's canPathBeArray() to return false for that field when it is resolved against the base
     * collection, which is what triggers validation-stage insertion.
     */
    void markFieldNonArray(const std::string& nonArrayField) {
        auto pathArrayness = std::make_shared<PathArrayness>();
        pathArrayness->addPath(
            FieldPath(nonArrayField), MultikeyComponents{}, true /* isFullRebuild */);
        getExpCtx()->setPathArraynessForNss(getExpCtx()->getNamespaceString(),
                                            std::move(pathArrayness));
    }

    std::unique_ptr<Pipeline> makePipeline(std::vector<std::string> stages) {
        std::vector<BSONObj> bsonStages;
        for (auto&& stage : stages) {
            bsonStages.push_back(fromjson(stage));
        }
        return pipeline_factory::makePipeline(
            bsonStages, getExpCtx(), pipeline_factory::kOptionsMinimal);
    }

    static bool isValidationStage(const boost::intrusive_ptr<DocumentSource>& ds) {
        return dynamic_cast<DocumentSourceInternalAssertDataAssumptions*>(ds.get()) != nullptr;
    }
};

// A leading stage that references a non-array path must not get a validation stage injected in
// front of it. Doing so would make the validation stage the pipeline front and defeat pushdown of
// the leading stage (e.g. $match, $sort, $geoNear) into the query executor.
TEST_F(GraphValidationRulesTest, DoesNotInsertValidationStageAtFrontOfPipeline) {
    markFieldNonArray("a");
    auto pipeline = makePipeline({"{$match: {a: {$gt: 5}}}"});

    pipeline_optimization::insertArraynessValidationStages(*pipeline);

    const auto& sources = pipeline->getSources();
    ASSERT_EQ(sources.size(), 1u);
    ASSERT_FALSE(isValidationStage(sources.front()));
}

// A non-leading stage that references a non-array path still gets a validation stage injected in
// front of it, since it is not eligible for query-executor pushdown.
TEST_F(GraphValidationRulesTest, InsertsValidationStageBeforeNonLeadingStage) {
    markFieldNonArray("a");
    auto pipeline = makePipeline({"{$match: {b: {$gt: 5}}}", "{$match: {a: {$gt: 5}}}"});

    pipeline_optimization::insertArraynessValidationStages(*pipeline);

    const auto& sources = pipeline->getSources();
    ASSERT_EQ(sources.size(), 3u);
    auto it = sources.begin();
    ASSERT_FALSE(isValidationStage(*it));    // Leading $match on 'b'.
    ASSERT_TRUE(isValidationStage(*++it));   // Injected before $match on 'a'.
    ASSERT_FALSE(isValidationStage(*++it));  // $match on 'a'.
}

// Validation stages are runtime-only and must not be inserted during explain.
TEST_F(GraphValidationRulesTest, DoesNotInsertValidationStageDuringExplain) {
    markFieldNonArray("a");
    getExpCtx()->setExplain(ExplainOptions::Verbosity::kQueryPlanner);
    auto pipeline = makePipeline({"{$match: {b: {$gt: 5}}}", "{$match: {a: {$gt: 5}}}"});

    pipeline_optimization::insertArraynessValidationStages(*pipeline);

    const auto& sources = pipeline->getSources();
    ASSERT_EQ(sources.size(), 2u);
    for (const auto& source : sources) {
        ASSERT_FALSE(isValidationStage(source));
    }
}

// removeArraynessValidationStages() undoes insertArraynessValidationStages().
TEST_F(GraphValidationRulesTest, RemoveUndoesInsert) {
    markFieldNonArray("a");
    auto pipeline = makePipeline({"{$match: {b: {$gt: 5}}}", "{$match: {a: {$gt: 5}}}"});

    pipeline_optimization::insertArraynessValidationStages(*pipeline);
    ASSERT_EQ(pipeline->getSources().size(), 3u);

    pipeline_optimization::removeArraynessValidationStages(*pipeline);

    const auto& sources = pipeline->getSources();
    ASSERT_EQ(sources.size(), 2u);
    for (const auto& source : sources) {
        ASSERT_FALSE(isValidationStage(source));
    }
}

}  // namespace
}  // namespace mongo
