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

#include "mongo/db/pipeline/lite_parsed_score_fusion_desugarer_utils.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/pipeline/document_source_hybrid_scoring_util.h"
#include "mongo/db/pipeline/lite_parsed_hybrid_search_desugarer_utils.h"
#include "mongo/util/assert_util.h"

#include <string_view>
#include <utility>

#include <fmt/format.h>

namespace mongo::lite_parsed_hybrid_search_desugarer::score_fusion_utils {
using namespace std::literals::string_view_literals;

ScoreFusionScoringOptions::ScoreFusionScoringOptions(const ScoreFusionSpec& spec) {
    _normalizationMethod = spec.getInput().getNormalization();
    const auto& combination = spec.getCombination();
    ScoreFusionCombinationMethodEnum combinationMethod = ScoreFusionCombinationMethodEnum::kAvg;
    boost::optional<IDLAnyType> combinationExpression = boost::none;
    if (combination.has_value() && combination->getMethod().has_value()) {
        combinationMethod = combination->getMethod().get();
        uassert(12559406,
                "combination.expression should only be specified when combination.method "
                "has the value \"expression\"",
                (combinationMethod != ScoreFusionCombinationMethodEnum::kExpression &&
                 !combination->getExpression().has_value()) ||
                    (combinationMethod == ScoreFusionCombinationMethodEnum::kExpression &&
                     combination->getExpression().has_value()));
        combinationExpression = combination->getExpression();
        uassert(12559407,
                "combination.expression and combination.weights cannot both be specified",
                !(combination->getWeights().has_value() && combinationExpression.has_value()));
    }
    _combinationMethod = std::move(combinationMethod);
    _combinationExpression = std::move(combinationExpression);
}

std::string_view ScoreFusionScoringOptions::getNormalizationString() const {
    switch (_normalizationMethod) {
        case ScoreFusionNormalizationEnum::kSigmoid:
            return "sigmoid"sv;
        case ScoreFusionNormalizationEnum::kMinMaxScaler:
            return "minMaxScaler"sv;
        case ScoreFusionNormalizationEnum::kNone:
            return "none"sv;
    }
    MONGO_UNREACHABLE_TASSERT(12559408);
}

std::string_view ScoreFusionScoringOptions::getCombinationMethodString() const {
    switch (_combinationMethod) {
        case ScoreFusionCombinationMethodEnum::kExpression:
            return "custom expression"sv;
        case ScoreFusionCombinationMethodEnum::kAvg:
            return "average"sv;
    }
    MONGO_UNREACHABLE_TASSERT(12559409);
}

BSONObj buildScoreAddFieldsBson(std::string_view inputPipelineName,
                                ScoreFusionNormalizationEnum normalization,
                                double weight) {
    BSONObjBuilder bob;
    {
        BSONObjBuilder addFieldsBob(bob.subobjStart("$addFields"sv));
        const std::string scoreField = hybrid_scoring_util::applyInternalFieldPrefixToFieldName(
            kInternalFieldsName, fmt::format("{}_score", inputPipelineName));
        BSONObjBuilder scoreBob(addFieldsBob.subobjStart(scoreField));
        BSONArrayBuilder multArr(scoreBob.subarrayStart("$multiply"sv));
        BSONObj scorePath = BSON("$meta" << "score");
        switch (normalization) {
            case ScoreFusionNormalizationEnum::kSigmoid:
                multArr.append(BSON("$sigmoid" << scorePath));
                break;
            case ScoreFusionNormalizationEnum::kMinMaxScaler:
            case ScoreFusionNormalizationEnum::kNone:
                multArr.append(scorePath);
                break;
        }
        multArr.append(weight);
    }
    return bob.obj();
}

BSONObj buildRawScoreAddFieldsBson(std::string_view inputPipelineName) {
    BSONObjBuilder bob;
    {
        BSONObjBuilder addFieldsBob(bob.subobjStart("$addFields"sv));
        const std::string rawScoreField = hybrid_scoring_util::applyInternalFieldPrefixToFieldName(
            kInternalFieldsName, fmt::format("{}_rawScore", inputPipelineName));
        addFieldsBob.append(rawScoreField, BSON("$meta" << "score"));
    }
    return bob.obj();
}

BSONObj buildAddInputPipelineScoreDetailsBson(std::string_view inputPipelineName,
                                              bool inputGeneratesScoreDetails) {
    BSONObjBuilder bob;
    {
        BSONObjBuilder addFieldsBob(bob.subobjStart("$addFields"sv));
        const std::string scoreDetailsField =
            hybrid_scoring_util::applyInternalFieldPrefixToFieldName(
                kInternalFieldsName, fmt::format("{}_scoreDetails", inputPipelineName));
        if (inputGeneratesScoreDetails) {
            addFieldsBob.append(scoreDetailsField,
                                BSON("details" << BSON("$meta" << "scoreDetails")));
        } else {
            addFieldsBob.append(scoreDetailsField, BSON("details" << BSONArrayBuilder().arr()));
        }
    }
    return bob.obj();
}

BSONObj buildMinMaxScalerSetWindowFieldsBson(std::string_view inputPipelineName) {
    const std::string internalFieldsScore =
        hybrid_scoring_util::applyInternalFieldPrefixToFieldName(
            kInternalFieldsName,
            hybrid_scoring_util::getScoreFieldFromPipelineName(inputPipelineName));
    BSONObjBuilder bob;
    {
        BSONObjBuilder swfBob(bob.subobjStart("$_internalSetWindowFields"sv));
        swfBob.append("sortBy", BSON(internalFieldsScore << -1));
        BSONObjBuilder outputBob(swfBob.subobjStart("output"sv));
        outputBob.append(internalFieldsScore,
                         BSON("$minMaxScaler" << BSON("input" << ("$" + internalFieldsScore))));
    }
    return bob.obj();
}

BSONObj buildSetFinalCombinedScoreBson(const std::vector<std::string>& pipelineNames,
                                       const ScoreFusionScoringOptions& scoringOptions) {
    BSONObjBuilder bob;
    {
        BSONObjBuilder smBob(bob.subobjStart("$setMetadata"sv));
        BSONObjBuilder scoreBob(smBob.subobjStart("score"sv));
        switch (scoringOptions.getCombinationMethod()) {
            case ScoreFusionCombinationMethodEnum::kExpression: {
                BSONObjBuilder letBob(scoreBob.subobjStart("$let"sv));
                BSONObjBuilder varsBob(letBob.subobjStart("vars"sv));
                for (const auto& pipelineName : pipelineNames) {
                    const std::string scoreField =
                        hybrid_scoring_util::applyInternalFieldPrefixToFieldName(
                            kInternalFieldsName,
                            hybrid_scoring_util::getScoreFieldFromPipelineName(pipelineName));
                    varsBob.append(pipelineName, fmt::format("${}", scoreField));
                }
                varsBob.done();
                scoringOptions.getCombinationExpression()->serializeToBSON("in", &letBob);
                letBob.done();
                break;
            }
            case ScoreFusionCombinationMethodEnum::kAvg: {
                BSONArrayBuilder avgArr(scoreBob.subarrayStart("$avg"sv));
                for (const auto& pipelineName : pipelineNames) {
                    avgArr.append(fmt::format(
                        "${}",
                        hybrid_scoring_util::applyInternalFieldPrefixToFieldName(
                            kInternalFieldsName,
                            hybrid_scoring_util::getScoreFieldFromPipelineName(pipelineName))));
                }
                break;
            }
        }
    }
    return bob.obj();
}

BSONObj buildCalculatedFinalScoreDetailsBson(const std::vector<std::string>& pipelineNames,
                                             const StringMap<double>& weights) {
    BSONObjBuilder bob;
    {
        BSONObjBuilder addFieldsBob(bob.subobjStart("$addFields"sv));
        BSONObjBuilder internalFieldsBob(addFieldsBob.subobjStart(kInternalFieldsName));
        BSONArrayBuilder calcArr(internalFieldsBob.subarrayStart("calculatedScoreDetails"sv));
        for (const auto& pipelineName : pipelineNames) {
            const std::string internalFieldsPipelineName =
                hybrid_scoring_util::applyInternalFieldPrefixToFieldName(kInternalFieldsName,
                                                                         pipelineName);
            double weight = hybrid_scoring_util::getPipelineWeight(weights, pipelineName);

            BSONObjBuilder mergeSub;
            mergeSub.append("inputPipelineName"sv, pipelineName);
            mergeSub.append("inputPipelineRawScore"sv,
                            fmt::format("${}_rawScore", internalFieldsPipelineName));
            mergeSub.append("weight"sv, weight);
            mergeSub.append("value"sv, fmt::format("${}_score", internalFieldsPipelineName));

            BSONArrayBuilder mergeArr;
            mergeArr.append(mergeSub.obj());
            mergeArr.append(fmt::format("${}.{}_scoreDetails", kInternalFieldsName, pipelineName));
            calcArr.append(BSON("$mergeObjects" << mergeArr.arr()));
        }
    }
    return bob.obj();
}

BSONObj buildSetMetadataScoreDetailsBson(const ScoreFusionScoringOptions& scoringOptions) {
    BSONObjBuilder bob;
    {
        BSONObjBuilder smBob(bob.subobjStart("$setMetadata"sv));
        BSONObjBuilder sdBob(smBob.subobjStart("scoreDetails"sv));
        sdBob.append("value", BSON("$meta" << "score"));
        sdBob.append("description", kScoreDetailsDescription);
        sdBob.append("normalization", scoringOptions.getNormalizationString());
        BSONObjBuilder combinationBob(sdBob.subobjStart("combination"sv));
        combinationBob.append("method", scoringOptions.getCombinationMethodString());
        if (scoringOptions.getCombinationMethod() ==
            ScoreFusionCombinationMethodEnum::kExpression) {
            combinationBob.append("expression",
                                  hybrid_scoring_util::score_details::stringifyExpression(
                                      scoringOptions.getCombinationExpression()));
        }
        combinationBob.done();
        sdBob.append("details",
                     fmt::format("${}",
                                 hybrid_scoring_util::applyInternalFieldPrefixToFieldName(
                                     kInternalFieldsName, "calculatedScoreDetails")));
    }
    return bob.obj();
}

StageSpecs buildScoreFusionInputPipelinePreamble(const NamespaceString& nss,
                                                 const LiteParsedPipeline& subPipeline,
                                                 const std::string& pipelineName,
                                                 double weight,
                                                 ScoreFusionNormalizationEnum normalization,
                                                 bool includeScoreDetails) {
    StageSpecs out;
    out.reserve(subPipeline.getStages().size() + 5);
    for (const auto& stage : subPipeline.getStages()) {
        out.push_back(stage->clone());
    }

    const bool inputGeneratesScoreDetails = subPipeline.isScoreDetailsPipeline();

    out.push_back(
        common_utils::parseOwnedStage(nss, common_utils::buildReplaceRootBson(kDocsName)));

    out.push_back(common_utils::parseOwnedStage(
        nss, buildScoreAddFieldsBson(pipelineName, normalization, weight)));

    if (includeScoreDetails) {
        out.push_back(common_utils::parseOwnedStage(nss, buildRawScoreAddFieldsBson(pipelineName)));
        out.push_back(common_utils::parseOwnedStage(
            nss, buildAddInputPipelineScoreDetailsBson(pipelineName, inputGeneratesScoreDetails)));
    }

    if (normalization == ScoreFusionNormalizationEnum::kMinMaxScaler) {
        out.push_back(
            common_utils::parseOwnedStage(nss, buildMinMaxScalerSetWindowFieldsBson(pipelineName)));
    }

    return out;
}

}  // namespace mongo::lite_parsed_hybrid_search_desugarer::score_fusion_utils
