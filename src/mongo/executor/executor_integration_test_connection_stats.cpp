// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/executor/executor_integration_test_connection_stats.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/async_client_factory.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/integration_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"

#include <string_view>

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
                           std::string_view errMsg) {
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
                               std::string_view errMsg) {
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
