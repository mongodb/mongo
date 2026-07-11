// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
#include "mongo/db/pipeline/search/mongot_extension_name_gen.h"
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
    // We must check prefix rather than exact equality because testing infrastructure uses uuid
    // suffixes to differentiate between different mongot/d instances.
    return std::find_if(extensionNames.begin(), extensionNames.end(), [&](const auto& name) {
               return std::string_view{name}.starts_with(kMongotExtensionName);
           }) != extensionNames.end();
}

}  // namespace detail

/**
 * Checks that the pipeline isn't empty and if the first stage is an extension-implemented mongot
 * stage ($vectorSearch, $search, or $searchMeta). Returns false for legacy implementations.
 *
 * TODO SERVER-117168 Remove this function.
 */
inline bool isExtensionMongotPipeline(
    const std::shared_ptr<IncrementalFeatureRolloutContext>& ifrContext,
    const std::vector<BSONObj>& pipeline) {
    if (pipeline.empty()) {
        return false;
    }
    const auto& firstStageBson = pipeline[0];
    using detail::is;
    // Note we don't need to worry about/consult 'featureFlagExtensionsInsideHybridSearch' because
    // the extension will enforce this behavior by toggling its IFR flag and retrying, so thankfully
    // this is enough, and we don't need to understand what context this BSON appears in (w.r.t.
    // views or sub-pipelines).
    if (is<DocumentSourceVectorSearch>(firstStageBson)) {
        return ifrContext->getSavedFlagValue(feature_flags::gFeatureFlagVectorSearchExtension) &&
            detail::hasMongotExtension(serverGlobalParams.extensions);
    } else if (is<DocumentSourceSearch>(firstStageBson) ||
               is<DocumentSourceSearchMeta>(firstStageBson)) {
        return ifrContext->getSavedFlagValue(feature_flags::gFeatureFlagSearchExtension) &&
            detail::hasMongotExtension(serverGlobalParams.extensions);
    }
    return false;
}

/**
 * Returns true if the pipeline's first stage is a hybrid search stage ($rankFusion or
 * $scoreFusion).
 * TODO SERVER-121094 Delete once $rankFusion/$scoreFusion desugar at LiteParsed time; the BSON
 * pipeline will no longer start with those stages by the time this is consulted.
 */
inline bool isHybridSearchBsonPipeline(const std::vector<BSONObj>& pipeline) {
    if (pipeline.empty()) {
        return false;
    }
    using detail::is;
    return is<DocumentSourceRankFusion>(pipeline[0]) || is<DocumentSourceScoreFusion>(pipeline[0]);
}

/**
 * Checks that the pipeline isn't empty and if the first stage in the pipeline is a mongot stage
 * using the legacy implementation. Namely, that includes:
 * - $vectorSearch (legacy implementation only, extensions intentionally need to avoid this kind of
 * special casing)
 * - $search (legacy implementation only)
 * - $searchMeta (legacy implementation only)
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
        // about/consult 'featureFlagExtensionsInsideHybridSearch' because the extension will
        // enforce this behavior by toggling 'gFeatureFlagVectorSearchExtension' and retrying, so
        // thankfully this is enough, and we don't need to understand what context this BSON appears
        // in (w.r.t. views or sub-pipelines).
        return !detail::hasMongotExtension(serverGlobalParams.extensions) ||
            !ifrContext->getSavedFlagValue(feature_flags::gFeatureFlagVectorSearchExtension);
    } else if (is<DocumentSourceSearch>(firstStageBson) ||
               is<DocumentSourceSearchMeta>(firstStageBson)) {
        // Return true if the $search/$searchMeta implementation would be the legacy
        // implementation. The extension enforces context restrictions by toggling
        // 'gFeatureFlagSearchExtension' and retrying, so this is enough.
        return !detail::hasMongotExtension(serverGlobalParams.extensions) ||
            !ifrContext->getSavedFlagValue(feature_flags::gFeatureFlagSearchExtension);
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
        return is<DocumentSourceListSearchIndexes>(firstStageBson);
    }
}

}  // namespace search_helper_bson_obj


}  // namespace mongo
