// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/dbmessage.h"
#include "mongo/rpc/message.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Shard-role specific service entry point.
 */
class [[MONGO_MOD_PUBLIC]] ServiceEntryPointShardRole final : public ServiceEntryPoint {
public:
    Future<DbResponse> handleRequest(OperationContext* opCtx,
                                     const Message& request,
                                     Date_t started) final;
};

}  // namespace mongo
