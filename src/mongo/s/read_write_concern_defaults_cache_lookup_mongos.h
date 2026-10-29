// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/read_write_concern_defaults_gen.h"
#include "mongo/util/modules.h"

#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * A function which handles looking up RWConcernDefault values from config servers.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] boost::optional<RWConcernDefault>
readWriteConcernDefaultsCacheLookupMongoS(OperationContext* opCtx);

}  // namespace mongo
