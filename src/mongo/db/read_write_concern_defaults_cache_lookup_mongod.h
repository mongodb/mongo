// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/read_write_concern_defaults_gen.h"
#include "mongo/util/modules.h"

#include <boost/optional/optional.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * A function which handles looking up RWConcernDefault values from where they are persisted in
 * config.settings.
 */
boost::optional<RWConcernDefault> readWriteConcernDefaultsCacheLookupMongoD(
    OperationContext* opCtx);

void readWriteConcernDefaultsMongodStartupChecks(OperationContext* opCtx, bool isReplicaSet);

}  // namespace mongo
