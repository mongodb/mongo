/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/query/stage_builder.h"

#include <memory>

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/exec/and_hash.h"
#include "mongo/db/exec/and_sorted.h"
#include "mongo/db/exec/collection_scan.h"
#include "mongo/db/exec/count_scan.h"
#include "mongo/db/exec/distinct_scan.h"
#include "mongo/db/exec/ensure_sorted.h"
#include "mongo/db/exec/fetch.h"
#include "mongo/db/exec/geo_near.h"
#include "mongo/db/exec/index_scan.h"
#include "mongo/db/exec/limit.h"
#include "mongo/db/exec/merge_sort.h"
#include "mongo/db/exec/or.h"
#include "mongo/db/exec/projection.h"
#include "mongo/db/exec/return_key.h"
#include "mongo/db/exec/shard_filter.h"
#include "mongo/db/exec/skip.h"
#include "mongo/db/exec/sort.h"
#include "mongo/db/exec/sort_key_generator.h"
#include "mongo/db/exec/text.h"
#include "mongo/db/index/fts_access_method.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/storage/oplog_hack.h"
#include "mongo/util/log.h"

namespace mongo {

// Returns a non-null pointer to the root of a plan tree, or a non-OK status if the PlanStage tree
// could not be constructed.
std::unique_ptr<PlanStage> buildStages(OperationContext* opCtx,
                                       const Collection* collection,
                                       const CanonicalQuery& cq,
                                       const QuerySolution& qsol,
                                       const QuerySolutionNode* root,
                                       WorkingSet* ws) {
    switch (root->getType()) {
        case STAGE_COLLSCAN: {
            const CollectionScanNode* csn = static_cast<const CollectionScanNode*>(root);
            CollectionScanParams params;
            params.tailable = csn->tailable;
            params.shouldTrackLatestOplogTimestamp = csn->shouldTrackLatestOplogTimestamp;
            params.direction = (csn->direction == 1) ? CollectionScanParams::FORWARD
                                                     : CollectionScanParams::BACKWARD;
            params.shouldWaitForOplogVisibility = csn->shouldWaitForOplogVisibility;
            params.minTs = csn->minTs;
            params.maxTs = csn->maxTs;
            params.stopApplyingFilterAfterFirstMatch = csn->stopApplyingFilterAfterFirstMatch;
            return std::make_unique<CollectionScan>(
                opCtx, collection, params, ws, csn->filter.get());
        }
        case STAGE_IXSCAN: {
            const IndexScanNode* ixn = static_cast<const IndexScanNode*>(root);

            invariant(collection);
            auto descriptor = collection->getIndexCatalog()->findIndexByName(
                opCtx, ixn->index.identifier.catalogName);
            invariant(descriptor,
                      str::stream() << "Namespace: " << collection->ns()
                                    << ", CanonicalQuery: " << cq.toStringShort()
                                    << ", IndexEntry: " << ixn->index.toString());

            // We use the node's internal name, keyPattern and multikey details here. For $**
            // indexes, these may differ from the information recorded in the index's descriptor.
            IndexScanParams params{descriptor,
                                   ixn->index.identifier.catalogName,
                                   ixn->index.keyPattern,
                                   ixn->index.multikeyPaths,
                                   ixn->index.multikey};
            params.bounds = ixn->bounds;
            params.direction = ixn->direction;
            params.addKeyMetadata = ixn->addKeyMetadata;
            params.shouldDedup = ixn->shouldDedup;
            return std::make_unique<IndexScan>(opCtx, std::move(params), ws, ixn->filter.get());
        }
        case STAGE_FETCH: {
            const FetchNode* fn = static_cast<const FetchNode*>(root);
            auto childStage = buildStages(opCtx, collection, cq, qsol, fn->children[0], ws);
            return std::make_unique<FetchStage>(
                opCtx, ws, std::move(childStage), fn->filter.get(), collection);
        }
        case STAGE_SORT: {
            const SortNode* sn = static_cast<const SortNode*>(root);
            auto childStage = buildStages(opCtx, collection, cq, qsol, sn->children[0], ws);
            SortStageParams params;
            params.pattern = sn->pattern;
            params.limit = sn->limit;
            params.allowDiskUse = sn->allowDiskUse;
            return std::make_unique<SortStage>(opCtx, params, ws, std::move(childStage));
        }
        case STAGE_SORT_KEY_GENERATOR: {
            const SortKeyGeneratorNode* keyGenNode = static_cast<const SortKeyGeneratorNode*>(root);
            auto childStage = buildStages(opCtx, collection, cq, qsol, keyGenNode->children[0], ws);
            return std::make_unique<SortKeyGeneratorStage>(
                cq.getExpCtx(), std::move(childStage), ws, keyGenNode->sortSpec);
        }
        case STAGE_RETURN_KEY: {
            auto returnKeyNode = static_cast<const ReturnKeyNode*>(root);
            auto childStage =
                buildStages(opCtx, collection, cq, qsol, returnKeyNode->children[0], ws);
            return std::make_unique<ReturnKeyStage>(
                opCtx, std::move(returnKeyNode->sortKeyMetaFields), ws, std::move(childStage));
        }
        case STAGE_PROJECTION_DEFAULT: {
            auto pn = static_cast<const ProjectionNodeDefault*>(root);
            auto childStage = buildStages(opCtx, collection, cq, qsol, pn->children[0], ws);
            return std::make_unique<ProjectionStageDefault>(opCtx,
                                                            pn->projection,
                                                            ws,
                                                            std::move(childStage),
                                                            pn->fullExpression,
                                                            cq.getCollator());
        }
        case STAGE_PROJECTION_COVERED: {
            auto pn = static_cast<const ProjectionNodeCovered*>(root);
            auto childStage = buildStages(opCtx, collection, cq, qsol, pn->children[0], ws);
            return std::make_unique<ProjectionStageCovered>(
                opCtx, pn->projection, ws, std::move(childStage), pn->coveredKeyObj);
        }
        case STAGE_PROJECTION_SIMPLE: {
            auto pn = static_cast<const ProjectionNodeSimple*>(root);
            auto childStage = buildStages(opCtx, collection, cq, qsol, pn->children[0], ws);
            return std::make_unique<ProjectionStageSimple>(
                opCtx, pn->projection, ws, std::move(childStage));
        }
        case STAGE_LIMIT: {
            const LimitNode* ln = static_cast<const LimitNode*>(root);
            auto childStage = buildStages(opCtx, collection, cq, qsol, ln->children[0], ws);
            return std::make_unique<LimitStage>(opCtx, ln->limit, ws, std::move(childStage));
        }
        case STAGE_SKIP: {
            const SkipNode* sn = static_cast<const SkipNode*>(root);
            auto childStage = buildStages(opCtx, collection, cq, qsol, sn->children[0], ws);
            return std::make_unique<SkipStage>(opCtx, sn->skip, ws, std::move(childStage));
        }
        case STAGE_AND_HASH: {
            const AndHashNode* ahn = static_cast<const AndHashNode*>(root);
            auto ret = std::make_unique<AndHashStage>(opCtx, ws);
            for (size_t i = 0; i < ahn->children.size(); ++i) {
                auto childStage = buildStages(opCtx, collection, cq, qsol, ahn->children[i], ws);
                ret->addChild(std::move(childStage));
            }
            return ret;
        }
        case STAGE_OR: {
            const OrNode* orn = static_cast<const OrNode*>(root);
            auto ret = std::make_unique<OrStage>(opCtx, ws, orn->dedup, orn->filter.get());
            for (size_t i = 0; i < orn->children.size(); ++i) {
                auto childStage = buildStages(opCtx, collection, cq, qsol, orn->children[i], ws);
                ret->addChild(std::move(childStage));
            }
            return ret;
        }
        case STAGE_AND_SORTED: {
            const AndSortedNode* asn = static_cast<const AndSortedNode*>(root);
            auto ret = std::make_unique<AndSortedStage>(opCtx, ws);
            for (size_t i = 0; i < asn->children.size(); ++i) {
                auto childStage = buildStages(opCtx, collection, cq, qsol, asn->children[i], ws);
                ret->addChild(std::move(childStage));
            }
            return ret;
        }
        case STAGE_SORT_MERGE: {
            const MergeSortNode* msn = static_cast<const MergeSortNode*>(root);
            MergeSortStageParams params;
            params.dedup = msn->dedup;
            params.pattern = msn->sort;
            params.collator = cq.getCollator();
            auto ret = std::make_unique<MergeSortStage>(opCtx, params, ws);
            for (size_t i = 0; i < msn->children.size(); ++i) {
                auto childStage = buildStages(opCtx, collection, cq, qsol, msn->children[i], ws);
                ret->addChild(std::move(childStage));
            }
            return ret;
        }
        case STAGE_GEO_NEAR_2D: {
            const GeoNear2DNode* node = static_cast<const GeoNear2DNode*>(root);

            GeoNearParams params;
            params.nearQuery = node->nq;
            params.baseBounds = node->baseBounds;
            params.filter = node->filter.get();
            params.addPointMeta = node->addPointMeta;
            params.addDistMeta = node->addDistMeta;

            invariant(collection);
            const IndexDescriptor* twoDIndex = collection->getIndexCatalog()->findIndexByName(
                opCtx, node->index.identifier.catalogName);
            invariant(twoDIndex);

            return std::make_unique<GeoNear2DStage>(params, opCtx, ws, twoDIndex);
        }
        case STAGE_GEO_NEAR_2DSPHERE: {
            const GeoNear2DSphereNode* node = static_cast<const GeoNear2DSphereNode*>(root);

            GeoNearParams params;
            params.nearQuery = node->nq;
            params.baseBounds = node->baseBounds;
            params.filter = node->filter.get();
            params.addPointMeta = node->addPointMeta;
            params.addDistMeta = node->addDistMeta;

            invariant(collection);
            const IndexDescriptor* s2Index = collection->getIndexCatalog()->findIndexByName(
                opCtx, node->index.identifier.catalogName);
            invariant(s2Index);

            return std::make_unique<GeoNear2DSphereStage>(params, opCtx, ws, s2Index);
        }
        case STAGE_TEXT: {
            const TextNode* node = static_cast<const TextNode*>(root);
            invariant(collection);
            const IndexDescriptor* desc = collection->getIndexCatalog()->findIndexByName(
                opCtx, node->index.identifier.catalogName);
            invariant(desc);
            const FTSAccessMethod* fam = static_cast<const FTSAccessMethod*>(
                collection->getIndexCatalog()->getEntry(desc)->accessMethod());
            invariant(fam);

            TextStageParams params(fam->getSpec());
            params.index = desc;
            params.indexPrefix = node->indexPrefix;
            // We assume here that node->ftsQuery is an FTSQueryImpl, not an FTSQueryNoop. In
            // practice, this means that it is illegal to use the StageBuilder on a QuerySolution
            // created by planning a query that contains "no-op" expressions.
            params.query = static_cast<FTSQueryImpl&>(*node->ftsQuery);
            params.wantTextScore = (cq.getProj() && cq.getProj()->wantTextScore());
            return std::make_unique<TextStage>(opCtx, params, ws, node->filter.get());
        }
        case STAGE_SHARDING_FILTER: {
            const ShardingFilterNode* fn = static_cast<const ShardingFilterNode*>(root);
            auto childStage = buildStages(opCtx, collection, cq, qsol, fn->children[0], ws);

            auto css = CollectionShardingState::get(opCtx, collection->ns());
            return std::make_unique<ShardFilterStage>(
                opCtx, css->getOrphansFilter(opCtx, collection), ws, std::move(childStage));
        }
        case STAGE_DISTINCT_SCAN: {
            const DistinctNode* dn = static_cast<const DistinctNode*>(root);

            invariant(collection);
            auto descriptor = collection->getIndexCatalog()->findIndexByName(
                opCtx, dn->index.identifier.catalogName);
            invariant(descriptor);

            // We use the node's internal name, keyPattern and multikey details here. For $**
            // indexes, these may differ from the information recorded in the index's descriptor.
            DistinctParams params{descriptor,
                                  dn->index.identifier.catalogName,
                                  dn->index.keyPattern,
                                  dn->index.multikeyPaths,
                                  dn->index.multikey};

            params.scanDirection = dn->direction;
            params.bounds = dn->bounds;
            params.fieldNo = dn->fieldNo;
            return std::make_unique<DistinctScan>(opCtx, std::move(params), ws);
        }
        case STAGE_COUNT_SCAN: {
            const CountScanNode* csn = static_cast<const CountScanNode*>(root);

            invariant(collection);
            auto descriptor = collection->getIndexCatalog()->findIndexByName(
                opCtx, csn->index.identifier.catalogName);
            invariant(descriptor);

            // We use the node's internal name, keyPattern and multikey details here. For $**
            // indexes, these may differ from the information recorded in the index's descriptor.
            CountScanParams params{descriptor,
                                   csn->index.identifier.catalogName,
                                   csn->index.keyPattern,
                                   csn->index.multikeyPaths,
                                   csn->index.multikey};

            params.startKey = csn->startKey;
            params.startKeyInclusive = csn->startKeyInclusive;
            params.endKey = csn->endKey;
            params.endKeyInclusive = csn->endKeyInclusive;
            return std::make_unique<CountScan>(opCtx, std::move(params), ws);
        }
        case STAGE_ENSURE_SORTED: {
            const EnsureSortedNode* esn = static_cast<const EnsureSortedNode*>(root);
            auto childStage = buildStages(opCtx, collection, cq, qsol, esn->children[0], ws);
            return std::make_unique<EnsureSortedStage>(
                opCtx, esn->pattern, ws, std::move(childStage));
        }
        case STAGE_CACHED_PLAN:
        case STAGE_CHANGE_STREAM_PROXY:
        case STAGE_COUNT:
        case STAGE_DELETE:
        case STAGE_EOF:
        case STAGE_IDHACK:
        case STAGE_MULTI_ITERATOR:
        case STAGE_MULTI_PLAN:
        case STAGE_PIPELINE_PROXY:
        case STAGE_QUEUED_DATA:
        case STAGE_RECORD_STORE_FAST_COUNT:
        case STAGE_SUBPLAN:
        case STAGE_TEXT_MATCH:
        case STAGE_TEXT_OR:
        case STAGE_TRIAL:
        case STAGE_UNKNOWN:
        case STAGE_UPDATE: {
            str::stream ss;
            root->appendToString(&ss, 0);
            string nodeStr(ss);
            warning() << "Can't build exec tree for node " << nodeStr << endl;
        }
    }

    MONGO_UNREACHABLE;
}

std::unique_ptr<PlanStage> StageBuilder::build(OperationContext* opCtx,
                                               const Collection* collection,
                                               const CanonicalQuery& cq,
                                               const QuerySolution& solution,
                                               WorkingSet* wsIn) {
    // Only QuerySolutions derived from queries parsed with context, or QuerySolutions derived from
    // queries that disallow extensions, can be properly executed. If the query does not have
    // $text/$where context (and $text/$where are allowed), then no attempt should be made to
    // execute the query.
    invariant(!cq.canHaveNoopMatchNodes());

    invariant(wsIn);
    invariant(solution.root);

    QuerySolutionNode* solutionNode = solution.root.get();
    return buildStages(opCtx, collection, cq, solution, solutionNode, wsIn);
}

}  // namespace mongo
