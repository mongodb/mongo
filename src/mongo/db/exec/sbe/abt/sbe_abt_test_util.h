/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include <memory>
#include <string>
#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/sbe/abt/abt_lower.h"
#include "mongo/db/exec/sbe/abt/abt_lower_defs.h"
#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/ce/sampling_executor.h"
#include "mongo/db/query/optimizer/node.h"  // IWYU pragma: keep
#include "mongo/db/query/optimizer/rewrites/const_eval.h"
#include "mongo/db/query/optimizer/syntax/syntax.h"
#include "mongo/db/query/optimizer/utils/utils.h"
#include "mongo/db/service_context_test_fixture.h"

#pragma once

namespace mongo::optimizer {

bool compareResults(const std::vector<BSONObj>& expected,
                    const std::vector<BSONObj>& actual,
                    bool preserveFieldOrder);

class NodeSBE : public ServiceContextTest {};

class ABTRecorder : public ce::SamplingExecutor {
public:
    ABTRecorder(ABTVector& nodes) : _nodes(nodes) {}
    ~ABTRecorder() override = default;

    std::pair<sbe::value::TypeTags, sbe::value::Value> execute(
        const Metadata& metadata,
        const QueryParameterMap& queryParameters,
        const PlanAndProps& planAndProps) const final;

private:
    // We don't own this.
    ABTVector& _nodes;
};

std::unique_ptr<mongo::Pipeline, mongo::PipelineDeleter> parsePipeline(
    const std::vector<BSONObj>& rawPipeline, NamespaceString nss, OperationContext* opCtx);
std::unique_ptr<mongo::Pipeline, mongo::PipelineDeleter> parsePipeline(
    const std::string& pipelineStr, NamespaceString nss, OperationContext* opCtx);

class ABTSBE : public sbe::EExpressionTestFixture {
protected:
    ABT constFold(ABT tree) {
        auto env = VariableEnvironment::build(tree);
        ConstEval{env}.optimize(tree);
        return tree;
    }
    // Helper that lowers and compiles an ABT expression and returns the evaluated result.
    // If the expression contains a variable, it will be bound to a slot along with its definition
    // before lowering.
    std::pair<sbe::value::TypeTags, sbe::value::Value> evalExpr(
        const ABT& tree,
        boost::optional<
            std::pair<ProjectionName, std::pair<sbe::value::TypeTags, sbe::value::Value>>> var) {
        auto env = VariableEnvironment::build(tree);

        SlotVarMap map;
        sbe::value::OwnedValueAccessor accessor;
        auto slotId = bindAccessor(&accessor);
        if (var) {
            auto& projName = var.get().first;
            map[projName] = slotId;

            auto [tag, val] = var.get().second;
            accessor.reset(tag, val);
        }

        sbe::InputParamToSlotMap inputParamToSlotMap;
        auto expr =
            SBEExpressionLowering{env, map, *runtimeEnv(), slotIdGenerator(), inputParamToSlotMap}
                .optimize(tree);

        auto compiledExpr = compileExpression(*expr);
        return runCompiledExpression(compiledExpr.get());
    }

    void assertEqualValues(std::pair<sbe::value::TypeTags, sbe::value::Value> res,
                           std::pair<sbe::value::TypeTags, sbe::value::Value> resConstFold) {
        auto [tag, val] = sbe::value::compareValue(
            res.first, res.second, resConstFold.first, resConstFold.second);
        ASSERT_EQ(tag, sbe::value::TypeTags::NumberInt32);
        ASSERT_EQ(val, 0);
    }
};

// Create a pipeline based on the given string, use a DocumentSourceQueue as input initialized with
// the provided documents encoded as json strings, and return the results as BSON.

ABT createValueArray(const std::vector<BSONObj>& inputObjs);
std::vector<BSONObj> runSBEAST(OperationContext* opCtx,
                               const std::string& pipelineStr,
                               const std::vector<BSONObj>& inputObjs);
std::vector<BSONObj> runPipeline(OperationContext* opCtx,
                                 const std::string& pipelineStr,
                                 const std::vector<BSONObj>& inputObjs);
}  // namespace mongo::optimizer
