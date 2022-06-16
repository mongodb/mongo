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

#include "mongo/db/pipeline/visitors/document_source_walker.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/pipeline/document_source_bucket_auto.h"
#include "mongo/db/pipeline/document_source_coll_stats.h"
#include "mongo/db/pipeline/document_source_current_op.h"
#include "mongo/db/pipeline/document_source_cursor.h"
#include "mongo/db/pipeline/document_source_exchange.h"
#include "mongo/db/pipeline/document_source_facet.h"
#include "mongo/db/pipeline/document_source_geo_near.h"
#include "mongo/db/pipeline/document_source_geo_near_cursor.h"
#include "mongo/db/pipeline/document_source_graph_lookup.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_index_stats.h"
#include "mongo/db/pipeline/document_source_internal_inhibit_optimization.h"
#include "mongo/db/pipeline/document_source_internal_shard_filter.h"
#include "mongo/db/pipeline/document_source_internal_split_pipeline.h"
#include "mongo/db/pipeline/document_source_internal_unpack_bucket.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_list_cached_and_active_users.h"
#include "mongo/db/pipeline/document_source_list_local_sessions.h"
#include "mongo/db/pipeline/document_source_list_sessions.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_merge.h"
#include "mongo/db/pipeline/document_source_operation_metrics.h"
#include "mongo/db/pipeline/document_source_out.h"
#include "mongo/db/pipeline/document_source_plan_cache_stats.h"
#include "mongo/db/pipeline/document_source_queue.h"
#include "mongo/db/pipeline/document_source_redact.h"
#include "mongo/db/pipeline/document_source_sample.h"
#include "mongo/db/pipeline/document_source_sample_from_random_cursor.h"
#include "mongo/db/pipeline/document_source_sequential_document_cache.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/document_source_skip.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/document_source_tee_consumer.h"
#include "mongo/db/pipeline/document_source_union_with.h"
#include "mongo/db/pipeline/document_source_unwind.h"
#include "mongo/s/query/document_source_merge_cursors.h"

namespace mongo {

template <class T>
bool DocumentSourceWalker::visitHelper(const DocumentSource* source) {
    const T* concrete = dynamic_cast<const T*>(source);
    if (concrete == nullptr) {
        return false;
    }

    _postVisitor->visit(concrete);
    return true;
}

void DocumentSourceWalker::walk(const Pipeline& pipeline) {
    const Pipeline::SourceContainer& sources = pipeline.getSources();

    if (_postVisitor != nullptr) {
        for (auto it = sources.begin(); it != sources.end(); it++) {
            // TODO: use acceptVisitor method when DocumentSources get ability to visit.
            // source->acceptVisitor(*_preVisitor);
            //
            // For now, however, we use a crutch walker which performs a series of dynamic casts.
            // Some types are commented out because of dependency issues (e.g. not in pipeline
            // target but in query_exec target)
            const DocumentSource* ds = it->get();
            const bool visited = visitHelper<DocumentSourceBucketAuto>(ds) ||
                visitHelper<DocumentSourceBucketAuto>(ds) ||
                visitHelper<DocumentSourceCollStats>(ds) ||
                visitHelper<DocumentSourceCurrentOp>(ds) ||
                // TODO: uncomment after fixing dependency
                // visitHelper<DocumentSourceCursor>(ds) ||
                visitHelper<DocumentSourceExchange>(ds) || visitHelper<DocumentSourceFacet>(ds) ||
                visitHelper<DocumentSourceGeoNear>(ds) ||

                // TODO: uncomment after fixing dependency
                //! visitHelper<DocumentSourceGeoNearCursor>(ds) ||
                visitHelper<DocumentSourceGraphLookUp>(ds) ||
                visitHelper<DocumentSourceGroup>(ds) || visitHelper<DocumentSourceIndexStats>(ds) ||
                visitHelper<DocumentSourceInternalInhibitOptimization>(ds) ||
                visitHelper<DocumentSourceInternalShardFilter>(ds) ||
                visitHelper<DocumentSourceInternalSplitPipeline>(ds) ||
                visitHelper<DocumentSourceInternalUnpackBucket>(ds) ||
                visitHelper<DocumentSourceLimit>(ds) ||
                visitHelper<DocumentSourceListCachedAndActiveUsers>(ds) ||
                visitHelper<DocumentSourceListLocalSessions>(ds) ||
                visitHelper<DocumentSourceListSessions>(ds) ||
                visitHelper<DocumentSourceLookUp>(ds) || visitHelper<DocumentSourceMatch>(ds) ||
                visitHelper<DocumentSourceMerge>(ds) ||
                // TODO: uncomment after fixing dependency
                // visitHelper<DocumentSourceMergeCursors>(ds) ||
                visitHelper<DocumentSourceOperationMetrics>(ds) ||
                visitHelper<DocumentSourceOut>(ds) ||
                visitHelper<DocumentSourcePlanCacheStats>(ds) ||
                visitHelper<DocumentSourceQueue>(ds) || visitHelper<DocumentSourceRedact>(ds) ||
                visitHelper<DocumentSourceSample>(ds) ||
                visitHelper<DocumentSourceSampleFromRandomCursor>(ds) ||
                visitHelper<DocumentSourceSequentialDocumentCache>(ds) ||
                visitHelper<DocumentSourceSingleDocumentTransformation>(ds) ||
                visitHelper<DocumentSourceSkip>(ds) || visitHelper<DocumentSourceSort>(ds) ||
                visitHelper<DocumentSourceTeeConsumer>(ds) ||
                visitHelper<DocumentSourceUnionWith>(ds) || visitHelper<DocumentSourceUnwind>(ds)
                // TODO: uncomment after fixing dependency
                //&& visitHelper<DocumentSourceUpdateOnAddShard>(ds)
                ;

            if (!visited) {
                uasserted(ErrorCodes::InternalErrorNotSupported,
                          str::stream() << "Stage is not supported: " << ds->getSourceName());
            }
        }
    }

    // TODO: reverse for pre-visitor
}

}  // namespace mongo
