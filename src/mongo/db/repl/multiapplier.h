// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/task_executor.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/functional.h"
#include "mongo/util/modules.h"

#include <functional>
#include <iosfwd>
#include <mutex>
#include <vector>

#include <boost/move/utility_core.hpp>

namespace mongo {
namespace repl {

class OpTime;

class MultiApplier {
    MultiApplier(const MultiApplier&) = delete;
    MultiApplier& operator=(const MultiApplier&) = delete;

public:
    /**
     * Callback function to report final status of applying operations.
     */
    using CallbackFn = unique_function<void(const Status&)>;

    using MultiApplyFn =
        std::function<StatusWith<OpTime>(OperationContext*, std::vector<OplogEntry>)>;

    /**
     * Creates MultiApplier in inactive state.
     *
     * Accepts list of oplog entries to apply in 'operations'.
     *
     * The callback function will be invoked (after schedule()) when the applied has
     * successfully applied all the operations or encountered a failure. Failures may occur if
     * we failed to apply an operation; or if the underlying scheduled work item
     * on the replication executor was canceled.
     *
     * It is an error for 'operations' to be empty but individual oplog entries
     * contained in 'operations' are not validated.
     */
    MultiApplier(executor::TaskExecutor* executor,
                 const std::vector<OplogEntry>& operations,
                 const MultiApplyFn& multiApply,
                 CallbackFn onCompletion);

    /**
     * Blocks while applier is active.
     */
    virtual ~MultiApplier();

    /**
     * Returns true if the applier has been started (but has not completed).
     */
    bool isActive() const;

    /**
     * Starts applier by scheduling initial db work to be run by the executor.
     */
    Status startup() noexcept;

    /**
     * Cancels current db work request.
     * Returns immediately if applier is not active.
     *
     * Callback function may be invoked with an ErrorCodes::CallbackCanceled status.
     */
    void shutdown();

    /**
     * Waits for active database worker to complete.
     * Returns immediately if applier is not active.
     */
    void join();

    // State transitions:
    // PreStart --> Running --> ShuttingDown --> Complete
    // It is possible to skip intermediate states. For example,
    // Calling shutdown() when the cloner has not started will transition from PreStart directly
    // to Complete.
    // This enum class is made public for testing.
    enum class State { kPreStart, kRunning, kShuttingDown, kComplete };

    /**
     * Returns current MultiApplier state.
     * For testing only.
     */
    State getState_forTest() const;

private:
    bool _isActive(WithLock lk) const;

    /**
     * DB worker callback function - applies all operations.
     */
    void _callback(const executor::TaskExecutor::CallbackArgs& cbd);
    void _finishCallback(const Status& result);

    // Not owned by us.
    executor::TaskExecutor* _executor;

    std::vector<OplogEntry> _operations;
    MultiApplyFn _multiApply;
    CallbackFn _onCompletion;

    // Protects member data of this MultiApplier.
    mutable std::mutex _mutex;

    stdx::condition_variable _condition;

    // Current multi applier state. See comments for State enum class for details.
    State _state = State::kPreStart;

    executor::TaskExecutor::CallbackHandle _dbWorkCallbackHandle;
};

/**
 * Insertion operator for MultiApplier::State. Formats fetcher state for output stream.
 * For testing only.
 */
std::ostream& operator<<(std::ostream& os, const MultiApplier::State& state);

}  // namespace repl
}  // namespace mongo
