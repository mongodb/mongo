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

#include "mongo/db/query/optimizer/utils/unit_test_pipeline_utils.h"

#include <map>
#include <ostream>
#include <set>
#include <utility>

#include <absl/container/node_hash_map.h>
#include <absl/container/node_hash_set.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/db/client.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/abt/canonical_query_translation.h"
#include "mongo/db/pipeline/abt/document_source_visitor.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/pipeline_test_util.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/cost_model/cost_model_gen.h"
#include "mongo/db/query/optimizer/explain.h"
#include "mongo/db/query/optimizer/node.h"  // IWYU pragma: keep
#include "mongo/db/query/optimizer/utils/strong_alias.h"
#include "mongo/db/query/optimizer/utils/unit_test_utils.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/util/intrusive_counter.h"

namespace mongo::optimizer {

std::unique_ptr<mongo::Pipeline, mongo::PipelineDeleter> parsePipeline(
    const NamespaceString& nss,
    StringData inputPipeline,
    OperationContext& opCtx,
    const std::vector<ExpressionContext::ResolvedNamespace>& involvedNss) {
    const BSONObj inputBson = fromjson("{pipeline: " + inputPipeline + "}");

    std::vector<BSONObj> rawPipeline;
    for (auto&& stageElem : inputBson["pipeline"].Array()) {
        ASSERT_EQUALS(stageElem.type(), BSONType::Object);
        rawPipeline.push_back(stageElem.embeddedObject());
    }

    AggregateCommandRequest request(nss, rawPipeline);
    boost::intrusive_ptr<ExpressionContextForTest> ctx(
        new ExpressionContextForTest(&opCtx, request));

    // Setup the resolved namespaces for other involved collections.
    for (const auto& resolvedNss : involvedNss) {
        ctx->setResolvedNamespace(resolvedNss.ns, resolvedNss);
    }

    static unittest::TempDir tempDir("ABTPipelineTest");
    ctx->tempDir = tempDir.path();

    return Pipeline::parse(request.getPipeline(), ctx);
}

ABT translatePipeline(const Metadata& metadata,
                      StringData pipelineStr,
                      ProjectionName scanProjName,
                      std::string scanDefName,
                      PrefixId& prefixId,
                      const std::vector<ExpressionContext::ResolvedNamespace>& involvedNss,
                      bool shouldParameterize,
                      QueryParameterMap* parameters,
                      size_t maxFilterDepth,
                      bool shouldNormalizeMatchExpr) {
    auto opCtx = cc().makeOperationContext();
    auto pipeline =
        parsePipeline(NamespaceString::createNamespaceString_forTest("a." + scanDefName),
                      pipelineStr,
                      *opCtx,
                      involvedNss);
    pipeline->optimizePipeline();

    // We normalize match expressions in the pipeline here to ensure the stability of the predicate
    // order after optimizations.
    if (shouldNormalizeMatchExpr) {
        pipeline = normalizeMatchStageInPipeline(std::move(pipeline));
    }

    if (shouldParameterize) {
        pipeline->parameterize();
    }
    QueryParameterMap qp;
    return translatePipelineToABT(metadata,
                                  *pipeline.get(),
                                  scanProjName,
                                  make<ScanNode>(scanProjName, std::move(scanDefName)),
                                  prefixId,
                                  parameters ? *parameters : qp,
                                  maxFilterDepth);
}

void serializeOptPhases(std::ostream& stream, opt::unordered_set<OptPhase> phaseSet) {
    // The order of phases in the golden file must be the same every time the test is run.
    std::set<StringData> orderedPhases;
    for (const auto& phase : phaseSet) {
        orderedPhases.insert(toStringData(phase));
    }

    stream << "optimization phases: " << std::endl;
    for (const auto& phase : orderedPhases) {
        stream << "\t" << phase << std::endl;
    }
}

void explainPreserveIndentation(std::ostream& stream, std::string baseTabs, std::string explain) {
    std::string currLine = "";
    for (char ch : explain) {
        if (ch == '\n') {
            stream << baseTabs << currLine << std::endl;
            currLine = "";
        } else {
            currLine += ch;
        }
    }
    stream << std::endl;
}

void serializeDistributionAndPaths(std::ostream& stream,
                                   DistributionAndPaths distributionAndPaths,
                                   std::string baseTabs) {
    stream << baseTabs << "distribution and paths: " << std::endl;
    stream << baseTabs << "\tdistribution type: " << toStringData(distributionAndPaths._type)
           << std::endl;
    stream << baseTabs << "\tdistribution paths: " << std::endl;
    for (const ABT& abt : distributionAndPaths._paths) {
        explainPreserveIndentation(stream, baseTabs + "\t\t", ExplainGenerator::explainV2(abt));
    }
}

void serializeMetadata(std::ostream& stream, Metadata metadata) {
    stream << "metadata: " << std::endl;

    stream << "\tnumber of partitions: " << metadata._numberOfPartitions << std::endl;

    // The ScanDefinitions are stored in an unordered map, and the order of the ScanDefinitions in
    // the golden file must be the same every time the test is run.
    std::map<std::string, ScanDefinition> orderedScanDefs;
    for (const auto& element : metadata._scanDefs) {
        orderedScanDefs.insert(element);
    }

    stream << "\tscan definitions: " << std::endl;
    for (const auto& element : orderedScanDefs) {
        stream << "\t\t" << element.first << ": " << std::endl;

        ScanDefinition scanDef = element.second;

        stream << "\t\t\toptions: " << std::endl;
        for (const auto& optionElem : scanDef.getOptionsMap()) {
            stream << "\t\t\t\t" << optionElem.first << ": " << optionElem.second << std::endl;
        }

        serializeDistributionAndPaths(stream, scanDef.getDistributionAndPaths(), "\t\t\t");

        stream << "\t\t\tindexes: " << std::endl;
        for (const auto& indexElem : scanDef.getIndexDefs()) {
            stream << "\t\t\t\t" << indexElem.first << ": " << std::endl;

            IndexDefinition indexDef = indexElem.second;

            stream << "\t\t\t\t\tcollation spec: " << std::endl;
            for (const auto& indexCollationEntry : indexDef.getCollationSpec()) {
                stream << "\t\t\t\t\t\tABT path: " << std::endl;
                explainPreserveIndentation(stream,
                                           "\t\t\t\t\t\t\t",
                                           ExplainGenerator::explainV2(indexCollationEntry._path));

                stream << "\t\t\t\t\t\tcollation op: " << toStringData(indexCollationEntry._op)
                       << std::endl;
            }

            stream << "\t\t\t\t\tversion: " << indexDef.getVersion() << std::endl;
            stream << "\t\t\t\t\tordering bits: " << indexDef.getOrdering() << std::endl;
            stream << "\t\t\t\t\tis multi-key: " << indexDef.isMultiKey() << std::endl;

            serializeDistributionAndPaths(stream, indexDef.getDistributionAndPaths(), "\t\t\t\t\t");

            std::string serializedReqMap =
                ExplainGenerator::explainPartialSchemaReqExpr(indexDef.getPartialReqMap());
            explainPreserveIndentation(stream, "\t\t\t\t\t", serializedReqMap);
        }

        stream << "\t\t\tcollection exists: " << scanDef.exists() << std::endl;
        stream << "\t\t\tCE type: ";
        if (const auto& ce = scanDef.getCE()) {
            stream << *ce << std::endl;
        } else {
            stream << "(empty)" << std::endl;
        }
    }
}

cost_model::CostModelCoefficients getPipelineTestDefaultCoefficients() {
    // These cost should reflect estimated aggregated execution time in milliseconds.
    // The coeffeicient us converts values from microseconds to milliseconds.
    cost_model::CostModelCoefficients coefficients{};
    constexpr double usToMs = 1.0e-3;
    coefficients.setDefaultStartupCost(0.000001);
    coefficients.setScanIncrementalCost(0.6 * usToMs);
    coefficients.setScanStartupCost(0.000001);
    coefficients.setIndexScanIncrementalCost(0.5 * usToMs);
    coefficients.setIndexScanStartupCost(0.000001);
    coefficients.setSeekCost(2.0 * usToMs);
    coefficients.setSeekStartupCost(0.000001);
    coefficients.setFilterIncrementalCost(0.2 * usToMs);
    coefficients.setFilterStartupCost(0.000001);
    coefficients.setEvalIncrementalCost(2.0 * usToMs);
    coefficients.setEvalStartupCost(0.000001);
    coefficients.setGroupByIncrementalCost(0.07 * usToMs);
    coefficients.setGroupByStartupCost(0.000001);

    coefficients.setUnwindIncrementalCost(0.03 * usToMs);
    coefficients.setUnwindStartupCost(0.000001);

    coefficients.setNestedLoopJoinIncrementalCost(0.2 * usToMs);
    coefficients.setNestedLoopJoinStartupCost(0.000001);

    coefficients.setHashJoinIncrementalCost(0.05 * usToMs);
    coefficients.setHashJoinStartupCost(0.000001);

    coefficients.setMergeJoinIncrementalCost(0.02 * usToMs);
    coefficients.setMergeJoinStartupCost(0.000001);

    coefficients.setUniqueIncrementalCost(0.7 * usToMs);
    coefficients.setUniqueStartupCost(0.000001);

    coefficients.setCollationIncrementalCost(2.5 * usToMs);
    coefficients.setCollationStartupCost(0.000001);

    coefficients.setCollationWithLimitIncrementalCost(1.0 * usToMs);
    coefficients.setCollationWithLimitStartupCost(0.000001);

    coefficients.setUnionIncrementalCost(0.02 * usToMs);
    coefficients.setUnionStartupCost(0.000001);

    coefficients.setExchangeIncrementalCost(0.1 * usToMs);
    coefficients.setExchangeStartupCost(0.000001);

    coefficients.setLimitSkipIncrementalCost(0.0 * usToMs);
    coefficients.setLimitSkipStartupCost(0.000001);
    return coefficients;
}

ABT optimizeABT(ABT abt,
                opt::unordered_set<OptPhase> phaseSet,
                Metadata metadata,
                cost_model::CostModelCoefficients&& costModel,
                PathToIntervalFn pathToInterval,
                bool phaseManagerDisableScan) {
    auto prefixId = PrefixId::createForTests();

    auto phaseManager = makePhaseManager(
        phaseSet, prefixId, metadata, std::move(costModel), DebugInfo::kDefaultForTests);
    if (phaseManagerDisableScan) {
        phaseManager.getHints()._disableScan = true;
    }

    ABT optimized = abt;
    phaseManager.optimize(optimized);
    return optimized;
}

void formatGoldenTestHeader(StringData variationName,
                            StringData pipelineStr,
                            StringData findCmd,
                            std::string scanDefName,
                            opt::unordered_set<OptPhase> phaseSet,
                            Metadata metadata,
                            std::ostream& stream) {

    stream << "==== VARIATION: " << variationName << " ====" << std::endl;
    stream << "-- INPUTS:" << std::endl;
    if (!findCmd.empty())
        stream << "find command: " << findCmd << std::endl;
    else
        stream << "pipeline: " << pipelineStr << std::endl;

    serializeMetadata(stream, metadata);
    if (!phaseSet.empty())  // optimize pipeline
        serializeOptPhases(stream, phaseSet);

    stream << std::endl << "-- OUTPUT:" << std::endl;
}

std::string formatGoldenTestExplain(ABT translated, std::ostream& stream) {
    auto explained = std::string{ExplainGenerator::explainV2(translated)};

    stream << explained << std::endl << std::endl;
    return explained;
}

void formatGoldenTestQueryParameters(QueryParameterMap& qp, std::ostream& stream) {
    stream << "Query parameters:" << std::endl;
    if (qp.empty()) {
        stream << "(no parameters)" << std::endl;
        return;
    }
    std::vector<int32_t> params;
    for (auto&& e : qp) {
        params.push_back(e.first);
    }
    std::sort(params.begin(), params.end());
    for (auto&& paramId : params) {
        stream << paramId << ": " << qp.at(paramId).get() << std::endl;
    }
}

std::string ABTGoldenTestFixture::testABTTranslationAndOptimization(
    StringData variationName,
    StringData pipelineStr,
    std::string scanDefName,
    opt::unordered_set<OptPhase> phaseSet,
    Metadata metadata,
    PathToIntervalFn pathToInterval,
    bool phaseManagerDisableScan,
    const std::vector<ExpressionContext::ResolvedNamespace>& involvedNss,
    bool shouldNormalizeMatchExpr) {
    auto&& stream = _ctx->outStream();

    formatGoldenTestHeader(
        variationName, pipelineStr, StringData{}, scanDefName, phaseSet, metadata, stream);

    auto prefixId = PrefixId::createForTests();
    ABT translated = translatePipeline(metadata,
                                       pipelineStr,
                                       prefixId.getNextId("scan"),
                                       scanDefName,
                                       prefixId,
                                       involvedNss,
                                       false /* shouldParameterize */,
                                       nullptr /* QueryParameterMap */,
                                       kMaxPathConjunctionDecomposition /* maxDepth */,
                                       shouldNormalizeMatchExpr);
    return formatGoldenTestExplain(!phaseSet.empty()
                                       ? optimizeABT(translated,
                                                     phaseSet,
                                                     metadata,
                                                     // TODO SERVER-71554
                                                     getPipelineTestDefaultCoefficients(),
                                                     pathToInterval,
                                                     phaseManagerDisableScan)
                                       : translated,
                                   stream);
}

/**
 * Golden test fixture to test parameterized CQ (find command) to ABT translation
 */
std::string ABTGoldenTestFixture::testParameterizedABTTranslation(StringData variationName,
                                                                  StringData findCmd,
                                                                  StringData pipelineStr,
                                                                  std::string scanDefName,
                                                                  Metadata metadata) {
    auto&& stream = _ctx->outStream();

    bool isFindCmd = findCmd != "";
    formatGoldenTestHeader(variationName,
                           isFindCmd ? "" : pipelineStr,
                           isFindCmd ? findCmd : "",
                           scanDefName,
                           {},
                           metadata,
                           stream);
    auto prefixId = PrefixId::createForTests();

    // Query is find command.
    if (isFindCmd) {
        auto opCtx = makeOperationContext();
        auto findCommand = query_request_helper::makeFromFindCommandForTests(fromjson(findCmd));
        auto cq = std::make_unique<CanonicalQuery>(
            CanonicalQueryParams{.expCtx = makeExpressionContext(opCtx.get(), *findCommand),
                                 .parsedFind = ParsedFindCommandParams{std::move(findCommand)}});
        QueryParameterMap qp;
        auto abt = translateCanonicalQueryToABT(
            metadata, *cq, ProjectionName{"test"}, make<ScanNode>("test", "test"), prefixId, qp);
        formatGoldenTestQueryParameters(qp, stream);
        return formatGoldenTestExplain(abt, stream);
    }

    // Query is aggregation pipeline.
    QueryParameterMap qp;
    ABT translated = translatePipeline(
        metadata, pipelineStr, prefixId.getNextId("scan"), scanDefName, prefixId, {}, true, &qp);
    formatGoldenTestQueryParameters(qp, stream);
    return formatGoldenTestExplain(translated, stream);
}

}  // namespace mongo::optimizer
