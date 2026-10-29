// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/logv2/log_severity.h"
#include "mongo/logv2/log_severity_suppressor.h"
#include "mongo/rpc/message.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"

namespace mongo {
class OperationContext;
struct DbResponse;

/**
 * This is the entrypoint from the transport layer into mongod or mongos.
 */
class [[MONGO_MOD_OPEN]] ServiceEntryPoint {
private:
    ServiceEntryPoint(const ServiceEntryPoint&) = delete;
    ServiceEntryPoint& operator=(const ServiceEntryPoint&) = delete;

protected:
    ServiceEntryPoint() = default;

public:
    virtual ~ServiceEntryPoint() = default;

    /**
     * Processes a request and fills out a DbResponse.
     */
    virtual Future<DbResponse> handleRequest(OperationContext* opCtx,
                                             const Message& request,
                                             Date_t started) = 0;
};
}  // namespace mongo
