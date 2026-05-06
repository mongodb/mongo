/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/base/status.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/profile_common.h"
#include "mongo/db/commands/profile_gen.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/profile_collection.h"
#include "mongo/db/profile_filter_impl.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/shard_role/shard_catalog/database.h"
#include "mongo/db/shard_role/shard_catalog/database_holder.h"
#include "mongo/db/shard_role/shard_catalog/db_raii.h"
#include "mongo/util/fail_point.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(hangAfterCanAcceptNonLocalWritesCheckInProfile);

namespace {

Status _setProfileSettings(OperationContext* opCtx,
                           Database* db,
                           const DatabaseName& dbName,
                           auto& dbProfileSettings,
                           ProfileSettings newSettings) {


    if (newSettings.level == 0) {
        dbProfileSettings.setDatabaseProfileSettings(dbName, newSettings);
        // No need to create the profile collection.
        return Status::OK();
    }

    // Create the profile colllection if possible.
    if (db) {
        Status status = profile_collection::createProfileCollection(opCtx, db);
        if (!status.isOK()) {
            return status;
        }
    }

    // Set the settings.
    // Must not set settings if creating profile collection failed.
    dbProfileSettings.setDatabaseProfileSettings(dbName, newSettings);

    return Status::OK();
}

ProfileSettings _computeNew(OperationContext* opCtx,
                            const ProfileSettings& oldSettings,
                            const ProfileCmdRequest& request,
                            int64_t profilingLevel) {
    ProfileSettings newSettings = oldSettings;
    if (profilingLevel >= 0 && profilingLevel <= 2) {
        newSettings.level = profilingLevel;
    }
    if (auto filterOrUnset = request.getFilter()) {
        if (auto filter = filterOrUnset->obj) {
            // filter: <match expression>
            newSettings.filter = std::make_shared<ProfileFilterImpl>(
                *filter, ExpressionContextBuilder{}.opCtx(opCtx).build());
        } else {
            // filter: "unset"
            newSettings.filter = nullptr;
        }
    }
    if (auto slowOpInProgMS = request.getSlowinprogms()) {
        newSettings.slowOpInProgressThreshold = Milliseconds(*slowOpInProgMS);
    }
    return newSettings;
}

bool _canProfile(OperationContext* opCtx,
                 const ProfileSettings& newSettings,
                 const ProfileSettings& oldSettings) {
    // Short circuit if we have nothing to do.
    if (newSettings == oldSettings) {
        return false;
    }

    uassert(ErrorCodes::CommandNotSupported,
            "the storage engine doesn't support profiling.",
            opCtx->getServiceContext()->getStorageEngine()->supportsCappedCollections());

    return true;
}

Database* _maybeCreateDB(OperationContext* opCtx, const DatabaseName& dbName) {
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (replCoord->canAcceptNonLocalWrites()) {
        hangAfterCanAcceptNonLocalWritesCheckInProfile.pauseWhileSet();
        // Primary or standalone: create the database for profiling.
        auto databaseHolder = DatabaseHolder::get(opCtx);
        return databaseHolder->openDb(opCtx, dbName);
    }
    return nullptr;
}

/**
 * Sets the profiling level, logging/profiling threshold, and logging/profiling sample rate for the
 * given database.
 */
class CmdProfile : public ProfileCmdBase {
public:
    CmdProfile() = default;

protected:
    ProfileSettings _applyProfilingLevel(OperationContext* opCtx,
                                         const DatabaseName& dbName,
                                         const ProfileCmdRequest& request) const final {
        const auto profilingLevel = request.getCommandParameter();

        const auto& provider = rss::ReplicatedStorageService::get(opCtx).getPersistenceProvider();
        uassert(ErrorCodes::CommandNotSupported,
                str::stream() << "Profile level " << profilingLevel
                              << " is not supported in this storage mode: " << provider.name(),
                provider.supportsProfilingLevel(profilingLevel));

        // An invalid profiling level (outside the range [0, 2]) represents a request to read the
        // current profiling level. Similarly, if the request does not include a filter, we only
        // need to read the current filter, if any. If we're not changing either value, then we can
        // acquire a shared lock instead of exclusive.
        const bool readOnly = (profilingLevel < 0 || profilingLevel > 2) && !request.getFilter();
        const LockMode dbMode = readOnly ? MODE_IS : MODE_IX;

        NamespaceString nss(NamespaceString::makeSystemDotProfileNamespace(dbName));
        AutoGetCollection ctx(opCtx, nss, dbMode);
        Database* db = ctx.getDb();

        auto& dbProfileSettings = DatabaseProfileSettings::get(opCtx->getServiceContext());

        // Fetches the database profiling level + filter or the server default if the db does not
        // exist.
        auto oldSettings = dbProfileSettings.getDatabaseProfileSettings(dbName);

        if (!readOnly) {
            // Construct new settings.
            auto newSettings = _computeNew(opCtx, oldSettings, request, profilingLevel);

            if (!_canProfile(
                    opCtx, newSettings, dbProfileSettings.getDatabaseProfileSettings(dbName))) {
                return oldSettings;
            }

            // Try to create database if we can.
            if (!db) {
                db = _maybeCreateDB(opCtx, dbName);
            }

            // Set the settings.
            uassertStatusOK(_setProfileSettings(opCtx, db, dbName, dbProfileSettings, newSettings));
        }

        return oldSettings;
    }
};

MONGO_REGISTER_COMMAND(CmdProfile).forShard();

}  // namespace
}  // namespace mongo
