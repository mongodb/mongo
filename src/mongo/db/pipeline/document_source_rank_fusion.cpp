/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "expression_context.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_replace_root.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/search/document_source_vector_search.h"
#include <algorithm>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_add_fields.h"
#include "mongo/db/pipeline/document_source_geo_near.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/document_source_rank_fusion.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/document_source_union_with.h"
#include "mongo/db/pipeline/document_source_unwind.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/search/document_source_search.h"
#include "mongo/db/pipeline/search/document_source_vector_search.h"
#include "mongo/db/pipeline/search/search_helper.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/views/resolved_view.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

REGISTER_DOCUMENT_SOURCE_WITH_FEATURE_FLAG(rankFusion,
                                           DocumentSourceRankFusion::LiteParsed::parse,
                                           DocumentSourceRankFusion::createFromBson,
                                           AllowedWithApiStrict::kNeverInVersion1,
                                           feature_flags::gFeatureFlagSearchHybridScoring);

namespace {
/**
 * Checks that the input pipeline is a valid ranked pipeline. This means it is either one of
 * $search, $vectorSearch, $geoNear, $rankFusion, $scoreFusion (which have ordered output) or has an
 * explicit $sort stage. A ranked pipeline must also be a 'selection pipeline', which means no stage
 * can modify the documents in any way. Only stages that retrieve, limit, or order documents are
 * allowed.
 */
static void rankFusionPipelineValidator(const Pipeline& pipeline) {
    // Note that we don't check for $rankFusion and $scoreFusion explicitly because it will be
    // desugared by this point.
    static const std::set<StringData> implicitlyOrderedStages{
        DocumentSourceVectorSearch::kStageName,
        DocumentSourceSearch::kStageName,
        DocumentSourceGeoNear::kStageName};
    auto sources = pipeline.getSources();
    auto firstStageName = sources.front()->getSourceName();
    auto isRankedPipeline = implicitlyOrderedStages.contains(firstStageName) ||
        std::any_of(sources.begin(), sources.end(), [](auto& stage) {
                                return stage->getSourceName() == DocumentSourceSort::kStageName;
                            });
    uassert(9191100,
            str::stream()
                << "All subpipelines to the $rankFusion stage must begin with one of $search, "
                   "$vectorSearch, $geoNear, $scoreFusion, $rankFusion or have a custom $sort in "
                   "the pipeline.",
            isRankedPipeline);

    std::for_each(sources.begin(), sources.end(), [](auto& stage) {
        if (stage->getSourceName() == DocumentSourceGeoNear::kStageName) {
            uassert(9191101,
                    str::stream() << "$geoNear can be used in a $rankFusion subpipeline but not "
                                     "when includeLocs or distanceField is specified because they "
                                     "modify the documents by adding an output field. Only stages "
                                     "that retrieve, limit, or order documents are allowed.",
                    stage->constraints().noFieldModifications);
        } else if (stage->getSourceName() == DocumentSourceSearch::kStageName) {
            uassert(
                9191102,
                str::stream()
                    << "$search can be used in a $rankFusion subpipeline but not when "
                       "returnStoredSource is set to true because it modifies the output fields. "
                       "Only stages that retrieve, limit, or order documents are allowed.",
                stage->constraints().noFieldModifications);
        } else {
            uassert(9191103,
                    str::stream() << stage->getSourceName()
                                  << " is not allowed in a $rankFusion subpipeline because it "
                                     "modifies the documents or transforms their fields. Only "
                                     "stages that retrieve, limit, or order documents are allowed.",
                    stage->constraints().noFieldModifications);
        }
    });
}
}  // namespace

std::unique_ptr<DocumentSourceRankFusion::LiteParsed> DocumentSourceRankFusion::LiteParsed::parse(
    const NamespaceString& nss, const BSONElement& spec) {
    std::vector<LiteParsedPipeline> liteParsedPipelines;

    auto parsedSpec = RankFusionSpec::parse(IDLParserContext(kStageName), spec.embeddedObject());

    // Ensure that all pipelines are valid ranked selection pipelines.
    for (const auto& input : parsedSpec.getInputs()) {
        liteParsedPipelines.emplace_back(LiteParsedPipeline(nss, input.getPipeline()));
    }

    return std::make_unique<DocumentSourceRankFusion::LiteParsed>(
        spec.fieldName(), nss, std::move(liteParsedPipelines));
}
BSONObj group() {
    return fromjson(R"({
        $group: {
            _id: null,
            docs: { $push: "$$ROOT" }
        }
    })");
}

BSONObj unwind(const std::string& prefix) {
    std::string rank = prefix + "_rank";
    return fromjson(R"({
        $unwind: {
            path: "$docs", includeArrayIndex: ")" +
                    rank + R"("
        }
    })");
}

// RRF Score = 1 divided by (rank + rank constant).
BSONObj addScoreField(const std::string& prefix, const int& rankConstant) {
    std::string score = prefix + "_score";
    std::string rank = "$" + prefix + "_rank";
    return BSON("$addFields" << BSON(
                    score << BSON("$divide" << BSON_ARRAY(
                                      1 << BSON("$add" << BSON_ARRAY(rank << rankConstant))))));
}

