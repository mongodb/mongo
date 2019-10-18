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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kAccessControl

#include "mongo/platform/basic.h"

#include "mongo/db/auth/user_cache_invalidator_job.h"

#include <string>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/client/connpool.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/user_cache_invalidator_job_parameters_gen.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/platform/compiler.h"
#include "mongo/platform/mutex.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/grid.h"
#include "mongo/util/background.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/duration.h"
#include "mongo/util/log.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {

class ThreadSleepInterval {

public:
    explicit ThreadSleepInterval(Seconds interval) : _interval(interval) {}

    void setInterval(Seconds interval) {
        {
            stdx::lock_guard<Latch> twiddle(_mutex);
            MONGO_LOG(5) << "setInterval: old=" << _interval << ", new=" << interval;
            _interval = interval;
        }
        _condition.notify_all();
    }

    void start() {
        stdx::unique_lock<Latch> lock(_mutex);
        _last = Date_t::now();
    }

    void abort() {
        stdx::unique_lock<Latch> lock(_mutex);
        _inShutdown = true;
        _condition.notify_all();
    }

    /**
     * Sleeps until either an interval has elapsed since the last call to wait(), or a shutdown
     * event is triggered via abort(). Returns false if interrupted due to shutdown, or true if an
     * interval has elapsed.
     */
    bool wait() {
        stdx::unique_lock<Latch> lock(_mutex);
        while (true) {
            if (_inShutdown) {
                return false;
            }

            Date_t now = Date_t::now();
            Date_t expiry = _last + _interval;
            MONGO_LOG(5) << "wait: now=" << now << ", expiry=" << expiry;

            if (now >= expiry) {
                _last = now;
                MONGO_LOG(5) << "wait: done";
                return true;
            }

            MONGO_LOG(5) << "wait: blocking";
            MONGO_IDLE_THREAD_BLOCK;
            _condition.wait_until(lock, expiry.toSystemTimePoint());
        }
    }

private:
    Seconds _interval;
    Mutex _mutex = MONGO_MAKE_LATCH("ThreadSleepInterval::_mutex");
    stdx::condition_variable _condition;
    bool _inShutdown = false;
    Date_t _last;
};

Seconds loadInterval() {
    return Seconds(userCacheInvalidationIntervalSecs.load());
}

ThreadSleepInterval* globalInvalidationInterval() {
    static auto p = new ThreadSleepInterval(loadInterval());
    return p;
}

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

Status userCacheInvalidationIntervalSecsNotify(const int& value) {
    globalInvalidationInterval()->setInterval(loadInterval());
    return Status::OK();
}

UserCacheInvalidator::UserCacheInvalidator(AuthorizationManager* authzManager)
    : _authzManager(authzManager) {}

UserCacheInvalidator::~UserCacheInvalidator() {
    globalInvalidationInterval()->abort();
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
    auto interval = globalInvalidationInterval();
    interval->start();
    while (interval->wait()) {
        auto opCtx = cc().makeOperationContext();
        StatusWith<OID> currentGeneration = getCurrentCacheGeneration(opCtx.get());
        if (!currentGeneration.isOK()) {
            warning() << "An error occurred while fetching current user cache generation "
                         "to check if user cache needs invalidation: "
                      << currentGeneration.getStatus();

            // When in doubt, invalidate the cache
            try {
                _authzManager->invalidateUserCache(opCtx.get());
            } catch (const DBException& e) {
                warning() << "Error invalidating user cache: " << e.toStatus();
            }
            continue;
        }

        if (currentGeneration.getValue() != _previousCacheGeneration) {
            log() << "User cache generation changed from " << _previousCacheGeneration << " to "
                  << currentGeneration.getValue() << "; invalidating user cache";
            try {
                _authzManager->invalidateUserCache(opCtx.get());
            } catch (const DBException& e) {
                warning() << "Error invalidating user cache: " << e.toStatus();
            }

            _previousCacheGeneration = currentGeneration.getValue();
        }
    }
}

std::string UserCacheInvalidator::name() const {
    return "UserCacheInvalidatorThread";
}

}  // namespace mongo
