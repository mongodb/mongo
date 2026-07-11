// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/init.h"
#include "mongo/db/commands/server_status/server_status_metric.h"
#include "mongo/logv2/log_detail.h"

namespace mongo::logv2::detail {
namespace {
auto& logCounter = *MetricBuilder<Counter64>{"logging.count"};
}  // namespace

MONGO_INITIALIZER(LogCounter)(InitializerContext* context) {
    setLogCounterCallback([]() { logCounter.increment(); });
}
}  // namespace mongo::logv2::detail
