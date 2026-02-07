/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/s/write_ops/unified_write_executor/write_op_batcher.h"
#include "mongo/util/modules.h"

#include <absl/container/flat_hash_set.h>

namespace mongo {
namespace unified_write_executor {

class WriteBatchQueryStatsRegistrar {
public:
    /**
     * The maximum num of write ops allowed to be requested for query stats metrics. This limit
     * prevents shard server responses from exceeding BSON object size limit due to the overhead
     * originating from CursorMetrics.
     */
    static constexpr size_t kMaxBatchOpsMetricsRequested = 10'000;

    WriteBatchQueryStatsRegistrar() {}

    /**
     * Register the write ops inside 'cmdRef' for query stats.
     */
    static void registerRequest(OperationContext* opCtx, WriteCommandRef cmdRef);

    /**
     * Set the field includeQueryStatsMetrics in 'updateOpEntry' if the write op at 'opIndex' is
     * requested for query stats metrics. It does nothing if the write op is not chosen, or we
     * already reach the limit.
     */
    void setIncludeQueryStatsMetricsIfRequested(CurOp* curOp,
                                                int opIndex,
                                                write_ops::UpdateOpEntry& updateOpEntry);

private:
    size_t _numOpsWithMetricsRequested = 0;
};
}  // namespace unified_write_executor
}  // namespace mongo
