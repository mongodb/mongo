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
#include "mongo/db/exec/shard_filter.h"
#include "mongo/db/exec/skip.h"
#include "mongo/db/exec/sort.h"
#include "mongo/db/exec/sort_key_generator.h"
#include "mongo/db/exec/text.h"
#include "mongo/db/index/fts_access_method.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/util/log.h"

namespace mongo {

using std::unique_ptr;

PlanStage* buildStages(OperationContext* opCtx,
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
            return new CollectionScan(opCtx, collection, params, ws, csn->filter.get());
        }
        case STAGE_IXSCAN: {
            const IndexScanNode* ixn = static_cast<const IndexScanNode*>(root);

            if (nullptr == collection) {
                warning() << "Can't ixscan null namespace";
                return nullptr;
            }

            auto descriptor = collection->getIndexCatalog()->findIndexByName(
                opCtx, ixn->index.identifier.catalogName);
            invariant(descriptor,
                      str::stream() << "Namespace: " << collection->ns() << ", CanonicalQuery: "
                                    << cq.toStringShort()
                                    << ", IndexEntry: "
                                    << ixn->index.toString());

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
            return new IndexScan(opCtx, std::move(params), ws, ixn->filter.get());
        }
        case STAGE_FETCH: {
            const FetchNode* fn = static_cast<const FetchNode*>(root);
            PlanStage* childStage = buildStages(opCtx, collection, cq, qsol, fn->children[0], ws);
            if (nullptr == childStage) {
                return nullptr;
            }
            return new FetchStage(opCtx, ws, childStage, fn->filter.get(), collection);
        }
        case STAGE_SORT: {
            const SortNode* sn = static_cast<const SortNode*>(root);
            PlanStage* childStage = buildStages(opCtx, collection, cq, qsol, sn->children[0], ws);
            if (nullptr == childStage) {
                return nullptr;
            }
            SortStageParams params;
            params.pattern = sn->pattern;
            params.limit = sn->limit;
            params.allowDiskUse = sn->allowDiskUse;
            return new SortStage(opCtx, params, ws, childStage);
        }
        case STAGE_SORT_KEY_GENERATOR: {
            const SortKeyGeneratorNode* keyGenNode = static_cast<const SortKeyGeneratorNode*>(root);
            PlanStage* childStage =
                buildStages(opCtx, collection, cq, qsol, keyGenNode->children[0], ws);
            if (nullptr == childStage) {
                return nullptr;
            }
            return new SortKeyGeneratorStage(
                opCtx, childStage, ws, keyGenNode->sortSpec, cq.getCollator());
        }
        case STAGE_PROJECTION_DEFAULT: {
            auto pn = static_cast<const ProjectionNodeDefault*>(root);
            unique_ptr<PlanStage> childStage{
                buildStages(opCtx, collection, cq, qsol, pn->children[0], ws)};
            if (nullptr == childStage) {
                return nullptr;
            }
            return new ProjectionStageDefault(opCtx,
                                              pn->projection,
                                              ws,
                                              std::move(childStage),
                                              pn->fullExpression,
                                              cq.getCollator());
        }
        case STAGE_PROJECTION_COVERED: {
            auto pn = static_cast<const ProjectionNodeCovered*>(root);
            unique_ptr<PlanStage> childStage{
                buildStages(opCtx, collection, cq, qsol, pn->children[0], ws)};
            if (nullptr == childStage) {
                return nullptr;
            }
            return new ProjectionStageCovered(
                opCtx, pn->projection, ws, std::move(childStage), pn->coveredKeyObj);
        }
        case STAGE_PROJECTION_SIMPLE: {
            auto pn = static_cast<const ProjectionNodeSimple*>(root);
            unique_ptr<PlanStage> childStage{
                buildStages(opCtx, collection, cq, qsol, pn->children[0], ws)};
            if (nullptr == childStage) {
                return nullptr;
            }
            return new ProjectionStageSimple(opCtx, pn->projection, ws, std::move(childStage));
        }
        case STAGE_LIMIT: {
            const LimitNode* ln = static_cast<const LimitNode*>(root);
            PlanStage* childStage = buildStages(opCtx, collection, cq, qsol, ln->children[0], ws);
            if (nullptr == childStage) {
                return nullptr;
            }
            return new LimitStage(opCtx, ln->limit, ws, childStage);
        }
        case STAGE_SKIP: {
            const SkipNode* sn = static_cast<const SkipNode*>(root);
            PlanStage* childStage = buildStages(opCtx, collection, cq, qsol, sn->children[0], ws);
            if (nullptr == childStage) {
                return nullptr;
            }
            return new SkipStage(opCtx, sn->skip, ws, childStage);
        }
        case STAGE_AND_HASH: {
            const AndHashNode* ahn = static_cast<const AndHashNode*>(root);
            auto ret = std::make_unique<AndHashStage>(opCtx, ws);
            for (size_t i = 0; i < ahn->children.size(); ++i) {
                PlanStage* childStage =
                    buildStages(opCtx, collection, cq, qsol, ahn->children[i], ws);
                if (nullptr == childStage) {
                    return nullptr;
                }
                ret->addChild(childStage);
            }
            return ret.release();
        }
        case STAGE_OR: {
            const OrNode* orn = static_cast<const OrNode*>(root);
            auto ret = std::make_unique<OrStage>(opCtx, ws, orn->dedup, orn->filter.get());
            for (size_t i = 0; i < orn->children.size(); ++i) {
                PlanStage* childStage =
                    buildStages(opCtx, collection, cq, qsol, orn->children[i], ws);
                if (nullptr == childStage) {
                    return nullptr;
                }
                ret->addChild(childStage);
            }
            return ret.release();
        }
        case STAGE_AND_SORTED: {
            const AndSortedNode* asn = static_cast<const AndSortedNode*>(root);
            auto ret = std::make_unique<AndSortedStage>(opCtx, ws);
            for (size_t i = 0; i < asn->children.size(); ++i) {
                PlanStage* childStage =
                    buildStages(opCtx, collection, cq, qsol, asn->children[i], ws);
                if (nullptr == childStage) {
                    return nullptr;
                }
                ret->addChild(childStage);
            }
            return ret.release();
        }
        case STAGE_SORT_MERGE: {
            const MergeSortNode* msn = static_cast<const MergeSortNode*>(root);
            MergeSortStageParams params;
            params.dedup = msn->dedup;
            params.pattern = msn->sort;
            params.collator = cq.getCollator();
            auto ret = std::make_unique<MergeSortStage>(opCtx, params, ws);
            for (size_t i = 0; i < msn->children.size(); ++i) {
                PlanStage* childStage =
                    buildStages(opCtx, collection, cq, qsol, msn->children[i], ws);
                if (nullptr == childStage) {
                    return nullptr;
                }
                ret->addChild(childStage);
            }
            return ret.release();
        }
        case STAGE_GEO_NEAR_2D: {
            const GeoNear2DNode* node = static_cast<const GeoNear2DNode*>(root);

            GeoNearParams params;
            params.nearQuery = node->nq;
            params.baseBounds = node->baseBounds;
            params.filter = node->filter.get();
            params.addPointMeta = node->addPointMeta;
            params.addDistMeta = node->addDistMeta;

            const IndexDescriptor* twoDIndex = collection->getIndexCatalog()->findIndexByName(
                opCtx, node->index.identifier.catalogName);
            invariant(twoDIndex);

            GeoNear2DStage* nearStage = new GeoNear2DStage(params, opCtx, ws, twoDIndex);

            return nearStage;
        }
        case STAGE_GEO_NEAR_2DSPHERE: {
            const GeoNear2DSphereNode* node = static_cast<const GeoNear2DSphereNode*>(root);

            GeoNearParams params;
            params.nearQuery = node->nq;
            params.baseBounds = node->baseBounds;
            params.filter = node->filter.get();
            params.addPointMeta = node->addPointMeta;
            params.addDistMeta = node->addDistMeta;

            const IndexDescriptor* s2Index = collection->getIndexCatalog()->findIndexByName(
                opCtx, node->index.identifier.catalogName);
            invariant(s2Index);

            return new GeoNear2DSphereStage(params, opCtx, ws, s2Index);
        }
        case STAGE_TEXT: {
            const TextNode* node = static_cast<const TextNode*>(root);
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
            // practice,
            // this means that it is illegal to use the StageBuilder on a QuerySolution created by
            // planning a query that contains "no-op" expressions. TODO: make StageBuilder::build()
            // fail in this case (this improvement is being tracked by SERVER-21510).
            params.query = static_cast<FTSQueryImpl&>(*node->ftsQuery);
            params.wantTextScore = (cq.getProj() && cq.getProj()->wantTextScore());
            return new TextStage(opCtx, params, ws, node->filter.get());
        }
        case STAGE_SHARDING_FILTER: {
            const ShardingFilterNode* fn = static_cast<const ShardingFilterNode*>(root);
            PlanStage* childStage = buildStages(opCtx, collection, cq, qsol, fn->children[0], ws);
            if (nullptr == childStage) {
                return nullptr;
            }

            auto css = CollectionShardingState::get(opCtx, collection->ns());
            return new ShardFilterStage(opCtx, css->getOrphansFilter(opCtx), ws, childStage);
        }
        case STAGE_DISTINCT_SCAN: {
            const DistinctNode* dn = static_cast<const DistinctNode*>(root);

            if (nullptr == collection) {
                warning() << "Can't distinct-scan null namespace";
                return nullptr;
            }

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
            return new DistinctScan(opCtx, std::move(params), ws);
        }
        case STAGE_COUNT_SCAN: {
            const CountScanNode* csn = static_cast<const CountScanNode*>(root);

            if (nullptr == collection) {
                warning() << "Can't fast-count null namespace (collection null)";
                return nullptr;
            }

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
            return new CountScan(opCtx, std::move(params), ws);
        }
        case STAGE_ENSURE_SORTED: {
            const EnsureSortedNode* esn = static_cast<const EnsureSortedNode*>(root);
            PlanStage* childStage = buildStages(opCtx, collection, cq, qsol, esn->children[0], ws);
            if (nullptr == childStage) {
                return nullptr;
            }
            return new EnsureSortedStage(opCtx, esn->pattern, ws, childStage);
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
    return nullptr;
}

// static (this one is used for Cached and MultiPlanStage)
bool StageBuilder::build(OperationContext* opCtx,
                         const Collection* collection,
                         const CanonicalQuery& cq,
                         const QuerySolution& solution,
                         WorkingSet* wsIn,
                         PlanStage** rootOut) {
    // Only QuerySolutions derived from queries parsed with context, or QuerySolutions derived from
    // queries that disallow extensions, can be properly executed. If the query does not have
    // $text/$where context (and $text/$where are allowed), then no attempt should be made to
    // execute the query.
    invariant(!cq.canHaveNoopMatchNodes());

    if (nullptr == wsIn || nullptr == rootOut) {
        return false;
    }
    QuerySolutionNode* solutionNode = solution.root.get();
    if (nullptr == solutionNode) {
        return false;
    }
    return nullptr != (*rootOut = buildStages(opCtx, collection, cq, solution, solutionNode, wsIn));
}

}  // namespace mongo
