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

#include "mongo/db/exec/sbe/abt/sbe_abt_test_util.h"

#include <cstddef>
#include <iosfwd>
#include <utility>

#include <absl/container/node_hash_map.h>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/sbe/abt/abt_lower.h"
#include "mongo/db/exec/sbe/abt/abt_lower_defs.h"
#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/expressions/runtime_environment.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/pipeline/abt/document_source_visitor.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_queue.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/cost_model/cost_model_gen.h"
#include "mongo/db/query/cqf_command_utils.h"
#include "mongo/db/query/optimizer/defs.h"
#include "mongo/db/query/optimizer/explain.h"
#include "mongo/db/query/optimizer/metadata.h"
#include "mongo/db/query/optimizer/node_defs.h"
#include "mongo/db/query/optimizer/opt_phase_manager.h"
#include "mongo/db/query/optimizer/reference_tracker.h"
#include "mongo/db/query/optimizer/syntax/expr.h"
#include "mongo/db/query/optimizer/utils/unit_test_utils.h"
#include "mongo/db/query/optimizer/utils/utils.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::optimizer {

namespace {
bool compareBSONObj(const BSONObj& actual, const BSONObj& expected, bool preserveFieldOrder) {
    BSONObj::ComparisonRulesSet rules = BSONObj::ComparisonRules::kConsiderFieldName;
    if (!preserveFieldOrder) {
        rules |= BSONObj::ComparisonRules::kIgnoreFieldOrder;
    }
    return actual.woCompare(expected, BSONObj(), rules) == 0;
}
}  // namespace

bool compareResults(const std::vector<BSONObj>& expected,
                    const std::vector<BSONObj>& actual,
                    const bool preserveFieldOrder) {
    if (expected.size() != actual.size()) {
        std::cout << "Different result size: expected: " << expected.size()
                  << " vs actual: " << actual.size() << "\n";

        std::cout << "Expected results:\n";
        for (const auto& result : expected) {
            std::cout << result << "\n";
        }
        std::cout << "Actual results:\n";
        for (const auto& result : actual) {
            std::cout << result << "\n";
        }

        return false;
    }

    for (size_t i = 0; i < expected.size(); i++) {
        if (!compareBSONObj(actual.at(i), expected.at(i), preserveFieldOrder)) {
            std::cout << "Result at position " << i << "/" << expected.size()
                      << " mismatch: expected: " << expected.at(i) << " vs actual: " << actual.at(i)
                      << "\n";
            return false;
        }
    }

    return true;
}

std::pair<sbe::value::TypeTags, sbe::value::Value> ABTRecorder::execute(
    const Metadata& /*metadata*/,
    const QueryParameterMap& /*queryParameters*/,
    const PlanAndProps& planAndProps) const {
    _nodes.push_back(planAndProps._node);
    return {sbe::value::TypeTags::Nothing, 0};
}

std::unique_ptr<mongo::Pipeline, mongo::PipelineDeleter> parsePipeline(
    const std::vector<BSONObj>& rawPipeline, NamespaceString nss, OperationContext* opCtx) {
    AggregateCommandRequest request(std::move(nss), rawPipeline);
    boost::intrusive_ptr<ExpressionContextForTest> ctx(
        new ExpressionContextForTest(opCtx, request));

    static unittest::TempDir tempDir("ABTPipelineTest");
    ctx->tempDir = tempDir.path();

    return Pipeline::parse(request.getPipeline(), ctx);
}

std::unique_ptr<mongo::Pipeline, mongo::PipelineDeleter> parsePipeline(
    const std::string& pipelineStr, NamespaceString nss, OperationContext* opCtx) {
    const BSONObj inputBson = fromjson("{pipeline: " + pipelineStr + "}");

    std::vector<BSONObj> rawPipeline;
    for (auto&& stageElem : inputBson["pipeline"].Array()) {
        ASSERT_EQUALS(stageElem.type(), BSONType::Object);
        rawPipeline.push_back(stageElem.embeddedObject());
    }
    return parsePipeline(rawPipeline, std::move(nss), opCtx);
}

ABT createValueArray(const std::vector<BSONObj>& inputObjs) {
    const auto [tag, val] = sbe::value::makeNewArray();
    auto outerArrayPtr = sbe::value::getArrayView(val);

    // TODO: SERVER-69566. Use makeArray.
    for (size_t i = 0; i < inputObjs.size(); i++) {
        const auto [tag1, val1] = sbe::value::makeNewArray();
        auto innerArrayPtr = sbe::value::getArrayView(val1);

        // Add record id.
        const auto [recordTag, recordVal] = sbe::value::makeNewRecordId(i);
        innerArrayPtr->push_back(recordTag, recordVal);

        const BSONObj& bsonObj = inputObjs.at(i);
        const auto [tag2, val2] =
            sbe::value::copyValue(sbe::value::TypeTags::bsonObject,
                                  sbe::value::bitcastFrom<const char*>(bsonObj.objdata()));
        innerArrayPtr->push_back(tag2, val2);

        outerArrayPtr->push_back(tag1, val1);
    }

    return make<Constant>(tag, val);
}

