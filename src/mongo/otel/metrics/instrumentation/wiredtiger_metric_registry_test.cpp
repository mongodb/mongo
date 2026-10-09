// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/otel/metrics/instrumentation/wiredtiger_metric_registry.h"

#include "mongo/otel/metrics/otel_metric_name_validation.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(WtStatNameTranslationTest, TranslatesSectionsAndSeparators) {
    ASSERT_EQ(wtStatNameToOtelName("cache: bytes read into cache"),
              "mongodb.serverStatus.wiredTiger.cache.bytes_read_into_cache");
    ASSERT_EQ(wtStatNameToOtelName("data-handle: connection data handles currently active"),
              "mongodb.serverStatus.wiredTiger.data-handle.connection_data_handles_currently_"
              "active");
    // Punctuation is dropped rather than turned into an extra underscore.
    ASSERT_EQ(wtStatNameToOtelName("checkpoint: most recent time (msecs)"),
              "mongodb.serverStatus.wiredTiger.checkpoint.most_recent_time_msecs");
    ASSERT_EQ(wtStatNameToOtelName("bytes read into cache"),
              "mongodb.serverStatus.wiredTiger.bytes_read_into_cache");
}

}  // namespace
}  // namespace mongo
