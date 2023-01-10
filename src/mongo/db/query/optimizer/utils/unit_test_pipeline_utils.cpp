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

#include "mongo/db/pipeline/abt/document_source_visitor.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/optimizer/explain.h"
#include "mongo/db/query/optimizer/rewrites/const_eval.h"
#include "mongo/db/query/optimizer/utils/unit_test_utils.h"
#include "mongo/unittest/temp_dir.h"


namespace mongo::optimizer {
namespace {
std::unique_ptr<unittest::TempDir> tempDir;

MONGO_INITIALIZER_WITH_PREREQUISITES(ABTPipelineTestInitTempDir, ("SetTempDirDefaultRoot"))
(InitializerContext*) {
    if (!tempDir) {
        tempDir = std::make_unique<unittest::TempDir>("ABTPipelineTest");
    }
}
}  // namespace

std::unique_ptr<mongo::Pipeline, mongo::PipelineDeleter> parsePipeline(
    const NamespaceString& nss,
    const std::string& inputPipeline,
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

    ctx->tempDir = tempDir->path();

    return Pipeline::parse(request.getPipeline(), ctx);
}

ABT translatePipeline(const Metadata& metadata,
                      const std::string& pipelineStr,
                      ProjectionName scanProjName,
                      std::string scanDefName,
                      PrefixId& prefixId,
                      const std::vector<ExpressionContext::ResolvedNamespace>& involvedNss) {
    auto opCtx = cc().makeOperationContext();
    auto pipeline =
        parsePipeline(NamespaceString("a." + scanDefName), pipelineStr, *opCtx, involvedNss);
    return translatePipelineToABT(metadata,
                                  *pipeline.get(),
                                  scanProjName,
                                  make<ScanNode>(scanProjName, std::move(scanDefName)),
                                  prefixId);
}

void serializeOptPhases(std::ostream& stream, opt::unordered_set<OptPhase> phaseSet) {
    // The order of phases in the golden file must be the same every time the test is run.
    std::set<std::string> orderedPhases;
    for (const auto& phase : phaseSet) {
        orderedPhases.insert(OptPhaseEnum::toString[static_cast<int>(phase)]);
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
    stream << baseTabs << "\tdistribution type: "
           << DistributionTypeEnum::toString[static_cast<int>(distributionAndPaths._type)]
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

                stream << "\t\t\t\t\t\tcollation op: "
                       << CollationOpEnum::toString[static_cast<int>(indexCollationEntry._op)]
                       << std::endl;
            }

            stream << "\t\t\t\t\tversion: " << indexDef.getVersion() << std::endl;
            stream << "\t\t\t\t\tordering bits: " << indexDef.getOrdering() << std::endl;
            stream << "\t\t\t\t\tis multi-key: " << indexDef.isMultiKey() << std::endl;

            serializeDistributionAndPaths(stream, indexDef.getDistributionAndPaths(), "\t\t\t\t\t");

            std::string serializedReqMap =
                ExplainGenerator::explainPartialSchemaReqMap(indexDef.getPartialReqMap());
            explainPreserveIndentation(stream, "\t\t\t\t\t", serializedReqMap);
        }

        stream << "\t\t\tcollection exists: " << scanDef.exists() << std::endl;
        stream << "\t\t\tCE type: " << scanDef.getCE() << std::endl;
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

std::string ABTGoldenTestFixture::testABTTranslationAndOptimization(
    const std::string& variationName,
    const std::string& pipelineStr,
    std::string scanDefName,
    opt::unordered_set<OptPhase> phaseSet,
    Metadata metadata,
    PathToIntervalFn pathToInterval,
    bool phaseManagerDisableScan,
    const std::vector<ExpressionContext::ResolvedNamespace>& involvedNss) {
    bool optimizePipeline = !phaseSet.empty();

    std::ostream& stream = _ctx->outStream();
    stream << "==== VARIATION: " << variationName << " ====" << std::endl;
    stream << "-- INPUTS:" << std::endl;
    stream << "pipeline: " << pipelineStr << std::endl;

    serializeMetadata(stream, metadata);
    if (optimizePipeline) {
        serializeOptPhases(stream, phaseSet);
    }

    stream << std::endl << "-- OUTPUT:" << std::endl;

    auto prefixId = PrefixId::createForTests();
    ABT translated = translatePipeline(
        metadata, pipelineStr, prefixId.getNextId("scan"), scanDefName, prefixId, involvedNss);

    std::string explained;
    if (optimizePipeline) {
        ABT optimized = optimizeABT(translated,
                                    phaseSet,
                                    metadata,
                                    // TODO SERVER-71554
                                    getPipelineTestDefaultCoefficients(),
                                    pathToInterval,
                                    phaseManagerDisableScan);
        explained = ExplainGenerator::explainV2(optimized);
    } else {
        explained = ExplainGenerator::explainV2(translated);
    }

    stream << explained << std::endl << std::endl;
    return explained;
}
}  // namespace mongo::optimizer
