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


#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/query_cmd/plan_cache_commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/db_raii.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/query/canonical_query_encoder.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/plan_cache/classic_plan_cache.h"
#include "mongo/db/query/plan_cache/sbe_plan_cache.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

PlanCache* getPlanCache(OperationContext* opCtx, const CollectionPtr& collection) {
    invariant(collection);
    PlanCache* planCache = CollectionQueryInfo::get(collection).getPlanCache();
    invariant(planCache);
    return planCache;
}

/**
 * Clears collection's plan cache. If query shape is provided, clears plans for that single query
 * shape only.
 */
Status clear(OperationContext* opCtx,
             const CollectionPtr& collection,
             PlanCache* planCache,
             const NamespaceString& nss,
             const BSONObj& cmdObj) {
    invariant(planCache);

    // According to the specification, the planCacheClear command runs in two modes:
    // - clear all query shapes; or
    // - clear plans for single query shape when a query shape is described in the
    //   command arguments.
    if (cmdObj.hasField("query")) {
        auto statusWithCQ = plan_cache_commands::canonicalize(opCtx, nss, cmdObj);
        if (!statusWithCQ.isOK()) {
            return statusWithCQ.getStatus();
        }

        auto cq = std::move(statusWithCQ.getValue());

        // Based on a query shape only, we cannot be sure whether a query with the given query shape
        // can be executed with the SBE engine or not. Therefore, we try to clean the plan caches in
        // both cases.
        stdx::unordered_set<uint32_t> planCacheCommandKeys = {canonical_query_encoder::computeHash(
            canonical_query_encoder::encodeForPlanCacheCommand(*cq))};
        plan_cache_commands::removePlanCacheEntriesByPlanCacheCommandKeys(planCacheCommandKeys,
                                                                          planCache);
        plan_cache_commands::removePlanCacheEntriesByPlanCacheCommandKeys(
            planCacheCommandKeys, collection->uuid(), &sbe::getPlanCache(opCtx));

        return Status::OK();
    }

    // If query is not provided, make sure sort, projection, and collation are not in arguments.
    // We do not want to clear the entire cache inadvertently when the user
    // forgets to provide a value for "query".
    if (cmdObj.hasField("sort") || cmdObj.hasField("projection") || cmdObj.hasField("collation")) {
        return Status(ErrorCodes::BadValue,
                      "sort, projection, or collation provided without query");
    }

    planCache->clear();

    auto version = CollectionQueryInfo::get(collection).getPlanCacheInvalidatorVersion();
    sbe::clearPlanCacheEntriesWith(opCtx->getServiceContext(),
                                   collection->uuid(),
                                   version,
                                   false /*matchSecondaryCollections*/);

    LOGV2_DEBUG(23908, 1, "Cleared plan cache", logAttrs(nss));

    return Status::OK();
}

}  // namespace

/**
 * The 'planCacheClear' command can be used to clear all entries from a collection's plan cache, or
 * to delete a particular plan cache entry. In the latter case, the plan cache entry to delete is
 * specified with an example query, like so:
 *
 *    {
 *        planCacheClear: <collection>,
 *        query: <query>,
 *        sort: <sort>,
 *        projection: <projection>
 *    }
 */
class PlanCacheClearCommand final : public BasicCommand {
public:
    PlanCacheClearCommand() : BasicCommand("planCacheClear") {}

    bool run(OperationContext* opCtx,
             const DatabaseName& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override;

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kOptIn;
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj& cmdObj) const override;

    std::string help() const override {
        return "Drops one or all plan cache entries in a collection.";
    }
};
MONGO_REGISTER_COMMAND(PlanCacheClearCommand).forShard();

Status PlanCacheClearCommand::checkAuthForOperation(OperationContext* opCtx,
                                                    const DatabaseName& dbName,
                                                    const BSONObj& cmdObj) const {
    AuthorizationSession* authzSession = AuthorizationSession::get(opCtx->getClient());
    ResourcePattern pattern = parseResourcePattern(dbName, cmdObj);

    if (authzSession->isAuthorizedForActionsOnResource(pattern, ActionType::planCacheWrite)) {
        return Status::OK();
    }

    return Status(ErrorCodes::Unauthorized, "unauthorized");
}

bool PlanCacheClearCommand::run(OperationContext* opCtx,
                                const DatabaseName& dbName,
                                const BSONObj& cmdObj,
                                BSONObjBuilder& result) {
    const NamespaceString nss(CommandHelpers::parseNsCollectionRequired(dbName, cmdObj));

    // This is a read lock. The query cache is owned by the collection.
    auto ctx = acquireCollection(
        opCtx,
        CollectionAcquisitionRequest::fromOpCtx(opCtx, nss, AcquisitionPrerequisites::kRead),
        MODE_IS);
    AutoStatsTracker statsTracker(opCtx,
                                  nss,
                                  Top::LockType::ReadLocked,
                                  AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
                                  DatabaseProfileSettings::get(opCtx->getServiceContext())
                                      .getDatabaseProfileLevel(nss.dbName()));
    if (!ctx.exists()) {
        // Clearing a non-existent collection always succeeds.
        return true;
    }

    auto planCache = getPlanCache(opCtx, ctx.getCollectionPtr());
    uassertStatusOK(clear(opCtx, ctx.getCollectionPtr(), planCache, nss, cmdObj));
    return true;
}

}  // namespace mongo
