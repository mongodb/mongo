// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/lite_parsed_rank_fusion_desugarer_utils.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/pipeline/document_source_hybrid_scoring_util.h"
#include "mongo/db/pipeline/lite_parsed_hybrid_search_desugarer_utils.h"

#include <string_view>

#include <fmt/format.h>

namespace mongo::lite_parsed_hybrid_search_desugarer::rank_fusion_utils {
using namespace std::literals::string_view_literals;

BSONObj buildSetWindowFieldsBson(const std::string& rankFieldName) {
    BSONObjBuilder bob;
    {
        BSONObjBuilder swfBob(bob.subobjStart("$_internalSetWindowFields"sv));
        swfBob.append("sortBy", BSON("order" << 1));
        BSONObjBuilder outputBob(swfBob.subobjStart("output"sv));
        outputBob.append(rankFieldName, BSON("$rank" << BSONObj()));
    }
    return bob.obj();
}

BSONObj buildScoreAddFieldsBson(std::string_view inputPipelineName,
                                int rankConstant,
                                double weight) {
    BSONObjBuilder bob;
    {
        BSONObjBuilder addFieldsBob(bob.subobjStart("$addFields"sv));
        const std::string scoreField = hybrid_scoring_util::applyInternalFieldPrefixToFieldName(
            kInternalFieldsName, fmt::format("{}_score", inputPipelineName));
        BSONObjBuilder scoreBob(addFieldsBob.subobjStart(scoreField));
        BSONArrayBuilder multArr(scoreBob.subarrayStart("$multiply"sv));
        const std::string rankPath =
            fmt::format("${}",
                        hybrid_scoring_util::applyInternalFieldPrefixToFieldName(
                            kInternalFieldsName, fmt::format("{}_rank", inputPipelineName)));
        multArr.append(BSON(
            "$divide" << BSON_ARRAY(1 << BSON("$add" << BSON_ARRAY(rankPath << rankConstant)))));
        multArr.append(weight);
    }
    return bob.obj();
}

BSONObj buildAddInputPipelineScoreDetailsBson(std::string_view inputPipelineName,
                                              bool inputGeneratesScore,
                                              bool inputGeneratesScoreDetails) {
    const std::string scoreDetailsField = hybrid_scoring_util::applyInternalFieldPrefixToFieldName(
        kInternalFieldsName, fmt::format("{}_scoreDetails", inputPipelineName));
    BSONObjBuilder bob;
    {
        BSONObjBuilder addFieldsBob(bob.subobjStart("$addFields"sv));
        if (inputGeneratesScoreDetails) {
            addFieldsBob.append(scoreDetailsField, BSON("$meta" << "scoreDetails"));
        } else if (inputGeneratesScore) {
            addFieldsBob.append(
                scoreDetailsField,
                BSON("value" << BSON("$meta" << "score") << "details" << BSONArrayBuilder().arr()));
        } else {
            addFieldsBob.append(scoreDetailsField, BSON("details" << BSONArrayBuilder().arr()));
        }
    }
    return bob.obj();
}

BSONObj buildRankAddFieldsBson(const std::vector<std::string>& pipelineNames) {
    BSONObjBuilder bob;
    {
        BSONObjBuilder addFieldsBob(bob.subobjStart("$addFields"sv));
        for (const auto& pipelineName : pipelineNames) {
            const std::string rankField = hybrid_scoring_util::applyInternalFieldPrefixToFieldName(
                kInternalFieldsName, fmt::format("{}_rank", pipelineName));
            const std::string rankPath = fmt::format("${}", rankField);
            addFieldsBob.append(rankField,
                                BSON("$cond" << BSON_ARRAY(BSON("$eq" << BSON_ARRAY(rankPath << 0))
                                                           << "NA" << rankPath)));
        }
    }
    return bob.obj();
}

BSONObj buildSetMetadataScoreBson(const std::vector<std::string>& pipelineNames) {
    BSONObjBuilder bob;
    {
        BSONObjBuilder smBob(bob.subobjStart("$setMetadata"sv));
        BSONObjBuilder scoreBob(smBob.subobjStart("score"sv));
        BSONArrayBuilder addArr(scoreBob.subarrayStart("$add"sv));
        for (const auto& pipelineName : pipelineNames) {
            addArr.append(
                fmt::format("${}",
                            hybrid_scoring_util::applyInternalFieldPrefixToFieldName(
                                kInternalFieldsName, fmt::format("{}_score", pipelineName))));
        }
    }
    return bob.obj();
}

BSONObj buildAddFieldsScoreBson(const std::vector<std::string>& pipelineNames) {
    BSONObjBuilder bob;
    {
        BSONObjBuilder afBob(bob.subobjStart("$addFields"sv));
        BSONObjBuilder scoreBob(afBob.subobjStart("score"sv));
        BSONArrayBuilder addArr(scoreBob.subarrayStart("$add"sv));
        for (const auto& pipelineName : pipelineNames) {
            addArr.append(
                fmt::format("${}",
                            hybrid_scoring_util::applyInternalFieldPrefixToFieldName(
                                kInternalFieldsName, fmt::format("{}_score", pipelineName))));
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
            // Path of the pipeline's scoreDetails subobject: <INTERNAL_FIELDS>.<p>
            const std::string internalFieldsPipelineName =
                hybrid_scoring_util::applyInternalFieldPrefixToFieldName(kInternalFieldsName,
                                                                         pipelineName);
            const std::string rankPath = fmt::format("${}_rank", internalFieldsPipelineName);
            double weight = hybrid_scoring_util::getPipelineWeight(weights, pipelineName);

            BSONObjBuilder mergeSub;
            mergeSub.append("inputPipelineName"sv, pipelineName);
            mergeSub.append("rank"sv, rankPath);
            mergeSub.append("weight",
                            BSON("$cond" << BSON_ARRAY(BSON("$eq" << BSON_ARRAY(rankPath << "NA"))
                                                       << "$$REMOVE" << weight)));

            BSONArrayBuilder mergeArr;
            mergeArr.append(mergeSub.obj());
            mergeArr.append(fmt::format("${}.{}_scoreDetails", kInternalFieldsName, pipelineName));
            calcArr.append(BSON("$mergeObjects" << mergeArr.arr()));
        }
    }
    return bob.obj();
}

BSONObj buildSetMetadataScoreDetailsBson() {
    BSONObjBuilder bob;
    {
        BSONObjBuilder smBob(bob.subobjStart("$setMetadata"sv));
        BSONObjBuilder sdBob(smBob.subobjStart("scoreDetails"sv));
        sdBob.append("value", BSON("$meta" << "score"));
        sdBob.append("description", kScoreDetailsDescription);
        sdBob.append("details",
                     fmt::format("${}",
                                 hybrid_scoring_util::applyInternalFieldPrefixToFieldName(
                                     kInternalFieldsName, "calculatedScoreDetails")));
    }
    return bob.obj();
}

BSONObj buildSortByScoreScalarBson() {
    return BSON("$sort" << BSON("score" << -1 << "_id" << 1));
}

StageSpecs buildRankFusionInputPipelinePreamble(const NamespaceString& nss,
                                                const LiteParsedPipeline& subPipeline,
                                                const std::string& pipelineName,
                                                double weight,
                                                bool includeScoreDetails) {
    StageSpecs out;
    out.reserve(subPipeline.getStages().size() + 4);
    for (const auto& stage : subPipeline.getStages()) {
        out.push_back(stage->clone());
    }

    // Rewrite rightmost $sort in-place to ensure sort-key metadata is output; no-op if no $sort
    // is present (e.g. $search and $vectorSearch emit scored output without an explicit $sort).
    common_utils::mutateRightmostSortToOutputSortKey(nss, out);

    const bool inputGeneratesScore = subPipeline.isScoredPipeline();
    const bool inputGeneratesScoreDetails = subPipeline.isScoreDetailsPipeline();

    out.push_back(
        common_utils::parseOwnedStage(nss, common_utils::buildReplaceRootBson(kDocsName)));

    const std::string rankFieldName = hybrid_scoring_util::applyInternalFieldPrefixToFieldName(
        kInternalFieldsName, fmt::format("{}_rank", pipelineName));
    out.push_back(common_utils::parseOwnedStage(nss, buildSetWindowFieldsBson(rankFieldName)));

    out.push_back(common_utils::parseOwnedStage(
        nss, buildScoreAddFieldsBson(pipelineName, kRankConstant, weight)));

    if (includeScoreDetails) {
        out.push_back(common_utils::parseOwnedStage(
            nss,
            buildAddInputPipelineScoreDetailsBson(
                pipelineName, inputGeneratesScore, inputGeneratesScoreDetails)));
    }

    return out;
}

}  // namespace mongo::lite_parsed_hybrid_search_desugarer::rank_fusion_utils
