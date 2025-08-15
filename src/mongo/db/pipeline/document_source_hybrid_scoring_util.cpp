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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/pipeline/document_source_add_fields.h"
#include "mongo/db/pipeline/document_source_geo_near.h"
#include "mongo/db/pipeline/document_source_internal_inhibit_optimization.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_rank_fusion.h"
#include "mongo/db/pipeline/document_source_replace_root.h"
#include "mongo/db/pipeline/document_source_sample.h"
#include "mongo/db/pipeline/document_source_score.h"
#include "mongo/db/pipeline/document_source_score_fusion.h"
#include "mongo/db/pipeline/document_source_set_metadata.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/document_source_skip.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/search/document_source_search.h"
#include "mongo/db/pipeline/search/document_source_vector_search.h"
#include "mongo/db/query/util/string_util.h"
#include "mongo/util/string_map.h"

#include <fmt/ranges.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

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
    const std::map<std::string, std::unique_ptr<Pipeline>>& inputPipelines,
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
    const std::map<std::string, std::unique_ptr<Pipeline>>& allPipelines,
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

Status isSelectionPipeline(const std::vector<BSONObj>& bsonPipeline) {
    if (bsonPipeline.empty()) {
        return Status(ErrorCodes::Error::BadValue, "Input pipeline must not be empty.");
    }

    for (const auto& stage : bsonPipeline) {
        if (auto status = isSelectionStage(stage); !status.isOK()) {
            return status;
        }
    }

    return Status::OK();
}

Status isSelectionStage(const BSONObj& bsonStage) {
    // Please keep the following in alphabetical order.
    static const StringDataSet validSelectionStagesForHybridSearch = {
        DocumentSourceInternalInhibitOptimization::kStageName,
        DocumentSourceLimit::kStageName,
        DocumentSourceMatch::kStageName,
        DocumentSourceRankFusion::kStageName,
        DocumentSourceSample::kStageName,
        DocumentSourceScore::kStageName,
        DocumentSourceScoreFusion::kStageName,
        DocumentSourceSkip::kStageName,
        DocumentSourceSort::kStageName,
        DocumentSourceVectorSearch::kStageName,
    };

    if (bsonStage.isEmpty()) {
        // Empty BSON stage was provided - it is not a selection stage.
        return Status(ErrorCodes::Error::InvalidBSON, "Input stages must not be empty.");
    }

    const auto& fieldName = bsonStage.firstElementFieldNameStringData();
    if (validSelectionStagesForHybridSearch.contains(fieldName)) {
        return Status::OK();
    }

    // The following stages are conditionally selection stages, depending on the specification.
    if (bsonStage.hasField(DocumentSourceGeoNear::kStageName)) {
        // $geoNear is only a selection stage if it does not specify 'includeLocs' or
        // 'distanceField'.
        const auto& spec = bsonStage[DocumentSourceGeoNear::kStageName];
        if (!spec.isABSONObj()) {
            // The spec for $geoNear should be a BSON object.
            return Status(ErrorCodes::Error::InvalidBSON,
                          "Spec for $geoNear must be a BSON object, but was given: " +
                              bsonStage.toString());
        }

        const auto& specBsonObj = spec.Obj();
        bool hasModificationFields =
            specBsonObj.hasField(DocumentSourceGeoNear::kDistanceFieldFieldName) ||
            specBsonObj.hasField(DocumentSourceGeoNear::kIncludeLocsFieldName);
        return hasModificationFields
            ? Status(ErrorCodes::Error::BadValue,
                     "$geoNear is only a selection stage if 'includeLocs' and 'distanceField' are "
                     "not specified, because these options modify the input documents by adding "
                     "output fields.")
            : Status::OK();
    }

    if (bsonStage.hasField(DocumentSourceSearch::kStageName)) {
        // $search is only a selection stage if 'returnStoredSource' is false.
        const auto& spec = bsonStage[DocumentSourceSearch::kStageName];
        if (!spec.isABSONObj()) {
            // The spec for $search should be a BSON object.
            return Status(ErrorCodes::Error::InvalidBSON,
                          "Spec for $search must be a BSON object, but was given: " +
                              bsonStage.toString());
        }

        const auto& specBsonObj = spec.Obj();
        if (!specBsonObj.hasField(mongot_cursor::kReturnStoredSourceArg)) {
            // The spec does not specify 'returnStoredSource' and the default is false.
            // This is a selection stage.
            return Status::OK();
        }

        const auto& returnStoredSourceArg = specBsonObj[mongot_cursor::kReturnStoredSourceArg];
        if (!returnStoredSourceArg.isBoolean()) {
            // 'returnStoredSource' should be a bool.
            return Status(ErrorCodes::Error::InvalidBSON,
                          "Spec for 'returnStoredSource' should be a boolean, but was given: " +
                              bsonStage.toString());
        }

        return returnStoredSourceArg.boolean()
            ? Status(ErrorCodes::Error::BadValue,
                     "$search is only a selection stage if 'returnStoredSource' is false because "
                     "it modifies the output fields.")
            : Status::OK();
    }

    // If here, then the stage was not a valid hybrid search selection stage.
    return Status(
        ErrorCodes::Error::BadValue,
        fieldName +
            " is not a selection stage because it modifies or transforms the input documents.");
}

