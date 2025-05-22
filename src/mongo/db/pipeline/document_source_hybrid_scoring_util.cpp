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

#include "mongo/db/pipeline/document_source_hybrid_scoring_util.h"

#include <fmt/ranges.h>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/pipeline/document_source_add_fields.h"
#include "mongo/db/pipeline/document_source_set_metadata.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/search/document_source_search.h"
#include "mongo/db/pipeline/search/document_source_vector_search.h"
#include "mongo/db/query/util/string_util.h"

namespace mongo::hybrid_scoring_util {

bool isScoreStage(const boost::intrusive_ptr<DocumentSource>& stage) {
    if (stage->getSourceName() != DocumentSourceSetMetadata::kStageName) {
        return false;
    }
    auto singleDocTransform = static_cast<DocumentSourceSingleDocumentTransformation*>(stage.get());
    auto setMetadataTransform =
        static_cast<SetMetadataTransformation*>(&singleDocTransform->getTransformer());
    return setMetadataTransform->getMetaType() == DocumentMetadataFields::MetaType::kScore;
}

double getPipelineWeight(const StringMap<double>& weights, const std::string& pipelineName) {
    // If no weight is provided, default to 1.
    return weights.contains(pipelineName) ? weights.at(pipelineName) : 1;
}

StringMap<double> validateWeights(
    const mongo::BSONObj& inputWeights,
    const std::map<std::string, std::unique_ptr<Pipeline, PipelineDeleter>>& inputPipelines,
    const StringData stageName) {
    // Output map of pipeline name, to weight of pipeline.
    StringMap<double> weights;
    // Keeps track of the weights that do not reference a valid pipeline most often from a
    // misspelling/typo.
    std::vector<std::string> invalidWeights;
    // Keeps track of the pipelines that have been successfully matched/taken by specified weights.
    // We use this to build a list of pipelines that have not been matched later,
    // if necessary to suggest pipelines that might have been misspelled.
    stdx::unordered_set<std::string> matchedPipelines;

    for (const auto& weightEntry : inputWeights) {
        // First validate that this pipeline exists.
        if (!inputPipelines.contains(weightEntry.fieldName())) {
            // This weight does not reference a valid pipeline.
            // The query will eventually fail, but we process all the weights first
            // to give the best suggestions in the error message.
            invalidWeights.push_back(weightEntry.fieldName());
            continue;
        }

        // The pipeline exists, but must not already have been seen; else its a duplicate.
        // Otherwise, add it to the output map.
        // This should never arise because the BSON processing layer filters out
        // redundant keys, but we leave it in as a defensive programming measure.
        uassert(9967401,
                str::stream() << "A pipeline named '" << weightEntry.fieldName()
                              << "' is specified more than once in the $" << stageName
                              << "'combinations.weight' object.",
                !weights.contains(weightEntry.fieldName()));

        // Unique, existing pipeline weight found.
        // Validate the weight number and add to output map.
        // weightEntry.Number() throws a uassert if non-numeric.
        double weight = weightEntry.Number();
        uassert(9460300,
                str::stream() << stageName << "'s pipeline weight must be non-negative, but given "
                              << weight << " for pipeline '" << weightEntry.fieldName() << "'.",
                weight >= 0);
        weights[weightEntry.fieldName()] = weight;
        matchedPipelines.insert(weightEntry.fieldName());
    }

    // All weights that the user has specified have been processed.
    // Check for error cases.
    if (int(inputPipelines.size()) < inputWeights.nFields()) {
        // There are more specified weights than input pipelines.
        // Give feedback on which possible weights are extraneous.
        tassert(9967501,
                "There must be at least some invalid weights when there are more weights "
                "than input pipelines to $" +
                    stageName,
                !invalidWeights.empty());
        // Fail query.
        uasserted(
            9460301,
            fmt::format(
                "${} input has more weights ({}) than pipelines ({}). "
                "If 'combination.weights' is specified, there must be a less or equal number of "
                "weights as pipelines, each of which is unique and existing. "
                "Possible extraneous specified weights = [{}]",
                stageName,
                inputWeights.nFields(),
                int(inputPipelines.size()),
                fmt::join(invalidWeights, ", ")));
    } else if (!invalidWeights.empty()) {
        // There are invalid / misspelled weights.
        // Fail the query with the best pipeline recommendations we can generate.
        failWeightsValidationWithPipelineSuggestions(
            inputPipelines, matchedPipelines, invalidWeights, stageName);
    }

    // Successfully validated weights.
    return weights;
}

void failWeightsValidationWithPipelineSuggestions(
    const std::map<std::string, std::unique_ptr<Pipeline, PipelineDeleter>>& allPipelines,
    const stdx::unordered_set<std::string>& matchedPipelines,
    const std::vector<std::string>& invalidWeights,
    const StringData stageName) {
    // The list of unmatchedPipelines is first computed to find
    // the valid set of possible suggestions.
    std::vector<std::string> unmatchedPipelines;
    for (const auto& pipeline : allPipelines) {
        if (!matchedPipelines.contains(pipeline.first)) {
            unmatchedPipelines.push_back(pipeline.first);
        }
    }

    // For each invalid weight, find the best possible suggested unmatched pipeline,
    // that is, the one with the shortest levenshtein distance.
    // The first entry in the pair is the name of the invalid weight,
    // the second entry is the list of the suggested unmatched pipeline.
    std::vector<std::pair<std::string, std::vector<std::string>>> suggestions =
        query_string_util::computeTypoSuggestions(unmatchedPipelines, invalidWeights);

    // 'i' is the index into the 'suggestions' array.
    auto convertSingleSuggestionToString = [&](const std::size_t i) -> std::string {
        std::string s = fmt::format("(provided: '{}' -> ", suggestions[i].first);
        if (suggestions[i].second.size() == 1) {
            s += fmt::format("suggested: '{}')", suggestions[i].second.front());
        } else {
            s += fmt::format("suggestions: [{}])", fmt::join(suggestions[i].second, ", "));
        }
        if (i < suggestions.size() - 1) {
            s += ", ";
        }
        return s;
    };

    // All best suggestions have been computed.
    // The build error message that contains all suggestions.
    std::string errorMsg = fmt::format(
        "${} stage contained ({}) weight(s) in "
        "'combination.weights' that did not reference valid pipeline names. "
        "Suggestions for valid pipeline names: ",
        stageName,
        std::to_string(invalidWeights.size()));
    for (std::size_t i = 0; i < suggestions.size(); i++) {
        errorMsg += convertSingleSuggestionToString(i);
    }

    // Fail query.
    uasserted(9967500, errorMsg);
}

namespace score_details {

boost::intrusive_ptr<DocumentSource> addScoreDetails(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const StringData inputPipelinePrefix,
    const bool inputGeneratesScore,
    const bool inputGeneratesScoreDetails) {
    const std::string scoreDetails = fmt::format("{}_scoreDetails", inputPipelinePrefix);
    BSONObjBuilder bob;
    {
        BSONObjBuilder addFieldsBob(bob.subobjStart("$addFields"_sd));

        if (inputGeneratesScoreDetails) {
            // If the input pipeline generates scoreDetails (for example, $search may generate
            // searchScoreDetails), then we'll use the existing details:
            // {$addFields: {prefix_scoreDetails: {$meta: "scoreDetails"}}}
            // We don't grab {$meta: "score"} because we assume any existing scoreDetails already
            // includes its own score at "scoreDetails.value".
            addFieldsBob.append(scoreDetails, BSON("$meta" << "scoreDetails"));
        } else if (inputGeneratesScore) {
            // If the input pipeline does not generate scoreDetails but does generate a "score" (for
            // example, a $text query sorted on the text score), we'll build our own scoreDetails
            // for the pipeline like:
            // {$addFields: {prefix_scoreDetails: {value: {$meta: "score"}, details: []}}}
            addFieldsBob.append(
                scoreDetails,
                BSON("value" << BSON("$meta" << "score") << "details" << BSONArrayBuilder().arr()));
        } else {
            // If the input pipeline generates neither "score" not "scoreDetails" (for example, a
            // pipeline with just a $sort), we don't have any interesting information to include in
            // scoreDetails (rank is added later). We'll still build empty scoreDetails to
            // reflect that:
            // {$addFields: {prefix_scoreDetails: {details: []}}}
            addFieldsBob.append(scoreDetails, BSON("details" << BSONArrayBuilder().arr()));
        }
    }
    const auto spec = bob.obj();
    return DocumentSourceAddFields::createFromBson(spec.firstElement(), expCtx);
}

std::pair<std::string, BSONObj> constructScoreDetailsForGrouping(const std::string pipelineName) {
    const std::string scoreDetailsName = fmt::format("{}_scoreDetails", pipelineName);
    return std::make_pair(scoreDetailsName,
                          BSON("$mergeObjects" << fmt::format("${}", scoreDetailsName)));
}

boost::intrusive_ptr<DocumentSource> constructCalculatedFinalScoreDetails(
    const std::map<std::string, std::unique_ptr<Pipeline, PipelineDeleter>>& inputs,
    const StringMap<double>& weights,
    const bool isRankFusion,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    std::vector<boost::intrusive_ptr<Expression>> detailsChildren;
    for (auto it = inputs.begin(); it != inputs.end(); it++) {
        const std::string& pipelineName = it->first;
        const std::string scoreDetailsFieldName = fmt::format("${}_scoreDetails", pipelineName);
        double weight = hybrid_scoring_util::getPipelineWeight(weights, pipelineName);

        BSONObjBuilder mergeObjectsArrSubObj;
        mergeObjectsArrSubObj.append("inputPipelineName"_sd, pipelineName);
        if (isRankFusion) {
            mergeObjectsArrSubObj.append("rank"_sd, fmt::format("${}_rank", pipelineName));
        } else {
            // ScoreFusion case.
            mergeObjectsArrSubObj.append("inputPipelineRawScore"_sd,
                                         fmt::format("${}_rawScore", pipelineName));
        }
        mergeObjectsArrSubObj.append("weight"_sd, weight);
        mergeObjectsArrSubObj.done();
        BSONArrayBuilder mergeObjectsArr;
        mergeObjectsArr.append(mergeObjectsArrSubObj.obj());
        mergeObjectsArr.append(scoreDetailsFieldName);
        mergeObjectsArr.done();
        BSONObj mergeObjectsObj = BSON("$mergeObjects"_sd << mergeObjectsArr.arr());
        boost::intrusive_ptr<Expression> mergeObjectsExpr =
            ExpressionFromAccumulator<AccumulatorMergeObjects>::parse(
                expCtx.get(), mergeObjectsObj.firstElement(), expCtx->variablesParseState);

        detailsChildren.push_back(std::move(mergeObjectsExpr));
    }

    boost::intrusive_ptr<Expression> arrayExpr =
        ExpressionArray::create(expCtx.get(), std::move(detailsChildren));

    auto addFields = DocumentSourceAddFields::create(
        "calculatedScoreDetails"_sd, std::move(arrayExpr), expCtx.get());
    return addFields;
}

std::string stringifyExpression(boost::optional<IDLAnyType> expression) {
    BSONObjBuilder expressionBob;
    expression->serializeToBSON("string", &expressionBob);
    expressionBob.done();
    std::string exprString = expressionBob.obj().toString();
    std::replace(exprString.begin(), exprString.end(), '\"', '\'');
    return exprString;
}
}  // namespace score_details
}  // namespace mongo::hybrid_scoring_util
