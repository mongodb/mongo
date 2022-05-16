/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/catalog/derived_metadata_controller.h"

#include "mongo/db/catalog/derived_metadata_collection_controller.h"
#include "mongo/db/catalog/derived_metadata_collection_count.h"
#include "mongo/db/operation_context.h"

namespace mongo {
void DerivedMetadataController::createAndRegister(OperationContext* opCtx,
                                                  const Collection* collection,
                                                  const BSONObj& writePreImage,
                                                  const BSONObj& writePostImage) {
    DocumentWriteImages write = {writePreImage, writePostImage};
    DerivedMetadataDelta delta = _createDelta(write);
    _registerDeltaOnCommit(opCtx, std::move(delta), collection);
}

void DerivedMetadataController::pullAndApplyDeltas(const Collection* collection,
                                                   const Timestamp& ts) {
    auto& collectionController = DerivedMetadataCollectionController::get(collection);
    auto& derivedCount = DerivedMetadataCount::get(collection);

    auto deltas = collectionController.fetchDeltasUpTo(ts);
    derivedCount.applyDeltas(deltas);
}

DerivedMetadataDelta DerivedMetadataController::_createDelta(const DocumentWriteImages& write) {
    DerivedMetadataDelta newDelta;
    DerivedMetadataCount::populateDelta(write, &newDelta);
    return newDelta;
}

void DerivedMetadataController::_registerDeltaOnCommit(OperationContext* opCtx,
                                                       DerivedMetadataDelta delta,
                                                       const Collection* collection) {
    auto& collectionController = DerivedMetadataCollectionController::get(collection);
    Timestamp commitTs;
    collectionController.registerDelta(std::move(delta), commitTs);
}
}  // namespace mongo
