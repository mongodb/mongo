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


#include "mongo/platform/basic.h"

#include "mongo/embedded/index_builds_coordinator_embedded.h"

#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {

void IndexBuildsCoordinatorEmbedded::shutdown(OperationContext* opCtx) {}

StatusWith<SharedSemiFuture<ReplIndexBuildState::IndexCatalogStats>>
IndexBuildsCoordinatorEmbedded::startIndexBuild(OperationContext* opCtx,
                                                const DatabaseName& dbName,
                                                const UUID& collectionUUID,
                                                const std::vector<BSONObj>& specs,
                                                const UUID& buildUUID,
                                                IndexBuildProtocol protocol,
                                                IndexBuildOptions indexBuildOptions) {
    invariant(!opCtx->lockState()->isLocked());

    auto statusWithOptionalResult =
        _filterSpecsAndRegisterBuild(opCtx, dbName, collectionUUID, specs, buildUUID, protocol);
    if (!statusWithOptionalResult.isOK()) {
        return statusWithOptionalResult.getStatus();
    }

    if (statusWithOptionalResult.getValue()) {
        invariant(statusWithOptionalResult.getValue()->isReady());
        // The requested index (specs) are already built or are being built. Return success early
        // (this is v4.0 behavior compatible).
        return statusWithOptionalResult.getValue().value();
    }

    auto status = _setUpIndexBuild(opCtx, buildUUID, Timestamp(), indexBuildOptions);
    if (!status.isOK()) {
        return status;
    }
    _runIndexBuild(opCtx, buildUUID, indexBuildOptions, boost::none /* resumeInfo */);

    auto replState = invariant(_getIndexBuild(buildUUID));
    return replState->sharedPromise.getFuture();
}

StatusWith<SharedSemiFuture<ReplIndexBuildState::IndexCatalogStats>>
IndexBuildsCoordinatorEmbedded::resumeIndexBuild(OperationContext* opCtx,
                                                 const DatabaseName& dbName,
                                                 const UUID& collectionUUID,
                                                 const std::vector<BSONObj>& specs,
                                                 const UUID& buildUUID,
                                                 const ResumeIndexInfo& resumeInfo) {
    MONGO_UNREACHABLE;
}

void IndexBuildsCoordinatorEmbedded::_signalPrimaryForAbortAndWaitForExternalAbort(
    OperationContext* opCtx, ReplIndexBuildState* replState, const Status& abortStatus) {}

void IndexBuildsCoordinatorEmbedded::_signalPrimaryForCommitReadiness(
    OperationContext* opCtx, std::shared_ptr<ReplIndexBuildState> replState) {}

void IndexBuildsCoordinatorEmbedded::_waitForNextIndexBuildActionAndCommit(
    OperationContext* opCtx,
    std::shared_ptr<ReplIndexBuildState> replState,
    const IndexBuildOptions& indexBuildOptions) {}

Status IndexBuildsCoordinatorEmbedded::voteAbortIndexBuild(OperationContext* opCtx,
                                                           const UUID& buildUUID,
                                                           const HostAndPort& hostAndPort,
                                                           const StringData& reason) {
    MONGO_UNREACHABLE;
}

Status IndexBuildsCoordinatorEmbedded::voteCommitIndexBuild(OperationContext* opCtx,
                                                            const UUID& buildUUID,
                                                            const HostAndPort& hostAndPort) {
    MONGO_UNREACHABLE;
}

Status IndexBuildsCoordinatorEmbedded::setCommitQuorum(OperationContext* opCtx,
                                                       const NamespaceString& nss,
                                                       const std::vector<StringData>& indexNames,
                                                       const CommitQuorumOptions& newCommitQuorum) {
    MONGO_UNREACHABLE;
}

bool IndexBuildsCoordinatorEmbedded::_signalIfCommitQuorumIsSatisfied(
    OperationContext* opCtx, std::shared_ptr<ReplIndexBuildState> replState) {
    MONGO_UNREACHABLE;
}

bool IndexBuildsCoordinatorEmbedded::_signalIfCommitQuorumNotEnabled(
    OperationContext* opCtx, std::shared_ptr<ReplIndexBuildState> replState) {
    MONGO_UNREACHABLE;
}

}  // namespace mongo
