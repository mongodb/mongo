/*    Copyright 2012 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kAccessControl

#include "mongo/platform/basic.h"

#include "mongo/db/auth/user_cache_invalidator_job.h"

#include <string>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/client/connpool.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/server_parameters.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/grid.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/background.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/exit.h"
#include "mongo/util/log.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {

// How often to check with the config servers whether authorization information has changed.
AtomicInt32 userCacheInvalidationIntervalSecs(30);  // 30 second default
stdx::mutex invalidationIntervalMutex;
stdx::condition_variable invalidationIntervalChangedCondition;
Date_t lastInvalidationTime;

class ExportedInvalidationIntervalParameter
    : public ExportedServerParameter<int, ServerParameterType::kStartupAndRuntime> {
public:
    ExportedInvalidationIntervalParameter()
        : ExportedServerParameter<int, ServerParameterType::kStartupAndRuntime>(
              ServerParameterSet::getGlobal(),
              "userCacheInvalidationIntervalSecs",
              &userCacheInvalidationIntervalSecs) {}

    virtual Status validate(const int& potentialNewValue) {
        if (potentialNewValue < 1 || potentialNewValue > 86400) {
            return Status(ErrorCodes::BadValue,
                          "userCacheInvalidationIntervalSecs must be between 1 "
                          "and 86400 (24 hours)");
        }
        return Status::OK();
    }

    // Without this the compiler complains that defining set(const int&)
    // hides set(const BSONElement&)
    using ExportedServerParameter<int, ServerParameterType::kStartupAndRuntime>::set;

    virtual Status set(const int& newValue) {
        stdx::unique_lock<stdx::mutex> lock(invalidationIntervalMutex);
        Status status =
            ExportedServerParameter<int, ServerParameterType::kStartupAndRuntime>::set(newValue);
        invalidationIntervalChangedCondition.notify_all();
        return status;
    }

} exportedIntervalParam;

StatusWith<OID> getCurrentCacheGeneration(OperationContext* opCtx) {
    try {
        BSONObjBuilder result;
        const bool ok = Grid::get(opCtx)->catalogClient()->runUserManagementReadCommand(
            opCtx, "admin", BSON("_getUserCacheGeneration" << 1), &result);
        if (!ok) {
            return getStatusFromCommandResult(result.obj());
        }
        return result.obj()["cacheGeneration"].OID();
    } catch (const DBException& e) {
        return StatusWith<OID>(e.toStatus());
    } catch (const std::exception& e) {
        return StatusWith<OID>(ErrorCodes::UnknownError, e.what());
    }
}

}  // namespace

UserCacheInvalidator::UserCacheInvalidator(AuthorizationManager* authzManager)
    : _authzManager(authzManager) {}

UserCacheInvalidator::~UserCacheInvalidator() {
    invariant(globalInShutdownDeprecated());
    // Wait to stop running.
    wait();
}

void UserCacheInvalidator::initialize(OperationContext* opCtx) {
    StatusWith<OID> currentGeneration = getCurrentCacheGeneration(opCtx);
    if (currentGeneration.isOK()) {
        _previousCacheGeneration = currentGeneration.getValue();
        return;
    }

    if (currentGeneration.getStatus().code() == ErrorCodes::CommandNotFound) {
        warning() << "_getUserCacheGeneration command not found while fetching initial user "
                     "cache generation from the config server(s).  This most likely means you are "
                     "running an outdated version of mongod on the config servers";
    } else {
        warning() << "An error occurred while fetching initial user cache generation from "
                     "config servers: "
                  << currentGeneration.getStatus();
    }
    _previousCacheGeneration = OID();
}

void UserCacheInvalidator::run() {
    Client::initThread("UserCacheInvalidator");
    lastInvalidationTime = Date_t::now();

    while (true) {
        stdx::unique_lock<stdx::mutex> lock(invalidationIntervalMutex);
        Date_t sleepUntil =
            lastInvalidationTime + Seconds(userCacheInvalidationIntervalSecs.load());
        Date_t now = Date_t::now();
        while (now < sleepUntil) {
            MONGO_IDLE_THREAD_BLOCK;
            invalidationIntervalChangedCondition.wait_until(lock, sleepUntil.toSystemTimePoint());
            sleepUntil = lastInvalidationTime + Seconds(userCacheInvalidationIntervalSecs.load());
            now = Date_t::now();
        }
        lastInvalidationTime = now;
        lock.unlock();

        if (globalInShutdownDeprecated()) {
            break;
        }

        auto opCtx = cc().makeOperationContext();
        StatusWith<OID> currentGeneration = getCurrentCacheGeneration(opCtx.get());
        if (!currentGeneration.isOK()) {
            if (currentGeneration.getStatus().code() == ErrorCodes::CommandNotFound) {
                warning() << "_getUserCacheGeneration command not found on config server(s), "
                             "this most likely means you are running an outdated version of mongod "
                             "on the config servers";
            } else {
                warning() << "An error occurred while fetching current user cache generation "
                             "to check if user cache needs invalidation: "
                          << currentGeneration.getStatus();
            }
            // When in doubt, invalidate the cache
            _authzManager->invalidateUserCache();
            continue;
        }

        if (currentGeneration.getValue() != _previousCacheGeneration) {
            log() << "User cache generation changed from " << _previousCacheGeneration << " to "
                  << currentGeneration.getValue() << "; invalidating user cache";
            _authzManager->invalidateUserCache();
            _previousCacheGeneration = currentGeneration.getValue();
        }
    }
}

std::string UserCacheInvalidator::name() const {
    return "UserCacheInvalidatorThread";
}

}  // namespace mongo
