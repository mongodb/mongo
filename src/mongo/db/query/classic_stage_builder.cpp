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


#include "mongo/platform/basic.h"

#include "mongo/db/query/classic_stage_builder.h"

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
#include "mongo/db/exec/eof.h"
#include "mongo/db/exec/fetch.h"
#include "mongo/db/exec/geo_near.h"
#include "mongo/db/exec/index_scan.h"
#include "mongo/db/exec/limit.h"
#include "mongo/db/exec/merge_sort.h"
#include "mongo/db/exec/or.h"
#include "mongo/db/exec/projection.h"
#include "mongo/db/exec/queued_data_stage.h"
#include "mongo/db/exec/return_key.h"
#include "mongo/db/exec/shard_filter.h"
#include "mongo/db/exec/skip.h"
#include "mongo/db/exec/sort.h"
#include "mongo/db/exec/sort_key_generator.h"
#include "mongo/db/exec/text_match.h"
#include "mongo/db/exec/text_or.h"
#include "mongo/db/index/fts_access_method.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo::stage_builder {
// Returns a non-null pointer to the root of a plan tree, or a non-OK status if the PlanStage tree
// could not be constructed.
std::unique_ptr<PlanStage> ClassicStageBuilder::build(const QuerySolutionNode* root) {
    auto* const expCtx = _cq.getExpCtxRaw();

    switch (root->getType()) {
        case STAGE_COLLSCAN: {
            const CollectionScanNode* csn = static_cast<const CollectionScanNode*>(root);
            CollectionScanParams params;
            params.tailable = csn->tailable;
            params.shouldTrackLatestOplogTimestamp = csn->shouldTrackLatestOplogTimestamp;
            params.assertTsHasNotFallenOff = csn->assertTsHasNotFallenOff;
            params.direction = (csn->direction == 1) ? CollectionScanParams::FORWARD
                                                     : CollectionScanParams::BACKWARD;
            params.shouldWaitForOplogVisibility = csn->shouldWaitForOplogVisibility;
            params.minRecord = csn->minRecord;
            params.maxRecord = csn->maxRecord;
            params.requestResumeToken = csn->requestResumeToken;
            params.resumeAfterRecordId = csn->resumeAfterRecordId;
            params.stopApplyingFilterAfterFirstMatch = csn->stopApplyingFilterAfterFirstMatch;
            params.boundInclusion = csn->boundInclusion;
            params.lowPriority = csn->lowPriority;
            return std::make_unique<CollectionScan>(
                expCtx, _collection, params, _ws, csn->filter.get());
        }
        case STAGE_IXSCAN: {
            const IndexScanNode* ixn = static_cast<const IndexScanNode*>(root);

            invariant(_collection);
            auto descriptor = _collection->getIndexCatalog()->findIndexByName(
                _opCtx, ixn->index.identifier.catalogName);
            invariant(descriptor,
                      str::stream() << "Namespace: " << _collection->ns().toStringForErrorMsg()
                                    << ", CanonicalQuery: " << _cq.toStringShortForErrorMsg()
                                    << ", IndexEntry: " << ixn->index.toString());

            // We use the node's internal name, keyPattern and multikey details here. For $**
            // indexes, these may differ from the information recorded in the index's descriptor.
            IndexScanParams params{descriptor,
                                   ixn->index.identifier.catalogName,
                                   ixn->index.keyPattern,
                                   ixn->index.multikeyPaths,
                                   ixn->index.multikey,
                                   ixn->lowPriority};
            params.bounds = ixn->bounds;
            params.direction = ixn->direction;
            params.addKeyMetadata = ixn->addKeyMetadata;
            params.shouldDedup = ixn->shouldDedup;
            return std::make_unique<IndexScan>(
                expCtx, _collection, std::move(params), _ws, ixn->filter.get());
        }
        case STAGE_FETCH: {
            const FetchNode* fn = static_cast<const FetchNode*>(root);
            auto childStage = build(fn->children[0].get());
            return std::make_unique<FetchStage>(
                expCtx, _ws, std::move(childStage), fn->filter.get(), _collection);
        }
        case STAGE_SORT_DEFAULT: {
            auto snDefault = static_cast<const SortNodeDefault*>(root);
            auto childStage = build(snDefault->children[0].get());
            return std::make_unique<SortStageDefault>(
                _cq.getExpCtx(),
                _ws,
                SortPattern{snDefault->pattern, _cq.getExpCtx()},
                snDefault->limit,
                snDefault->maxMemoryUsageBytes,
                snDefault->addSortKeyMetadata,
                std::move(childStage));
        }
        case STAGE_SORT_SIMPLE: {
            auto snSimple = static_cast<const SortNodeSimple*>(root);
            auto childStage = build(snSimple->children[0].get());
            return std::make_unique<SortStageSimple>(
                _cq.getExpCtx(),
                _ws,
                SortPattern{snSimple->pattern, _cq.getExpCtx()},
                snSimple->limit,
                snSimple->maxMemoryUsageBytes,
                snSimple->addSortKeyMetadata,
                std::move(childStage));
        }
        case STAGE_SORT_KEY_GENERATOR: {
            const SortKeyGeneratorNode* keyGenNode = static_cast<const SortKeyGeneratorNode*>(root);
            auto childStage = build(keyGenNode->children[0].get());
            return std::make_unique<SortKeyGeneratorStage>(
                _cq.getExpCtx(), std::move(childStage), _ws, keyGenNode->sortSpec);
        }
        case STAGE_RETURN_KEY: {
            auto returnKeyNode = static_cast<const ReturnKeyNode*>(root);
            auto childStage = build(returnKeyNode->children[0].get());
            return std::make_unique<ReturnKeyStage>(
                expCtx, std::move(returnKeyNode->sortKeyMetaFields), _ws, std::move(childStage));
        }
        case STAGE_PROJECTION_DEFAULT: {
            auto pn = static_cast<const ProjectionNodeDefault*>(root);
            auto childStage = build(pn->children[0].get());
            return std::make_unique<ProjectionStageDefault>(
                _cq.getExpCtx(),
                _cq.getFindCommandRequest().getProjection(),
                _cq.getProj(),
                _ws,
                std::move(childStage));
        }
        case STAGE_PROJECTION_COVERED: {
            auto pn = static_cast<const ProjectionNodeCovered*>(root);
            auto childStage = build(pn->children[0].get());
            return std::make_unique<ProjectionStageCovered>(
                _cq.getExpCtxRaw(),
                _cq.getFindCommandRequest().getProjection(),
                _cq.getProj(),
                _ws,
                std::move(childStage),
                pn->coveredKeyObj);
        }
        case STAGE_PROJECTION_SIMPLE: {
            auto pn = static_cast<const ProjectionNodeSimple*>(root);
            auto childStage = build(pn->children[0].get());
            return std::make_unique<ProjectionStageSimple>(
                _cq.getExpCtxRaw(),
                _cq.getFindCommandRequest().getProjection(),
                _cq.getProj(),
                _ws,
                std::move(childStage));
        }
        case STAGE_LIMIT: {
            const LimitNode* ln = static_cast<const LimitNode*>(root);
            auto childStage = build(ln->children[0].get());
            return std::make_unique<LimitStage>(expCtx, ln->limit, _ws, std::move(childStage));
        }
        case STAGE_SKIP: {
            const SkipNode* sn = static_cast<const SkipNode*>(root);
            auto childStage = build(sn->children[0].get());
            return std::make_unique<SkipStage>(expCtx, sn->skip, _ws, std::move(childStage));
        }
        case STAGE_AND_HASH: {
            const AndHashNode* ahn = static_cast<const AndHashNode*>(root);
            auto ret = std::make_unique<AndHashStage>(expCtx, _ws);
            for (size_t i = 0; i < ahn->children.size(); ++i) {
                auto childStage = build(ahn->children[i].get());
                ret->addChild(std::move(childStage));
            }
            return ret;
        }
        case STAGE_OR: {
            const OrNode* orn = static_cast<const OrNode*>(root);
            auto ret = std::make_unique<OrStage>(expCtx, _ws, orn->dedup, orn->filter.get());
            for (size_t i = 0; i < orn->children.size(); ++i) {
                auto childStage = build(orn->children[i].get());
                ret->addChild(std::move(childStage));
            }
            return ret;
        }
        case STAGE_AND_SORTED: {
            const AndSortedNode* asn = static_cast<const AndSortedNode*>(root);
            auto ret = std::make_unique<AndSortedStage>(expCtx, _ws);
            for (size_t i = 0; i < asn->children.size(); ++i) {
                auto childStage = build(asn->children[i].get());
                ret->addChild(std::move(childStage));
            }
            return ret;
        }
        case STAGE_SORT_MERGE: {
            const MergeSortNode* msn = static_cast<const MergeSortNode*>(root);
            MergeSortStageParams params;
            params.dedup = msn->dedup;
            params.pattern = msn->sort;
            params.collator = _cq.getCollator();
            auto ret = std::make_unique<MergeSortStage>(expCtx, params, _ws);
            for (size_t i = 0; i < msn->children.size(); ++i) {
                auto childStage = build(msn->children[i].get());
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

            invariant(_collection);
            const IndexDescriptor* twoDIndex = _collection->getIndexCatalog()->findIndexByName(
                _opCtx, node->index.identifier.catalogName);
            invariant(twoDIndex);

            return std::make_unique<GeoNear2DStage>(params, expCtx, _ws, _collection, twoDIndex);
        }
        case STAGE_GEO_NEAR_2DSPHERE: {
            const GeoNear2DSphereNode* node = static_cast<const GeoNear2DSphereNode*>(root);

            GeoNearParams params;
            params.nearQuery = node->nq;
            params.baseBounds = node->baseBounds;
            params.filter = node->filter.get();
            params.addPointMeta = node->addPointMeta;
            params.addDistMeta = node->addDistMeta;

            invariant(_collection);
            const IndexDescriptor* s2Index = _collection->getIndexCatalog()->findIndexByName(
                _opCtx, node->index.identifier.catalogName);
            invariant(s2Index);

            return std::make_unique<GeoNear2DSphereStage>(
                params, expCtx, _ws, _collection, s2Index);
        }
        case STAGE_TEXT_OR: {
            tassert(5432204,
                    "text index key prefix must be defined before processing TEXT_OR node",
                    _ftsKeyPrefixSize);

            auto node = static_cast<const TextOrNode*>(root);
            auto ret = std::make_unique<TextOrStage>(
                expCtx, *_ftsKeyPrefixSize, _ws, node->filter.get(), _collection);
            for (auto&& childNode : root->children) {
                ret->addChild(build(childNode.get()));
            }
            return ret;
        }
        case STAGE_TEXT_MATCH: {
            auto node = static_cast<const TextMatchNode*>(root);
            tassert(5432200, "collection object is not provided", _collection);
            auto catalog = _collection->getIndexCatalog();
            tassert(5432201, "index catalog is unavailable", catalog);
            auto desc = catalog->findIndexByName(_opCtx, node->index.identifier.catalogName);
            tassert(5432202,
                    str::stream() << "no index named '" << node->index.identifier.catalogName
                                  << "' found in catalog",
                    catalog);
            auto fam = static_cast<const FTSAccessMethod*>(catalog->getEntry(desc)->accessMethod());
            tassert(5432203, "access method for index is not defined", fam);

            // We assume here that node->ftsQuery is an FTSQueryImpl, not an FTSQueryNoop. In
            // practice, this means that it is illegal to use the StageBuilder on a QuerySolution
            // created by planning a query that contains "no-op" expressions.
            TextMatchParams params{desc,
                                   fam->getSpec(),
                                   node->indexPrefix,
                                   static_cast<const FTSQueryImpl&>(*node->ftsQuery)};

            // Children of this node may need to know about the key prefix size, so we'll set it
            // here before recursively descending into procession child nodes, and will reset once a
            // text sub-tree is constructed.
            _ftsKeyPrefixSize.emplace(params.spec.numExtraBefore());
            ON_BLOCK_EXIT([&] { _ftsKeyPrefixSize = {}; });

            return std::make_unique<TextMatchStage>(
                expCtx, build(root->children[0].get()), params, _ws);
        }
        case STAGE_SHARDING_FILTER: {
            const ShardingFilterNode* fn = static_cast<const ShardingFilterNode*>(root);
            auto childStage = build(fn->children[0].get());

            auto scopedCss = CollectionShardingState::assertCollectionLockedAndAcquire(
                _opCtx, _collection->ns());
            return std::make_unique<ShardFilterStage>(
                expCtx,
                scopedCss->getOwnershipFilter(
                    _opCtx, CollectionShardingState::OrphanCleanupPolicy::kDisallowOrphanCleanup),
                _ws,
                std::move(childStage));
        }
        case STAGE_DISTINCT_SCAN: {
            const DistinctNode* dn = static_cast<const DistinctNode*>(root);

            invariant(_collection);
            auto descriptor = _collection->getIndexCatalog()->findIndexByName(
                _opCtx, dn->index.identifier.catalogName);
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
            return std::make_unique<DistinctScan>(expCtx, _collection, std::move(params), _ws);
        }
        case STAGE_COUNT_SCAN: {
            const CountScanNode* csn = static_cast<const CountScanNode*>(root);

            invariant(_collection);
            auto descriptor = _collection->getIndexCatalog()->findIndexByName(
                _opCtx, csn->index.identifier.catalogName);
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
            return std::make_unique<CountScan>(expCtx, _collection, std::move(params), _ws);
        }
        case STAGE_EOF: {
            return std::make_unique<EOFStage>(expCtx);
        }
        case STAGE_VIRTUAL_SCAN: {
            const auto* vsn = static_cast<const VirtualScanNode*>(root);

            // The classic stage builder currently only supports VirtualScanNodes which represent
            // collection scans that do not produce record ids.
            invariant(!vsn->hasRecordId);
            invariant(vsn->scanType == VirtualScanNode::ScanType::kCollScan);

            auto qds = std::make_unique<QueuedDataStage>(expCtx, _ws);
            for (auto&& arr : vsn->docs) {
                // The VirtualScanNode should only have a single element that carrys the document
                // as the QueuedDataStage cannot handle a recordId properly.
                BSONObjIterator arrIt{arr};
                invariant(arrIt.more());
                auto firstElt = arrIt.next();
                invariant(firstElt.type() == BSONType::Object);
                invariant(!arrIt.more());

                // Only add the first element to the working set.
                auto wsID = _ws->allocate();
                qds->pushBack(wsID);
                auto* member = _ws->get(wsID);
                member->keyData.clear();
                member->doc = {{}, Document{firstElt.embeddedObject()}};
            }
            return qds;
        }
        case STAGE_BATCHED_DELETE:
        case STAGE_CACHED_PLAN:
        case STAGE_COUNT:
        case STAGE_DELETE:
        case STAGE_EQ_LOOKUP:
        case STAGE_GROUP:
        case STAGE_IDHACK:
        case STAGE_MOCK:
        case STAGE_MULTI_ITERATOR:
        case STAGE_MULTI_PLAN:
        case STAGE_QUEUED_DATA:
        case STAGE_RECORD_STORE_FAST_COUNT:
        case STAGE_SAMPLE_FROM_TIMESERIES_BUCKET:
        case STAGE_SUBPLAN:
        case STAGE_TRIAL:
        case STAGE_UNKNOWN:
        case STAGE_UNPACK_TIMESERIES_BUCKET:
        case STAGE_TIMESERIES_MODIFY:
        case STAGE_SPOOL:
        case STAGE_SENTINEL:
        case STAGE_COLUMN_SCAN:
        case STAGE_UPDATE: {
            LOGV2_WARNING(4615604, "Can't build exec tree for node", "node"_attr = *root);
        }
    }

    MONGO_UNREACHABLE;
}
}  // namespace mongo::stage_builder
