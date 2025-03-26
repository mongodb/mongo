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

#include "mongo/db/pipeline/document_source_add_fields.h"
#include "mongo/db/pipeline/document_source_set_metadata.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/search/document_source_search.h"
#include "mongo/db/pipeline/search/document_source_vector_search.h"

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

// TODO SERVER-100754: A pipeline that begins with a $match stage that isTextQuery() should also
// count.
// TODO SERVER-100754 This custom logic should be able to be replaced by using DepsTracker to
// walk the pipeline and see if "score" metadata is produced.
bool isScoredPipeline(const Pipeline& pipeline) {
    // Note that we don't check for $rankFusion and $scoreFusion explicitly because it will be
    // desugared by this point.
    static const std::set<StringData> implicitlyScoredStages{DocumentSourceVectorSearch::kStageName,
                                                             DocumentSourceSearch::kStageName};
    auto sources = pipeline.getSources();
    if (sources.empty()) {
        return false;
    }

    auto firstStageName = sources.front()->getSourceName();
    return implicitlyScoredStages.contains(firstStageName) ||
        std::any_of(sources.begin(), sources.end(), isScoreStage);
}

double getPipelineWeight(const StringMap<double>& weights, const std::string& pipelineName) {
    // If no weight is provided, default to 1.
    return weights.contains(pipelineName) ? weights.at(pipelineName) : 1;
}

namespace score_details {

boost::intrusive_ptr<DocumentSource> addScoreDetails(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const std::string& prefix,
    const bool inputGeneratesScore,
    const bool inputGeneratesScoreDetails) {
    const std::string scoreDetails = fmt::format("{}_scoreDetails", prefix);
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
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    std::vector<boost::intrusive_ptr<Expression>> detailsChildren;
    for (auto it = inputs.begin(); it != inputs.end(); it++) {
        const std::string& pipelineName = it->first;
        const std::string rankFieldName = fmt::format("${}_rank", pipelineName);
        const std::string scoreDetailsFieldName = fmt::format("${}_scoreDetails", pipelineName);
        double weight = getPipelineWeight(weights, pipelineName);

        auto mergeObjectsObj =
            BSON("$mergeObjects"_sd << BSON_ARRAY(BSON("inputPipelineName"_sd
                                                       << pipelineName << "rank"_sd << rankFieldName
                                                       << "weight"_sd << weight)
                                                  << scoreDetailsFieldName));
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

boost::intrusive_ptr<DocumentSource> constructScoreDetailsMetadata(
    const std::string& scoreDetailsDescription,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    auto setScoreDetails = DocumentSourceSetMetadata::create(
        expCtx,
        Expression::parseObject(expCtx.get(),
                                BSON("value" << "$score"
                                             << "description" << scoreDetailsDescription
                                             << "details"
                                             << "$calculatedScoreDetails"),
                                expCtx->variablesParseState),
        DocumentMetadataFields::kScoreDetails);
    return setScoreDetails;
}
}  // namespace score_details
}  // namespace mongo::hybrid_scoring_util
