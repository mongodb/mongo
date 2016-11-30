/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <iosfwd>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/task_executor.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/mutex.h"

namespace mongo {
namespace repl {

class OpTime;

class MultiApplier {
    MONGO_DISALLOW_COPYING(MultiApplier);

public:
    /**
     * Operations sorted by timestamp in ascending order.
     */
    using Operations = std::vector<OplogEntry>;

    using OperationPtrs = std::vector<const OplogEntry*>;

    /**
     * Callback function to report final status of applying operations.
     */
    using CallbackFn = stdx::function<void(const Status&)>;

    /**
     * Type of function to to apply a single operation. In production, this function
     * would have the same outcome as calling SyncTail::syncApply() ('inSteadyStateReplication'
     * value will be embedded in the function implementation).
     */
    using ApplyOperationFn = stdx::function<Status(OperationPtrs*)>;

    using MultiApplyFn = stdx::function<StatusWith<OpTime>(
        OperationContext*, MultiApplier::Operations, MultiApplier::ApplyOperationFn)>;

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
                 const Operations& operations,
                 const ApplyOperationFn& applyOperation,
                 const MultiApplyFn& multiApply,
                 const CallbackFn& onCompletion);

    /**
     * Blocks while applier is active.
     */
    virtual ~MultiApplier();

    /**
     * Returns diagnostic information.
     */
    std::string getDiagnosticString() const;

    /**
     * Returns an informational string.
     */
    std::string toString() const;

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
    bool _isActive_inlock() const;

    /**
     * DB worker callback function - applies all operations.
     */
    void _callback(const executor::TaskExecutor::CallbackArgs& cbd);
    void _finishCallback(const Status& result);

    // Not owned by us.
    executor::TaskExecutor* _executor;

    Operations _operations;
    ApplyOperationFn _applyOperation;
    MultiApplyFn _multiApply;
    CallbackFn _onCompletion;

    // Protects member data of this MultiApplier.
    mutable stdx::mutex _mutex;

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
