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

#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"

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
