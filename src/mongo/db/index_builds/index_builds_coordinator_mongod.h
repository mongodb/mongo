// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/database_name.h"
#include "mongo/db/index_builds/commit_quorum_options.h"
#include "mongo/db/index_builds/index_builds_coordinator.h"
#include "mongo/db/index_builds/repl_index_build_state.h"
#include "mongo/db/index_builds/resumable_index_builds_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/synchronized_value.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <mutex>
#include <string_view>
#include <vector>

#include <boost/optional/optional.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * This implementation of the IndexBuildsCoordinator is for replica set member and standalone nodes.
 * It has a thread pool that runs index builds asynchronously, returning results via the base class'
 * established Futures and Promises protocol. Replica set member state is tracked and affects how
 * index builds are run: their role in the cross replica set index builds.
 *
 * The IndexBuildsCoordinator is instantiated on the ServiceContext as a decoration, and is
 * accessible via the ServiceContext.
 */
class IndexBuildsCoordinatorMongod : public IndexBuildsCoordinator {
    IndexBuildsCoordinatorMongod(const IndexBuildsCoordinatorMongod&) = delete;
    IndexBuildsCoordinatorMongod& operator=(const IndexBuildsCoordinatorMongod&) = delete;

public:
    /**
     * Sets up the thread pool.
     */
    IndexBuildsCoordinatorMongod();

    /**
     * Shuts down the thread pool, signals interrupt to all index builds, then waits for all of the
     * threads to finish.
     */
    void shutdown(OperationContext* opCtx) override;

    /**
     * Sets up the in-memory and persisted state of the index build, then passes the build off to an
     * asynchronous thread to run. A Future is returned so that the user can await the asynchronous
     * build result.
     *
     * Returns an error status if there are any errors setting up the index build.
     */
    StatusWith<SharedSemiFuture<ReplIndexBuildState::IndexCatalogStats>> startIndexBuild(
        OperationContext* opCtx,
        const DatabaseName& dbName,
        const UUID& collectionUUID,
        const std::vector<IndexBuildInfo>& indexes,
        const UUID& buildUUID,
        IndexBuildOptions indexBuildOptions) override;

    /**
     * Reconstructs the in-memory state of the index build, then passes the build off to an
     * asynchronous thread to run. A Future is returned so that the user can await the asynchronous
     * build result.
     */
    StatusWith<SharedSemiFuture<ReplIndexBuildState::IndexCatalogStats>> resumeIndexBuild(
        OperationContext* opCtx,
        const DatabaseName& dbName,
        const UUID& collectionUUID,
        const std::vector<IndexBuildInfo>& indexes,
        const UUID& buildUUID,
        const ResumeIndexInfo& resumeInfo,
        IndexBuildOptions indexBuildOptions) override;

    Status voteAbortIndexBuild(OperationContext* opCtx,
                               const UUID& buildUUID,
                               const HostAndPort& hostAndPort,
                               std::string_view reason) override;

    Status voteCommitIndexBuild(OperationContext* opCtx,
                                const UUID& buildUUID,
                                const HostAndPort& hostAndPort) override;

    Status setCommitQuorum(OperationContext* opCtx,
                           const NamespaceString& nss,
                           const std::vector<std::string_view>& indexNames,
                           const CommitQuorumOptions& newCommitQuorum) override;

private:
    /**
     * Signals index builder to commit.
     */
    void _sendCommitQuorumSatisfiedSignal(OperationContext* opCtx,
                                          std::shared_ptr<ReplIndexBuildState> replState);


    bool _signalIfCommitQuorumIsSatisfied(OperationContext* opCtx,
                                          std::shared_ptr<ReplIndexBuildState> replState) override;


    bool _signalIfCommitQuorumNotEnabled(OperationContext* opCtx,
                                         std::shared_ptr<ReplIndexBuildState> replState) override;

    void _signalPrimaryForAbortAndWaitForExternalAbort(OperationContext* opCtx,
                                                       ReplIndexBuildState* replState) override;

    void _signalPrimaryForCommitReadiness(OperationContext* opCtx,
                                          std::shared_ptr<ReplIndexBuildState> replState) override;

    IndexBuildAction _waitForNextIndexBuildAction(
        OperationContext* opCtx, std::shared_ptr<ReplIndexBuildState> replState) override;

    void _waitForNextIndexBuildActionAndCommit(OperationContext* opCtx,
                                               std::shared_ptr<ReplIndexBuildState> replState,
                                               const IndexBuildOptions& indexBuildOptions) override;

    StatusWith<SharedSemiFuture<ReplIndexBuildState::IndexCatalogStats>> _startIndexBuild(
        OperationContext* opCtx,
        const DatabaseName& dbName,
        const UUID& collectionUUID,
        const std::vector<IndexBuildInfo>& indexes,
        const UUID& buildUUID,
        IndexBuildOptions indexBuildOptions,
        const boost::optional<ResumeIndexInfo>& resumeInfo);

    /**
     * Returns the thread pool, lazily creating and starting it if uninitialized.
     * Primary driven index builds should be interrupted by stepdown, while two phase index builds
     * are built simultaneously on all nodes.
     */
    ThreadPool& _ensureThreadPool(bool killableByStepdown);

    // Thread pool on which index builds are run.
    synchronized_value<std::unique_ptr<ThreadPool>> _threadPool;

    // Manages _numActiveIndexBuilds and _indexBuildFinished.
    mutable std::mutex _throttlingMutex;

    // Protected by _throttlingMutex.
    int _numActiveIndexBuilds = 0;

    // Condition signalled to indicate that an index build thread finished executing.
    stdx::condition_variable _indexBuildFinished;
};

}  // namespace mongo
