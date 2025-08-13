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


#include <memory>
#include <utility>
#include <vector>

#include <boost/container/small_vector.hpp>
// IWYU pragma: no_include "boost/intrusive/detail/iterator.hpp"
// IWYU pragma: no_include "boost/move/detail/iterator_to_raw_pointer.hpp"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/classic/and_hash.h"
#include "mongo/db/exec/classic/and_sorted.h"
#include "mongo/db/exec/classic/collection_scan.h"
#include "mongo/db/exec/classic/count_scan.h"
#include "mongo/db/exec/classic/distinct_scan.h"
#include "mongo/db/exec/classic/eof.h"
#include "mongo/db/exec/classic/fetch.h"
#include "mongo/db/exec/classic/geo_near.h"
#include "mongo/db/exec/classic/index_scan.h"
#include "mongo/db/exec/classic/limit.h"
#include "mongo/db/exec/classic/merge_sort.h"
#include "mongo/db/exec/classic/mock_stage.h"
#include "mongo/db/exec/classic/or.h"
#include "mongo/db/exec/classic/projection.h"
#include "mongo/db/exec/classic/return_key.h"
#include "mongo/db/exec/classic/shard_filter.h"
#include "mongo/db/exec/classic/skip.h"
#include "mongo/db/exec/classic/sort.h"
#include "mongo/db/exec/classic/sort_key_generator.h"
#include "mongo/db/exec/classic/text_match.h"
#include "mongo/db/exec/classic/text_or.h"
#include "mongo/db/exec/collection_scan_common.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/matcher/matcher.h"
#include "mongo/db/index/fts_access_method.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/index_catalog.h"
#include "mongo/db/local_catalog/index_catalog_entry.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/compiler/logical_model/sort_pattern/sort_pattern.h"
#include "mongo/db/query/compiler/metadata/index_entry.h"
#include "mongo/db/query/compiler/physical_model/index_bounds/index_bounds.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/stage_builder/classic_stage_builder.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo::stage_builder {
// Returns a non-null pointer to the root of a plan tree, or a non-OK status if the PlanStage tree
// could not be constructed.
std::unique_ptr<PlanStage> ClassicStageBuilder::build(const QuerySolutionNode* root) {
    auto* const expCtx = _cq.getExpCtxRaw();

    const auto& collectionPtr = _collection.getCollectionPtr();

    auto result = [&]() -> std::unique_ptr<PlanStage> {
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
                params.resumeScanPoint = csn->resumeScanPoint;
                params.stopApplyingFilterAfterFirstMatch = csn->stopApplyingFilterAfterFirstMatch;
                params.boundInclusion = csn->boundInclusion;
                return std::make_unique<CollectionScan>(
                    expCtx, _collection, params, _ws, csn->filter.get());
            }
            case STAGE_IXSCAN: {
                const IndexScanNode* ixn = static_cast<const IndexScanNode*>(root);

                invariant(collectionPtr);
                auto descriptor = collectionPtr->getIndexCatalog()->findIndexByName(
                    _opCtx, ixn->index.identifier.catalogName);
                tassert(8862202,
                        str::stream() << "Index descriptor not found. Namespace: "
                                      << collectionPtr->ns().toStringForErrorMsg()
                                      << ", CanonicalQuery: " << _cq.toStringShortForErrorMsg()
                                      << ", IndexEntry: " << ixn->index.toString(),
                        descriptor);

                // We use the node's internal name, keyPattern and multikey details here. For
                // $** indexes, these may differ from the information recorded in the index's
                // descriptor.
                IndexScanParams params{descriptor,
                                       ixn->index.identifier.catalogName,
                                       ixn->index.keyPattern,
                                       ixn->index.multikeyPaths,
                                       ixn->index.multikey};
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
                const SortKeyGeneratorNode* keyGenNode =
                    static_cast<const SortKeyGeneratorNode*>(root);
                auto childStage = build(keyGenNode->children[0].get());
                return std::make_unique<SortKeyGeneratorStage>(
                    _cq.getExpCtx(), std::move(childStage), _ws, keyGenNode->sortSpec);
            }
            case STAGE_RETURN_KEY: {
                auto returnKeyNode = static_cast<const ReturnKeyNode*>(root);
                auto childStage = build(returnKeyNode->children[0].get());
                return std::make_unique<ReturnKeyStage>(expCtx,
                                                        std::move(returnKeyNode->sortKeyMetaFields),
                                                        _ws,
                                                        std::move(childStage));
            }
            case STAGE_PROJECTION_DEFAULT: {
                auto pn = static_cast<const ProjectionNodeDefault*>(root);
                auto childStage = build(pn->children[0].get());
                return std::make_unique<ProjectionStageDefault>(
                    _cq.getExpCtx(),
                    // In case of a "distinct" query, we may add a projection stage without the
                    // canonical query actually explicitly having a projection.
                    _cq.getDistinct() && _cq.getDistinct()->getProjectionSpec()
                        ? *_cq.getDistinct()->getProjectionSpec()
                        : _cq.getFindCommandRequest().getProjection(),
                    &pn->proj,
                    _ws,
                    std::move(childStage));
            }
            case STAGE_PROJECTION_COVERED: {
                auto pn = static_cast<const ProjectionNodeCovered*>(root);
                auto childStage = build(pn->children[0].get());
                return std::make_unique<ProjectionStageCovered>(
                    _cq.getExpCtxRaw(),
                    // In case of a "distinct" query, we may add a projection stage without the
                    // canonical query actually explicitly having a projection.
                    _cq.getDistinct() && _cq.getDistinct()->getProjectionSpec()
                        ? *_cq.getDistinct()->getProjectionSpec()
                        : _cq.getFindCommandRequest().getProjection(),
                    &pn->proj,
                    _ws,
                    std::move(childStage),
                    pn->coveredKeyObj);
            }
            case STAGE_PROJECTION_SIMPLE: {
                auto pn = static_cast<const ProjectionNodeSimple*>(root);
                auto childStage = build(pn->children[0].get());
                auto* proj = _cq.getProj();
                tassert(10853300, "'getProj()' must not return null", proj);
                return std::make_unique<ProjectionStageSimple>(
                    _cq.getExpCtxRaw(),
                    _cq.getFindCommandRequest().getProjection(),
                    proj,
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

                invariant(collectionPtr);
                const IndexDescriptor* twoDIndex =
                    collectionPtr->getIndexCatalog()->findIndexByName(
                        _opCtx, node->index.identifier.catalogName);
                invariant(twoDIndex);

                return std::make_unique<GeoNear2DStage>(
                    params, expCtx, _ws, _collection, twoDIndex);
            }
            case STAGE_GEO_NEAR_2DSPHERE: {
                const GeoNear2DSphereNode* node = static_cast<const GeoNear2DSphereNode*>(root);

                GeoNearParams params;
                params.nearQuery = node->nq;
                params.baseBounds = node->baseBounds;
                params.filter = node->filter.get();
                params.addPointMeta = node->addPointMeta;
                params.addDistMeta = node->addDistMeta;

                invariant(collectionPtr);
                const IndexDescriptor* s2Index = collectionPtr->getIndexCatalog()->findIndexByName(
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
                tassert(5432200, "collection object is not provided", collectionPtr);
                auto catalog = collectionPtr->getIndexCatalog();
                tassert(5432201, "index catalog is unavailable", catalog);
                auto desc = catalog->findIndexByName(_opCtx, node->index.identifier.catalogName);
                tassert(5432202,
                        str::stream() << "no index named '" << node->index.identifier.catalogName
                                      << "' found in catalog",
                        desc);
                auto fam =
                    static_cast<const FTSAccessMethod*>(catalog->getEntry(desc)->accessMethod());
                tassert(5432203, "access method for index is not defined", fam);

                // We assume here that node->ftsQuery is an FTSQueryImpl, not an FTSQueryNoop.
                // In practice, this means that it is illegal to use the StageBuilder on a
                // QuerySolution created by planning a query that contains "no-op" expressions.
                TextMatchParams params{desc,
                                       fam->getSpec(),
                                       node->indexPrefix,
                                       static_cast<const FTSQueryImpl&>(*node->ftsQuery)};

                // Children of this node may need to know about the key prefix size, so we'll
                // set it here before recursively descending into procession child nodes, and
                // will reset once a text sub-tree is constructed.
                _ftsKeyPrefixSize.emplace(params.spec.numExtraBefore());
                ON_BLOCK_EXIT([&] { _ftsKeyPrefixSize = {}; });

                return std::make_unique<TextMatchStage>(
                    expCtx, build(root->children[0].get()), params, _ws);
            }
            case STAGE_SHARDING_FILTER: {
                const ShardingFilterNode* fn = static_cast<const ShardingFilterNode*>(root);
                auto childStage = build(fn->children[0].get());

                auto shardFilterer = _collection.getShardingFilter(_opCtx);
                invariant(
                    shardFilterer,
                    "Attempting to use shard filter when there's no shard filter available for "
                    "the collection");

                return std::make_unique<ShardFilterStage>(
                    expCtx, std::move(*shardFilterer), _ws, std::move(childStage));
            }
            case STAGE_DISTINCT_SCAN: {
                const DistinctNode* dn = static_cast<const DistinctNode*>(root);

                invariant(collectionPtr);
                auto descriptor = collectionPtr->getIndexCatalog()->findIndexByName(
                    _opCtx, dn->index.identifier.catalogName);
                tassert(8862201, "Index descriptor cannot be null", descriptor);

                std::unique_ptr<ShardFiltererImpl> shardFilterer;
                if (dn->isShardFiltering) {
                    auto shardingFilter = _collection.getShardingFilter(_opCtx);
                    tassert(
                        9245806,
                        "Attempting to use shard filter when there's no shard filter available for "
                        "the collection",
                        shardingFilter);

                    shardFilterer = std::make_unique<ShardFiltererImpl>(std::move(*shardingFilter));
                }

                // We use the node's internal name, keyPattern and multikey details here. For $**
                // indexes, these may differ from the information recorded in the index's
                // descriptor.
                DistinctParams params{descriptor,
                                      dn->index.identifier.catalogName,
                                      dn->index.keyPattern,
                                      dn->index.multikeyPaths,
                                      dn->index.multikey};

                params.scanDirection = dn->direction;
                params.bounds = dn->bounds;
                params.fieldNo = dn->fieldNo;
                return std::make_unique<DistinctScan>(expCtx,
                                                      _collection,
                                                      std::move(params),
                                                      _ws,
                                                      std::move(shardFilterer),
                                                      dn->isFetching);
            }
            case STAGE_COUNT_SCAN: {
                const CountScanNode* csn = static_cast<const CountScanNode*>(root);

                invariant(collectionPtr);
                auto descriptor = collectionPtr->getIndexCatalog()->findIndexByName(
                    _opCtx, csn->index.identifier.catalogName);
                tassert(8862200, "Index descriptor cannot be null", descriptor);

                // We use the node's internal name, keyPattern and multikey details here. For
                // $** indexes, these may differ from the information recorded in the index's
                // descriptor.
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
                const EofNode* eofn = static_cast<const EofNode*>(root);
                return std::make_unique<EOFStage>(expCtx, eofn->type);
            }
            case STAGE_VIRTUAL_SCAN: {
                const auto* vsn = static_cast<const VirtualScanNode*>(root);

                // The classic stage builder currently only supports VirtualScanNodes which
                // represent collection scans that do not produce record ids.
                invariant(!vsn->hasRecordId);
                invariant(vsn->scanType == VirtualScanNode::ScanType::kCollScan);

                auto mockStage = std::make_unique<MockStage>(expCtx, _ws);
                for (auto&& arr : vsn->docs) {
                    // The VirtualScanNode should only have a single element that carrys the
                    // document as the MockStage cannot handle a recordId properly.
                    BSONObjIterator arrIt{arr};
                    invariant(arrIt.more());
                    auto firstElt = arrIt.next();
                    invariant(firstElt.type() == BSONType::object);
                    invariant(!arrIt.more());
                    BSONObj doc = firstElt.embeddedObject();

                    if (vsn->filter && !exec::matcher::matchesBSON(vsn->filter.get(), doc)) {
                        mockStage->enqueueStateCode(PlanStage::NEED_TIME);
                    } else {
                        auto wsID = _ws->allocate();
                        auto* member = _ws->get(wsID);
                        member->keyData.clear();
                        member->doc = {{}, Document{doc}.getOwned()};
                        member->transitionToOwnedObj();
                        mockStage->enqueueAdvanced(wsID);
                    }
                }
                return mockStage;
            }
            case STAGE_BATCHED_DELETE:
            case STAGE_CACHED_PLAN:
            case STAGE_COUNT:
            case STAGE_DELETE:
            case STAGE_EQ_LOOKUP:
            case STAGE_EQ_LOOKUP_UNWIND:
            case STAGE_GROUP:
            case STAGE_IDHACK:
            case STAGE_MATCH:
            case STAGE_REPLACE_ROOT:
            case STAGE_MOCK:
            case STAGE_MULTI_ITERATOR:
            case STAGE_MULTI_PLAN:
            case STAGE_QUEUED_DATA:
            case STAGE_RECORD_STORE_FAST_COUNT:
            case STAGE_SAMPLE_FROM_TIMESERIES_BUCKET:
            case STAGE_SUBPLAN:
            case STAGE_TRIAL:
            case STAGE_UNKNOWN:
            case STAGE_UNPACK_SAMPLED_TS_BUCKET:
            case STAGE_UNPACK_TS_BUCKET:
            case STAGE_TIMESERIES_MODIFY:
            case STAGE_SPOOL:
            case STAGE_SENTINEL:
            case STAGE_UPDATE:
            case STAGE_UNWIND:
            case STAGE_SEARCH:
            case STAGE_WINDOW: {
                LOGV2_WARNING(4615604,
                              "Can't build exec tree for node",
                              "node"_attr = redact(root->toString()));
            }
        }
        MONGO_UNREACHABLE;
    }();

    if (_planStageQsnMap) {
        _planStageQsnMap->insert({result.get(), root});
    }
    return result;
}
}  // namespace mongo::stage_builder
