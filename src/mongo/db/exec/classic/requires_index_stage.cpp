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

#include "mongo/db/exec/classic/requires_index_stage.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/index_catalog.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo {

RequiresIndexStage::RequiresIndexStage(const char* stageType,
                                       ExpressionContext* expCtx,
                                       VariantCollectionPtrOrAcquisition collection,
                                       const IndexDescriptor* indexDescriptor,
                                       WorkingSet* workingSet)
    : RequiresCollectionStage(stageType, expCtx, collection),
      _indexIdent(indexDescriptor->getEntry()->getIdent()),
      _indexName(indexDescriptor->indexName()),
      _entry(indexDescriptor->getEntry()),
      _workingSetIndexId(workingSet->registerIndexIdent(_indexIdent)) {}

void RequiresIndexStage::doSaveStateRequiresCollection() {
    doSaveStateRequiresIndex();

    // Set the index entry to null, since accessing this pointer is illegal during yield.
    _entry = nullptr;
}

void RequiresIndexStage::doRestoreStateRequiresCollection() {
    auto desc = collectionPtr()->getIndexCatalog()->findIndexByIdent(
        expCtx()->getOperationContext(), _indexIdent);
    uassert(ErrorCodes::QueryPlanKilled,
            str::stream() << "query plan killed :: index '" << _indexName << "' dropped",
            desc);

    // Re-obtain the index entry pointer that was set to null during yield preparation. It is safe
    // to access the index entry when the query is active, as its validity is protected by at least
    // MODE_IS collection locks; or, in the case of lock-free reads, its lifetime is managed by the
    // CollectionCatalog stashed on the RecoveryUnit snapshot, which is kept alive until the query
    // yields.
    _entry = desc->getEntry();

    doRestoreStateRequiresIndex();
}

}  // namespace mongo
