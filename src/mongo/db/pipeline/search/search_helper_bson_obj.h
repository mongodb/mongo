// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/feature_flag.h"
#include "mongo/db/pipeline/document_source_internal_document_results_and_metadata.h"
#include "mongo/db/pipeline/document_source_rank_fusion.h"
#include "mongo/db/pipeline/document_source_rank_fusion_gen.h"
#include "mongo/db/pipeline/document_source_score_fusion.h"
#include "mongo/db/pipeline/document_source_score_fusion_gen.h"
#include "mongo/db/pipeline/search/document_source_internal_search_id_lookup.h"
#include "mongo/db/pipeline/search/document_source_internal_search_mongot_remote.h"
#include "mongo/db/pipeline/search/document_source_list_search_indexes.h"
#include "mongo/db/pipeline/search/document_source_search.h"
#include "mongo/db/pipeline/search/document_source_search_meta.h"
#include "mongo/db/pipeline/search/document_source_vector_search.h"
#include "mongo/db/pipeline/search/mongot_extension_name_gen.h"
#include "mongo/db/pipeline/search/search_helper.h"
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

/**
 * A mongot $lookup subpipeline (i.e. a $search/$searchMeta/$vectorSearch subpipeline on a
 * mongot-indexed view) begins with a "prefix" of stages that must run before the
 * localField/foreignField equality $match. The prefix is one "source" stage followed by zero or
 * more "support" stages:
 *   - source: the search-producing stage that must come first. This can be the user-facing
 *     $search/$searchMeta/$vectorSearch, its legacy desugared $_internalSearchMongotRemote, or the
 *     extension-desugared $_internalDocumentResultsAndMetadata / $_extensionSearch(Meta) /
 *     $_extensionVectorSearch.
 *   - support: $_internalSearchIdLookup (which also applies the view transforms) and the
 *     storedSource $replaceRoot (see isStoredSourceReplaceRoot for the recognized shapes).
 *
 * The join $match must be inserted immediately after this whole prefix: after the source stage so
 * the mongot stage stays first, and after the support stages so the view transforms / storedSource
 * promotion have already been applied when the equality match runs.
 *
 * TODO SERVER-121094 Revisit once legacy mongot branches are removed from pipeline
 * parsing/desugaring/resolution.
 */
namespace mongot_lookup_prefix {
namespace detail {
// The storedSource $replaceRoot promotes the mongot 'storedSource' field to the document root.
// Two shapes exist depending on which desugaring produced the stage:
//   - Legacy $search (search_helpers::promoteStoredSourceOrAddIdLookup):
//       {$replaceRoot: {newRoot: {$ifNull: ["$storedSource", "$$ROOT"]}}}
//   - Extension $search:
//       {$replaceRoot: {newRoot: "$storedSource"}}
// Both promote the "$storedSource" field, so either is recognized here.
inline bool isStoredSourceReplaceRoot(const BSONObj& stage) {
    auto replaceRootElem = stage["$replaceRoot"];
    if (!replaceRootElem || replaceRootElem.type() != BSONType::object) {
        return false;
    }
    auto newRoot = replaceRootElem.Obj()["newRoot"];
    if (!newRoot) {
        return false;
    }
    static const std::string storedSourceRef =
        "$" + std::string{search_helpers::kProtocolStoredFieldsName};
    // Extension shape: newRoot is the "$storedSource" field path directly.
    if (newRoot.type() == BSONType::string) {
        return newRoot.valueStringData() == storedSourceRef;
    }
    // Legacy shape: newRoot is {$ifNull: ["$storedSource", "$$ROOT"]}.
    if (newRoot.type() != BSONType::object) {
        return false;
    }
    auto ifNull = newRoot.Obj()["$ifNull"];
    if (!ifNull || ifNull.type() != BSONType::array) {
        return false;
    }
    auto args = ifNull.Obj();
    return args.nFields() == 2 && args[0].type() == BSONType::string &&
        args[0].valueStringData() == storedSourceRef && args[1].type() == BSONType::string &&
        args[1].valueStringData() == "$$ROOT"sv;
}
}  // namespace detail

/**
 * Returns true if 'stage' is a mongot source stage (see above).
 */
inline bool isSourceStage(const BSONObj& stage) {
    return
        // TODO SERVER-131408 Remove the mongot-extension-specific recognition below (the
        // $_internalDocumentResultsAndMetadata / $_extensionSearch / $_extensionSearchMeta /
        // $_extensionVectorSearch stage-name checks) once $lookup no longer needs to special-case
        // the extension desugar shapes here.
        stage.hasField(DocumentSourceInternalDocumentResultsAndMetadata::kStageName) ||
        stage.hasField(search_helpers::kExtensionSearchStageName) ||
        stage.hasField(search_helpers::kExtensionSearchMetaStageName) ||
        stage.hasField(search_helpers::kExtensionVectorSearchStageName) ||
        stage.hasField(DocumentSourceInternalSearchMongotRemote::kStageName) ||
        stage.hasField(DocumentSourceSearch::kStageName) ||
        stage.hasField(DocumentSourceSearchMeta::kStageName) ||
        stage.hasField(DocumentSourceVectorSearch::kStageName);
}

/**
 * Returns true if 'stage' is a mongot support stage (see above): $_internalSearchIdLookup or the
 * storedSource $replaceRoot.
 */
inline bool isSupportStage(const BSONObj& stage) {
    return stage.hasField(DocumentSourceInternalSearchIdLookUp::kStageName) ||
        detail::isStoredSourceReplaceRoot(stage);
}

/**
 * Returns the length of the leading mongot prefix of 'pipeline' (a source stage followed by any
 * support stages), or 0 if 'pipeline' does not start with a mongot source stage. Prefer this over
 * extractPrefix when only the prefix boundary is needed, to avoid copying the stages.
 */
inline size_t prefixEndIdx(const std::vector<BSONObj>& pipeline) {
    if (pipeline.empty() || !isSourceStage(pipeline[0])) {
        return 0;
    }
    size_t endIdx = 1;
    while (endIdx < pipeline.size() && isSupportStage(pipeline[endIdx])) {
        ++endIdx;
    }
    return endIdx;
}

/**
 * Returns the leading mongot prefix of 'pipeline' (a source stage followed by any support stages),
 * or an empty vector if 'pipeline' does not start with a mongot source stage.
 */
inline std::vector<BSONObj> extractPrefix(const std::vector<BSONObj>& pipeline) {
    return std::vector<BSONObj>(pipeline.begin(), pipeline.begin() + prefixEndIdx(pipeline));
}
}  // namespace mongot_lookup_prefix

}  // namespace search_helper_bson_obj


}  // namespace mongo
