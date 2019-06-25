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

#include "mongo/db/exec/requires_collection_stage.h"

#include "mongo/db/catalog/collection_catalog.h"

namespace mongo {

template <typename CollectionT>
void RequiresCollectionStageBase<CollectionT>::doSaveState() {
    doSaveStateRequiresCollection();

    // A stage may not access storage while in a saved state.
    _collection = nullptr;
}

template <typename CollectionT>
void RequiresCollectionStageBase<CollectionT>::doRestoreState() {
    invariant(!_collection);

    // We should be holding a lock associated with the name of the collection prior to yielding,
    // even if the collection was renamed during yield.
    dassert(getOpCtx()->lockState()->isCollectionLockedForMode(_nss, MODE_IS));

    const CollectionCatalog& catalog = CollectionCatalog::get(getOpCtx());
    auto newNss = catalog.lookupNSSByUUID(_collectionUUID);
    uassert(ErrorCodes::QueryPlanKilled,
            str::stream() << "collection dropped. UUID " << _collectionUUID,
            newNss);

    // TODO SERVER-31695: Allow queries to survive collection rename, rather than throwing here when
    // a rename has happened during yield.
    uassert(ErrorCodes::QueryPlanKilled,
            str::stream() << "collection renamed from '" << _nss << "' to '" << *newNss
                          << "'. UUID "
                          << _collectionUUID,
            *newNss == _nss);

    // At this point we know that the collection name has not changed, and therefore we have
    // restored locks on the correct name. It is now safe to restore the Collection pointer. The
    // collection must exist, since we already successfully looked up the namespace string by UUID
    // under the correct lock manager locks.
    _collection = catalog.lookupCollectionByUUID(_collectionUUID);
    invariant(_collection);

    uassert(ErrorCodes::QueryPlanKilled,
            str::stream()
                << "Database epoch changed due to a database-level event such as 'restartCatalog'.",
            getDatabaseEpoch(_collection) == _databaseEpoch);

    doRestoreStateRequiresCollection();
}

template class RequiresCollectionStageBase<const Collection*>;
template class RequiresCollectionStageBase<Collection*>;

}  // namespace mongo
