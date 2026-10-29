// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace [[MONGO_MOD_PUBLIC]] mongo {

constexpr inline std::string_view kIsExternalClientOnRouterFieldName{"isExternalClientOnRouter"};

/**
 * Returns a settable boolean indicating whether the given operation context originated from an
 * external (user) client connection on a router (mongos).
 *
 * This flag is propagated from mongos to mongod so that the shard can distinguish user-initiated
 * commands routed through mongos from internal system commands (e.g. resharding, balancer). Without
 * this flag, all commands arriving from mongos are treated as "internal" because the mongos-to-
 * mongod connection is an internal client, which causes read preference metrics to miscount
 * user operations.
 */
bool& isExternalClientOnRouter(OperationContext*);

}  // namespace mongo
