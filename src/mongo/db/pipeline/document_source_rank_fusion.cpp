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
#include "mongo/db/pipeline/document_source_set_metadata.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/document_source_union_with.h"
#include "mongo/db/pipeline/document_source_unwind.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/search/document_source_search.h"
#include "mongo/db/pipeline/search/document_source_vector_search.h"
#include "mongo/db/pipeline/search/search_helper.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

REGISTER_DOCUMENT_SOURCE_WITH_FEATURE_FLAG(rankFusion,
                                           DocumentSourceRankFusion::LiteParsed::parse,
                                           DocumentSourceRankFusion::createFromBson,
                                           AllowedWithApiStrict::kNeverInVersion1,
                                           feature_flags::gFeatureFlagRankFusionBasic);

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

    static const std::string rankPipelineMsg =
        "All subpipelines to the $rankFusion stage must begin with one of $search, "
        "$vectorSearch, $geoNear, $scoreFusion, $rankFusion or have a custom $sort in "
        "the pipeline.";
    uassert(9834300,
            str::stream() << "$rankFusion input pipeline cannot be empty. " << rankPipelineMsg,
            !sources.empty());

    auto firstStageName = sources.front()->getSourceName();
    auto isRankedPipeline = implicitlyOrderedStages.contains(firstStageName) ||
        std::any_of(sources.begin(), sources.end(), [](auto& stage) {
                                return stage->getSourceName() == DocumentSourceSort::kStageName;
                            });
    uassert(9191100, rankPipelineMsg, isRankedPipeline);

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

/**
 * Validates the weights inputs. If weights are specified by the user, there must be exactly one
 * weight per input pipeline.
 */
void rankFusionWeightsValidator(
    const std::map<std::string, std::unique_ptr<Pipeline, PipelineDeleter>>& pipelines,
    const StringMap<double>& weights) {
    // The size of weights and pipelines should actually be equal, but if we have more pipelines
    // than weights, we'll throw a more specific error below that specifies which pipeline is
    // missing a weight.
    uassert(9460301,
            "$rankFusion input has more weights than pipelines. If combination.weights is "
            "specified, there must be only one weight per named input pipeline.",
            weights.size() <= pipelines.size());

    for (const auto& pipelineIt : pipelines) {
        auto pipelineName = pipelineIt.first;
        uassert(9460302,
                str::stream()
                    << "$rankFusion input pipeline \"" << pipelineName
                    << "\" is missing a weight, even though combination.weights is specified.",
                weights.contains(pipelineName));
    }
}

StringMap<double> extractAndValidateWeights(
    const RankFusionSpec& spec,
    const std::map<std::string, std::unique_ptr<Pipeline, PipelineDeleter>>& pipelines) {
    StringMap<double> weights;

    const auto& combinationSpec = spec.getCombination();
    if (!combinationSpec.has_value()) {
        return weights;
    }

    for (const auto& elem : combinationSpec->getWeights()) {
        // elem.Number() throws a uassert if non-numeric.
        double weight = elem.Number();
        uassert(9460300,
                str::stream() << "Rank fusion pipeline weight must be non-negative, but given "
                              << weight,
                weight >= 0);
        weights[elem.fieldName()] = weight;
    }
    rankFusionWeightsValidator(pipelines, weights);
    return weights;
}

