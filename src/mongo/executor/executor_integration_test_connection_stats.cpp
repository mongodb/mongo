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

#include "src/mongo/executor/executor_integration_test_connection_stats.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/async_client_factory.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/integration_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::executor {
namespace {
bool assertConnectionStatsBase(const AsyncClientFactory& factory,
                               const HostAndPort& remote,
                               BSONObjBuilder& bob,
                               std::function<bool(const ConnectionStatsPer&)> connectionPoolTest,
                               std::function<bool(const GRPCConnectionStats&)> gRPCTest) {
    if (unittest::shouldUseGRPCEgress()) {
        factory.appendStats(bob);
        return gRPCTest(GRPCConnectionStats::parse(bob.asTempObj(), IDLParserContext("GRPCStats")));
    }

    ConnectionPoolStats stats;
    factory.appendConnectionStats(&stats);
    stats.appendToBSON(bob);
    return connectionPoolTest(stats.statsByHost[remote]);
}
}  // namespace

void assertConnectionStats(const AsyncClientFactory& factory,
                           const HostAndPort& remote,
                           std::function<bool(const ConnectionStatsPer&)> connectionPoolTest,
                           std::function<bool(const GRPCConnectionStats&)> gRPCTest,
                           StringData errMsg) {
    LOGV2(
        9924601, "Asserting connection stats", "usingGRPC"_attr = unittest::shouldUseGRPCEgress());

    BSONObjBuilder bob;
    if (!assertConnectionStatsBase(factory, remote, bob, connectionPoolTest, gRPCTest)) {
        FAIL(std::string{errMsg} + " Stats: " + bob.obj().toString());
    }
}

void assertConnectionStatsSoon(const AsyncClientFactory& factory,
                               const HostAndPort& remote,
                               std::function<bool(const ConnectionStatsPer&)> connectionPoolTest,
                               std::function<bool(const GRPCConnectionStats&)> gRPCTest,
                               StringData errMsg) {
    auto timeout = Seconds(30);
    LOGV2(9924600,
          "Asserting connection stats will hit targets by timeout.",
          "usingGRPC"_attr = unittest::shouldUseGRPCEgress(),
          "timeout"_attr = timeout);

    auto start = getGlobalServiceContext()->getFastClockSource()->now();
    std::string statsOnError;
    while (true) {
        BSONObjBuilder bob;
        if (assertConnectionStatsBase(factory, remote, bob, connectionPoolTest, gRPCTest)) {
            return;
        }
        sleepFor(Milliseconds(100));

        if (getGlobalServiceContext()->getFastClockSource()->now() - start >= timeout) {
            statsOnError = " Stats: " + bob.obj().toString();
            break;
        }
    }
    FAIL(std::string{errMsg} + statsOnError);
}

}  // namespace mongo::executor
