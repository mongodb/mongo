// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/executor/async_client_factory.h"
#include "mongo/executor/connection_pool_stats.h"
#include "mongo/transport/grpc_connection_stats_gen.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo::executor {

/**
 * Similar to assertConnectionStatsSoon but asserts on the values immediately.
 */
[[MONGO_MOD_PUBLIC]] void assertConnectionStats(
    const AsyncClientFactory& factory,
    const HostAndPort& remote,
    std::function<bool(const ConnectionStatsPer&)> connectionPoolTest,
    std::function<bool(const GRPCConnectionStats&)> gRPCTest,
    std::string_view errMsg);

/**
 * Asserts that the connection stats reach a certain value within a 30 second window. If the test is
 * configred to use gRPC, the provided gRPCTest function will be called with the parsed stats from
 * the provided AsyncClientFactory until it returns true or the timeout is reached. Otherwise,
 * connectionPoolTest will be called with the ConnectionStatsPer associated with remote. Upon a
 * failure, a stringified version of the stats will be added to the errMsg.
 * TODO: SERVER-66126 Some callsites can switched to use assertConnectionStats.
 */
[[MONGO_MOD_PUBLIC]] void assertConnectionStatsSoon(
    const AsyncClientFactory& factory,
    const HostAndPort& remote,
    std::function<bool(const ConnectionStatsPer&)> connectionPoolTest,
    std::function<bool(const GRPCConnectionStats&)> gRPCTest,
    std::string_view errMsg);

}  // namespace mongo::executor
