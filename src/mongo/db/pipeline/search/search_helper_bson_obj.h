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

#pragma once

#include "mongo/db/feature_flag.h"
#include "mongo/db/pipeline/document_source_rank_fusion.h"
#include "mongo/db/pipeline/document_source_rank_fusion_gen.h"
#include "mongo/db/pipeline/document_source_score_fusion.h"
#include "mongo/db/pipeline/document_source_score_fusion_gen.h"
#include "mongo/db/pipeline/search/document_source_list_search_indexes.h"
#include "mongo/db/pipeline/search/document_source_search.h"
#include "mongo/db/pipeline/search/document_source_search_meta.h"
#include "mongo/db/pipeline/search/document_source_vector_search.h"
#include "mongo/util/modules.h"

namespace mongo {

namespace search_helper_bson_obj {

namespace detail {
/**
 * Returns true if 'spec' uses the name of 'DocSourceName' type, false otherwise.
 */
template <typename DocSourceName>
bool is(const BSONObj& spec) {
    return spec.hasField(DocSourceName::kStageName);
}

inline bool hasMongotExtension(const auto& extensionNames) {
    return std::find_if(extensionNames.begin(), extensionNames.end(), [&](const auto& name) {
               return name == "mongot-extension";
           }) != extensionNames.end();
}

/**
 * Returns true if the given $vectorSearch inner spec has returnStoredSource: true.
 * TODO SERVER-121764: Remove this function when the extension supports returnStoredSource.
 */
inline bool isReturnStoredSource(const BSONObj& vectorSearchSpec) {
    auto returnStoredSourceElem =
        vectorSearchSpec[DocumentSourceVectorSearch::kReturnStoredSourceFieldName];
    return returnStoredSourceElem.isBoolean() && returnStoredSourceElem.boolean();
}

/**
 * Returns true if the given stage BSONObj is a $vectorSearch stage with returnStoredSource: true.
 * TODO SERVER-121764: Remove this function when the extension supports returnStoredSource.
 */
inline bool isVectorSearchReturnStoredSource(const BSONObj& stageBson) {
    auto vectorSearchElem = stageBson[DocumentSourceVectorSearch::kStageName];
    if (!vectorSearchElem || vectorSearchElem.type() != BSONType::object) {
        return false;
    }
    return isReturnStoredSource(vectorSearchElem.Obj());
}

}  // namespace detail

/**
 * Checks that the pipeline isn't empty and if the first stage in the pipeline is the $vectorSearch
 * extension stage. The legacy stage returns false.
 *
 * TODO SERVER-117168 Remove this function.
 */
inline bool isExtensionVectorSearchPipeline(
    const std::shared_ptr<IncrementalFeatureRolloutContext>& ifrContext,
    const std::vector<BSONObj>& pipeline) {
    if (pipeline.empty()) {
        return false;
    }
    const auto& firstStageBson = pipeline[0];
    using detail::is;
    if (is<DocumentSourceVectorSearch>(firstStageBson)) {
        // Note we don't need to worry about/consult 'featureFlagExtensionViewsAndUnionWith'
        // because the extension will enforce this behavior by toggling
        // 'gFeatureFlagVectorSearchExtension' and retrying, so thankfully this is enough, and
        // we don't need to understand what context this BSON appears in (w.r.t. views or
        // sub-pipelines).
        // TODO SERVER-121764: Remove the returnStoredSource check when the extension supports it.
        // Fall back to legacy execution when returnStoredSource is true, as the extension does
        // not support this option.
        return ifrContext->getSavedFlagValue(feature_flags::gFeatureFlagVectorSearchExtension) &&
            detail::hasMongotExtension(serverGlobalParams.extensions) &&
            !detail::isVectorSearchReturnStoredSource(firstStageBson);
    }
    return false;
}

/**
 * Checks that the pipeline isn't empty and if the first stage in the pipeline is a mongot stage
 * Namely, that includes:
 * - $search
 * - $vectorSearch (legacy implementation only, extensions intentionally need to avoid this kind of
 * special casing)
 * - $searchMeta
 * - $listSearchIndexes
 * - $rankFusion (starting with a search stage)
 * - $scoreFusion (starting with a search stage).
 */
inline bool isMongotPipeline(const std::shared_ptr<IncrementalFeatureRolloutContext>& ifrContext,
                             const std::vector<BSONObj>& pipeline) {
    if (pipeline.empty()) {
        return false;
    }
    const auto& firstStageBson = pipeline[0];
    using detail::is;
    if (is<DocumentSourceVectorSearch>(firstStageBson)) {
        // Return true if the $vectorSearch implementation would be the legacy
        // DocumentSourceVectorSearch-based implementation. Note we don't need to worry
        // about/consult 'featureFlagExtensionViewsAndUnionWith' because the extension will enforce
        // this behavior by toggling 'gFeatureFlagVectorSearchExtension' and retrying, so thankfully
        // this is enough, and we don't need to understand what context this BSON appears in (w.r.t.
        // views or sub-pipelines).
        // TODO SERVER-121764: Remove the returnStoredSource check when the extension supports it.
        // Also fall back to legacy when returnStoredSource is true, as the extension does not
        // support this option.
        return !detail::hasMongotExtension(serverGlobalParams.extensions) ||
            !ifrContext->getSavedFlagValue(feature_flags::gFeatureFlagVectorSearchExtension) ||
            detail::isVectorSearchReturnStoredSource(firstStageBson);
    } else if (is<DocumentSourceRankFusion>(firstStageBson)) {
        // Note that the $rankFusion/$scoreFusion firstStageBsons are syntactic sugar. When
        // desugared, the first firstStageBson in its first pipeline in the $rankFusion/$scoreFusion
        // query (ex: $rankFusion:
        // {
        //    input: {
        //        pipelines: {
        //            searchPipeline: [
        //                { $search: {...} },
        //                { $sort: {author: 1} }
        //            ]
        //        }
        //    }
        //})
        // will become the first firstStageBson in the final desugared ouput. Thus, there is an
        // extra recursive call to check for this.
        auto rankFusionFirstPipeline =
            firstStageBson[DocumentSourceRankFusion::kStageName][RankFusionSpec::kInputFieldName]
                          [RankFusionInputSpec::kPipelinesFieldName]
                              .Obj()
                              .firstElement()
                              .Array();
        if (rankFusionFirstPipeline.empty()) {
            return false;
        }
        return isMongotPipeline(ifrContext, std::vector<BSONObj>{rankFusionFirstPipeline[0].Obj()});
    } else if (is<DocumentSourceScoreFusion>(firstStageBson)) {
        auto scoreFusionFirstPipeline =
            firstStageBson[DocumentSourceScoreFusion::kStageName][ScoreFusionSpec::kInputFieldName]
                          [ScoreFusionInputsSpec::kPipelinesFieldName]
                              .Obj()
                              .firstElement()
                              .Array();
        if (scoreFusionFirstPipeline.empty()) {
            return false;
        }
        return isMongotPipeline(ifrContext,
                                std::vector<BSONObj>{scoreFusionFirstPipeline[0].Obj()});
    } else {
        return is<DocumentSourceSearch>(firstStageBson) ||
            is<DocumentSourceSearchMeta>(firstStageBson) ||
            is<DocumentSourceListSearchIndexes>(firstStageBson);
    }
}

}  // namespace search_helper_bson_obj


}  // namespace mongo