std::vector<BSONObj> getPipeline(BSONElement inputPipeElem) {
    return parsePipelineFromBSON(inputPipeElem);
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

BSONObj addMetadataFields(const std::string& prefix, const std::string& metadataFieldName) {
    std::string scoreDestinationField = metadataFieldName + "_score";
    std::string detailsDestinationField = metadataFieldName + "_scoreDetails";
    return BSON("$addFields" << BSON(scoreDestinationField << BSON("$meta"
                                                                   << "score")
                                                           << detailsDestinationField
                                                           << BSON("$meta"
                                                                   << "scoreDetails")));
}

BSONObj removeMetadataFields(const std::string& prefix, const std::string& metadataFieldName) {
    std::string scoreLocField = "docs." + metadataFieldName + "_score";
    std::string detailsLocField = "docs." + metadataFieldName + "_scoreDetails";

    return BSON("$project" << BSON(scoreLocField << 0 << detailsLocField << 0));
}
// RRF Score = weight * (1 / (rank + rank constant)).
BSONObj addScoreField(const std::string& prefix,
                      const int rankConstant,
                      const double weight,
                      const bool includeScoreDetails,
                      const std::string& metadataFieldName) {
    std::string score = prefix + "_score";
    std::string scorePath = "$docs." + metadataFieldName + "_score";
    std::string detailsPath = "$docs." + metadataFieldName + "_scoreDetails";
    std::string rank = "$" + prefix + "_rank";
    std::string scoreDetails = prefix + "_scoreDetails";

    // We're building the following BSON:
    /*
    {$addFields:
        {score:
            {multiply:
                [{$divide: [1, {$add: [rank, rankConstant]}]}]
            },
        scoreDetails:
            [{$ifNull: [$scoreDetails: {value: score, details: "Not Calculated"}]}]
        }
    }
    */
    BSONObjBuilder bob;
    BSONObjBuilder addFieldsBob(bob.subobjStart("$addFields"_sd));
    BSONObjBuilder scoreField(addFieldsBob.subobjStart(score));
    BSONArrayBuilder multiplyArray(scoreField.subarrayStart("$multiply"_sd));
    multiplyArray.append(
        BSON("$divide" << BSON_ARRAY(1 << BSON("$add" << BSON_ARRAY(rank << rankConstant)))));
    multiplyArray.append(weight);
    multiplyArray.done();
    scoreField.done();
    if (includeScoreDetails) {
        addFieldsBob.append(
            scoreDetails,
            BSON("$ifNull" << BSON_ARRAY(detailsPath << BSON("value" << scorePath << "details"
                                                                     << "Not Calculated"))));
        addFieldsBob.append(prefix + "_rank", rank);
    }
    addFieldsBob.done();

    return bob.obj();
}

std::list<boost::intrusive_ptr<DocumentSource>> buildFirstPipelineStages(
    const std::string& prefixOne,
    const int rankConstant,
    const double weight,
    const bool includeScoreDetails,
    const std::string metadataFieldName,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {

    auto groupStage = DocumentSourceGroup::createFromBson(group().firstElement(), expCtx);

    auto unwindStage =
        DocumentSourceUnwind::createFromBson(unwind(prefixOne).firstElement(), expCtx);

    auto addFields = DocumentSourceAddFields::createFromBson(
        addScoreField(prefixOne, rankConstant, weight, includeScoreDetails, metadataFieldName)
            .firstElement(),
        expCtx);

    if (includeScoreDetails) {
        auto preserveMetadataStage = DocumentSourceAddFields::createFromBson(
            addMetadataFields(prefixOne, metadataFieldName).firstElement(), expCtx);

        auto removeMetadataStage = DocumentSourceProject::createFromBson(
            removeMetadataFields(prefixOne, metadataFieldName).firstElement(), expCtx);
        return {preserveMetadataStage, groupStage, unwindStage, addFields, removeMetadataStage};
    }

    return {groupStage, unwindStage, addFields};
}

BSONObj groupEachScore(
    const std::map<std::string, std::unique_ptr<Pipeline, PipelineDeleter>>& pipelines,
    const bool includeScoreDetails) {
    // For each sub-pipeline, build the following string:
    // ", name_score: {$max: {ifNull: ["$name_score", 0]}}, name_rank: {$max: {ifNull:
    // ["$name_rank", 0]}}" These strings are appended to each other
    const auto allScores = [&]() {
        StringBuilder sb;
        for (auto it = pipelines.begin(); it != pipelines.end(); it++) {
            sb << ", ";
            const auto scoreName = it->first + "_score";
            const auto rankName = it->first + "_rank";
            const auto scoreDetailsName = it->first + "_scoreDetails";
            sb << scoreName << R"(: {$max: {$ifNull: ["$)" + scoreName + R"(", NumberLong(0)]}})";
            // We only need to preserve the rank if we're calculating score details.
            if (includeScoreDetails) {
                sb << ", " << rankName
                   << R"(: {$max: {$ifNull: ["$)" + rankName + R"(", NumberLong(0)]}})";
                sb << ", " << scoreDetailsName
                   << R"(: {"$mergeObjects": "$)" + scoreDetailsName + R"("})";
            }
        }
        return sb.str();
    };

    return fromjson(R"({
        $group: {
                _id: "$docs._id",
                docs: {$first: "$docs"}
                )" + allScores() +
                    R"(}
    })");
}

BSONObj calculateFinalScore(
    const std::map<std::string, std::unique_ptr<Pipeline, PipelineDeleter>>& inputs) {
    // Generate a string with an array of all the fields containing a score for a given pipeline.
    const auto& allInputs = [&] {
        StringBuilder sb;
        sb << "[";
        bool first = true;
        for (auto it = inputs.begin(); it != inputs.end(); it++) {
            if (!first) {
                sb << ", ";
            }
            first = false;
            sb << "\"$" << it->first << "_score\"";
        }
        sb << "]";
        return sb.str();
    };
    BSONObj finalScore = fromjson(R"({
        $addFields: {
            score: {
                $add: )" + allInputs() +
                                  R"(
            }
        }
    })");
    return finalScore;
}

