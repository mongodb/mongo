// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/repl/clang_checked/checked_mutex.h"
#include "mongo/db/repl/clang_checked/mutex.h"
#include "mongo/db/repl/clang_checked/thread_safety_annotations.h"
#include "mongo/executor/task_executor.h"
#include "mongo/util/net/hostandport.h"

#include <functional>
#include <mutex>
#include <vector>

#include <boost/optional.hpp>

namespace mongo {
namespace repl {

/**
 * A node that restarts rolls back to its last stable checkpoint. If it is acting as a sync source
 * for an initial sync in progress, writes between that checkpoint and the syncing node's
 * beginApplyingTimestamp can be lost from the syncing node's view: cloning may already have read
 * past them, and oplog replay only covers entries at or after beginApplyingTimestamp. Unclean
 * shutdown is caught by the rollback ID, but clean shutdown is not, so every node records its clean
 * shutdowns in local.system.cleanShutdownLog and an initial syncing node consults its sync source's
 * history.
 *
 * These helpers are shared by the DBClient call sites in the cloners and the Fetcher call sites in
 * InitialSyncer so that both reach the same verdict from the same queries.
 */

/**
 * The baseline recorded when the sync source reports no clean shutdown, and the _id of the sentinel
 * document a node seeds its own collection with on startup so that it always has something to
 * report. Both meanings are the same value on purpose: a sync source that has never cleanly shut
 * down returns the sentinel, and one too old to have the collection at all returns an empty batch,
 * and neither should match any check.
 *
 * The value sits one below the _id of a node's first real clean shutdown, so that first shutdown is
 * caught rather than mistaken for the baseline itself.
 */
constexpr long long kNoCleanShutdownId = -1;

/**
 * Builds the find issued once at the start of an initial sync attempt to establish the baseline:
 * the most recently recorded clean shutdown on the sync source.
 */
BSONObj makeCleanShutdownBaselineFindCmd();

/**
 * Builds the find issued at each check point: the oldest clean shutdown the sync source recorded
 * after 'baseCleanShutdownId'. Only the first shutdown after the baseline can matter, because a
 * later one cannot reopen a window that the first one already closed.
 */
BSONObj makeCleanShutdownCheckFindCmd(long long baseCleanShutdownId);

/**
 * Interprets the response to makeCleanShutdownBaselineFindCmd, returning the _id to use as the
 * baseline for the rest of the attempt, or kNoCleanShutdownId if the sync source returned nothing.
 */
StatusWith<long long> parseCleanShutdownBaseline(const std::vector<BSONObj>& docs);

/**
 * Renders the verdict for one check, given the single document returned by
 * makeCleanShutdownCheckFindCmd, or boost::none if it matched nothing.
 *
 * Returns OK if initial sync may proceed, or InitialSyncFailure if the sync source restarted in a
 * way that could have lost writes this attempt has already cloned.
 */
Status checkCleanShutdownResult(const boost::optional<BSONObj>& firstDocAfterBase,
                                long long baseCleanShutdownId,
                                Timestamp beginApplyingTimestamp,
                                const HostAndPort& syncSource);

/**
 * Extracts the documents from the cursor of a find response, so that a raw remote command response
 * can be handed to the functions above.
 */
StatusWith<std::vector<BSONObj>> extractFirstBatch(const BSONObj& response);

/**
 * Maintains a sync source and the baseline clean shutdown _id observed on it, and checks whether it
 * has cleanly shut down since that baseline was taken. This mirrors RollbackChecker, which does the
 * same for the rollback ID, and is used the same way:
 *
 * 1) Create one per initial sync attempt, passing an executor and the sync source. If the sync
 *    source changes, make a new one - a baseline is only meaningful against the node it came from.
 * 2) Call reset() to record the baseline.
 * 3) Call checkForCleanShutdown() to find out whether the sync source has since restarted in a way
 *    that could have rolled back writes the attempt already cloned.
 */
class CleanShutdownChecker {
    CleanShutdownChecker(const CleanShutdownChecker&) = delete;
    CleanShutdownChecker& operator=(const CleanShutdownChecker&) = delete;

public:
    using CallbackFn = std::function<void(const Status& status)>;
    using RemoteCommandCallbackFn = executor::TaskExecutor::RemoteCommandCallbackFn;
    using CallbackHandle = executor::TaskExecutor::CallbackHandle;

    CleanShutdownChecker(executor::TaskExecutor* executor, HostAndPort syncSource);

    virtual ~CleanShutdownChecker();

    /**
     * Records the sync source's most recent clean shutdown _id as the baseline, then calls
     * 'nextAction' with the status of the command.
     */
    StatusWith<CallbackHandle> reset(const CallbackFn& nextAction);

    /**
     * Calls 'nextAction' with OK if initial sync may proceed, or InitialSyncFailure if the sync
     * source cleanly shut down since reset() in a way that could have lost writes older than
     * 'beginApplyingTimestamp'. Checking does not move the baseline ID.
     *
     * Once a call has actually observed a shutdown and judged it safe, later calls skip the query
     * and call 'nextAction' with OK directly: a later shutdown cannot reopen a window the checked
     * one already closed, so there is nothing left to learn, and re-querying risks a false failure
     * if the capped collection has since truncated away the document that proved it.
     */
    StatusWith<CallbackHandle> checkForCleanShutdown(Timestamp beginApplyingTimestamp,
                                                     const CallbackFn& nextAction);

    /**
     * Returns the current baseline, or kNoCleanShutdownId if reset() found no recorded shutdown.
     */
    long long getBaseCleanShutdownId();

private:
    StatusWith<CallbackHandle> _scheduleFind(const BSONObj& findCmd,
                                             const RemoteCommandCallbackFn& nextAction);

    executor::TaskExecutor* _executor;
    HostAndPort _syncSource;

    mutable clang_checked::CheckedMutex<std::mutex> _mutex;
    long long _baseCleanShutdownId MONGO_LOCKING_GUARDED_BY(_mutex) = kNoCleanShutdownId;
    bool _verifiedSafe MONGO_LOCKING_GUARDED_BY(_mutex) = false;
};

}  // namespace repl
}  // namespace mongo
