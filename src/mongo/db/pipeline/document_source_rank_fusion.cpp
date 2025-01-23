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

#include "mongo/db/pipeline/document_source_rank_fusion.h"

#include <algorithm>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_add_fields.h"
#include "mongo/db/pipeline/document_source_geo_near.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/document_source_replace_root.h"
#include "mongo/db/pipeline/document_source_set_metadata.h"
#include "mongo/db/pipeline/document_source_set_window_fields.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/document_source_union_with.h"
#include "mongo/db/pipeline/document_source_unwind.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/search/document_source_search.h"
#include "mongo/db/pipeline/search/document_source_vector_search.h"
#include "mongo/db/pipeline/search/search_helper.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/logv2/log.h"
#include "mongo/util/string_map.h"

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

auto makeSureSortKeyIsOutput(const auto& stageList) {
    DocumentSourceSort* rightMostSort = nullptr;
    for (auto&& stage : stageList) {
        if (auto sortStage = dynamic_cast<DocumentSourceSort*>(stage.get())) {
            rightMostSort = sortStage;
        }
    }
    if (rightMostSort)
        rightMostSort->pleaseOutputSortKeyMetadata();
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

auto stageToBson(const auto stagePtr) {
    std::vector<Value> array;
    stagePtr->serializeToArray(array);
    return array[0].getDocument().toBson();
}

auto setWindowFields(const auto& expCtx, const std::string& rankFieldName) {
    // TODO SERVER-98562 We shouldn't need to provide this sort, since it's not used other than to
    // pass the parse-time validation checks.
    const SortPattern dummySortPattern{fromjson("{order: 1}"), expCtx};
    return make_intrusive<DocumentSourceInternalSetWindowFields>(
        expCtx,
        boost::none,  // partitionBy
        dummySortPattern,
        std::vector<WindowFunctionStatement>{
            WindowFunctionStatement{rankFieldName,
                                    window_function::Expression::parse(
                                        fromjson("{$rank: {}}"), dummySortPattern, expCtx.get())}},
        internalDocumentSourceSetWindowFieldsMaxMemoryBytes.load(),
        SbeCompatibility::notCompatible);
}

/**
 * Builds and returns an $addFields stage like this one:
 * {$addFields: {
 *      prefix_scoreDetails:
 *          [{$ifNull: [{$meta: "scoreDetails"}, {value: score, details: "Not Calculated"}]}]
 *      }
 *  }
 */
auto addScoreDetails(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                     const std::string& prefix) {
    const std::string scoreDetails = fmt::format("{}_scoreDetails", prefix);
    BSONObjBuilder bob;
    {
        BSONObjBuilder addFieldsBob(bob.subobjStart("$addFields"_sd));
        addFieldsBob.append(scoreDetails,
                            fromjson(
                                R"({$ifNull: [
                                    {$meta: "scoreDetails"},
                                    {value: {$meta: "score"}, details: "Not Calculated"}
                                ]})"));
    }
    const auto spec = bob.obj();
    return DocumentSourceAddFields::createFromBson(spec.firstElement(), expCtx);
}

/**
 * Builds and returns an $addFields stage, like the following:
 * {$addFields:
 *     {prefix_score:
 *         {multiply:
 *             [{$divide: [1, {$add: [rank, rankConstant]}]}]
 *         },
 *     }
 * }
 */
auto addScoreField(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                   const std::string& prefix,
                   const int rankConstant,
                   const double weight) {
    const std::string score = fmt::format("{}_score", prefix);
    const std::string rankPath = fmt::format("${}_rank", prefix);
    const std::string scorePath = fmt::format("${}", score);

    BSONObjBuilder bob;
    {
        BSONObjBuilder addFieldsBob(bob.subobjStart("$addFields"_sd));
        {
            BSONObjBuilder scoreField(addFieldsBob.subobjStart(score));
            {
                BSONArrayBuilder multiplyArray(scoreField.subarrayStart("$multiply"_sd));
                // RRF Score = weight * (1 / (rank + rank constant)).

                multiplyArray.append(fromjson(
                    fmt::format("{{$divide: [1, {{$add: ['{}', {}]}}]}}", rankPath, rankConstant)));
                multiplyArray.append(weight);
            }
        }
    }

    const auto spec = bob.obj();
    return DocumentSourceAddFields::createFromBson(spec.firstElement(), expCtx);
}

/**
 * Builds and returns a $replaceRoot stage: {$replaceWith: {docs: "$$ROOT"}}.
 * This has the effect of storing the unmodified user's document in the path '$docs'.
 */
auto nestUserDocs(const auto& expCtx) {
    static const BSONObj replaceRootObj = fromjson("{$replaceWith: {docs: \"$$ROOT\"}}");
    return DocumentSourceReplaceRoot::createFromBson(replaceRootObj.firstElement(), expCtx);
}

