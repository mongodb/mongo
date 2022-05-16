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

#include <vector>

#include "mongo/bson/timestamp.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/derived_metadata_common.h"
#include "mongo/db/catalog/derived_metadata_type.h"
#include "mongo/util/concurrency/with_lock.h"

namespace mongo {

/*
 * Maintains an in-memory count of documents in a collection. Decorates the shared state of a
 * collection.
 */
class DerivedMetadataCount : public DerivedMetadataType {
public:
    DerivedMetadataCount() {}
    ~DerivedMetadataCount() {}

    static DerivedMetadataCount& get(const Collection* coll);

    std::pair<Timestamp, long long> getLatestValue();

    static void populateDelta(const DocumentWriteImages& write, DerivedMetadataDelta* delta);

    void applyDeltas(const std::vector<DerivedMetadataDelta>& deltas);

private:
    void _updateLatestValue(WithLock, Timestamp newTimestamp, long long newCount);

    // Protects the state below.
    Mutex _valueMutex = MONGO_MAKE_LATCH("DerivedMetadataCollectionCount::_valueMutex");

    std::pair<Timestamp, long long> _latestTimestampedValue;
};
}  // namespace mongo
