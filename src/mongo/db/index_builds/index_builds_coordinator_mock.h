// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/index_builds/index_builds_coordinator.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace [[MONGO_MOD_PUBLIC]] mongo {

class IndexBuildsCoordinatorMock : public IndexBuildsCoordinator {
public:
    void shutdown(OperationContext* opCtx) override;

    StatusWith<SharedSemiFuture<ReplIndexBuildState::IndexCatalogStats>> startIndexBuild(
        OperationContext* opCtx,
        const DatabaseName& dbName,
        const UUID& collectionUUID,
        const std::vector<IndexBuildInfo>& indexes,
        const UUID& buildUUID,
        IndexBuildOptions indexBuildOptions) override;

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
};
}  // namespace mongo
