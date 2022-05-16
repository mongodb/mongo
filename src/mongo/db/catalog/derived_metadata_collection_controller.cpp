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

#include "mongo/db/catalog/derived_metadata_collection_controller.h"

#include "mongo/db/operation_context.h"

namespace mongo {
const auto getCollectionController =
    SharedCollectionDecorations::declareDecoration<DerivedMetadataCollectionController>();

DerivedMetadataCollectionController& DerivedMetadataCollectionController::get(
    const Collection* coll) {
    return getCollectionController(coll->getSharedDecorations());
}

void DerivedMetadataCollectionController::registerDelta(DerivedMetadataDelta delta,
                                                        const Timestamp& commitTs) {
    delta.timestamp = commitTs;

    stdx::lock_guard lock(_queueMutex);
    _deltaQueue.emplace(std::make_pair(commitTs, delta));
}

std::vector<DerivedMetadataDelta> DerivedMetadataCollectionController::fetchDeltasUpTo(
    const Timestamp& timestamp) {
    std::vector<DerivedMetadataDelta> requestedDeltas;
    std::map<Timestamp, DerivedMetadataDelta>::iterator it;

    for (it = _deltaQueue.begin(); it != _deltaQueue.end(); ++it) {
        const auto& currTimestamp = it->first;
        const auto& currDelta = it->second;
        if (currTimestamp > timestamp) {
            break;
        }
        requestedDeltas.push_back(currDelta);
    }

    // Erase the deltas up to the validTs.
    stdx::lock_guard lock(_queueMutex);
    _deltaQueue.erase(_deltaQueue.begin(), it);

    return requestedDeltas;
}

}  // namespace mongo