Status isRankedPipeline(const std::vector<BSONObj>& bsonPipeline) {
    if (bsonPipeline.empty()) {
        return Status(ErrorCodes::Error::BadValue, "Input pipeline must not be empty.");
    }

    // Please keep the following in alphabetical order.
    static const StringDataSet implicitlyRankedStages{
        DocumentSourceGeoNear::kStageName,
        DocumentSourceRankFusion::kStageName,
        DocumentSourceScoreFusion::kStageName,
        DocumentSourceSearch::kStageName,
        DocumentSourceVectorSearch::kStageName,
    };

    // Check if the pipeline begins with an implicitly ranked stage.
    const auto& firstStage = bsonPipeline.front();
    bool firstStageIsImplicitlyRanked = !firstStage.isEmpty() &&
        implicitlyRankedStages.contains(*firstStage.getFieldNames<std::set<std::string>>().begin());

    // Check if the pipeline has an explicit $sort stage.
    bool hasSortStage = std::any_of(bsonPipeline.begin(), bsonPipeline.end(), [](auto&& stage) {
        return stage.hasField(DocumentSourceSort::kStageName);
    });

    return (firstStageIsImplicitlyRanked || hasSortStage)
        ? Status::OK()
        : Status(ErrorCodes::Error::BadValue,
                 "Pipeline did not begin with a ranked stage and did not contain an explicit $sort "
                 "stage.");
}

bool pipelineContainsScoreStage(const std::vector<BSONObj>& bsonPipeline) {
    // Check if the pipeline has an explicit $score stage.
    bool hasScoreStage = std::any_of(bsonPipeline.begin(), bsonPipeline.end(), [](auto&& stage) {
        return stage.hasField(DocumentSourceScore::kStageName);
    });

    return hasScoreStage;
}

