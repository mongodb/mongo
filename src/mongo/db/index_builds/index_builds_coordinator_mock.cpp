// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/index_builds/index_builds_coordinator_mock.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/db/index_builds/repl_index_build_state.h"

#include <string_view>

namespace mongo {

void IndexBuildsCoordinatorMock::shutdown(OperationContext* opCtx) {}

StatusWith<SharedSemiFuture<ReplIndexBuildState::IndexCatalogStats>>
IndexBuildsCoordinatorMock::startIndexBuild(OperationContext* opCtx,
                                            const DatabaseName& dbName,
                                            const UUID& collectionUUID,
                                            const std::vector<IndexBuildInfo>& indexes,
                                            const UUID& buildUUID,
                                            IndexBuildOptions indexBuildOptions) {
    ReplIndexBuildState::IndexCatalogStats stats;
    return Future<ReplIndexBuildState::IndexCatalogStats>::makeReady(stats);
}

StatusWith<SharedSemiFuture<ReplIndexBuildState::IndexCatalogStats>>
IndexBuildsCoordinatorMock::resumeIndexBuild(OperationContext* opCtx,
                                             const DatabaseName& dbName,
                                             const UUID& collectionUUID,
                                             const std::vector<IndexBuildInfo>& indexes,
                                             const UUID& buildUUID,
                                             const ResumeIndexInfo& resumeInfo,
                                             IndexBuildOptions indexBuildOptions) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

Status IndexBuildsCoordinatorMock::voteAbortIndexBuild(OperationContext* opCtx,
                                                       const UUID& buildUUID,
                                                       const HostAndPort& hostAndPort,
                                                       std::string_view reason) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

Status IndexBuildsCoordinatorMock::voteCommitIndexBuild(OperationContext* opCtx,
                                                        const UUID& buildUUID,
                                                        const HostAndPort& hostAndPort) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

Status IndexBuildsCoordinatorMock::setCommitQuorum(OperationContext* opCtx,
                                                   const NamespaceString& nss,
                                                   const std::vector<std::string_view>& indexNames,
                                                   const CommitQuorumOptions& newCommitQuorum) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

bool IndexBuildsCoordinatorMock::_signalIfCommitQuorumIsSatisfied(
    OperationContext* opCtx, std::shared_ptr<ReplIndexBuildState> replState) {
    return true;
}


bool IndexBuildsCoordinatorMock::_signalIfCommitQuorumNotEnabled(
    OperationContext* opCtx, std::shared_ptr<ReplIndexBuildState> replState) {
    return true;
}

void IndexBuildsCoordinatorMock::_signalPrimaryForAbortAndWaitForExternalAbort(
    OperationContext* opCtx, ReplIndexBuildState* replState) {}

void IndexBuildsCoordinatorMock::_signalPrimaryForCommitReadiness(
    OperationContext* opCtx, std::shared_ptr<ReplIndexBuildState> replState) {}

IndexBuildAction IndexBuildsCoordinatorMock::_waitForNextIndexBuildAction(
    OperationContext* opCtx, std::shared_ptr<ReplIndexBuildState> replState) {
    return IndexBuildAction::kNoAction;
}

void IndexBuildsCoordinatorMock::_waitForNextIndexBuildActionAndCommit(
    OperationContext* opCtx,
    std::shared_ptr<ReplIndexBuildState> replState,
    const IndexBuildOptions& indexBuildOptions) {}

}  // namespace mongo
