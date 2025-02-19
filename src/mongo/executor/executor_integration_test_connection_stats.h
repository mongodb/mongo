/**
 *    Copyright (C) 2025-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/executor/async_client_factory.h"
#include "mongo/executor/connection_pool_stats.h"
#include "mongo/transport/grpc_connection_stats_gen.h"

namespace mongo::executor {

/**
 * Similar to assertConnectionStatsSoon but asserts on the values immediately.
 */
void assertConnectionStats(const AsyncClientFactory& factory,
                           const HostAndPort& remote,
                           std::function<bool(const ConnectionStatsPer&)> connectionPoolTest,
                           std::function<bool(const GRPCConnectionStats&)> gRPCTest,
                           StringData errMsg);

/**
 * Asserts that the connection stats reach a certain value within a 30 second window. If the test is
 * configred to use gRPC, the provided gRPCTest function will be called with the parsed stats from
 * the provided AsyncClientFactory until it returns true or the timeout is reached. Otherwise,
 * connectionPoolTest will be called with the ConnectionStatsPer associated with remote. Upon a
 * failure, a stringified version of the stats will be added to the errMsg.
 * TODO: SERVER-66126 Some callsites can switched to use assertConnectionStats.
 */
void assertConnectionStatsSoon(const AsyncClientFactory& factory,
                               const HostAndPort& remote,
                               std::function<bool(const ConnectionStatsPer&)> connectionPoolTest,
                               std::function<bool(const GRPCConnectionStats&)> gRPCTest,
                               StringData errMsg);

}  // namespace mongo::executor
