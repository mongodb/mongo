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

#include "mongo/db/query/stats/maxdiff_test_utils.h"

#include <map>
#include <memory>
#include <ostream>
#include <utility>

#include <absl/container/node_hash_map.h>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/document_source_queue.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/ce/histogram_common.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/query/stats/ce_histogram.h"
#include "mongo/db/query/stats/max_diff.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/temp_dir.h"

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

size_t getActualCard(OperationContext* opCtx,
                     const std::vector<SBEValue>& input,
                     const std::string& query) {
    return runPipeline(opCtx, query, convertToBSON(input)).size();
}

ScalarHistogram makeHistogram(std::vector<SBEValue>& randData, size_t nBuckets) {
    sortValueVector(randData);
    const DataDistribution& dataDistrib = getDataDistribution(randData);
    return genMaxDiffHistogram(dataDistrib, nBuckets);
}

std::string printValueArray(const std::vector<SBEValue>& values) {
    std::stringstream strStream;
    for (size_t i = 0; i < values.size(); ++i) {
        strStream << " " << values[i].get();
    }
    return strStream.str();
}

std::string plotArrayEstimator(const CEHistogram& estimator, const std::string& header) {
    std::ostringstream os;
    os << header << "\n";
    if (!estimator.getScalar().empty()) {
        os << "Scalar histogram:\n" << estimator.getScalar().plot();
    }
    if (!estimator.getArrayUnique().empty()) {
        os << "Array unique histogram:\n" << estimator.getArrayUnique().plot();
    }
    if (!estimator.getArrayMin().empty()) {
        os << "Array min histogram:\n" << estimator.getArrayMin().plot();
    }
    if (!estimator.getArrayMax().empty()) {
        os << "Array max histogram:\n" << estimator.getArrayMax().plot();
    }
    if (!estimator.getTypeCounts().empty()) {
        os << "Per scalar data type value counts: ";
        for (auto tagCount : estimator.getTypeCounts()) {
            os << tagCount.first << "=" << tagCount.second << " ";
        }
    }
    if (!estimator.getArrayTypeCounts().empty()) {
        os << "\nPer array data type value counts: ";
        for (auto tagCount : estimator.getArrayTypeCounts()) {
            os << tagCount.first << "=" << tagCount.second << " ";
        }
    }
    if (estimator.isArray()) {
        os << "\nEmpty array count: " << estimator.getEmptyArrayCount();
    }
    os << "\n";

    return os.str();
}

}  // namespace mongo::stats
