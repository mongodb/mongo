// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/lite_parsed_rank_fusion.h"
#include "mongo/db/pipeline/lite_parsed_score_fusion.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <string_view>

namespace mongo::lite_parsed_hybrid_search_desugarer {

/**
 * Desugars a $rankFusion lite-parsed stage into the equivalent list of LiteParsedDocumentSource
 * stages.
 *
 * `nss` is the namespace used to parse the synthesized stages. In production callers, this is
 * `pipeline->getOriginalParseNss()` from the outer LiteParsedPipeline.
 *
 * `userCollName` is the collection name to use for synthesized $unionWith stages. In production
 * callers, this is `pipeline->getOriginalParseNss().coll()` (the namespace the outer aggregation
 * runs against).
 */
StageSpecs desugarRankFusion(const LiteParsedRankFusion& stage,
                             const NamespaceString& nss,
                             std::string_view userCollName);

/**
 * StageExpander implementation as required by LiteParsedDesugarer::StageExpander. Downcasts
 * `stage` to LiteParsedRankFusion, derives `userCollName` from `pipeline->getOriginalParseNss()`,
 * runs `desugarRankFusion`, and replaces the stage at `index` with the result. Returns the index
 * after the inserted block.
 */
size_t rankFusionStageExpander(LiteParsedPipeline* pipeline,
                               size_t index,
                               LiteParsedDocumentSource& stage);

/**
 * Desugars a $scoreFusion lite-parsed stage into the equivalent list of LiteParsedDocumentSource
 * stages.
 *
 * `nss` is the namespace used to parse the synthesized stages. In production callers, this is
 * `pipeline->getOriginalParseNss()` from the outer LiteParsedPipeline.
 *
 * `userCollName` is the collection name to use for synthesized $unionWith stages. In production
 * callers, this is `pipeline->getOriginalParseNss().coll()` (the namespace the outer aggregation
 * runs against).
 */
StageSpecs desugarScoreFusion(const LiteParsedScoreFusion& stage,
                              const NamespaceString& nss,
                              std::string_view userCollName);

/**
 * StageExpander implementation as required by LiteParsedDesugarer::StageExpander. Downcasts
 * `stage` to LiteParsedScoreFusion, derives `userCollName` from `pipeline->getOriginalParseNss()`,
 * runs `desugarScoreFusion`, and replaces the stage at `index` with the result. Returns the index
 * after the inserted block.
 */
size_t scoreFusionStageExpander(LiteParsedPipeline* pipeline,
                                size_t index,
                                LiteParsedDocumentSource& stage);

}  // namespace mongo::lite_parsed_hybrid_search_desugarer