std::vector<BSONObj> runSBEAST(OperationContext* opCtx,
                               const std::string& pipelineStr,
                               const std::vector<BSONObj>& inputObjs,
                               OptPhaseManager::PhasesAndRewrites phasesAndRewrites,
                               size_t maxFilterDepth) {
    auto prefixId = PrefixId::createForTests();
    Metadata metadata{{}};

    auto pipeline =
        parsePipeline(pipelineStr, NamespaceString::createNamespaceString_forTest("test"), opCtx);

    ABT valueArray = createValueArray(inputObjs);

    const ProjectionName scanProjName = prefixId.getNextId("scan");
    QueryParameterMap qp;
    ABT tree = translatePipelineToABT(metadata,
                                      *pipeline.get(),
                                      scanProjName,
                                      make<ValueScanNode>(ProjectionNameVector{scanProjName},
                                                          boost::none,
                                                          std::move(valueArray),
                                                          true /*hasRID*/),
                                      prefixId,
                                      qp,
                                      maxFilterDepth);

    OPTIMIZER_DEBUG_LOG(
        6264807, 5, "SBE translated ABT", "explain"_attr = ExplainGenerator::explainV2(tree));

    auto phaseManager = makePhaseManager(std::move(phasesAndRewrites),
                                         prefixId,
                                         {{}},
                                         boost::none /*costModel*/,
                                         DebugInfo::kDefaultForTests);

    PlanAndProps planAndProps = phaseManager.optimizeAndReturnProps(std::move(tree));

    OPTIMIZER_DEBUG_LOG(6264808,
                        5,
                        "SBE optimized ABT",
                        "explain"_attr = ExplainGenerator::explainV2(planAndProps._node));

    SlotVarMap map;
    boost::optional<sbe::value::SlotId> ridSlot;
    auto runtimeEnv = std::make_unique<sbe::RuntimeEnvironment>();
    sbe::value::SlotIdGenerator ids;
    sbe::InputParamToSlotMap inputParamToSlotMap;

    auto env = VariableEnvironment::build(planAndProps._node);
    SBENodeLowering g{
        env, *runtimeEnv, ids, inputParamToSlotMap, phaseManager.getMetadata(), planAndProps._map};
    auto sbePlan = g.optimize(planAndProps._node, map, ridSlot);
    ASSERT_EQ(1, map.size());
    tassert(6624260, "Unexpected rid slot", !ridSlot);
    uassert(6624249, "Cannot optimize SBE plan", sbePlan != nullptr);

    sbe::CompileCtx ctx(std::move(runtimeEnv));
    sbePlan->prepare(ctx);

    std::vector<sbe::value::SlotAccessor*> accessors;
    for (auto& [name, slot] : map) {
        accessors.emplace_back(sbePlan->getAccessor(ctx, slot));
    }
    // For now assert we only have one final projection.
    ASSERT_EQ(1, accessors.size());

    sbePlan->attachToOperationContext(opCtx);
    sbePlan->open(false);

    std::vector<BSONObj> results;
    while (sbePlan->getNext() != sbe::PlanState::IS_EOF) {
        if (results.size() > 1000) {
            uasserted(6624250, "Too many results!");
        }

        std::ostringstream os;
        os << accessors.at(0)->getViewOfValue();
        results.push_back(fromjson(os.str()));
    };
    sbePlan->close();

    return results;
}

std::vector<BSONObj> runSBEAST(OperationContext* opCtx,
                               const std::string& pipelineStr,
                               const std::vector<BSONObj>& inputObjs) {
    OPTIMIZER_DEBUG_LOG(
        8197301, 5, "Run test with default rewrites", "pipeline"_attr = pipelineStr);
    auto resultsDefault = runSBEAST(opCtx,
                                    pipelineStr,
                                    inputObjs,
                                    OptPhaseManager::PhasesAndRewrites::getDefaultForProd(),
                                    kMaxPathConjunctionDecomposition);

    OPTIMIZER_DEBUG_LOG(8197301,
                        5,
                        "Run test again without splitting Filters or generating SargableNodes",
                        "pipeline"_attr = pipelineStr);
    auto resultsNoSargable = runSBEAST(opCtx,
                                       pipelineStr,
                                       inputObjs,
                                       OptPhaseManager::PhasesAndRewrites::getDefaultForUnindexed(),
                                       1);

    compareResults(resultsDefault, resultsNoSargable, true /*preserveFieldOrder*/);
    return resultsDefault;
}

std::vector<BSONObj> runPipeline(OperationContext* opCtx,
                                 const std::string& pipelineStr,
                                 const std::vector<BSONObj>& inputObjs) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test");
    std::unique_ptr<mongo::Pipeline, mongo::PipelineDeleter> pipeline =
        parsePipeline(pipelineStr, nss, opCtx);

    const auto queueStage = DocumentSourceQueue::create(pipeline->getContext());
    for (const auto& bsonObj : inputObjs) {
        queueStage->emplace_back(Document{bsonObj});
    }

    pipeline->addInitialSource(queueStage);

    boost::intrusive_ptr<ExpressionContext> expCtx;
    expCtx.reset(new ExpressionContext(opCtx, nullptr, nss));

    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> planExec =
        plan_executor_factory::make(expCtx, std::move(pipeline));

    std::vector<BSONObj> results;
    bool done = false;
    while (!done) {
        BSONObj outObj;
        auto result = planExec->getNext(&outObj, nullptr);
        switch (result) {
            case PlanExecutor::ADVANCED:
                results.push_back(outObj);
                break;
            case PlanExecutor::IS_EOF:
                done = true;
                break;

            default:
                MONGO_UNREACHABLE;
        }
    }

    return results;
}

}  // namespace mongo::optimizer
