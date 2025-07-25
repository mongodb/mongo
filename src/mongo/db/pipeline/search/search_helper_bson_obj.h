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

#include "mongo/db/pipeline/document_source_rank_fusion.h"
#include "mongo/db/pipeline/document_source_score_fusion.h"
#include "mongo/db/pipeline/search/document_source_list_search_indexes.h"
#include "mongo/db/pipeline/search/document_source_search.h"
#include "mongo/db/pipeline/search/document_source_search_meta.h"
#include "mongo/db/pipeline/search/document_source_vector_search.h"

#include "document_source_list_search_indexes.h"

namespace mongo {

namespace search_helper_bson_obj {

/**
 * Checks that the pipeline isn't empty and if the first stage in the pipeline is a mongot stage
 * (either $search, $vectorSearch, $searchMeta, $listSearchIndexes, $rankFusion, or $scoreFusion).
 */
inline bool isMongotPipeline(const std::vector<BSONObj> pipeline) {
    // Note that the $rankFusion/$scoreFusion stages are syntactic sugar. When desugared, the first
    // stage in its first pipeline in the $rankFusion/$scoreFusion query (ex: $rankFusion: {
    //    input: {
    //        pipelines: {
    //            searchPipeline: [
    //                { $search: {...} },
    //                { $sort: {author: 1} }
    //            ]
    //        }
    //    }
    //}) will become the first stage in the final desugared ouput. Thus, there is an extra recursive
    // call to check for this.
    if (pipeline.size() >= 1 &&
        (pipeline[0][DocumentSourceSearch::kStageName] ||
         pipeline[0][DocumentSourceVectorSearch::kStageName] ||
         pipeline[0][DocumentSourceSearchMeta::kStageName] ||
         pipeline[0][DocumentSourceListSearchIndexes::kStageName] ||
         (pipeline[0][DocumentSourceRankFusion::kStageName] &&
          isMongotPipeline(std::vector<BSONObj>{
              pipeline[0][DocumentSourceRankFusion::kStageName][RankFusionSpec::kInputFieldName]
                      [RankFusionInputSpec::kPipelinesFieldName]
                          .Obj()
                          .firstElement()
                          .Array()[0]
                          .Obj()})) ||
         (pipeline[0][DocumentSourceScoreFusion::kStageName] &&
          isMongotPipeline(std::vector<BSONObj>{
              pipeline[0][DocumentSourceScoreFusion::kStageName][ScoreFusionSpec::kInputFieldName]
                      [ScoreFusionInputsSpec::kPipelinesFieldName]
                          .Obj()
                          .firstElement()
                          .Array()[0]
                          .Obj()})))) {
        return true;
    }
    return false;
}

inline bool isStoredSource(const std::vector<BSONObj> pipeline) {
    if (pipeline.size() >= 1 && pipeline[0][DocumentSourceSearch::kStageName]) {
        auto searchStage = pipeline[0][DocumentSourceSearch::kStageName];
        auto storedSourceElem = searchStage[mongot_cursor::kReturnStoredSourceArg];
        return !storedSourceElem.eoo() && storedSourceElem.Bool();
    }
    return false;
}


}  // namespace search_helper_bson_obj


}  // namespace mongo
