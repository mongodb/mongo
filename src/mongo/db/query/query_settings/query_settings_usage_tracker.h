// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/util/modules.h"

namespace mongo::query_settings {

class QuerySettingsUsageTracker final {
public:
    static QuerySettingsUsageTracker& get(OperationContext* opCtx);
    static QuerySettingsUsageTracker& get(ServiceContext* serviceContext);

    QuerySettingsUsageTracker() = default;
    QuerySettingsUsageTracker(const QuerySettingsUsageTracker&&) = delete;

    BSONObj generateServerStatusSection(OperationContext* opCtx) const;

    void setQuerySettingsUsageMetrics(int count, int size, int rejectCount);
    void setBackfillMemoryUsedBytes(int n);
    void setMissingRepresentativeQueries(int n);
    void setBufferedRepresentativeQueries(int n);
    void incrementInsertedRepresentativeQueries(int n);
    void incrementFailedBackfills(int n);
    void incrementSucceededBackfills(int n);

private:
    struct QuerySettingsUsageMetrics {
        int count = 0;
        int size = 0;
        int rejectCount = 0;
    };

    struct QuerySettingsBackfillMetrics {
        Atomic<int> memoryUsedBytes;
        Atomic<int> succeededBackfills;
        Atomic<int> failedBackfills;
        Atomic<int> missingRepresentativeQueries;
        Atomic<int> bufferedRepresentativeQueries;
        Atomic<int> insertedRepresentativeQueries;
    };

    void serializeUsageMetrics(BSONObjBuilder& bob) const;
    void serializeBackfillMetrics(BSONObjBuilder& bob) const;

    synchronized_value<QuerySettingsUsageMetrics> _usageMetrics;
    QuerySettingsBackfillMetrics _backfillMetrics;
};

}  // namespace mongo::query_settings
