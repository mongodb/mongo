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

#include "mongo/db/index_builds/index_builds_coordinator_mock.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/db/index_builds/repl_index_build_state.h"

namespace mongo {

void IndexBuildsCoordinatorMock::shutdown(OperationContext* opCtx) {}

StatusWith<SharedSemiFuture<ReplIndexBuildState::IndexCatalogStats>>
IndexBuildsCoordinatorMock::startIndexBuild(OperationContext* opCtx,
                                            const DatabaseName& dbName,
                                            const UUID& collectionUUID,
                                            const std::vector<IndexBuildInfo>& indexes,
                                            const UUID& buildUUID,
                                            IndexBuildProtocol protocol,
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
                                             const ResumeIndexInfo& resumeInfo) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

Status IndexBuildsCoordinatorMock::voteAbortIndexBuild(OperationContext* opCtx,
                                                       const UUID& buildUUID,
                                                       const HostAndPort& hostAndPort,
                                                       StringData reason) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

Status IndexBuildsCoordinatorMock::voteCommitIndexBuild(OperationContext* opCtx,
                                                        const UUID& buildUUID,
                                                        const HostAndPort& hostAndPort) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

Status IndexBuildsCoordinatorMock::setCommitQuorum(OperationContext* opCtx,
                                                   const NamespaceString& nss,
                                                   const std::vector<StringData>& indexNames,
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
