/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/pipeline/document_source_bucket_auto.h"
#include "mongo/db/pipeline/document_source_change_stream_add_post_image.h"
#include "mongo/db/pipeline/document_source_change_stream_add_pre_image.h"
#include "mongo/db/pipeline/document_source_change_stream_check_invalidate.h"
#include "mongo/db/pipeline/document_source_change_stream_check_resumability.h"
#include "mongo/db/pipeline/document_source_change_stream_check_topology_change.h"
#include "mongo/db/pipeline/document_source_change_stream_handle_topology_change.h"
#include "mongo/db/pipeline/document_source_change_stream_transform.h"
#include "mongo/db/pipeline/document_source_change_stream_unwind_transaction.h"
#include "mongo/db/pipeline/document_source_coll_stats.h"
#include "mongo/db/pipeline/document_source_current_op.h"
#include "mongo/db/pipeline/document_source_cursor.h"
#include "mongo/db/pipeline/document_source_densify.h"
#include "mongo/db/pipeline/document_source_exchange.h"
#include "mongo/db/pipeline/document_source_facet.h"
#include "mongo/db/pipeline/document_source_find_and_modify_image_lookup.h"
#include "mongo/db/pipeline/document_source_geo_near.h"
#include "mongo/db/pipeline/document_source_geo_near_cursor.h"
#include "mongo/db/pipeline/document_source_graph_lookup.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_index_stats.h"
#include "mongo/db/pipeline/document_source_internal_all_collection_stats.h"
#include "mongo/db/pipeline/document_source_internal_apply_oplog_update.h"
#include "mongo/db/pipeline/document_source_internal_compute_geo_near_distance.h"
#include "mongo/db/pipeline/document_source_internal_convert_bucket_index_stats.h"
#include "mongo/db/pipeline/document_source_internal_inhibit_optimization.h"
#include "mongo/db/pipeline/document_source_internal_shard_filter.h"
#include "mongo/db/pipeline/document_source_internal_shardserver_info.h"
#include "mongo/db/pipeline/document_source_internal_split_pipeline.h"
#include "mongo/db/pipeline/document_source_internal_unpack_bucket.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_list_cached_and_active_users.h"
#include "mongo/db/pipeline/document_source_list_catalog.h"
#include "mongo/db/pipeline/document_source_list_local_sessions.h"
#include "mongo/db/pipeline/document_source_list_sampled_queries.h"
#include "mongo/db/pipeline/document_source_list_sessions.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_merge.h"
#include "mongo/db/pipeline/document_source_operation_metrics.h"
#include "mongo/db/pipeline/document_source_out.h"
#include "mongo/db/pipeline/document_source_plan_cache_stats.h"
#include "mongo/db/pipeline/document_source_query_stats.h"
#include "mongo/db/pipeline/document_source_queue.h"
#include "mongo/db/pipeline/document_source_redact.h"
#include "mongo/db/pipeline/document_source_replace_root.h"
#include "mongo/db/pipeline/document_source_sample.h"
#include "mongo/db/pipeline/document_source_sample_from_random_cursor.h"
#include "mongo/db/pipeline/document_source_sequential_document_cache.h"
#include "mongo/db/pipeline/document_source_set_variable_from_subpipeline.h"
#include "mongo/db/pipeline/document_source_set_window_fields.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/document_source_skip.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/document_source_streaming_group.h"
#include "mongo/db/pipeline/document_source_tee_consumer.h"
#include "mongo/db/pipeline/document_source_union_with.h"
#include "mongo/db/pipeline/document_source_unwind.h"
#include "mongo/db/pipeline/visitors/document_source_visitor_registry.h"
#include "mongo/db/s/document_source_analyze_shard_key_read_write_distribution.h"
#include "mongo/db/s/resharding/document_source_resharding_add_resume_id.h"
#include "mongo/db/s/resharding/document_source_resharding_iterate_transaction.h"
#include "mongo/db/s/resharding/document_source_resharding_ownership_match.h"

