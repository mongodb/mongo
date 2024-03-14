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


#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/user_cache_invalidator_job.h"
#include "mongo/db/auth/user_cache_invalidator_job_parameters_gen.h"
#include "mongo/db/client.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/grid.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kAccessControl


namespace mongo {
namespace {

using OIDorTimestamp = UserCacheInvalidator::OIDorTimestamp;

const auto getUserCacheInvalidator =
    ServiceContext::declareDecoration<std::unique_ptr<UserCacheInvalidator>>();

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

        uassert(
            4664501, "Must include 'authInfoOpTime'", authInfoOpTimeElem.type() == bsonTimestamp);
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

/**
 * TODO: SERVER-86458 - remove.
 *
 * RAII type for making the OperationContext it is instantiated with use the router service util it
 * goes out of scope.
 */
class ScopedSetRouterService {
public:
    ScopedSetRouterService(OperationContext* opCtx);
    ~ScopedSetRouterService();

private:
    OperationContext* const _opCtx;
    Service* const _originalService;
};

ScopedSetRouterService::ScopedSetRouterService(OperationContext* opCtx)
    : _opCtx(opCtx), _originalService(opCtx->getService()) {
    // Verify that the opCtx is not using the router service already.
    stdx::lock_guard<Client> lk(*_opCtx->getClient());

    auto service = opCtx->getServiceContext()->getService(ClusterRole::RouterServer);
    invariant(service);
    _opCtx->getClient()->setService(service);
}

ScopedSetRouterService::~ScopedSetRouterService() {
    // Verify that the opCtx is still using the router service.
    stdx::lock_guard<Client> lk(*_opCtx->getClient());
    invariant(_opCtx->getService()->role().has(ClusterRole::RouterServer));
    _opCtx->getClient()->setService(_originalService);
}

}  // namespace

Status userCacheInvalidationIntervalSecsNotify(const int& value) {
    LOGV2_DEBUG(20259, 5, "setInterval", "newInterval"_attr = loadInterval());
    if (hasGlobalServiceContext()) {
        auto service = getGlobalServiceContext();
        if (getUserCacheInvalidator(service)) {
            getUserCacheInvalidator(service)->setPeriod(loadInterval());
        }
    }
    return Status::OK();
}

void UserCacheInvalidator::setPeriod(Milliseconds period) {
    _job->setPeriod(period);
}

UserCacheInvalidator::UserCacheInvalidator(AuthorizationManager* authzManager)
    : _authzManager(authzManager) {}

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
    invariant(serverGlobalParams.clusterRole.has(ClusterRole::RouterServer));
    ScopedSetRouterService guard(opCtx);
    auto invalidator =
        std::make_unique<UserCacheInvalidator>(AuthorizationManager::get(opCtx->getService()));
    invalidator->initialize(opCtx);

    auto periodicRunner = serviceCtx->getPeriodicRunner();
    invariant(periodicRunner);

    // This job is killable. When interrupted, we will warn, and retry after the configured
    // interval.
    PeriodicRunner::PeriodicJob job(
        "UserCacheInvalidator",
        [serviceCtx](Client* client) { getUserCacheInvalidator(serviceCtx)->run(); },
        loadInterval(),
        true /*isKillableByStepdown*/);

    invalidator->_job =
        std::make_unique<PeriodicJobAnchor>(periodicRunner->makeJob(std::move(job)));

    // Make sure the invalidator is moved to the service context by the time we call start()
    getUserCacheInvalidator(serviceCtx) = std::move(invalidator);
    getUserCacheInvalidator(serviceCtx)->_job->start();
}

void UserCacheInvalidator::run() try {
    auto opCtx = cc().makeOperationContext();
    ScopedSetRouterService guard(opCtx.get());
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
            AuthorizationManager::get(opCtx->getService())->invalidateUserCache();
        } catch (const DBException& e) {
            LOGV2_WARNING(20268, "Error invalidating user cache", "error"_attr = e.toStatus());
        }
        _previousGeneration = swCurrentGeneration.getValue();
    } else {
        // If LDAP authorization is enabled and the authz cache generation has not changed, the
        // external users should be refreshed to ensure that any cached users evicted on the config
        // server are appropriately refreshed here.
        auto refreshStatus = _authzManager->refreshExternalUsers(opCtx.get());
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
