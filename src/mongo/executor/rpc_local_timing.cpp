// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/executor/rpc_local_timing.h"

#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_service.h"
#include "mongo/util/assert_util.h"

#include <algorithm>
#include <vector>

namespace mongo {
namespace executor {

namespace {
auto& rpcLocalLatencyHistogram =
    otel::metrics::MetricsService::instance()
        .createInt64Histogram<std::string_view, std::string_view>(
            otel::metrics::MetricNames::kNetworkRpcLocalLatency,
            "Elapsed local-path latency for one outbound RPC segment, excluding the wait for the "
            "full response message.",
            otel::metrics::MetricUnit::kMicroseconds,
            otel::metrics::AttributeDefinition<std::string_view>{
                .name = "kind",
                .values = {toString(RpcLocalTimingKind::kInitial),
                           toString(RpcLocalTimingKind::kExhaustContinuation),
                           toString(RpcLocalTimingKind::kFireAndForget)}},
            otel::metrics::AttributeDefinition<std::string_view>{
                .name = "mode",
                .values = {toString(RpcLocalTimingMode::kAsync),
                           toString(RpcLocalTimingMode::kSync)}},
            {.explicitBucketBoundaries =
                 std::vector<double>{0,      5,      10,     25,      50,      100,     250,
                                     500,    1000,   2500,   5000,    10000,   25000,   50000,
                                     100000, 250000, 500000, 1000000, 2500000, 5000000, 10000000}});
}  // namespace

void recordRpcLocalLatency(RpcLocalTiming& timing,
                           RpcLocalTimingKind kind,
                           RpcLocalTimingMode mode) {
    auto elapsed = timing.finish();
    if (!elapsed) {
        return;
    }

    // Recording a negative value here will be process fatal, but the default tick source is from
    // std::chrono::steady_clock, which is monotonic. Still, clamp to 0 to avoid surprises.
    rpcLocalLatencyHistogram.record(std::max(elapsed->count(), int64_t{0}),
                                    {toString(kind), toString(mode)});
}

}  // namespace executor
}  // namespace mongo
