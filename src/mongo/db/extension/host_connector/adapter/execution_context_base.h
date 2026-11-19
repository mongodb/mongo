// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/base/status.h"
#include "mongo/util/modules.h"

#include <cstdint>

namespace mongo::extension::host_connector {

/**
 * ExecutionContextBase is the abstract interface shared by every execution context the host
 * exposes to extensions: the query execution context used by aggregation stages and the command
 * execution context used by extension commands.
 *
 * Both are backed by the operation's OperationContext, so they share the ability to report
 * interrupts and the operation deadline. Concrete implementations are wrapped by their respective
 * adapters for exposure to the public extension API.
 */
class ExecutionContextBase {
public:
    ExecutionContextBase() = default;
    virtual ~ExecutionContextBase() = default;

    virtual Status checkForInterrupt() const = 0;
    virtual int64_t getDeadlineTimestampMs() const = 0;
};

}  // namespace mongo::extension::host_connector
