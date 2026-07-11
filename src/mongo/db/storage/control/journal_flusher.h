// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/background.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"

#include <memory>
#include <mutex>
#include <string>

#include <boost/optional/optional.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {
/**
 * A periodic and signalable thread that flushes data to disk. Constructor parameter will dictate
 * whether to periodically flush or only on signal.
 *
 * This thread is helpful for two reasons:
 *  - Periodically flushing data to disk may protect users doing writes with {j: false} from losing
 *    a great deal of their data across a server crash.
 *  - Asynchronously grouping data flush requests reduces the total number of flushes executed,
 *    reducing i/o load on the system and improving write performance. This thread groups both the
 *    periodic flushes and immediate flush requests from the rest of the system.
 *
 * And incidentally helpful for another reason:
 *  - waitUntilDurable() calls update the replication JournalListener, so more frequent calls may be
 *    helpful to unblock replication related operations more quickly.
 */
class JournalFlusher final : public BackgroundJob {
public:
    /**
     * Setting 'disablePeriodicFlushes' to true will cause the JournalFlusher thread to only execute
     * a data flush upon explicit request: flushes will no longer be executed periodically in
     * addition. This is useful for storage engines that do not want frequent durability updates,
     * like engines without a journal where the cost of durability is high (using checkpoints
     * instead).
     */
    explicit JournalFlusher(bool disablePeriodicFlushes)
        : BackgroundJob(/*deleteSelf*/ false), _disablePeriodicFlushes(disablePeriodicFlushes) {}

    static JournalFlusher* get(ServiceContext* serviceCtx);
    static JournalFlusher* get(OperationContext* opCtx);
    static void set(ServiceContext* serviceCtx, std::unique_ptr<JournalFlusher> journalFlusher);

    std::string name() const override {
        return "JournalFlusher";
    }

    /**
     * Runs data flushes every 'storageGlobalParams.journalCommitIntervalMs' millis (unless
     * '_disablePeriodicFlushes' is set) or immediately if  waitForJournalFlush() is called.
     */
    void run() override;

    /**
     * Signals the thread to quit and then waits until it does. The given 'reason' is returned to
     * any operations that were waiting for the journal to flush.
     */
    void shutdown(const Status& reason);

    /**
     * Signals the thread to pause and then waits until it does.
     * Callers of waitForJournalFlush() will continue to be blocked while the journal flusher is
     * paused.
     */
    void pause();

    /**
     * Signals the thread to resume from a pause.
     */
    void resume();

    /**
     * Signals an immediate journal flush and waits for it to complete before returning.
     *
     * Retries internally on InterruptedDueToReplStateChange errors.
     * Will throw ErrorCodes::isShutdownError errors.
     *
     * Warning: Timestamped writes are not guaranteed to be persisted when this function is called
     * in parallel with replication rollback due to concurrent recoverToStableTimestamp(). But
     * untimestamped writes will be retained.
     */
    void waitForJournalFlush(Interruptible* interruptible = Interruptible::notInterruptible());

    /**
     * Signals an immediate journal flush and returns without waiting for completion.
     */
    void triggerJournalFlush();

    /**
     * Interrupts the journal flusher thread via its operation context with an
     * InterruptedDueToReplStateChange error.
     */
    void interruptJournalFlusherForReplStateChange();

private:
    // Journal flusher internal states.
    enum class States {
        Running,
        Paused,
        ShutDown,
    };

    /**
     * Signals an immediate journal flush under mutex and returns without waiting for completion.
     */
    void _triggerJournalFlush(WithLock lk);

    // Serializes setting/resetting _uniqueCtx and marking _uniqueCtx killed.
    mutable std::mutex _opCtxMutex;

    // Saves a reference to the flusher thread's operation context so it can be interrupted if the
    // flusher is active.
    boost::optional<ServiceContext::UniqueOperationContext> _uniqueCtx;

    // Protects the state below.
    mutable std::mutex _stateMutex;

    // Signaled to wake up the thread, if the thread is waiting or paused. The thread will check
    // whether _flushJournalNow, _needToPause, or _shuttingDown is set and flush, pause, or stop
    // accordingly.
    mutable stdx::condition_variable _flushJournalNowCV;

    // Facilitates waiting for journal flusher state change after waking up the journal flusher
    // thread.
    mutable stdx::condition_variable _stateChangeCV;
    States _state = States::Running;

    bool _flushJournalNow = false;
    bool _needToPause = false;
    bool _shuttingDown = false;
    Status _shutdownReason = Status::OK();

    // New callers get a future from nextSharedPromise. The JournalFlusher thread will swap that to
    // currentSharedPromise at the start of every round of flushing, and reset nextSharedPromise
    // with a new shared promise.
    std::unique_ptr<SharedPromise<void>> _currentSharedPromise =
        std::make_unique<SharedPromise<void>>();
    std::unique_ptr<SharedPromise<void>> _nextSharedPromise =
        std::make_unique<SharedPromise<void>>();

    // Controls whether to ignore the 'storageGlobalParams.journalCommitIntervalMs' setting. If set,
    // data flushes will only be executed upon explicit request, no longer periodically in addition
    // to upon request.
    bool _disablePeriodicFlushes;
};

}  // namespace mongo
