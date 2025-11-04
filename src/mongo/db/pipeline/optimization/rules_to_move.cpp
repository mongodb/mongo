/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/pipeline/document_source_densify.h"
#include "mongo/db/pipeline/document_source_geo_near.h"
#include "mongo/db/pipeline/document_source_graph_lookup.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_internal_all_collection_stats.h"
#include "mongo/db/pipeline/document_source_internal_compute_geo_near_distance.h"
#include "mongo/db/pipeline/document_source_internal_list_collections.h"
#include "mongo/db/pipeline/document_source_internal_projection.h"
#include "mongo/db/pipeline/document_source_internal_replace_root.h"
#include "mongo/db/pipeline/document_source_internal_shard_filter.h"
#include "mongo/db/pipeline/document_source_internal_unpack_bucket.h"
#include "mongo/db/pipeline/document_source_list_mql_entities.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/pipeline/document_source_plan_cache_stats.h"
#include "mongo/db/pipeline/document_source_redact.h"
#include "mongo/db/pipeline/document_source_sample.h"
#include "mongo/db/pipeline/document_source_sequential_document_cache.h"
#include "mongo/db/pipeline/document_source_set_window_fields.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/document_source_skip.h"
#include "mongo/db/pipeline/document_source_streaming_group.h"
#include "mongo/db/pipeline/document_source_union_with.h"
#include "mongo/db/pipeline/optimization/rule_based_rewriter.h"
#include "mongo/db/pipeline/search/document_source_internal_search_id_lookup.h"
#include "mongo/db/pipeline/search/document_source_search.h"
#include "mongo/db/pipeline/search/document_source_vector_search.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::rule_based_rewrites::pipeline {

// These will become redundant in a follow-up PR under SERVER-110104.
REGISTER_RULES(DocumentSourceStreamingGroup, OPTIMIZE_AT_RULE(DocumentSourceStreamingGroup));
REGISTER_RULES(DocumentSourceInternalGeoNearDistance,
               OPTIMIZE_AT_RULE(DocumentSourceInternalGeoNearDistance));
REGISTER_RULES(DocumentSourceSetVariableFromSubPipeline,
               OPTIMIZE_AT_RULE(DocumentSourceSetVariableFromSubPipeline));

// TODO(SERVER-112281): Split these into separate files by team ownership.

// Owned by the Query Optimization team.
REGISTER_RULES(DocumentSourceMatch, OPTIMIZE_AT_RULE(DocumentSourceMatch));
REGISTER_RULES(DocumentSourceInternalChangeStreamMatch,
               OPTIMIZE_AT_RULE(DocumentSourceInternalChangeStreamMatch));
REGISTER_RULES(DocumentSourceSample, OPTIMIZE_AT_RULE(DocumentSourceSample));
REGISTER_RULES(DocumentSourceRedact, OPTIMIZE_AT_RULE(DocumentSourceRedact));
REGISTER_RULES(DocumentSourceSingleDocumentTransformation,
               OPTIMIZE_AT_RULE(DocumentSourceSingleDocumentTransformation));
REGISTER_RULES(DocumentSourceSkip, OPTIMIZE_AT_RULE(DocumentSourceSkip));
REGISTER_RULES(DocumentSourceListMqlEntities, OPTIMIZE_AT_RULE(DocumentSourceListMqlEntities));
REGISTER_RULES(DocumentSourceLimit, OPTIMIZE_AT_RULE(DocumentSourceLimit));
REGISTER_RULES(DocumentSourceGroup, OPTIMIZE_AT_RULE(DocumentSourceGroup));
REGISTER_RULES(DocumentSourceLookUp, OPTIMIZE_AT_RULE(DocumentSourceLookUp));
REGISTER_RULES(DocumentSourceUnionWith, OPTIMIZE_AT_RULE(DocumentSourceUnionWith));
REGISTER_RULES(DocumentSourcePlanCacheStats, OPTIMIZE_AT_RULE(DocumentSourcePlanCacheStats));
REGISTER_RULES(DocumentSourceUnwind, OPTIMIZE_AT_RULE(DocumentSourceUnwind));
REGISTER_RULES(DocumentSourceInternalReplaceRoot,
               OPTIMIZE_AT_RULE(DocumentSourceInternalReplaceRoot));
REGISTER_RULES(DocumentSourceGraphLookUp, OPTIMIZE_AT_RULE(DocumentSourceGraphLookUp));
REGISTER_RULES(DocumentSourceInternalProjection,
               OPTIMIZE_AT_RULE(DocumentSourceInternalProjection));
REGISTER_RULES(DocumentSourceSort, OPTIMIZE_AT_RULE(DocumentSourceSort));
REGISTER_RULES(DocumentSourceInternalShardFilter,
               OPTIMIZE_AT_RULE(DocumentSourceInternalShardFilter));

// Owned by the Query Execution team.
REGISTER_RULES(DocumentSourceSequentialDocumentCache,
               OPTIMIZE_AT_RULE(DocumentSourceSequentialDocumentCache));

// Owned by the Query Integration team.
REGISTER_RULES(DocumentSourceVectorSearch, OPTIMIZE_AT_RULE(DocumentSourceVectorSearch));
REGISTER_RULES(DocumentSourceInternalSetWindowFields,
               OPTIMIZE_AT_RULE(DocumentSourceInternalSetWindowFields));
REGISTER_RULES(DocumentSourceGeoNear, OPTIMIZE_AT_RULE(DocumentSourceGeoNear));
REGISTER_RULES(DocumentSourceInternalSearchIdLookUp,
               OPTIMIZE_AT_RULE(DocumentSourceInternalSearchIdLookUp));
REGISTER_RULES(DocumentSourceSearch, OPTIMIZE_AT_RULE(DocumentSourceSearch));
REGISTER_RULES(DocumentSourceInternalDensify, OPTIMIZE_AT_RULE(DocumentSourceInternalDensify));
REGISTER_RULES(DocumentSourceInternalUnpackBucket,
               OPTIMIZE_AT_RULE(DocumentSourceInternalUnpackBucket));

// Owned by Catalog & Routing
REGISTER_RULES(DocumentSourceInternalListCollections,
               OPTIMIZE_AT_RULE(DocumentSourceInternalListCollections));
REGISTER_RULES(DocumentSourceInternalAllCollectionStats,
               OPTIMIZE_AT_RULE(DocumentSourceInternalAllCollectionStats));

}  // namespace mongo::rule_based_rewrites::pipeline