BSONObj calculateFinalScoreDetails(
    const std::map<std::string, std::unique_ptr<Pipeline, PipelineDeleter>>& inputs) {
    // Generate the following string for each pipeline:
    // "name: {rank: $name_rank, details: $name_scoreDetails}""
    // And add them all to an array to be merged together.
    BSONObjBuilder bob;
    BSONArrayBuilder mergeNamedDetailsBob(bob.subarrayStart("$mergeObjects"_sd));
    for (auto it = inputs.begin(); it != inputs.end(); it++) {
        mergeNamedDetailsBob.append(
            BSON(it->first << BSON("$mergeObjects"
                                   << BSON_ARRAY(BSON("rank"
                                                      << "$" + it->first + "_rank")
                                                 << "$" + it->first + "_scoreDetails"))));
    }
    mergeNamedDetailsBob.done();
    // Create the following object:
    /*
        { $addFields: {
            calculatedScoreDetails: {
                $mergeObjects: [<generated above>]
            }
        }
    */
    BSONObj finalScoreDetails = BSON("$addFields" << BSON("calculatedScoreDetails" << bob.obj()));
    return finalScoreDetails;
}

boost::intrusive_ptr<DocumentSource> buildUnionWithPipeline(
    const std::string& prefix,
    const int rankConstant,
    const double weight,
    std::unique_ptr<Pipeline, PipelineDeleter> oneInputPipeline,
    const bool includeScoreDetails,
    const std::string& metadataFieldName,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    std::vector<BSONObj> bsonPipeline = oneInputPipeline->serializeToBson();
    bsonPipeline.push_back(group());
    bsonPipeline.push_back(unwind(prefix));
    bsonPipeline.push_back(
        addScoreField(prefix, rankConstant, weight, includeScoreDetails, metadataFieldName));

    auto collName = expCtx->getNamespaceString().coll();

    BSONObj inputToUnionWith =
        BSON("$unionWith" << BSON("coll" << collName << "pipeline" << bsonPipeline));
    return DocumentSourceUnionWith::createFromBson(inputToUnionWith.firstElement(), expCtx);
}

std::list<boost::intrusive_ptr<DocumentSource>> buildScoreAndMergeStages(
    const std::map<std::string, std::unique_ptr<Pipeline, PipelineDeleter>>& inputPipelines,
    const bool includeScoreDetails,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    auto group = DocumentSourceGroup::createFromBson(
        groupEachScore(inputPipelines, includeScoreDetails).firstElement(), expCtx);
    auto addFields = DocumentSourceAddFields::createFromBson(
        calculateFinalScore(inputPipelines).firstElement(), expCtx);

    // Note that the scoreDetails fields go here in the pipeline. We create them below to be able
    // to return them immediately once all stages are generated.
    BSONObj sortObj = fromjson(R"({
        $sort: { score: -1, _id: 1}
    })");
    auto sort = DocumentSourceSort::createFromBson(sortObj.firstElement(), expCtx);

    BSONObj replaceRootObj = fromjson(R"({
        $replaceRoot: {newRoot: "$docs"}
    })");
    auto replaceRoot =
        DocumentSourceReplaceRoot::createFromBson(replaceRootObj.firstElement(), expCtx);

    if (includeScoreDetails) {
        auto addFieldsDetails = DocumentSourceAddFields::createFromBson(
            calculateFinalScoreDetails(inputPipelines).firstElement(), expCtx);
        auto setDetails = DocumentSourceSetMetadata::create(
            expCtx,
            Expression::parseObject(expCtx.get(),
                                    BSON("value"
                                         << "$score"
                                         << "details"
                                         << "$calculatedScoreDetails"),
                                    expCtx->variablesParseState),
            DocumentMetadataFields::kScoreDetails);
        return {group, addFields, addFieldsDetails, setDetails, sort, replaceRoot};
    }
    return {group, addFields, sort, replaceRoot};
}

}  // namespace

