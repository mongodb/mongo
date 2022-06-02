/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/service_context.h"
#include "mongo/platform/atomic_word.h"

namespace mongo {

/**
 * Encapsulates per-process statistics for the shard split subsystem.
 */
class ShardSplitStatistics {
    ShardSplitStatistics(const ShardSplitStatistics&) = delete;
    ShardSplitStatistics& operator=(const ShardSplitStatistics&) = delete;

public:
    ShardSplitStatistics() = default;
    static ShardSplitStatistics* get(ServiceContext* service);

    void incrementTotalCommitted(Milliseconds durationWithCatchup,
                                 Milliseconds durationWithoutCatchup);
    void incrementTotalAborted();

    void appendInfoForServerStatus(BSONObjBuilder* builder) const;

private:
    // Total number of times shard splits successfully committed.
    AtomicWord<std::int64_t> _totalCommitted{0};
    // Total duration of successfully committed shard splits.
    AtomicWord<std::int64_t> _totalCommittedDurationMillis{0};
    // Total duration of successfully committed shard splits, excluding the block timestamp catchup.
    AtomicWord<std::int64_t> _totalCommittedDurationWithoutCatchupMillis{0};
    // Total number of times shard splits were aborted.
    AtomicWord<std::int64_t> _totalAborted{0};
};

}  // namespace mongo