std::list<boost::intrusive_ptr<DocumentSource>> buildFirstPipelineStages(
    const std::string& prefixOne,
    const int& rankConstant,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    auto groupStage = DocumentSourceGroup::createFromBson(group().firstElement(), expCtx);

    auto unwindStage =
        DocumentSourceUnwind::createFromBson(unwind(prefixOne).firstElement(), expCtx);

    auto addFields = DocumentSourceAddFields::createFromBson(
        addScoreField(prefixOne, rankConstant).firstElement(), expCtx);
    return {groupStage, unwindStage, addFields};
}

BSONObj groupEachScore(const std::string& prefixOne, const std::string& prefixTwo) {
    std::string scoreOne = prefixOne + "_score";
    std::string scoreTwo = prefixTwo + "_score";
    return fromjson(R"({
        $group: {
                _id: "$docs._id",
                docs: {$first: "$docs"},
                )" + scoreOne +
                    R"(: {$max: {$ifNull: ["$)" + scoreOne + R"(", 0]}},
                )" + scoreTwo +
                    R"(: {$max: {$ifNull: ["$)" + scoreTwo + R"(", 0]}}
            }
    })");
}

BSONObj calculateFinalScore(const std::string& prefixOne, const std::string& prefixTwo) {
    BSONObj finalScore = fromjson(R"({
        $addFields: {
            score: {
                $add: ["$)" + prefixOne +
                                  R"(_score", "$)" + prefixTwo + R"(_score"]
            }
        }
    })");
    return finalScore;
}

boost::intrusive_ptr<DocumentSource> buildUnionWithPipeline(
    const std::string& prefix,
    const int& rankConstant,
    std::vector<mongo::BSONObj> pipeline,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    pipeline.push_back(group());
    pipeline.push_back(unwind(prefix));
    pipeline.push_back(addScoreField(prefix, rankConstant));

    auto collName = expCtx->ns.coll();

    BSONObj inputToUnionWith =
        BSON("$unionWith" << BSON("coll" << collName << "pipeline" << pipeline));
    return DocumentSourceUnionWith::createFromBson(inputToUnionWith.firstElement(), expCtx);
}

std::list<boost::intrusive_ptr<DocumentSource>> buildScoreAndMergeStages(
    const std::string& prefixOne,
    const std::string& prefixTwo,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    auto group = DocumentSourceGroup::createFromBson(
        groupEachScore(prefixOne, prefixTwo).firstElement(), expCtx);
    auto addFields = DocumentSourceAddFields::createFromBson(
        calculateFinalScore(prefixOne, prefixTwo).firstElement(), expCtx);

    BSONObj sortObj = fromjson(R"({
        $sort: { score: -1, _id: 1}
    })");
    auto sort = DocumentSourceSort::createFromBson(sortObj.firstElement(), expCtx);

    BSONObj replaceRootObj = fromjson(R"({
        $replaceRoot: {newRoot: "$docs"}
    })");
    auto replaceRoot =
        DocumentSourceReplaceRoot::createFromBson(replaceRootObj.firstElement(), expCtx);
    return {group, addFields, sort, replaceRoot};
}


std::list<boost::intrusive_ptr<DocumentSource>> DocumentSourceRankFusion::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "The " << kStageName
                          << " stage specification must be an object, found "
                          << typeName(elem.type()),
            elem.type() == BSONType::Object);

    auto spec = RankFusionSpec::parse(IDLParserContext(kStageName), elem.embeddedObject());

    std::list<std::unique_ptr<Pipeline, PipelineDeleter>> inputPipelines;
    std::list<std::vector<mongo::BSONObj>> inputPipelinesBsonObj;

    // Ensure that all pipelines are valid ranked selection pipelines.
    for (const auto& input : spec.getInputs()) {
        inputPipelinesBsonObj.push_back(input.getPipeline());
        inputPipelines.push_back(
            Pipeline::parse(input.getPipeline(), pExpCtx, rankFusionPipelineValidator));
    }

    auto rankConstant = 60;
    std::list<boost::intrusive_ptr<DocumentSource>> rankFusionPipeline;

    // Add the first input pipeline to our final output.
    const std::string& pipelinePrefixOne = "first";
    std::unique_ptr<Pipeline, PipelineDeleter> inputPipelineOne = std::move(inputPipelines.front());
    inputPipelines.pop_front();
    inputPipelinesBsonObj.pop_front();

    rankFusionPipeline = inputPipelineOne->getSources();
    auto firstPipelineStages = buildFirstPipelineStages(pipelinePrefixOne, rankConstant, pExpCtx);
    for (const auto& stage : firstPipelineStages) {
        rankFusionPipeline.push_back(stage);
    }

    if (inputPipelines.empty()) {
        return rankFusionPipeline;
    }

    // Add all secondary input pipelines to our final output.
    const std::string& pipelinePrefixTwo = "second";
    std::unique_ptr<Pipeline, PipelineDeleter> inputPipelineTwo = std::move(inputPipelines.front());
    std::vector<mongo::BSONObj> inputPipelineTwoBson = inputPipelinesBsonObj.front();
    inputPipelinesBsonObj.pop_front();

    auto unionWithStages =
        buildUnionWithPipeline(pipelinePrefixTwo, rankConstant, inputPipelineTwoBson, pExpCtx);
    rankFusionPipeline.push_back(unionWithStages);

    // Build all remaining stages to perform the fusion.
    auto finalStages = buildScoreAndMergeStages(pipelinePrefixOne, pipelinePrefixTwo, pExpCtx);
    for (const auto& stage : finalStages) {
        rankFusionPipeline.push_back(stage);
    }

    return rankFusionPipeline;
}
}  // namespace mongo