std::unique_ptr<DocumentSourceRankFusion::LiteParsed> DocumentSourceRankFusion::LiteParsed::parse(
    const NamespaceString& nss, const BSONElement& spec) {
    uassert(ErrorCodes::FailedToParse,
            str::stream() << kStageName << " must take a nested object but found: " << spec,
            spec.type() == BSONType::Object);

    auto parsedSpec = RankFusionSpec::parse(IDLParserContext(kStageName), spec.embeddedObject());
    auto inputPipesObj = parsedSpec.getInput().getPipelines();

    // Ensure that all pipelines are valid ranked selection pipelines.
    std::vector<LiteParsedPipeline> liteParsedPipelines;
    for (const auto& elem : inputPipesObj) {
        liteParsedPipelines.emplace_back(nss, getPipeline(elem));
    }

    return std::make_unique<DocumentSourceRankFusion::LiteParsed>(
        spec.fieldName(), nss, std::move(liteParsedPipelines));
}

std::list<boost::intrusive_ptr<DocumentSource>> DocumentSourceRankFusion::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "The " << kStageName
                          << " stage specification must be an object, found "
                          << typeName(elem.type()),
            elem.type() == BSONType::Object);

    auto spec = RankFusionSpec::parse(IDLParserContext(kStageName), elem.embeddedObject());

    // It's important to use an ordered map here, so that we get stability in the desugaring =>
    // stability in the query shape.
    std::map<std::string, std::unique_ptr<Pipeline, PipelineDeleter>> inputPipelines;
    std::string metadataFieldName = UUID::gen().toString();
    // Ensure that all pipelines are valid ranked selection pipelines.
    for (const auto& elem : spec.getInput().getPipelines()) {
        auto pipeline = Pipeline::parse(getPipeline(elem), pExpCtx);
        rankFusionPipelineValidator(*pipeline);

        auto inputName = elem.fieldName();
        uassertStatusOKWithContext(
            FieldPath::validateFieldName(inputName),
            "$rankFusion pipeline names must follow the naming rules of field path expressions.");
        inputPipelines[inputName] = std::move(pipeline);
    }

    StringMap<double> weights = extractAndValidateWeights(spec, inputPipelines);

    // For now, the rankConstant is always 60.
    static const double rankConstant = 60;
    const bool includeScoreDetails = spec.getScoreDetails();
    std::list<boost::intrusive_ptr<DocumentSource>> outputStages;
    for (auto it = inputPipelines.begin(); it != inputPipelines.end(); it++) {
        const auto& name = it->first;
        auto& pipeline = it->second;

        // If weights weren't specified, the default weight is 1.
        double pipelineWeight = weights.empty() ? 1 : weights.at(name);

        if (outputStages.empty()) {
            // First pipeline.
            while (!pipeline->getSources().empty()) {
                outputStages.push_back(pipeline->popFront());
            }
            auto firstPipelineStages = buildFirstPipelineStages(name,
                                                                rankConstant,
                                                                pipelineWeight,
                                                                includeScoreDetails,
                                                                metadataFieldName,
                                                                pExpCtx);
            for (const auto& stage : firstPipelineStages) {
                outputStages.push_back(std::move(stage));
            }
        } else {
            auto unionWithStage = buildUnionWithPipeline(name,
                                                         rankConstant,
                                                         pipelineWeight,
                                                         std::move(pipeline),
                                                         includeScoreDetails,
                                                         metadataFieldName,
                                                         pExpCtx);
            outputStages.push_back(unionWithStage);
        }
    }

    // Build all remaining stages to perform the fusion.
    auto finalStages = buildScoreAndMergeStages(inputPipelines, includeScoreDetails, pExpCtx);
    for (const auto& stage : finalStages) {
        outputStages.push_back(stage);
    }

    return outputStages;
}
}  // namespace mongo
