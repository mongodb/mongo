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

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/pipeline/document_source_add_fields.h"
#include "mongo/db/pipeline/document_source_internal_inhibit_optimization.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_rank_fusion.h"
#include "mongo/db/pipeline/document_source_sample.h"
#include "mongo/db/pipeline/document_source_set_metadata.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/document_source_skip.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/search/document_source_search.h"
#include "mongo/db/pipeline/search/document_source_vector_search.h"
#include "mongo/util/str.h"

namespace mongo::hybrid_scoring_util {

// TODO SERVER-100754: A pipeline that begins with a $match stage that isTextQuery() should also
// count.
// TODO SERVER-100754 This custom logic should be able to be replaced by using DepsTracker to
// walk the pipeline and see if "score" metadata is produced.
bool isScoredPipeline(const Pipeline& pipeline) {
    // Note that we don't check for $rankFusion explicitly because it will be
    // desugared by this point.
    static const std::set<StringData> implicitlyScoredStages{DocumentSourceVectorSearch::kStageName,
                                                             DocumentSourceSearch::kStageName};
    auto sources = pipeline.getSources();
    if (sources.empty()) {
        return false;
    }

    auto firstStageName = sources.front()->getSourceName();
    return implicitlyScoredStages.contains(firstStageName);
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
    static const std::set<StringData> validSelectionStagesForHybridSearch = {
        DocumentSourceInternalInhibitOptimization::kStageName,
        DocumentSourceLimit::kStageName,
        DocumentSourceMatch::kStageName,
        DocumentSourceRankFusion::kStageName,
        DocumentSourceSample::kStageName,
        DocumentSourceSkip::kStageName,
        DocumentSourceSort::kStageName,
        DocumentSourceVectorSearch::kStageName,
    };

    if (bsonStage.isEmpty()) {
        // Empty BSON stage was provided - it is not a selection stage.
        return Status(ErrorCodes::Error::InvalidBSON, "Input stages must not be empty.");
    }

    auto fieldName = *bsonStage.getFieldNames<std::set<std::string>>().begin();
    if (validSelectionStagesForHybridSearch.contains(fieldName)) {
        return Status::OK();
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
        if (!specBsonObj.hasField(kReturnStoredSourceArg)) {
            // The spec does not specify 'returnStoredSource' and the default is false.
            // This is a selection stage.
            return Status::OK();
        }

        const auto& returnStoredSourceArg = specBsonObj[kReturnStoredSourceArg];
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
    static const std::set<StringData> implicitlyRankedStages{
        DocumentSourceRankFusion::kStageName,
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

Status isScoredPipeline(const std::vector<BSONObj>& bsonPipeline,
                        const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    if (bsonPipeline.empty()) {
        return Status(ErrorCodes::Error::BadValue, "Input pipeline must not be empty.");
    }

    // Please keep the following in alphabetical order.
    static const std::set<StringData> implicitlyScoredStages{
        DocumentSourceRankFusion::kStageName,
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

    return firstStageIsImplicitlyScored
        ? Status::OK()
        : Status(ErrorCodes::Error::BadValue, "Pipeline did not begin with a scored stage.");
}

bool isHybridSearchPipeline(const std::vector<BSONObj>& bsonPipeline) {
    tassert(10473000, "Input pipeline must not be empty.", !bsonPipeline.empty());

    // Please keep the following in alphabetical order.
    static const std::set<StringData> hybridScoringStages{
        DocumentSourceRankFusion::kStageName,
    };

    for (const auto& stage : bsonPipeline) {
        tassert(10473001, "Input pipeline stage must not be empty.", !stage.isEmpty());
        if (hybridScoringStages.contains(*(stage.getFieldNames<std::set<std::string>>().begin()))) {
            return true;
        }
    };

    return false;
}
}  // namespace mongo::hybrid_scoring_util