auto buildFirstPipelineStages(const std::string& prefixOne,
                              const int rankConstant,
                              const double weight,
                              const bool includeScoreDetails,
                              const boost::intrusive_ptr<ExpressionContext>& expCtx) {

    std::list<boost::intrusive_ptr<DocumentSource>> outputStages = {
        nestUserDocs(expCtx),
        setWindowFields(expCtx, fmt::format("{}_rank", prefixOne)),
        addScoreField(expCtx, prefixOne, rankConstant, weight),
    };
    if (includeScoreDetails) {
        outputStages.push_back(addScoreDetails(expCtx, prefixOne));
    }
    return outputStages;
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
            const auto& pipelineName = it->first;
            sb << fmt::format("{0}_score: {{$max: {{$ifNull: [\"${0}_score\", NumberLong(0)]}}}}",
                              pipelineName);
            // We only need to preserve the rank if we're calculating score details.
            if (includeScoreDetails) {
                sb << fmt::format(
                    ", {0}_rank: {{$max: {{$ifNull: [\"${0}_rank\", NumberLong(0)]}}}}",
                    pipelineName);
                sb << fmt::format(", {0}_scoreDetails: {{$mergeObjects: \"${0}_scoreDetails\"}}",
                                  pipelineName);
            }
        }
        return sb.str();
    };

    return fromjson(
        fmt::format("{{$group: {{_id: '$docs._id', docs: {{$first: '$docs'}}{0}}}}}", allScores()));
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
    return fromjson(fmt::format(R"({{$addFields: {{score: {{$add: {0}}}}}}})", allInputs()));
}

BSONObj calculateFinalScoreDetails(
    const std::map<std::string, std::unique_ptr<Pipeline, PipelineDeleter>>& inputs) {
    // Generate the following string for each pipeline:
    // "name: {rank: $name_rank, details: $name_scoreDetails}""
    // And add them all to an array to be merged together.
    BSONObjBuilder bob;
    BSONArrayBuilder mergeNamedDetailsBob(bob.subarrayStart("$mergeObjects"_sd));
    for (auto it = inputs.begin(); it != inputs.end(); it++) {
        mergeNamedDetailsBob.append(fromjson(
            fmt::format("{{{0}: {{$mergeObjects: [{{rank: '${0}_rank'}}, '${0}_scoreDetails']}}}}",
                        it->first)));
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
    return fromjson(
        fmt::format("{{$addFields: {{calculatedScoreDetails: {0}}}}}", bob.obj().toString()));
}

boost::intrusive_ptr<DocumentSource> buildUnionWithPipeline(
    const std::string& prefix,
    const int rankConstant,
    const double weight,
    std::unique_ptr<Pipeline, PipelineDeleter> oneInputPipeline,
    const bool includeScoreDetails,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {

    makeSureSortKeyIsOutput(oneInputPipeline->getSources());
    oneInputPipeline->pushBack(nestUserDocs(expCtx));
    oneInputPipeline->pushBack(setWindowFields(expCtx, fmt::format("{}_rank", prefix)));
    oneInputPipeline->pushBack(addScoreField(expCtx, prefix, rankConstant, weight));
    if (includeScoreDetails) {
        oneInputPipeline->pushBack(addScoreDetails(expCtx, prefix));
    }
    std::vector<BSONObj> bsonPipeline = oneInputPipeline->serializeToBson();

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
        $sort: {score: -1, _id: 1}
    })");
    auto sort = DocumentSourceSort::createFromBson(sortObj.firstElement(), expCtx);

    static const BSONObj replaceRootObj = fromjson(R"({
        $replaceRoot: {newRoot: "$docs"}
    })");
    auto restoreUserDocs =
        DocumentSourceReplaceRoot::createFromBson(replaceRootObj.firstElement(), expCtx);

    if (includeScoreDetails) {
        auto addFieldsDetails = DocumentSourceAddFields::createFromBson(
            calculateFinalScoreDetails(inputPipelines).firstElement(), expCtx);
        auto setDetails = DocumentSourceSetMetadata::create(
            expCtx,
            Expression::parseObject(
                expCtx.get(),
                fromjson("{value: '$score', details: '$calculatedScoreDetails'}"),
                expCtx->variablesParseState),
            DocumentMetadataFields::kScoreDetails);
        return {group, addFields, addFieldsDetails, setDetails, sort, restoreUserDocs};
    }
    return {group, addFields, sort, restoreUserDocs};
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
    // Ensure that all pipelines are valid ranked selection pipelines.
    for (const auto& elem : spec.getInput().getPipelines()) {
        auto pipeline = Pipeline::parse(getPipeline(elem), pExpCtx);
        rankFusionPipelineValidator(*pipeline);

        auto inputName = elem.fieldName();
        uassertStatusOKWithContext(
            FieldPath::validateFieldName(inputName),
            "$rankFusion pipeline names must follow the naming rules of field path expressions.");
        uassert(
            9921000,
            str::stream() << "$rankFusion pipeline names must be unique, but found duplicate name '"
                          << inputName << "'.",
            !inputPipelines.contains(inputName));
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
            makeSureSortKeyIsOutput(pipeline->getSources());
            while (!pipeline->getSources().empty()) {
                outputStages.push_back(pipeline->popFront());
            }
            auto firstPipelineStages = buildFirstPipelineStages(
                name, rankConstant, pipelineWeight, includeScoreDetails, pExpCtx);
            for (const auto& stage : firstPipelineStages) {
                outputStages.push_back(std::move(stage));
            }
        } else {
            auto unionWithStage = buildUnionWithPipeline(name,
                                                         rankConstant,
                                                         pipelineWeight,
                                                         std::move(pipeline),
                                                         includeScoreDetails,
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
