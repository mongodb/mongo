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

#include "mongo/db/exec/requires_index_stage.h"

namespace mongo {

RequiresIndexStage::RequiresIndexStage(const char* stageType,
                                       OperationContext* opCtx,
                                       const IndexDescriptor* indexDescriptor,
                                       WorkingSet* workingSet)
    : RequiresCollectionStage(stageType, opCtx, indexDescriptor->getCollection()),
      _weakIndexCatalogEntry(collection()->getIndexCatalog()->getEntryShared(indexDescriptor)) {
    auto indexCatalogEntry = _weakIndexCatalogEntry.lock();
    _indexDescriptor = indexCatalogEntry->descriptor();
    _indexAccessMethod = indexCatalogEntry->accessMethod();
    invariant(_indexDescriptor);
    invariant(_indexAccessMethod);
    _indexName = _indexDescriptor->indexName();
    _workingSetIndexId = workingSet->registerIndexAccessMethod(_indexAccessMethod);
}

void RequiresIndexStage::doSaveStateRequiresCollection() {
    doSaveStateRequiresIndex();

    // Set catalog pointers to null, since accessing these pointers is illegal during yield.
    _indexDescriptor = nullptr;
    _indexAccessMethod = nullptr;
}

void RequiresIndexStage::doRestoreStateRequiresCollection() {
    // Attempt to lock the weak_ptr. If the resulting shared_ptr is null, then our index is no
    // longer valid and the query should die. We must also check the `isDropped()` flag on the index
    // catalog entry if we are able lock the weak_ptr.
    auto indexCatalogEntry = _weakIndexCatalogEntry.lock();
    uassert(ErrorCodes::QueryPlanKilled,
            str::stream() << "query plan killed :: index '" << _indexName << "' dropped",
            indexCatalogEntry && !indexCatalogEntry->isDropped());

    // Re-obtain catalog pointers that were set to null during yield preparation. It is safe to
    // access the catalog entry by raw pointer when the query is active, as its validity is
    // protected by at least MODE_IS collection locks.
    _indexDescriptor = indexCatalogEntry->descriptor();
    _indexAccessMethod = indexCatalogEntry->accessMethod();
    invariant(_indexDescriptor);
    invariant(_indexAccessMethod);

    doRestoreStateRequiresIndex();
}

}  // namespace mongo