Status isScoredPipeline(const std::vector<BSONObj>& bsonPipeline,
                        const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    if (bsonPipeline.empty()) {
        return Status(ErrorCodes::Error::BadValue, "Input pipeline must not be empty.");
    }

    // Please keep the following in alphabetical order.
    static const StringDataSet implicitlyScoredStages{
        DocumentSourceRankFusion::kStageName,
        DocumentSourceScoreFusion::kStageName,
        DocumentSourceSearch::kStageName,
        DocumentSourceVectorSearch::kStageName,
    };

    const auto& firstStage = bsonPipeline.front();

    // A $match stage w/ a $text operator is a scored stage.
    if (firstStage.hasField(DocumentSourceMatch::kStageName) &&
        firstStage[DocumentSourceMatch::kStageName].isABSONObj()) {
        const auto& matchSpec = firstStage[DocumentSourceMatch::kStageName].Obj();
        std::unique_ptr<MatchExpression> expr = uassertStatusOK(MatchExpressionParser::parse(
            matchSpec, expCtx, ExtensionsCallbackNoop(), Pipeline::kAllowedMatcherFeatures));
        if (DocumentSourceMatch::containsTextOperator(*expr)) {
            return Status::OK();
        }
    }

    // Check if the pipeline begins with an implicitly scored stage.
    bool firstStageIsImplicitlyScored = !firstStage.isEmpty() &&
        implicitlyScoredStages.contains(*firstStage.getFieldNames<std::set<std::string>>().begin());

    return firstStageIsImplicitlyScored || pipelineContainsScoreStage(bsonPipeline)
        ? Status::OK()
        : Status(
              ErrorCodes::Error::BadValue,
              "Pipeline did not begin with a scored stage and did not contain an explicit $score "
              "stage.");
}

bool isHybridSearchPipeline(const std::vector<BSONObj>& bsonPipeline) {
    // Please keep the following in alphabetical order.
    static const std::set<StringData> hybridScoringStages{
        DocumentSourceRankFusion::kStageName,
        DocumentSourceScoreFusion::kStageName,
    };

    for (const auto& stage : bsonPipeline) {
        tassert(10473001, "Input pipeline stage must not be empty.", !stage.isEmpty());
        if (hybridScoringStages.contains(*(stage.getFieldNames<std::set<std::string>>().begin()))) {
            return true;
        }
    };

    return false;
}

void validateIsHybridSearchNotSetByUser(boost::intrusive_ptr<ExpressionContext> expCtx,
                                        const BSONObj& spec) {
    if (spec.hasField(kIsHybridSearchFlagFieldName)) {
        assertAllowedInternalIfRequired(expCtx->getOperationContext(),
                                        kIsHybridSearchFlagFieldName,
                                        AllowedWithClientType::kInternal);
    }
}

void assertForeignCollectionIsNotTimeseries(const NamespaceString& nss,
                                            const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    const auto opCtx = expCtx->getOperationContext();
    const auto collectionCatalog = CollectionCatalog::get(opCtx);

    if (auto collectionPtr = collectionCatalog->lookupCollectionByNamespace(opCtx, nss)) {
        uassert(10787900,
                "$rankFusion and $scoreFusion are unsupported on timeseries collections",
                !collectionPtr->isTimeseriesCollection());
    } else if (auto viewPtr = collectionCatalog->lookupView(opCtx, nss)) {
        uassert(10787901,
                "$rankFusion and $scoreFusion are unsupported on timeseries collections",
                !viewPtr->timeseries());
    } else {
        // Note that we try our best to ban timeseries collections on hybrid search.
        // However, in a sharded collections environment, a mongod shard might not know the
        // information about the timeseries collection (if it is owned by another shard). In
        // that case, it is non-trivial to ban the timeseries query.
        // TODO SERVER-108218 Ban hybrid search inside of subpipelines on time series collections.
        LOGV2(10787902,
              "$rankFusion and $scoreFusion are unsupported on timeseries collections, but not "
              "enough information is available to determine if a subpipeline is running on a "
              "timeseries collection.");
    }
}

boost::intrusive_ptr<DocumentSource> buildReplaceRootStage(
    const StringData internalFieldsDocsName,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    return DocumentSourceReplaceRoot::createFromBson(
        BSON("$replaceWith" << BSON(internalFieldsDocsName << "$$ROOT")).firstElement(), expCtx);
}

