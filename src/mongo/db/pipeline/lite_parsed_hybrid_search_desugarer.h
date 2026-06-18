/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
