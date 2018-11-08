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

void RequiresIndexStage::doSaveStateRequiresCollection() {
    doSaveStateRequiresIndex();

    // During yield, we relinquish our shared ownership of the index catalog entry. This allows the
    // index to be dropped during yield, but permits us to check via the weak_ptr interface
    // whether the index is still valid on yield recovery.
    //
    // We also set catalog pointers to null, since accessing these pointers is illegal during yield.
    _indexCatalogEntry.reset();
    _indexDescriptor = nullptr;
    _indexAccessMethod = nullptr;
}

void RequiresIndexStage::doRestoreStateRequiresCollection() {
    // Reacquire shared ownership of the index catalog entry. If we're unable to do so, then the
    // our index is no longer valid, and the query should die.
    _indexCatalogEntry = _weakIndexCatalogEntry.lock();
    uassert(ErrorCodes::QueryPlanKilled,
            str::stream() << "query plan killed :: index named '" << _indexName
                          << "' is no longer valid",
            _indexCatalogEntry);

    _indexDescriptor = _indexCatalogEntry->descriptor();
    _indexAccessMethod = _indexCatalogEntry->accessMethod();

    doRestoreStateRequiresIndex();
}

}  // namespace mongo
