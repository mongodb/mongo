/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/index_builds_coordinator.h"
#include "mongo/util/concurrency/thread_pool.h"

namespace mongo {

class OperationContext;
class ServiceContext;

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
    void shutdown() override;

    /**
     * Sets up the in-memory and persisted state of the index build, then passes the build off to an
     * asynchronous thread to run. A Future is returned so that the user can await the asynchronous
     * build result.
     *
     * Returns an error status if there are any errors setting up the index build.
     */
    StatusWith<SharedSemiFuture<ReplIndexBuildState::IndexCatalogStats>> startIndexBuild(
        OperationContext* opCtx,
        StringData dbName,
        CollectionUUID collectionUUID,
        const std::vector<BSONObj>& specs,
        const UUID& buildUUID,
        IndexBuildProtocol protocol,
        IndexBuildOptions indexBuildOptions) override;

    Status voteCommitIndexBuild(const UUID& buildUUID, const HostAndPort& hostAndPort) override;

    Status setCommitQuorum(OperationContext* opCtx,
                           const NamespaceString& nss,
                           const std::vector<StringData>& indexNames,
                           const CommitQuorumOptions& newCommitQuorum) override;

private:
    /**
     * Keeps track of the relevant replica set member states. Index builds are managed differently
     * depending on the state of the replica set member.
     *
     * These states follow the replica set member states, as maintained by MemberState in the
     * ReplicationCoordinator. If not in Primary or InitialSync modes, then the default will be
     * Secondary, with the expectation that a replica set member must always transition to Secondary
     * before Primary.
     */
    enum class ReplState { Primary, Secondary, InitialSync };

    /**
     * TODO: not yet implemented.
     */
    Status _finishScanningPhase();

    /**
     * TODO: not yet implemented.
     */
    Status _finishVerificationPhase();

    /**
     * TODO: not yet implemented.
     */
    Status _finishCommitPhase();

    /**
     * TODO: not yet implemented.
     */
    StatusWith<bool> _checkCommitQuorum(const BSONObj& commitQuorum,
                                        const std::vector<HostAndPort>& confirmedMembers);

    /**
     * TODO: not yet implemented.
     */
    void _refreshReplStateFromPersisted(OperationContext* opCtx, const UUID& buildUUID);

    // Thread pool on which index builds are run.
    ThreadPool _threadPool;
};

}  // namespace mongo
