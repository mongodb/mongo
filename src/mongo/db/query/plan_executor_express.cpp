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
#include "mongo/db/query/index_bounds_builder.h"

namespace mongo {
PlanExecutorExpress::PlanExecutorExpress(OperationContext* opCtx,
                                         std::unique_ptr<CanonicalQuery> cq,
                                         VariantCollectionPtrOrAcquisition coll,
                                         boost::optional<ScopedCollectionFilter> collectionFilter,
                                         bool isClusteredOnId)
    : _opCtx(opCtx),
      _cq(std::move(cq)),
      _coll(coll),
      _commonStats("EXPRESS"),
      _nss(_cq->nss()),
      _planExplainer(&_commonStats, isClusteredOnId),
      _shardFilterer(std::move(collectionFilter)) {}

PlanExecutor::ExecState PlanExecutorExpress::getNext(BSONObj* out, RecordId* dlOut) {

    if (_done) {
        _commonStats.isEOF = true;
        return ExecState::IS_EOF;
    }
    _done = true;
    _commonStats.works++;

    BSONObj doc;
    bool found = findById(_cq->getQueryObj(), doc, dlOut);
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

bool PlanExecutorExpress::findById(const BSONObj& query, BSONObj& result, RecordId* ridOut) const {
    Snapshotted<BSONObj> snapDoc;
    const auto& collptr = _coll.getCollectionPtr();

    if (_planExplainer.isClusteredOnId()) {
        RecordId rid = record_id_helpers::keyForObj(
            IndexBoundsBuilder::objFromElement(query["_id"], collptr->getDefaultCollator()));
        if (!rid.isNull() && collptr->findDoc(_opCtx, rid, &snapDoc)) {
            result = std::move(snapDoc.value());
            if (ridOut) {
                *ridOut = std::move(rid);
            }
            return true;
        }
        return false;
    }
    const IndexCatalog* catalog = collptr->getIndexCatalog();
    const IndexDescriptor* desc = catalog->findIdIndex(_opCtx);
    invariant(desc);  // we must have an _id index since it's not clustered

    const IndexCatalogEntry* entry = catalog->getEntry(desc);
    RecordId rid = entry->accessMethod()->asSortedData()->findSingle(
        _opCtx, collptr, entry, query["_id"].wrap());

    if (!rid.isNull() && collptr->findDoc(_opCtx, rid, &snapDoc)) {
        result = std::move(snapDoc.value());
        if (ridOut) {
            *ridOut = std::move(rid);
        }
        return true;
    }
    return false;
}

}  // namespace mongo
