// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/modules.h"

#include <functional>

namespace mongo::catalog_helper {

[[MONGO_MOD_NEEDS_REPLACEMENT]]
extern StorageEngine::TimestampMonitor::TimestampListener
    kCollectionCatalogCleanupTimestampListener;

[[MONGO_MOD_PRIVATE]] extern FailPoint setAutoGetCollectionWait;

/**
 * Executes the provided callback on the 'setAutoGetCollectionWait' FailPoint.
 */
[[MONGO_MOD_PRIVATE]]
static void setAutoGetCollectionWaitFailpointExecute(
    const std::function<void(const BSONObj&)>& callback) {
    setAutoGetCollectionWait.execute(callback);
}

}  // namespace mongo::catalog_helper
