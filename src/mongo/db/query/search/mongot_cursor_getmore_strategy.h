/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/cursor_id.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/search/mongot_cursor.h"
#include "mongo/executor/task_executor_cursor_options.h"

namespace mongo {
namespace executor {

/**
 * Defines the GetMore strategy for TaskExecutorCursor when configuring requests sent to mongot.
 */
class MongotTaskExecutorCursorGetMoreStrategy final : public TaskExecutorCursorGetMoreStrategy {
public:
    // Specifies the factor by which the batchSize sent from mongod to mongot increases per batch.
    // TODO SERVER-88412: Convert this into an atomic, configurable cluster parameter.
    static constexpr double kInternalSearchBatchSizeGrowthFactor = 2.0;

    MongotTaskExecutorCursorGetMoreStrategy(
        bool preFetchNextBatch = true,
        std::function<boost::optional<long long>()> calcDocsNeededFn = nullptr,
        boost::optional<long long> startingBatchSize = mongot_cursor::kDefaultMongotBatchSize);

    MongotTaskExecutorCursorGetMoreStrategy(MongotTaskExecutorCursorGetMoreStrategy&& other) =
        default;

    ~MongotTaskExecutorCursorGetMoreStrategy() final {}

    /**
     * Generates a BSONObj that represents the next getMore command to be dispatched to mongot.
     * CursorId and NamespaceString are included in the command object, and prevBatchNumReceived is
     * used to configure batchSize tuning.
     */
    BSONObj createGetMoreRequest(const CursorId& cursorId,
                                 const NamespaceString& nss,
                                 long long prevBatchNumReceived) final;

    bool shouldPrefetch() const final {
        return _preFetchNextBatch;
    }

    long long getCurrentBatchSize() const {
        tassert(8953003,
                "getCurrentBatchSize() should only be called when using the batchSize field.",
                _currentBatchSize.has_value());
        return *_currentBatchSize;
    }

    std::vector<long long> getBatchSizeHistory() const {
        return _batchSizeHistory;
    }

private:
    /**
     * If batchSize tuning is not enabled, we'll see the initial batchSize for any getMore requests.
     * Otherwise, we'll apply tuning strategies to optimize batchSize of each batch requested.
     */
    long long _getNextBatchSize(long long prevBatchNumReceived);

    bool _preFetchNextBatch;

    // TODO SERVER-86736 Remove _calcDocsNeededFn and replace with pointer to SharedSearchState
    // to compute docs needed within the cursor.
    // Set to nullptr if docsRequested should not be set on getMore requests.
    std::function<boost::optional<long long>()> _calcDocsNeededFn;

    // Set to boost::none if batchSize should not be set on getMore requests.
    boost::optional<long long> _currentBatchSize;

    // TODO SERVER-86738 Incorporate batchSizeHistory into explain executionStats with $search.
    // Tracks all batchSizes sent to mongot, to be included in the $search explain execution stats.
    std::vector<long long> _batchSizeHistory;
};
}  // namespace executor
}  // namespace mongo
