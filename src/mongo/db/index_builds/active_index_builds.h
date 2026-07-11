// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/database_name.h"
#include "mongo/db/index_builds/index_builds_manager.h"
#include "mongo/db/index_builds/repl_index_build_state.h"
#include "mongo/db/index_builds/resumable_index_builds_gen.h"
#include "mongo/db/operation_context.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/interruptible.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>


namespace mongo {

enum class IndexBuildOutcome {
    kSuccess,
    kFailure,
    // The index build was interrupted before it could succeed or fail on this node (e.g. by a
    // stepdown) and will be resumed.
    kToBeResumed,
};

/**
 * This is a helper class used by IndexBuildsCoordinator to safely manage the data structures
 * that keep track of active index builds. It is owned by IndexBuildsCoordinator, and should
 * only ever be used inside it.
 */
class ActiveIndexBuilds {

public:
    /**
     * Invariants that there are no index builds in-progress.
     */
    ~ActiveIndexBuilds();

    /**
     * Waits for all index builds to stop after they have been interrupted during shutdown.
     * Leaves the index builds in a recoverable state.
     *
     * This should only be called when certain the server will not start any new index builds --
     * i.e. when the server is not accepting user requests and no internal operations are
     * concurrently starting new index builds.
     */
    void waitForAllIndexBuildsToStopForShutdown();

    /**
     * The following functions all have equivalent definitions in IndexBuildsCoordinator. The
     * IndexBuildsCoordinator functions forward to these functions. For descriptions of what they
     * do, see IndexBuildsCoordinator.
     */
    void waitForAllIndexBuildsToStop(Interruptible* opCtx);

    void assertNoIndexBuildInProgress() const;

    void waitUntilAnIndexBuildFinishes(OperationContext* opCtx, Date_t deadline);

    void sleepIndexBuilds_forTestOnly(bool sleep);

    void verifyNoIndexBuilds_forTestOnly() const;

    StatusWith<std::shared_ptr<ReplIndexBuildState>> getIndexBuild(const UUID& buildUUID) const;

    std::vector<std::shared_ptr<ReplIndexBuildState>> getAllIndexBuilds() const;

    void awaitNoIndexBuildInProgressForCollection(OperationContext* opCtx,
                                                  const UUID& collectionUUID,
                                                  IndexBuildProtocol protocol);

    void awaitNoIndexBuildInProgressForCollection(OperationContext* opCtx,
                                                  const UUID& collectionUUID);

    void awaitNoBgOpInProgForDb(OperationContext* opCtx, const DatabaseName& dbName);

    /**
     * Unregisters the index build.
     */
    void unregisterIndexBuild(IndexBuildsManager* indexBuildsManager,
                              std::shared_ptr<ReplIndexBuildState> replIndexBuildState,
                              IndexBuildOutcome outcome);

    void incrementResumeSucceeded(IndexBuildPhaseEnum phase);
    void incrementResumeFailed();

    /**
     * Returns a list of index builds matching the criteria 'indexBuildFilter'.
     */
    using IndexBuildFilterFn = std::function<bool(const ReplIndexBuildState& replState)>;
    std::vector<std::shared_ptr<ReplIndexBuildState>> filterIndexBuilds(
        IndexBuildFilterFn indexBuildFilter) const;

    /**
     * Registers an index build so that the rest of the system can discover it.
     */
    Status registerIndexBuild(std::shared_ptr<ReplIndexBuildState> replIndexBuildState);

    /**
     * Get the number of in-progress index builds.
     */
    size_t getActiveIndexBuildsCount() const;

    /**
     * Provides passthrough access to ReplIndexBuildState for index build info.
     * Does nothing if build UUID does not refer to an active index build.
     */
    void appendBuildInfo(const UUID& buildUUID, BSONObjBuilder* builder) const;

    /**
     * When _sleepForTest is true, this function will sleep for 100ms and then check the value
     * of _sleepForTest again.
     */
    void sleepIfNecessary_forTestOnly() const;

private:
    /**
     * Helper function for filterIndexBuilds. This function is necessary because some callers
     * already hold the mutex before calling this function.
     */
    std::vector<std::shared_ptr<ReplIndexBuildState>> _filterIndexBuilds_inlock(
        WithLock lk, IndexBuildFilterFn indexBuildFilter) const;

    // Manages all of the below state
    mutable std::mutex _mutex;

    // Build UUID to index build information
    stdx::unordered_map<UUID, std::shared_ptr<ReplIndexBuildState>, UUID::Hash> _allIndexBuilds;

    // Waiters are notified whenever _allIndexBuilds has something added or removed.
    stdx::condition_variable _indexBuildsCondVar;

    // Generation counter of completed index builds. Used in conjuction with the condition
    // variable to receive notifications when an index build completes.
    uint32_t _indexBuildsCompletedGen = 0;

    bool _sleepForTest = false;
};
}  // namespace mongo
