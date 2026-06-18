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

#include "mongo/base/string_data.h"
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

// Prefix for the flat scalar group keys (e.g. "__hs_<p>_score") used in the desugared $group's
// per-pipeline accumulators and in the subsequent $replaceRoot's wrapper object.
inline constexpr std::string_view kHsFlatFieldPrefix = "__hs_"_sd;

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
