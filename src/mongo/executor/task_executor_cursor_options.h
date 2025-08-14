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
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/client_cursor/cursor_id.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/executor/task_executor_cursor_parameters_gen.h"
#include "mongo/util/modules.h"

namespace MONGO_MOD_PUB mongo {
namespace executor {

/**
 * TaskExecutorCursorGetMoreStrategy is an abstract class that allows for customization of the logic
 * a TaskExecutorCursor uses to configure GetMore requests and toggle pre-fetching of those
 * GetMores.
 */
class MONGO_MOD_OPEN TaskExecutorCursorGetMoreStrategy {
public:
    /**
     * Generates a BSONObj that represents the next getMore command to be dispatched. Some
     * implementations (e.g., MongotTaskExecutorCursorGetMoreStrategy) may change internal state
     * (like batchSize) per batch, with the assumption that each call is for the subsequent batch.
     * For that reason, it's important this method is only called once per batch.
     *
     * The CursorId and NamespaceString are necessary to be included in the command obj, and
     * prevBatchNumReceived may be used to configure the request options (again, like batchSize).
     */
    virtual BSONObj createGetMoreRequest(const CursorId& cursorId,
                                         const NamespaceString& nss,
                                         long long prevBatchNumReceived,
                                         long long totalNumReceived) = 0;

    /**
     * If true, we'll fetch the next batch as soon as the current one is received. If false, we'll
     * fetch the next batch when the current batch is exhausted and 'getNext()' is invoked.
     *
     * totalNumReceived and numBatchesReceived may or may not be used, based on implementation, to
     * determine if prefetch is enabled.
     */
    virtual bool shouldPrefetch(long long totalNumReceived, long long numBatchesReceived) const = 0;

    virtual ~TaskExecutorCursorGetMoreStrategy() {}
};

/**
 * The default implementation of TaskExecutorCursorGetMoreStrategy simply holds a batchSize and
 * preFetchNextBatch flag that are used for all successive GetMore requests.
 */
class DefaultTaskExecutorCursorGetMoreStrategy final : public TaskExecutorCursorGetMoreStrategy {
public:
    DefaultTaskExecutorCursorGetMoreStrategy(boost::optional<int64_t> batchSize = boost::none,
                                             bool preFetchNextBatch = true)
        : _batchSize(batchSize), _preFetchNextBatch(preFetchNextBatch) {}

    DefaultTaskExecutorCursorGetMoreStrategy(DefaultTaskExecutorCursorGetMoreStrategy&& other) =
        default;

    ~DefaultTaskExecutorCursorGetMoreStrategy() final {}

    BSONObj createGetMoreRequest(const CursorId& cursorId,
                                 const NamespaceString& nss,
                                 long long prevBatchNumReceived,
                                 long long totalNumReceived) final;

    bool shouldPrefetch(long long totalNumReceived, long long numBatchesReceived) const final {
        return _preFetchNextBatch;
    }

private:
    const boost::optional<int64_t> _batchSize;
    const bool _preFetchNextBatch;
};

struct TaskExecutorCursorOptions {
    /**
     * Construct the options from any pre-constructed GetMore strategy.
     */
    TaskExecutorCursorOptions(bool pinConnection,
                              std::shared_ptr<TaskExecutorCursorGetMoreStrategy> getMoreStrategy =
                                  std::make_shared<DefaultTaskExecutorCursorGetMoreStrategy>(),
                              std::shared_ptr<PlanYieldPolicy> yieldPolicy = nullptr);

    /**
     * Construct the options with the default GetMore strategy from a given batchSize and
     * preFetchNextBatch flag.
     */
    TaskExecutorCursorOptions(bool pinConnection,
                              boost::optional<int64_t> batchSize,
                              bool preFetchNextBatch = true,
                              std::shared_ptr<PlanYieldPolicy> yieldPolicy = nullptr);

    TaskExecutorCursorOptions(TaskExecutorCursorOptions& other) = default;
    TaskExecutorCursorOptions(TaskExecutorCursorOptions&& other) = default;

    // Specifies whether the TEC should use a `PinnedConnectionTaskExecutor` to run all
    // operations on the cursor over the same transport session.
    bool pinConnection;

    // Specifies the strategy with which the cursor should configure GetMore requests.
    // Using shared_ptr to allow tries on network failure, don't share the pointer on other
    // purpose.
    std::shared_ptr<TaskExecutorCursorGetMoreStrategy> getMoreStrategy;

    // Optional yield policy allows us to yield(release storage resources) during remote call.
    // Using shared_ptr to allow tries on network failure, don't share the pointer on other
    // purpose. In practice, this will always be of type PlanYieldPolicyRemoteCursor, but
    // for dependency reasons, we must use the generic PlanYieldPolicy here.
    std::shared_ptr<PlanYieldPolicy> yieldPolicy;
};
}  // namespace executor
}  // namespace MONGO_MOD_PUB mongo
