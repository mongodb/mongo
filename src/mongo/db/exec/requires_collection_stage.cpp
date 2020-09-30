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

namespace mongo {

void RequiresCollectionStage::doSaveState() {
    doSaveStateRequiresCollection();
}

void RequiresCollectionStage::doRestoreState(const RestoreContext& context) {
    // We should be holding a lock associated with the name of the collection prior to yielding,
    // even if the collection was renamed during yield.
    dassert(opCtx()->lockState()->isCollectionLockedForMode(_nss, MODE_IS));

    auto collectionDropped = [this]() {
        uasserted(ErrorCodes::QueryPlanKilled,
                  str::stream() << "collection dropped. UUID " << _collectionUUID);
    };

    auto collectionRenamed = [this](const NamespaceString& newNss) {
        uasserted(ErrorCodes::QueryPlanKilled,
                  str::stream() << "collection renamed from '" << _nss << "' to '" << newNss
                                << "'. UUID " << _collectionUUID);
    };

    if (context.type() == RestoreContext::RestoreType::kExternal) {
        // RequiresCollectionStage requires a collection to be provided in restore. However, it may
        // be null in case the collection got dropped or renamed.
        auto collPtr = context.collection();
        invariant(collPtr);
        _collection = collPtr;

        // If we restore externally and get a null Collection we need to figure out if this was a
        // drop or rename. The external lookup could have been done for UUID or namespace.
        const auto& coll = *collPtr;

        // If collection exists uuid does not match assume lookup was over namespace and treat this
        // as a drop.
        if (coll && coll->uuid() != _collectionUUID) {
            collectionDropped();
        }

        // If we didn't get a valid collection but can still find the UUID in the catalog then we
        // treat this as a rename.
        if (!coll) {
            const CollectionCatalog& catalog = CollectionCatalog::get(opCtx());
            auto newNss = catalog.lookupNSSByUUID(opCtx(), _collectionUUID);
            if (newNss && *newNss != _nss) {
                collectionRenamed(*newNss);
            }
        }
    }

    const auto& coll = *_collection;

    if (!coll) {
        collectionDropped();
    }

    // TODO SERVER-31695: Allow queries to survive collection rename, rather than throwing here
    // when a rename has happened during yield.
    if (const auto& newNss = coll->ns(); newNss != _nss) {
        collectionRenamed(newNss);
    }

    uassert(ErrorCodes::QueryPlanKilled,
            str::stream() << "The catalog was closed and reopened",
            getCatalogEpoch() == _catalogEpoch);

    doRestoreStateRequiresCollection();
}

}  // namespace mongo
