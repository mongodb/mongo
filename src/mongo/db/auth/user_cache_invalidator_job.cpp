// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/auth/user_cache_invalidator_job.h"

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/user_cache_invalidator_job_parameters_gen.h"
#include "mongo/db/client.h"
#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"

#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kAccessControl


namespace mongo {
namespace {

using OIDorTimestamp = UserCacheInvalidator::OIDorTimestamp;

const auto getUserCacheInvalidator = ServiceContext::declareDecoration<UserCacheInvalidator>();

Seconds loadInterval() {
    return Seconds(userCacheInvalidationIntervalSecs.load());
}

StatusWith<OIDorTimestamp> getCurrentCacheGeneration(OperationContext* opCtx) {
    try {
        BSONObjBuilder result;
        const bool ok = Grid::get(opCtx)->catalogClient()->runUserManagementReadCommand(
            opCtx, DatabaseName::kAdmin, BSON("_getUserCacheGeneration" << 1), &result);
        if (!ok) {
            return getStatusFromCommandResult(result.obj());
        }

        const auto resultObj = result.obj();
        const auto cacheGenerationElem = resultObj["cacheGeneration"];
        const auto authInfoOpTimeElem = resultObj["authInfoOpTime"];
        uassert(4664500,
                "It is illegal to include both 'cacheGeneration' and 'authInfoOpTime'",
                !cacheGenerationElem || !authInfoOpTimeElem);

        if (cacheGenerationElem)
            return OIDorTimestamp(cacheGenerationElem.OID());

        uassert(4664501,
                "Must include 'authInfoOpTime'",
                authInfoOpTimeElem.type() == BSONType::timestamp);
        return authInfoOpTimeElem.timestamp();
    } catch (const DBException& e) {
        return e.toStatus();
    }
}

std::string oidOrTimestampToString(const OIDorTimestamp& oidOrTimestamp) {
    if (oidOrTimestamp.index() == 0) {  // OID
        return get<OID>(oidOrTimestamp).toString();
    } else if (oidOrTimestamp.index() == 1) {  // Timestamp
        return get<Timestamp>(oidOrTimestamp).toString();
    }
    MONGO_UNREACHABLE;
}

}  // namespace

Status userCacheInvalidationIntervalSecsNotify(const int& value) {
    LOGV2_DEBUG(20259, 5, "setInterval", "newInterval"_attr = loadInterval());
    if (hasGlobalServiceContext()) {
        getUserCacheInvalidator(getGlobalServiceContext()).setPeriod(loadInterval());
    }
    return Status::OK();
}

void UserCacheInvalidator::setPeriod(Milliseconds period) {
    std::lock_guard lg(_jobMutex);
    if (_job) {
        _job.setPeriod(period);
    }
}

void UserCacheInvalidator::initialize(OperationContext* opCtx) {
    auto swCurrentGeneration = getCurrentCacheGeneration(opCtx);
    if (swCurrentGeneration.isOK()) {
        _previousGeneration = swCurrentGeneration.getValue();
        return;
    }

    LOGV2_WARNING(20265,
                  "An error occurred while fetching initial user cache generation from config "
                  "servers",
                  "error"_attr = swCurrentGeneration.getStatus());
    _previousGeneration = OID();
}

void UserCacheInvalidator::start(ServiceContext* serviceCtx, OperationContext* opCtx) {
    // UserCacheInvalidator should only run on a router.
    invariant(opCtx->getService()->role().has(ClusterRole::RouterServer));

    auto& invalidator = getUserCacheInvalidator(serviceCtx);

    invalidator.initialize(opCtx);

    auto periodicRunner = serviceCtx->getPeriodicRunner();
    invariant(periodicRunner);

    // This job is killable. When interrupted, we will warn, and retry after the configured
    // interval.
    PeriodicRunner::PeriodicJob job(
        "UserCacheInvalidator",
        [serviceCtx](Client* client) { getUserCacheInvalidator(serviceCtx).run(); },
        loadInterval(),
        true /*isKillableByStepdown*/);

    std::lock_guard lg(invalidator._jobMutex);
    // UserCacheInvalidator job must not already be present
    invariant(!invalidator._job);
    invalidator._job = periodicRunner->makeJob(std::move(job));
    invalidator._job.start();
}

void UserCacheInvalidator::stop(ServiceContext* serviceCtx) {
    auto& invalidator = getUserCacheInvalidator(serviceCtx);

    // Guard changes to _job with a mutex because this can run concurrently with start()
    std::lock_guard lg(invalidator._jobMutex);
    if (invalidator._job) {
        invalidator._job.stop();
        invalidator._job.detach();
    }
}

void UserCacheInvalidator::run() try {
    auto opCtx = cc().makeOperationContext();
    invariant(opCtx->getService()->role().has(ClusterRole::RouterServer));

    // Get current cache generation from the config server.
    auto swCurrentGeneration = getCurrentCacheGeneration(opCtx.get());
    if (!swCurrentGeneration.isOK()) {
        LOGV2_WARNING(20266,
                      "An error occurred while fetching current user cache generation from "
                      "config servers",
                      "error"_attr = swCurrentGeneration.getStatus());

        // When in doubt, invalidate the cache
        try {
            AuthorizationManager::get(opCtx->getService())->invalidateUserCache();
        } catch (const DBException& e) {
            LOGV2_WARNING(20267, "Error invalidating user cache", "error"_attr = e.toStatus());
        }
        return;
    }

    if (swCurrentGeneration.getValue() != _previousGeneration) {
        LOGV2(20263,
              "User cache generation changed; invalidating user cache",
              "previousGeneration"_attr = oidOrTimestampToString(_previousGeneration),
              "currentGeneration"_attr = oidOrTimestampToString(swCurrentGeneration.getValue()));
        try {
            // Invalidate user cache from router server.
            AuthorizationManager::get(opCtx->getService())->invalidateUserCache();
        } catch (const DBException& e) {
            LOGV2_WARNING(20268, "Error invalidating user cache", "error"_attr = e.toStatus());
        }
        _previousGeneration = swCurrentGeneration.getValue();
    } else {
        // If LDAP authorization is enabled and the authz cache generation has not changed, the
        // external users should be refreshed to ensure that any cached users evicted on the config
        // server are appropriately refreshed here.
        auto refreshStatus =
            AuthorizationManager::get(opCtx->getService())->refreshExternalUsers(opCtx.get());
        if (!refreshStatus.isOK()) {
            LOGV2_WARNING(5914803,
                          "Error refreshing external users in user cache, so invalidating external "
                          "users in cache",
                          "error"_attr = refreshStatus);
            try {
                AuthorizationManager::get(opCtx->getService())
                    ->invalidateUsersFromDB(DatabaseName::kExternal);
            } catch (const DBException& e) {
                LOGV2_WARNING(5914805,
                              "Error invalidating $external users from user cache",
                              "error"_attr = e.toStatus());
            }
        }
    }
} catch (const DBException& e) {
    LOGV2_WARNING(7466000, "Error invalidating user cache", "error"_attr = e.toStatus());
}

}  // namespace mongo