namespace mongo {

/**
 * Register 'visit()' functions for all mongod DocumentSources for the visitor specified as the
 * template parameter in the DocumentSource visitor regsitry in the given ServiceContext. Using this
 * function helps provide compile-time safety that ensures visitor implementors have provided an
 * implementation for all DocumentSoures. This function is intended to be used in the following
 * manner:
 *
 * // Define visit functions for all DocumentSources
 * void visit(FooVisitorCtx* ctx, const DocumentSourceMatch& match) { ... }
 * ...
 *
 * const ServiceContext::ConstructorActionRegisterer fooRegisterer{
 *   "FooRegisterer", [](ServiceContext* service) {
 *       registerMongodVisitor<FooVisitorCtx>(service);
 *   }};
 */
template <typename T>
void registerMongodVisitor(ServiceContext* service) {
    auto& registry = getDocumentSourceVisitorRegistry(service);
    registerVisitFuncs<T,
                       // These document sources are defined in the 'query_exec' library, so having
                       // them here causes a circular dependency. We should ideally factor them out
                       // into their own library (or as part of libpipeline) but this requires a
                       // large refactor of the 'query_exec' library.
                       // It should be safe to ignore these for now as the only user of the visitor
                       // is CQF, which won't encounter these DocumentSources.
                       // DocumentSourceCursor,
                       // DocumentSourceGeoNearCursor,
                       DocumentSourceBucketAuto,
                       DocumentSourceChangeStreamAddPostImage,
                       DocumentSourceChangeStreamAddPreImage,
                       DocumentSourceChangeStreamCheckInvalidate,
                       DocumentSourceChangeStreamCheckResumability,
                       DocumentSourceChangeStreamCheckTopologyChange,
                       DocumentSourceChangeStreamHandleTopologyChange,
                       DocumentSourceChangeStreamTransform,
                       DocumentSourceChangeStreamUnwindTransaction,
                       DocumentSourceCollStats,
                       DocumentSourceCurrentOp,
                       DocumentSourceExchange,
                       DocumentSourceFacet,
                       DocumentSourceFindAndModifyImageLookup,
                       DocumentSourceGeoNear,
                       DocumentSourceGraphLookUp,
                       DocumentSourceGroup,
                       DocumentSourceIndexStats,
                       DocumentSourceInternalAllCollectionStats,
                       DocumentSourceInternalApplyOplogUpdate,
                       DocumentSourceInternalConvertBucketIndexStats,
                       DocumentSourceInternalDensify,
                       DocumentSourceInternalGeoNearDistance,
                       DocumentSourceInternalInhibitOptimization,
                       DocumentSourceInternalSetWindowFields,
                       DocumentSourceInternalShardFilter,
                       DocumentSourceInternalShardServerInfo,
                       DocumentSourceInternalSplitPipeline,
                       DocumentSourceInternalUnpackBucket,
                       DocumentSourceLimit,
                       DocumentSourceListCachedAndActiveUsers,
                       DocumentSourceListCatalog,
                       DocumentSourceListLocalSessions,
                       analyze_shard_key::DocumentSourceListSampledQueries,
                       DocumentSourceListSessions,
                       DocumentSourceLookUp,
                       DocumentSourceMatch,
                       DocumentSourceMerge,
                       DocumentSourceOperationMetrics,
                       DocumentSourceOut,
                       DocumentSourcePlanCacheStats,
                       DocumentSourceQueue,
                       DocumentSourceRedact,
                       DocumentSourceSample,
                       DocumentSourceSampleFromRandomCursor,
                       DocumentSourceSequentialDocumentCache,
                       DocumentSourceSetVariableFromSubPipeline,
                       DocumentSourceSingleDocumentTransformation,
                       DocumentSourceSkip,
                       DocumentSourceSort,
                       DocumentSourceStreamingGroup,
                       DocumentSourceTeeConsumer,
                       DocumentSourceQueryStats,
                       DocumentSourceUnionWith,
                       DocumentSourceUnwind>(&registry);
}

/**
 * See 'registerMongodVisitor'. This function has the same semantics except for the DocumentSources
 * defined in the 's/sharding_runtime_d' module.
 */
template <typename T>
void registerShardingRuntimeDVisitor(ServiceContext* service) {
    auto& registry = getDocumentSourceVisitorRegistry(service);
    registerVisitFuncs<T,
                       analyze_shard_key::DocumentSourceAnalyzeShardKeyReadWriteDistribution,
                       DocumentSourceReshardingAddResumeId,
                       DocumentSourceReshardingIterateTransaction,
                       DocumentSourceReshardingOwnershipMatch>(&registry);
}

}  // namespace mongo
