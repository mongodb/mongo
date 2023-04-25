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

#include "mongo/db/auth/user_cache_invalidator_job.h"

#include <string>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/user_cache_invalidator_job_parameters_gen.h"
#include "mongo/db/client.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/grid.h"
#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"

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
            opCtx, "admin", BSON("_getUserCacheGeneration" << 1), &result);
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
        return stdx::get<OID>(oidOrTimestamp).toString();
    } else if (oidOrTimestamp.index() == 1) {  // Timestamp
        return stdx::get<Timestamp>(oidOrTimestamp).toString();
    }
    MONGO_UNREACHABLE;
}

}  // namespace

Status userCacheInvalidationIntervalSecsNotify(const int& value) {
    LOGV2_DEBUG(20259,
                5,
                "setInterval: new={newInterval}",
                "setInterval",
                "newInterval"_attr = loadInterval());
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
    auto invalidator =
        std::make_unique<UserCacheInvalidator>(AuthorizationManager::get(serviceCtx));
    invalidator->initialize(opCtx);

    auto periodicRunner = serviceCtx->getPeriodicRunner();
    invariant(periodicRunner);

    PeriodicRunner::PeriodicJob job(
        "UserCacheInvalidator",
        [serviceCtx](Client* client) { getUserCacheInvalidator(serviceCtx)->run(); },
        loadInterval());

    invalidator->_job =
        std::make_unique<PeriodicJobAnchor>(periodicRunner->makeJob(std::move(job)));

    // Make sure the invalidator is moved to the service context by the time we call start()
    getUserCacheInvalidator(serviceCtx) = std::move(invalidator);
    getUserCacheInvalidator(serviceCtx)->_job->start();
}

void UserCacheInvalidator::run() {
    auto opCtx = cc().makeOperationContext();
    auto swCurrentGeneration = getCurrentCacheGeneration(opCtx.get());
    if (!swCurrentGeneration.isOK()) {
        LOGV2_WARNING(20266,
                      "An error occurred while fetching current user cache generation from "
                      "config servers",
                      "error"_attr = swCurrentGeneration.getStatus());

        // When in doubt, invalidate the cache
        try {
            _authzManager->invalidateUserCache(opCtx.get());
        } catch (const DBException& e) {
            LOGV2_WARNING(20267, "Error invalidating user cache", "error"_attr = e.toStatus());
        }
        return;
    }

    if (swCurrentGeneration.getValue() != _previousGeneration) {
        LOGV2(20263,
              "User cache generation changed from {previousGeneration} to "
              "{currentGeneration}; invalidating user cache",
              "User cache generation changed; invalidating user cache",
              "previousGeneration"_attr = oidOrTimestampToString(_previousGeneration),
              "currentGeneration"_attr = oidOrTimestampToString(swCurrentGeneration.getValue()));
        try {
            _authzManager->invalidateUserCache(opCtx.get());
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
                _authzManager->invalidateUsersFromDB(opCtx.get(), DatabaseName::kExternal);
            } catch (const DBException& e) {
                LOGV2_WARNING(5914805,
                              "Error invalidating $external users from user cache",
                              "error"_attr = e.toStatus());
            }
        }
    }
}

}  // namespace mongo
