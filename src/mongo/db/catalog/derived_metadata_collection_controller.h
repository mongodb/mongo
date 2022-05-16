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

#pragma once

#include <map>
#include <vector>

#include "mongo/bson/timestamp.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/derived_metadata_common.h"

namespace mongo {

/*
 * Decorates a Collection's shared state. Responsible for tracking derived metadata changes (e.g.
 * count, dataSize) as collection writes occur on the server.
 */
class DerivedMetadataCollectionController {
public:
    DerivedMetadataCollectionController() {}
    ~DerivedMetadataCollectionController() {}

    static DerivedMetadataCollectionController& get(const Collection* coll);

    /*
     * Registers a Delta containing the derived metadata changes for a write and what time the
     * associated write committed.
     */
    void registerDelta(DerivedMetadataDelta delta, const Timestamp& commitTs);

    bool hasDeltas() {
        return !_deltaQueue.empty();
    };

    /*
     * Returns a vector of Deltas with timestamps less than or equal to 'timestamp', deleting them
     * from the controller's internal queue.
     */
    std::vector<DerivedMetadataDelta> fetchDeltasUpTo(const Timestamp& timestamp);

private:
    // Protects the state below.
    Mutex _queueMutex = MONGO_MAKE_LATCH("DerivedMetadataCollectionController::_queueMutex");

    std::map<Timestamp, DerivedMetadataDelta> _deltaQueue;
};
}  // namespace mongo
