// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

namespace mongo {

class OperationContext;

/**
 * Schedules a drop for the temporary collections listed on the kAggTempCollections collection.
 * The list of collections to be dropped is read and snapshotted synchronously within this method;
 * but the actual drop happens asynchronously after this method returns.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] void dropAggTempCollections(OperationContext* opCtx);

}  // namespace mongo
