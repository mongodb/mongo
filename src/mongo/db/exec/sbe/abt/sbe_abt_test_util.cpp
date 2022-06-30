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

#include "mongo/db/commands/cqf/cqf_command_utils.h"
#include "mongo/db/exec/sbe/abt/abt_lower.h"
#include "mongo/db/pipeline/abt/document_source_visitor.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/document_source_queue.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/optimizer/explain.h"
#include "mongo/db/query/optimizer/opt_phase_manager.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/temp_dir.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::optimizer {

std::unique_ptr<mongo::Pipeline, mongo::PipelineDeleter> parsePipeline(
    const std::string& pipelineStr, NamespaceString nss, OperationContext* opCtx) {
    const BSONObj inputBson = fromjson("{pipeline: " + pipelineStr + "}");

    std::vector<BSONObj> rawPipeline;
    for (auto&& stageElem : inputBson["pipeline"].Array()) {
        ASSERT_EQUALS(stageElem.type(), BSONType::Object);
        rawPipeline.push_back(stageElem.embeddedObject());
    }

    AggregateCommandRequest request(std::move(nss), rawPipeline);
    boost::intrusive_ptr<ExpressionContextForTest> ctx(
        new ExpressionContextForTest(opCtx, request));

    unittest::TempDir tempDir("ABTPipelineTest");
    ctx->tempDir = tempDir.path();

    return Pipeline::parse(request.getPipeline(), ctx);
}

ABT createValueArray(const std::vector<std::string>& jsonVector) {
    const auto [tag, val] = sbe::value::makeNewArray();
    auto outerArrayPtr = sbe::value::getArrayView(val);

    for (const std::string& s : jsonVector) {
        const auto [tag1, val1] = sbe::value::makeNewArray();
        auto innerArrayPtr = sbe::value::getArrayView(val1);

        const BSONObj& bsonObj = fromjson(s);
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
                               const std::vector<std::string>& jsonVector) {
    PrefixId prefixId;
    Metadata metadata{{}};

    auto pipeline = parsePipeline(pipelineStr, NamespaceString("test"), opCtx);

    ABT tree = createValueArray(jsonVector);

    const ProjectionName scanProjName = prefixId.getNextId("scan");
    tree = translatePipelineToABT(
        metadata,
        *pipeline.get(),
        scanProjName,
        make<ValueScanNode>(ProjectionNameVector{scanProjName}, std::move(tree)),
        prefixId);

    OPTIMIZER_DEBUG_LOG(
        6264807, 5, "SBE translated ABT", "explain"_attr = ExplainGenerator::explainV2(tree));

    OptPhaseManager phaseManager(
        OptPhaseManager::getAllRewritesSet(), prefixId, {{}}, DebugInfo::kDefaultForTests);
    ASSERT_TRUE(phaseManager.optimize(tree));

    OPTIMIZER_DEBUG_LOG(
        6264808, 5, "SBE optimized ABT", "explain"_attr = ExplainGenerator::explainV2(tree));

    SlotVarMap map;
    sbe::value::SlotIdGenerator ids;

    auto env = VariableEnvironment::build(tree);
    SBENodeLowering g{env,
                      map,
                      ids,
                      phaseManager.getMetadata(),
                      phaseManager.getNodeToGroupPropsMap(),
                      phaseManager.getRIDProjections()};
    auto sbePlan = g.optimize(tree);
    uassert(6624249, "Cannot optimize SBE plan", sbePlan != nullptr);

    sbe::CompileCtx ctx(std::make_unique<sbe::RuntimeEnvironment>());
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

std::vector<BSONObj> runPipeline(OperationContext* opCtx,
                                 const std::string& pipelineStr,
                                 const std::vector<std::string>& jsonVector) {
    NamespaceString nss("test");
    std::unique_ptr<mongo::Pipeline, mongo::PipelineDeleter> pipeline =
        parsePipeline(pipelineStr, nss, opCtx);

    const auto queueStage = DocumentSourceQueue::create(pipeline->getContext());
    for (const std::string& s : jsonVector) {
        BSONObj bsonObj = fromjson(s);
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
