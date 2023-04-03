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

namespace mongo {

class OperationContext;
class ServiceContext;

/**
 * This implementation of the IndexBuildsCoordinator is for embedded server nodes. Nothing
 * is run asynchronously and no network calls are made. Index builds are run without awaiting cross
 * replica set communications.
 *
 * The IndexBuildsCoordinator is instantiated on the ServiceContext as a decoration, and is
 * accessible via the ServiceContext.
 */
class IndexBuildsCoordinatorEmbedded : public IndexBuildsCoordinator {
    IndexBuildsCoordinatorEmbedded(const IndexBuildsCoordinatorEmbedded&) = delete;
    IndexBuildsCoordinatorEmbedded& operator=(const IndexBuildsCoordinatorEmbedded&) = delete;

public:
    IndexBuildsCoordinatorEmbedded() = default;

    /**
     * Does nothing.
     */
    void shutdown(OperationContext* opCtx) override;

    StatusWith<SharedSemiFuture<ReplIndexBuildState::IndexCatalogStats>> startIndexBuild(
        OperationContext* opCtx,
        const DatabaseName& dbName,
        const UUID& collectionUUID,
        const std::vector<BSONObj>& specs,
        const UUID& buildUUID,
        IndexBuildProtocol protocol,
        IndexBuildOptions indexBuildOptions) override;

    StatusWith<SharedSemiFuture<ReplIndexBuildState::IndexCatalogStats>> resumeIndexBuild(
        OperationContext* opCtx,
        const DatabaseName& dbName,
        const UUID& collectionUUID,
        const std::vector<BSONObj>& specs,
        const UUID& buildUUID,
        const ResumeIndexInfo& resumeInfo) override;

    /**
     * None of the following functions should ever be called on an embedded server node.
     */
    Status voteAbortIndexBuild(OperationContext* opCtx,
                               const UUID& buildUUID,
                               const HostAndPort& hostAndPort,
                               const StringData& reason) override;
    Status voteCommitIndexBuild(OperationContext* opCtx,
                                const UUID& buildUUID,
                                const HostAndPort& hostAndPort) override;
    Status setCommitQuorum(OperationContext* opCtx,
                           const NamespaceString& nss,
                           const std::vector<StringData>& indexNames,
                           const CommitQuorumOptions& newCommitQuorum) override;

private:
    bool _signalIfCommitQuorumIsSatisfied(OperationContext* opCtx,
                                          std::shared_ptr<ReplIndexBuildState> replState) override;

    bool _signalIfCommitQuorumNotEnabled(OperationContext* opCtx,
                                         std::shared_ptr<ReplIndexBuildState> replState) override;

    void _signalPrimaryForAbortAndWaitForExternalAbort(OperationContext* opCtx,
                                                       ReplIndexBuildState* replState,
                                                       const Status& abortStatus) override;

    void _signalPrimaryForCommitReadiness(OperationContext* opCtx,
                                          std::shared_ptr<ReplIndexBuildState> replState) override;

    IndexBuildAction _drainSideWritesUntilNextActionIsAvailable(
        OperationContext* opCtx, std::shared_ptr<ReplIndexBuildState> replState) {
        return {};
    };

    void _waitForNextIndexBuildActionAndCommit(OperationContext* opCtx,
                                               std::shared_ptr<ReplIndexBuildState> replState,
                                               const IndexBuildOptions& indexBuildOptions) override;
};

}  // namespace mongo
