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

#include "mongo/db/query/plan_executor_express.h"
#include "mongo/db/exec/projection.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/query/collation/collation_index_key.h"
#include "mongo/db/query/index_bounds_builder.h"
#include "mongo/db/query/planner_ixselect.h"

namespace mongo {
/*
 * Tries to find an index suitable for use in the express equality path. Excludes indexes which
 * cannot 1) satisfy the given query with exact bounds and 2) provably return at most one result
 * doc. If at least one suitable index remains, returns the name of the index with the fewest
 * fields. If not, returns boost::none.
 */
boost::optional<std::string> getIndexForExpressEquality(const CanonicalQuery& cq,
                                                        const QueryPlannerParams& plannerParams) {
    const auto& findCommand = cq.getFindCommandRequest();

    const bool needsShardFilter = plannerParams.options & QueryPlannerParams::INCLUDE_SHARD_FILTER;
    const bool hasLimitOne = (findCommand.getLimit() && findCommand.getLimit().get() == 1);
    const auto& data =
        static_cast<ComparisonMatchExpressionBase*>(cq.getPrimaryMatchExpression())->getData();
    const bool collationRelevant = data.type() == BSONType::String ||
        data.type() == BSONType::Object || data.type() == BSONType::Array;

    RelevantFieldIndexMap fields;
    QueryPlannerIXSelect::getFields(cq.getPrimaryMatchExpression(), &fields);
    std::vector<IndexEntry> indexes =
        QueryPlannerIXSelect::findRelevantIndices(fields, plannerParams.indices);

    int numFields = -1;
    const IndexEntry* bestEntry = nullptr;
    for (const auto& e : indexes) {
        if (
            // Indexes like geo/hash cannot generate exact bounds. Coarsely filter these out.
            e.type != IndexType::INDEX_BTREE ||
            // Also ensure that the query and index collators match.
            (collationRelevant &&
             !CollatorInterface::collatorsMatch(cq.getCollator(), e.collator)) ||
            // Sparse indexes cannot support comparisons to null.
            (e.sparse && data.isNull()) ||
            // Partial indexes may not be able to answer the query.
            (e.filterExpr &&
             !expression::isSubsetOf(cq.getPrimaryMatchExpression(), e.filterExpr))) {
            continue;
        }
        const auto currNFields = e.keyPattern.nFields();
        if (
            // We cannot guarantee that the result has at most one result doc.
            ((!e.unique || currNFields != 1) && !hasLimitOne) ||
            // TODO SERVER-87016: Support shard filtering for limitOne query with non-unique index.
            (!e.unique && needsShardFilter) ||
            // This index is suitable but has more fields than the best so far.
            (bestEntry && numFields <= currNFields)) {
            continue;
        }
        bestEntry = &e;
        numFields = currNFields;
    }
    if (bestEntry) {
        return bestEntry->identifier.catalogName;
    }
    return boost::none;
}

PlanExecutorExpressParams PlanExecutorExpressParams::makeExecutorParamsForIdQuery(
    OperationContext* opCtx,
    VariantCollectionPtrOrAcquisition coll,
    boost::optional<ScopedCollectionFilter> collectionFilter,
    bool isClusteredOnId) {
    return PlanExecutorExpressParams(
        opCtx, coll, std::move(collectionFilter), isClusteredOnId, boost::none /* indexName */);
}

PlanExecutorExpressParams PlanExecutorExpressParams::makeExecutorParamsForIndexedEqualityQuery(
    OperationContext* opCtx,
    VariantCollectionPtrOrAcquisition coll,
    boost::optional<ScopedCollectionFilter> collectionFilter,
    std::string indexName) {
    return PlanExecutorExpressParams(opCtx,
                                     coll,
                                     std::move(collectionFilter),
                                     false /* isClusteredOnId */,
                                     std::move(indexName));
}

PlanExecutorExpressParams::PlanExecutorExpressParams(
    OperationContext* opCtx,
    VariantCollectionPtrOrAcquisition coll,
    boost::optional<ScopedCollectionFilter> collectionFilter,
    bool isClusteredOnId,
    boost::optional<const std::string> indexName)
    : _opCtx(opCtx),
      _coll(coll),
      _collectionFilter(std::move(collectionFilter)),
      _isClusteredOnId{isClusteredOnId},
      _indexName(std::move(indexName)) {}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> PlanExecutorExpress::makeExecutor(
    std::unique_ptr<CanonicalQuery> cq, PlanExecutorExpressParams params) {
    return {new PlanExecutorExpress(std::move(cq),
                                    params._opCtx,
                                    params._coll,
                                    std::move(params._collectionFilter),
                                    params._isClusteredOnId,
                                    std::move(params._indexName)),
            PlanExecutor::Deleter{params._opCtx}};
}

PlanExecutorExpress::PlanExecutorExpress(std::unique_ptr<CanonicalQuery> cq,
                                         OperationContext* opCtx,
                                         VariantCollectionPtrOrAcquisition coll,
                                         boost::optional<ScopedCollectionFilter> collectionFilter,
                                         bool isClusteredOnId,
                                         boost::optional<const std::string> indexName)
    : _opCtx(opCtx),
      _cq(std::move(cq)),
      _isClusteredOnId{isClusteredOnId},
      _coll(coll),
      _indexName(std::move(indexName)),
      _commonStats("EXPRESS"),
      _nss(_cq->nss()),
      _planExplainer(&_commonStats, isClusteredOnId, _indexName),
      _shardFilterer(std::move(collectionFilter)) {
    if (_indexName) {
        auto descriptor =
            _coll.getCollectionPtr()->getIndexCatalog()->findIndexByName(_opCtx, _indexName.get());
        tassert(8623700,
                str::stream() << "Missing index. Namespace: "
                              << _coll.getCollectionPtr()->ns().toStringForErrorMsg()
                              << ", index name: " << *_indexName,
                descriptor);
        _entry = descriptor->getEntry();
    } else if (!_isClusteredOnId) {
        auto descriptor = _coll.getCollectionPtr()->getIndexCatalog()->findIdIndex(_opCtx);
        tassert(8623701,
                str::stream() << "Missing _id index. Namespace: "
                              << _coll.getCollectionPtr()->ns().toStringForErrorMsg(),
                descriptor);
        _entry = descriptor->getEntry();
        _indexName.emplace("_id_");
    }
}

PlanExecutor::ExecState PlanExecutorExpress::getNext(BSONObj* out, RecordId* dlOut) {
    if (_done) {
        _commonStats.isEOF = true;
        return ExecState::IS_EOF;
    }
    _done = true;
    _commonStats.works++;

    RecordId rid = getRIDForPoint(
        static_cast<ComparisonMatchExpressionBase*>(_cq->getPrimaryMatchExpression())->getData());

    // It's possible that we could do a covered index scan here, to avoid fetching the whole
    // document. However, 1) the impact would be small, since we always expect to return at most
    // one document, and 2) it would require a query like find({_id: <num>}, {_id: 1}), which we
    // assume is an uncommon pattern.
    const auto& collptr = _coll.getCollectionPtr();
    Snapshotted<BSONObj> snapDoc;
    BSONObj doc;
    bool found = false;
    if (!rid.isNull() && collptr->findDoc(_opCtx, rid, &snapDoc)) {
        doc = std::move(snapDoc.value());
        if (dlOut) {
            *dlOut = std::move(rid);
        }
        found = true;
    }

    auto belongsToShard = [&](const BSONObj& doc) {
        if (_shardFilterer && _shardFilterer->isCollectionSharded()) {
            return _shardFilterer->documentBelongsToMe(doc) ==
                ShardFilterer::DocumentBelongsResult::kBelongs;
        }
        return true;
    };

    if (found && belongsToShard(doc)) {
        invariant(!doc.isEmpty());
        if (_cq->getProj()) {
            // Only simple projections are currently supported.
            auto proj = _cq->getProj();
            auto projType = proj->type();
            if (projType == projection_ast::ProjectType::kInclusion) {
                doc = ProjectionStageSimple::transform(
                    doc, _cq->getProj()->getRequiredFields(), projType);
            } else {
                doc = ProjectionStageSimple::transform(
                    doc, _cq->getProj()->getExcludedPaths(), projType);
            }
        }
        *out = std::move(doc);
        _commonStats.advanced++;
        return ExecState::ADVANCED;
    };
    _commonStats.isEOF = true;
    return ExecState::IS_EOF;
}

RecordId PlanExecutorExpress::getRIDForPoint(const BSONElement& val) const {
    const auto& collptr = _coll.getCollectionPtr();

    // For a clustered collection, compute the RID directly. Otherwise, do an index access.
    if (_isClusteredOnId) {
        return record_id_helpers::keyForObj(
            IndexBoundsBuilder::objFromElement(val, collptr->getDefaultCollator()));
    }

    uassert(ErrorCodes::QueryPlanKilled,
            str::stream() << "query plan killed :: index '" << _indexName.get() << "' dropped",
            _entry);
    const IndexDescriptor* desc = _entry->descriptor();

    // For the _id index, we can do a point look up in the index.
    // TODO SERVER-87148: We may be able to use the findSingle() path with any single-field index,
    // or maybe any non-dotted single-field index.
    auto sortedAccessMethod = _entry->accessMethod()->asSortedData();
    if (desc->isIdIndex() == 1) {
        return sortedAccessMethod->findSingle(_opCtx, collptr, _entry, val.wrap());
    }

    // Build the start and end bounds for the equality by appending a fully-open bound for each
    // remaining field in the compound index.
    BSONObjBuilder startBob, endBob;
    CollationIndexKey::collationAwareIndexKeyAppend(val, _cq->getCollator(), &startBob);
    CollationIndexKey::collationAwareIndexKeyAppend(val, _cq->getCollator(), &endBob);
    for (int i = 1; i < desc->getNumFields(); ++i) {
        if (desc->ordering().get(i) == 1) {
            startBob.appendMinKey("");
            endBob.appendMaxKey("");
        } else {
            startBob.appendMaxKey("");
            endBob.appendMinKey("");
        }
    }
    auto startKey = startBob.obj();
    auto endKey = endBob.obj();

    // Now seek to the first matching key in the index.
    auto indexCursor = sortedAccessMethod->newCursor(_opCtx, true /* forward */);
    indexCursor->setEndPosition(endKey, true /* endKeyInclusive */);
    auto keyStringForSeek = IndexEntryComparison::makeKeyStringFromBSONKeyForSeek(
        startKey,
        sortedAccessMethod->getSortedDataInterface()->getKeyStringVersion(),
        sortedAccessMethod->getSortedDataInterface()->getOrdering(),
        true /* forward */,
        true /* startKeyInclusive */);
    auto kv = indexCursor->seek(keyStringForSeek);
    if (!kv) {
        return {};
    }
    return kv->loc;
}

}  // namespace mongo
