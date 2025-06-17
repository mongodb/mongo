/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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


#include "mongo/db/query/collection_index_usage_tracker_decoration.h"

#include "mongo/db/aggregated_index_usage_tracker.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/service_context.h"
#include "mongo/util/decorable.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {

namespace {
const auto getCollectionIndexUsageTrackerDecoration =
    Collection::declareDecoration<CollectionIndexUsageTrackerDecoration>();

}  // namespace

const CollectionIndexUsageTracker& CollectionIndexUsageTrackerDecoration::get(
    const Collection* collection) {
    return *getCollectionIndexUsageTrackerDecoration(collection)._collectionIndexUsageTracker;
}

CollectionIndexUsageTracker& CollectionIndexUsageTrackerDecoration::write(Collection* collection) {
    auto& decoration = getCollectionIndexUsageTrackerDecoration(collection);

    // Make copy of existing CollectionIndexUsageTracker and store it in our writable Collection
    // instance.
    decoration._collectionIndexUsageTracker =
        new CollectionIndexUsageTracker(*decoration._collectionIndexUsageTracker);

    return *decoration._collectionIndexUsageTracker;
}

CollectionIndexUsageTrackerDecoration::CollectionIndexUsageTrackerDecoration() {
    // This can get instantiated in unittests that doesn't set a global service context.
    if (!hasGlobalServiceContext())
        return;

    _collectionIndexUsageTracker =
        new CollectionIndexUsageTracker(AggregatedIndexUsageTracker::get(getGlobalServiceContext()),
                                        getGlobalServiceContext()->getPreciseClockSource());
}

}  // namespace mongo
