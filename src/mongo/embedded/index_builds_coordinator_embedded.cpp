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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/embedded/index_builds_coordinator_embedded.h"

#include "mongo/db/catalog/uuid_catalog.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

void IndexBuildsCoordinatorEmbedded::shutdown() {}

StatusWith<SharedSemiFuture<ReplIndexBuildState::IndexCatalogStats>>
IndexBuildsCoordinatorEmbedded::startIndexBuild(OperationContext* opCtx,
                                                CollectionUUID collectionUUID,
                                                const std::vector<BSONObj>& specs,
                                                const UUID& buildUUID,
                                                IndexBuildProtocol protocol) {
    std::vector<std::string> indexNames;
    for (auto& spec : specs) {
        std::string name = spec.getStringField(IndexDescriptor::kIndexNameFieldName);
        if (name.empty()) {
            return Status(
                ErrorCodes::CannotCreateIndex,
                str::stream() << "Cannot create an index for a spec '" << spec
                              << "' without a non-empty string value for the 'name' field");
        }
        indexNames.push_back(name);
    }

    auto nss = UUIDCatalog::get(opCtx).lookupNSSByUUID(collectionUUID);
    auto dbName = nss.db().toString();
    auto replIndexBuildState = std::make_shared<ReplIndexBuildState>(
        buildUUID, collectionUUID, dbName, indexNames, specs, protocol);

    Status status = _registerIndexBuild(opCtx, replIndexBuildState);
    if (!status.isOK()) {
        return status;
    }

    _runIndexBuild(opCtx, buildUUID);

    return replIndexBuildState->sharedPromise.getFuture();
}

Status IndexBuildsCoordinatorEmbedded::commitIndexBuild(OperationContext* opCtx,
                                                        const std::vector<BSONObj>& specs,
                                                        const UUID& buildUUID) {
    MONGO_UNREACHABLE;
}

void IndexBuildsCoordinatorEmbedded::signalChangeToPrimaryMode() {
    MONGO_UNREACHABLE;
}

void IndexBuildsCoordinatorEmbedded::signalChangeToSecondaryMode() {
    MONGO_UNREACHABLE;
}

void IndexBuildsCoordinatorEmbedded::signalChangeToInitialSyncMode() {
    MONGO_UNREACHABLE;
}

Status IndexBuildsCoordinatorEmbedded::voteCommitIndexBuild(const UUID& buildUUID,
                                                            const HostAndPort& hostAndPort) {
    MONGO_UNREACHABLE;
}

Status IndexBuildsCoordinatorEmbedded::setCommitQuorum(const NamespaceString& nss,
                                                       const std::vector<StringData>& indexNames,
                                                       const CommitQuorumOptions& newCommitQuorum) {
    MONGO_UNREACHABLE;
}

}  // namespace mongo