BSONObj groupDocsByIdAcrossInputPipeline(const StringData internalFieldsDocsName,
                                         const StringData internalFieldsName,
                                         const std::vector<std::string>& pipelineNames,
                                         const bool includeScoreDetails) {
    // For each sub-pipeline, build the following obj:
    // name_score: {$max: {ifNull: ["$name_score", 0]}}
    // If scoreDetails is enabled, build:
    // If $rankFusion:
    // <INTERNAL_FIELDS>.name_rank: {$max: {ifNull: ["$<INTERNAL_FIELDS>.name_rank", 0]}}
    // If $scoreFusion:
    // <INTERNAL_FIELDS>.name_rawScore: {$max: {ifNull: ["$<INTERNAL_FIELDS>.name_rawScore", 0]}}
    // Both $rankFusion and $scoreFusion:
    // <INTERNAL_FIELDS>.name_scoreDetails: {$mergeObjects: $<INTERNAL_FIELDS>.name_scoreDetails}
    BSONObjBuilder bob;
    {
        BSONObjBuilder groupBob(bob.subobjStart("$group"_sd));
        groupBob.append("_id", "$" + internalFieldsDocsName + "._id");
        groupBob.append(internalFieldsDocsName, BSON("$first" << ("$" + internalFieldsDocsName)));

        BSONObjBuilder internalFieldsBob(groupBob.subobjStart(internalFieldsName));
        BSONObjBuilder pushBob(internalFieldsBob.subobjStart("$push"_sd));

        for (const auto& pipelineName : pipelineNames) {
            const std::string scoreName =
                hybrid_scoring_util::getScoreFieldFromPipelineName(pipelineName);
            pushBob.append(
                scoreName,
                BSON("$ifNull" << BSON_ARRAY(
                         fmt::format("${}",
                                     hybrid_scoring_util::applyInternalFieldPrefixToFieldName(
                                         internalFieldsName, scoreName))
                         << 0)));
            if (includeScoreDetails) {
                if (internalFieldsName == DocumentSourceRankFusion::kRankFusionInternalFieldsName) {
                    const std::string rankName = fmt::format("{}_rank", pipelineName);
                    pushBob.append(
                        rankName,
                        BSON("$ifNull" << BSON_ARRAY(
                                 fmt::format(
                                     "${}",
                                     hybrid_scoring_util::applyInternalFieldPrefixToFieldName(
                                         internalFieldsName, rankName))
                                 << 0)));
                } else if (internalFieldsName ==
                           DocumentSourceScoreFusion::kScoreFusionInternalFieldsName) {
                    const std::string rawScoreName = fmt::format("{}_rawScore", pipelineName);
                    pushBob.append(
                        rawScoreName,
                        BSON("$ifNull" << BSON_ARRAY(
                                 fmt::format(
                                     "${}",
                                     hybrid_scoring_util::applyInternalFieldPrefixToFieldName(
                                         internalFieldsName, rawScoreName))
                                 << 0)));
                }

                const std::string scoreDetailsName = fmt::format("{}_scoreDetails", pipelineName);
                pushBob.append(scoreDetailsName,
                               fmt::format("${}",
                                           hybrid_scoring_util::applyInternalFieldPrefixToFieldName(
                                               internalFieldsName, scoreDetailsName)));
            }
        }
        pushBob.done();
        internalFieldsBob.done();
        groupBob.done();
    }
    bob.done();
    return bob.obj();
}

