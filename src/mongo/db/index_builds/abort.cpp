// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/index_builds/abort.h"

#include "mongo/db/index_builds/index_builds_coordinator.h"
#include "mongo/db/index_builds/primary_driven/registry.h"
#include "mongo/db/index_builds/primary_driven/util.h"
#include "mongo/db/shard_role/lock_manager/exception_util.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"

#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kIndex

namespace mongo::index_builds {
namespace {

/**
 * Aborts the matching primary-driven index builds that are registered but have not yet been
 * resumed. Returns whether anything was aborted.
 */
bool abortUnresumedPrimaryDrivenIndexBuilds(
    OperationContext* opCtx,
    const std::function<bool(const primary_driven::Registry::Entry&)>& match,
    const std::string& reason) {
    bool abortedAny = false;
    for (auto&& [buildUUID, build] : primary_driven::registry(opCtx->getServiceContext()).all()) {
        if (!match(build)) {
            continue;
        }

        LOGV2(13333700,
              "Index build: aborting primary-driven index build that has not yet been resumed",
              "buildUUID"_attr = buildUUID,
              "collectionUUID"_attr = build.collectionUUID,
              "reason"_attr = reason);

        writeConflictRetry(opCtx,
                           "abortUnresumedPrimaryDrivenIndexBuild",
                           NamespaceStringOrUUID{build.dbName, build.collectionUUID},
                           [&] {
                               uassertStatusOK(primary_driven::abort(
                                   opCtx,
                                   build.dbName,
                                   build.collectionUUID,
                                   buildUUID,
                                   build.indexes,
                                   build.indexBuildIdent,
                                   Status{ErrorCodes::IndexBuildAborted, reason}));
                           });
        abortedAny = true;
    }
    return abortedAny;
}

}  // namespace

void abort(OperationContext* opCtx,
           const NamespaceString& ns,
           const UUID& collectionUUID,
           const std::string& reason,
           const std::function<void()>& unlock,
           const std::function<bool()>& lock) {
    auto& coordinator = *IndexBuildsCoordinator::get(opCtx);

    // Every path out of this loop either holds the caller's locks and has seen no index builds
    // while holding them, or the caller has declined to re-acquire. Builds can start whenever the
    // locks are yielded, so each yield has to be followed by another lock.
    while (true) {
        // Aborting a build the coordinator is running means signalling its builder thread and
        // waiting for it to exit, which it cannot do while we hold the collection lock.
        if (coordinator.inProgForCollection(collectionUUID)) {
            unlock();
            coordinator.abortCollectionIndexBuilds(opCtx, ns, collectionUUID, reason);
            if (!lock()) {
                return;
            }
            continue;
        }

        // The coordinator has no builds for this collection, so any primary-driven builds still
        // registered have not yet been resumed.
        if (!abortUnresumedPrimaryDrivenIndexBuilds(
                opCtx,
                [&](const primary_driven::Registry::Entry& build) {
                    return build.collectionUUID == collectionUUID;
                },
                reason)) {
            return;
        }

        // That abort wrote to the catalog, making the caller's acquisition of the old instance
        // invalid. Unlock and re-lock to refresh it.
        unlock();
        if (!lock()) {
            return;
        }
    }
}

void abort(OperationContext* opCtx,
           const DatabaseName& dbName,
           const std::string& reason,
           const std::function<void()>& unlock,
           const std::function<bool()>& lock) {
    auto& coordinator = *IndexBuildsCoordinator::get(opCtx);

    // Every path out of this loop either holds the caller's locks and has seen no index builds
    // while holding them, or the caller has declined to re-acquire. Builds can start whenever the
    // locks are yielded, so each yield has to be followed by another lock.
    while (true) {
        // Aborting a build the coordinator is running means signalling its builder thread and
        // waiting for it to exit, which it cannot do while we hold the database lock.
        if (coordinator.inProgForDb(dbName)) {
            unlock();
            coordinator.abortDatabaseIndexBuilds(opCtx, dbName, reason);
            if (!lock()) {
                return;
            }
            continue;
        }

        // The coordinator has no builds for this database, so any primary-driven builds still
        // registered have not yet been resumed.
        if (!abortUnresumedPrimaryDrivenIndexBuilds(
                opCtx,
                [&](const primary_driven::Registry::Entry& build) {
                    return build.dbName == dbName;
                },
                reason)) {
            return;
        }

        // That abort wrote to the catalog, making the caller's acquisitions of the old collection
        // instances invalid. Unlock and re-lock to refresh them.
        unlock();
        if (!lock()) {
            return;
        }
    }
}

}  // namespace mongo::index_builds
