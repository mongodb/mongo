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

#include "mongo/db/query/compiler/stats/maxdiff_test_utils.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/document_source_queue.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <utility>

namespace mongo::stats {

static std::vector<BSONObj> convertToBSON(const std::vector<SBEValue>& input) {
    std::vector<BSONObj> result;

    for (size_t i = 0; i < input.size(); i++) {
        const auto [objTag, objVal] = sbe::value::makeNewObject();
        sbe::value::ValueGuard vg(objTag, objVal);

        const auto [tag, val] = input[i].get();
        // Copy the value because objVal owns its value, and the ValueGuard releases not only
        // objVal, but also its Value (in the case below - copyVal).
        const auto [copyTag, copyVal] = sbe::value::copyValue(tag, val);
        sbe::value::getObjectView(objVal)->push_back("a", copyTag, copyVal);

        std::ostringstream os;
        os << std::make_pair(objTag, objVal);
        result.push_back(fromjson(os.str()));
    }

    return result;
}

std::unique_ptr<mongo::Pipeline> parsePipeline(const std::vector<BSONObj>& rawPipeline,
                                               NamespaceString nss,
                                               OperationContext* opCtx) {
    AggregateCommandRequest request(std::move(nss), rawPipeline);
    boost::intrusive_ptr<ExpressionContextForTest> ctx(
        new ExpressionContextForTest(opCtx, request));

    static unittest::TempDir tempDir("ABTPipelineTest");
    ctx->setTempDir(tempDir.path());

    return Pipeline::parse(request.getPipeline(), ctx);
}

std::unique_ptr<mongo::Pipeline> parsePipeline(const std::string& pipelineStr,
                                               NamespaceString nss,
                                               OperationContext* opCtx) {
    const BSONObj inputBson = fromjson("{pipeline: " + pipelineStr + "}");

    std::vector<BSONObj> rawPipeline;
    for (auto&& stageElem : inputBson["pipeline"].Array()) {
        ASSERT_EQUALS(stageElem.type(), BSONType::object);
        rawPipeline.push_back(stageElem.embeddedObject());
    }
    return parsePipeline(rawPipeline, std::move(nss), opCtx);
}

std::vector<BSONObj> runPipeline(OperationContext* opCtx,
                                 const std::string& pipelineStr,
                                 const std::vector<BSONObj>& inputObjs) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test");
    std::unique_ptr<mongo::Pipeline> pipeline = parsePipeline(pipelineStr, nss, opCtx);

    const auto queueStage = DocumentSourceQueue::create(pipeline->getContext());
    for (const auto& bsonObj : inputObjs) {
        queueStage->emplace_back(Document{bsonObj});
    }

    pipeline->addInitialSource(queueStage);

    auto expCtx = ExpressionContextBuilder{}.opCtx(opCtx).ns(nss).build();
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

size_t getActualCard(OperationContext* opCtx,
                     const std::vector<SBEValue>& input,
                     const std::string& query) {
    return runPipeline(opCtx, query, convertToBSON(input)).size();
}

std::string printValueArray(const std::vector<SBEValue>& values) {
    std::stringstream strStream;
    for (size_t i = 0; i < values.size(); ++i) {
        strStream << " " << values[i].get();
    }
    return strStream.str();
}

}  // namespace mongo::stats