BSONObj projectReduceInternalFields(const StringData internalFieldsDocsName,
                                    const StringData internalFieldsName,
                                    const std::vector<std::string>& pipelineNames,
                                    const bool includeScoreDetails) {
    BSONObjBuilder bob;
    {
        BSONObjBuilder projectBob(bob.subobjStart("$project"_sd));
        projectBob.append(internalFieldsDocsName, 1);

        BSONObjBuilder internalFieldsBob(projectBob.subobjStart(internalFieldsName));
        BSONObjBuilder reduceBob(internalFieldsBob.subobjStart("$reduce"_sd));

        reduceBob.append("input", "$" + internalFieldsName);

        BSONObjBuilder initialValueBob(reduceBob.subobjStart("initialValue"_sd));
        for (const auto& pipelineName : pipelineNames) {
            initialValueBob.append(fmt::format("{}_score", pipelineName), 0);
            if (includeScoreDetails) {
                if (internalFieldsName == DocumentSourceRankFusion::kRankFusionInternalFieldsName) {
                    initialValueBob.append(fmt::format("{}_rank", pipelineName), 0);
                } else if (internalFieldsName ==
                           DocumentSourceScoreFusion::kScoreFusionInternalFieldsName) {
                    initialValueBob.append(fmt::format("{}_rawScore", pipelineName), 0);
                }
                initialValueBob.append(fmt::format("{}_scoreDetails", pipelineName), BSONObj{});
            }
        }
        initialValueBob.done();

        BSONObjBuilder inBob(reduceBob.subobjStart("in"_sd));
        for (const auto& pipelineName : pipelineNames) {
            initialValueBob.append(
                fmt::format("{}_score", pipelineName),
                BSON("$max" << BSON_ARRAY(fmt::format("$$value.{}_score", pipelineName)
                                          << fmt::format("$$this.{}_score", pipelineName))));
            if (includeScoreDetails) {
                if (internalFieldsName == DocumentSourceRankFusion::kRankFusionInternalFieldsName) {
                    initialValueBob.append(
                        fmt::format("{}_rank", pipelineName),
                        BSON("$max" << BSON_ARRAY(fmt::format("$$value.{}_rank", pipelineName)
                                                  << fmt::format("$$this.{}_rank", pipelineName))));
                } else if (internalFieldsName ==
                           DocumentSourceScoreFusion::kScoreFusionInternalFieldsName) {
                    initialValueBob.append(
                        fmt::format("{}_rawScore", pipelineName),
                        BSON("$max"
                             << BSON_ARRAY(fmt::format("$$value.{}_rawScore", pipelineName)
                                           << fmt::format("$$this.{}_rawScore", pipelineName))));
                }
                initialValueBob.append(
                    fmt::format("{}_scoreDetails", pipelineName),
                    BSON("$mergeObjects"
                         << BSON_ARRAY(fmt::format("$$value.{}_scoreDetails", pipelineName)
                                       << fmt::format("$$this.{}_scoreDetails", pipelineName))));
            }
        }
        inBob.done();

        reduceBob.done();
        internalFieldsBob.done();
        projectBob.done();
    }
    bob.done();
    return bob.obj();
}

BSONObj promoteEmbeddedDocsObject(const StringData internalFieldsDocsName) {
    BSONObjBuilder bob;
    {
        BSONObjBuilder replaceRootBob(bob.subobjStart("$replaceRoot"_sd));
        BSONObjBuilder newRootBob(replaceRootBob.subobjStart("newRoot"_sd));

        BSONArrayBuilder mergeObjectsArrayBab;
        mergeObjectsArrayBab.append("$" + internalFieldsDocsName);
        mergeObjectsArrayBab.append("$$ROOT");
        mergeObjectsArrayBab.done();

        newRootBob.append("$mergeObjects", mergeObjectsArrayBab.arr());
        newRootBob.done();
        replaceRootBob.done();
    }
    bob.done();
    return bob.obj();
}

BSONObj projectRemoveEmbeddedDocsObject(const StringData internalFieldsDocsName) {
    BSONObjBuilder bob;
    {
        BSONObjBuilder projectBob(bob.subobjStart("$project"_sd));
        projectBob.append(internalFieldsDocsName, 0);
        projectBob.done();
    }
    bob.done();
    return bob.obj();
}

BSONObj projectRemoveInternalFieldsObject(const StringData internalFieldsName) {
    BSONObjBuilder bob;
    {
        BSONObjBuilder projectBob(bob.subobjStart("$project"_sd));
        projectBob.append(internalFieldsName, 0);
        projectBob.done();
    }
    bob.done();
    return bob.obj();
}

