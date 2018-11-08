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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kIndex

#include "mongo/platform/basic.h"

#include "mongo/db/index/duplicate_key_tracker.h"

#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

namespace mongo {

namespace {
static constexpr StringData kKeyField = "key"_sd;
}

DuplicateKeyTracker::DuplicateKeyTracker(const IndexCatalogEntry* entry, const NamespaceString& nss)
    : _idCounter(0), _indexCatalogEntry(entry), _nss(nss) {

    invariant(_indexCatalogEntry->descriptor()->unique());
}

DuplicateKeyTracker::~DuplicateKeyTracker() {}

NamespaceString DuplicateKeyTracker::makeTempNamespace() {
    return NamespaceString("local.system.indexBuildConstraints-" + UUID::gen().toString());
}

Status DuplicateKeyTracker::recordDuplicates(OperationContext* opCtx,
                                             Collection* tempCollection,
                                             const std::vector<BSONObj>& keys) {
    invariant(tempCollection->ns() == nss());
    invariant(opCtx->lockState()->inAWriteUnitOfWork());
    invariant(opCtx->lockState()->isCollectionLockedForMode(tempCollection->ns().ns(), MODE_IX));

    for (auto&& key : keys) {
        BSONObjBuilder builder;
        builder.append("_id", _idCounter++);
        builder.append(kKeyField, key);

        BSONObj obj = builder.obj();

        LOG(2) << "Recording conflict for DuplicateKeyTracker: " << obj.toString();
        Status s = tempCollection->insertDocument(opCtx, InsertStatement(obj), nullptr, false);
        if (!s.isOK())
            return s;
    }
    return Status::OK();
}

Status DuplicateKeyTracker::constraintsSatisfiedForIndex(OperationContext* opCtx,
                                                         Collection* tempCollection) const {
    invariant(tempCollection->ns() == nss());
    invariant(opCtx->lockState()->isCollectionLockedForMode(tempCollection->ns().ns(), MODE_IS));
    invariant(!opCtx->lockState()->inAWriteUnitOfWork());

    auto collScan = InternalPlanner::collectionScan(
        opCtx, tempCollection->ns().ns(), tempCollection, PlanExecutor::YieldPolicy::YIELD_AUTO);

    BSONObj conflict;
    PlanExecutor::ExecState state;
    while (PlanExecutor::ExecState::ADVANCED == (state = collScan->getNext(&conflict, nullptr))) {

        LOG(2) << "Resolving conflict for DuplicateKeyTracker: " << conflict.toString();

        BSONObj keyObj = conflict[kKeyField].Obj();

        auto cursor =
            _indexCatalogEntry->accessMethod()->getSortedDataInterface()->newCursor(opCtx);
        auto entry = cursor->seekExact(keyObj);

        // If there is not an exact match, there is no duplicate.
        if (!entry) {
            continue;
        }

        // If the following entry has the same key, this is a duplicate.
        entry = cursor->next();
        if (entry && entry->key.woCompare(keyObj, BSONObj(), /*considerFieldNames*/ false) == 0) {
            return buildDupKeyErrorStatus(keyObj,
                                          _indexCatalogEntry->descriptor()->parentNS(),
                                          _indexCatalogEntry->descriptor()->indexName(),
                                          _indexCatalogEntry->descriptor()->keyPattern());
        }
    }

    if (PlanExecutor::IS_EOF != state) {
        return WorkingSetCommon::getMemberObjectStatus(conflict);
    }
    return Status::OK();
}

}  // namespace mongo
