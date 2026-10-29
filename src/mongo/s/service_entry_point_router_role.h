// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/transport/service_entry_point.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * The entry point from the TransportLayer into Mongos.
 */
class [[MONGO_MOD_PUBLIC]] ServiceEntryPointRouterRole final : public ServiceEntryPoint {
public:
    static Future<DbResponse> handleRequestImpl(OperationContext* opCtx,
                                                const Message& request,
                                                Date_t started);

    Future<DbResponse> handleRequest(OperationContext* opCtx,
                                     const Message& request,
                                     Date_t started) final;
};

}  // namespace mongo
