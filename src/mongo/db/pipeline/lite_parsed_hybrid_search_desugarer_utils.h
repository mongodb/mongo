// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/util/string_map.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace mongo::lite_parsed_hybrid_search_desugarer::common_utils {
using namespace std::literals::string_view_literals;

// Prefix for the flat scalar group keys (e.g. "__hs_<p>_score") used in the desugared $group's
// per-pipeline accumulators and in the subsequent $replaceRoot's wrapper object.
inline constexpr std::string_view kHsFlatFieldPrefix = "__hs_"sv;

// Parses a synthesized BSONObj stage into an owned LPDS at namespace `nss`. Not for use with
// stages that hold nested LiteParsedPipelines -- use buildUnionWithLPDS for $unionWith.
std::unique_ptr<LiteParsedDocumentSource> parseOwnedStage(const NamespaceString& nss,
                                                          BSONObj stageBson);

// Walks `stages` right-to-left and rewrites the rightmost $sort to emit sort-key metadata.
// No-op if no $sort is present (e.g. $search and $vectorSearch emit scored output without one).
void mutateRightmostSortToOutputSortKey(const NamespaceString& nss, StageSpecs& stages);

// Builds a $unionWith LPDS whose subpipeline is `perPipelineStages`, targeting `userCollName`
// on the same DB as `nss`.
std::unique_ptr<LiteParsedDocumentSource> buildUnionWithLPDS(const NamespaceString& nss,
                                                             std::string_view userCollName,
                                                             StageSpecs perPipelineStages);

// Validates the user-provided combination.weights BSON against the input pipeline names and
// returns the resulting weights map.
StringMap<double> validateWeights(const BSONObj& inputWeights,
                                  const std::vector<std::string>& pipelineNames,
                                  std::string_view stageName);

// {$replaceWith: {<docsName>: "$$ROOT"}}
BSONObj buildReplaceRootBson(std::string_view docsName);

// {$sort: {score: {$meta: "score"}, _id: 1}}
BSONObj buildSortByScoreMetaBson();

// {$project: {<internalFieldsName>: 0}}
BSONObj buildProjectRemoveInternalFieldsBson(std::string_view internalFieldsName);

// {$group: {_id: "$<docsName>._id",
//           <docsName>: {$first: "$<docsName>"},
//           __hs_<p>_score: {$max: {$ifNull: ["$<internalFieldsName>.<p>_score", 0]}},
//           [if scoreDetails:] __hs_<p><detailsScalarSuffix>: {$max: ...},
//                              __hs_<p>_scoreDetails: {$mergeObjects: ...},
//           ...}}
//
// `detailsScalarSuffix` is the per-pipeline scoreDetails scalar field suffix:
//   "_rank" for $rankFusion, "_rawScore" for $scoreFusion.
BSONObj buildGroupBson(const std::vector<std::string>& pipelineNames,
                       bool includeScoreDetails,
                       std::string_view internalFieldsName,
                       std::string_view docsName,
                       std::string_view detailsScalarSuffix);

// {$replaceRoot: {newRoot: {$mergeObjects: ["$<docsName>",
//                                            {<internalFieldsName>: {<p>_score: "$__hs_<p>_score",
//                                                                    ...}}]}}}
//
// `detailsScalarSuffix`: "_rank" for $rankFusion, "_rawScore" for $scoreFusion.
BSONObj buildReplaceRootMergeBson(const std::vector<std::string>& pipelineNames,
                                  bool includeScoreDetails,
                                  std::string_view internalFieldsName,
                                  std::string_view docsName,
                                  std::string_view detailsScalarSuffix);

}  // namespace mongo::lite_parsed_hybrid_search_desugarer::common_utils