namespace score_details {

std::pair<std::string, BSONObj> constructScoreDetailsForGrouping(const std::string pipelineName) {
    const std::string scoreDetailsName = fmt::format("{}_scoreDetails", pipelineName);
    return std::make_pair(scoreDetailsName,
                          BSON("$mergeObjects" << fmt::format("${}", scoreDetailsName)));
}

std::string stringifyExpression(boost::optional<IDLAnyType> expression) {
    BSONObjBuilder expressionBob;
    expression->serializeToBSON("string", &expressionBob);
    expressionBob.done();
    std::string exprString = expressionBob.obj().toString();
    std::replace(exprString.begin(), exprString.end(), '\"', '\'');
    return exprString;
}

boost::intrusive_ptr<DocumentSource> constructCalculatedFinalScoreDetails(
    const StringData internalFieldsName,
    const std::vector<std::string>& pipelineNames,
    const StringMap<double>& weights,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    BSONObjBuilder bob;
    {
        BSONObjBuilder addFieldsBob(bob.subobjStart("$addFields"_sd));
        {
            BSONObjBuilder internalFieldsBob(addFieldsBob.subobjStart(internalFieldsName));
            {
                BSONArrayBuilder calculatedScoreDetailsArr;
                for (const auto& pipelineName : pipelineNames) {
                    const std::string internalFieldsPipelineName =
                        hybrid_scoring_util::applyInternalFieldPrefixToFieldName(internalFieldsName,
                                                                                 pipelineName);
                    const std::string scoreDetailsFieldName =
                        fmt::format("${}_scoreDetails", pipelineName);
                    double weight = hybrid_scoring_util::getPipelineWeight(weights, pipelineName);
                    BSONObjBuilder mergeObjectsArrSubObj;
                    mergeObjectsArrSubObj.append("inputPipelineName"_sd, pipelineName);
                    if (internalFieldsName ==
                        DocumentSourceRankFusion::kRankFusionInternalFieldsName) {
                        std::string internalFieldsInputPipelineRankPath =
                            fmt::format("${}_rank", internalFieldsPipelineName);
                        mergeObjectsArrSubObj.append("rank"_sd,
                                                     internalFieldsInputPipelineRankPath);
                        // In the scoreDetails output, for any input pipeline that didn't output a
                        // document in the
                        // result, the default "rank" will be "NA" and the weight will be omitted to
                        // make it clear to the user that the final score for that document result
                        // did not take into account its input pipeline's rank/weight.
                        mergeObjectsArrSubObj.append(
                            "weight",
                            BSON("$cond" << BSON_ARRAY(
                                     BSON("$eq" << BSON_ARRAY(internalFieldsInputPipelineRankPath
                                                              << "NA"))
                                     << "$$REMOVE" << weight)));
                    } else if (internalFieldsName ==
                               DocumentSourceScoreFusion::kScoreFusionInternalFieldsName) {
                        mergeObjectsArrSubObj.append(
                            "inputPipelineRawScore"_sd,
                            fmt::format("${}_rawScore", internalFieldsPipelineName));
                        mergeObjectsArrSubObj.append("weight"_sd, weight);
                        mergeObjectsArrSubObj.append(
                            "value"_sd, fmt::format("${}_score", internalFieldsPipelineName));
                    }
                    mergeObjectsArrSubObj.done();
                    BSONArrayBuilder mergeObjectsArr;
                    mergeObjectsArr.append(mergeObjectsArrSubObj.obj());
                    mergeObjectsArr.append(
                        fmt::format("${}.{}_scoreDetails", internalFieldsName, pipelineName));
                    mergeObjectsArr.done();
                    BSONObj mergeObjectsObj = BSON("$mergeObjects"_sd << mergeObjectsArr.arr());
                    calculatedScoreDetailsArr.append(mergeObjectsObj);
                }
                calculatedScoreDetailsArr.done();
                internalFieldsBob.append("calculatedScoreDetails", calculatedScoreDetailsArr.arr());
            }
            internalFieldsBob.done();
        }
    }
    const BSONObj spec = bob.obj();
    return DocumentSourceAddFields::createFromBson(spec.firstElement(), expCtx);
}

}  // namespace score_details
}  // namespace mongo::hybrid_scoring_util
