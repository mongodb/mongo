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

#pragma once

#include "mongo/db/service_context.h"
#include "mongo/util/background.h"
#include "mongo/util/concurrency/mutex.h"

namespace mongo {

/**
 * Maintains a global read lock while mongod is fsyncLocked.
 */
class FSyncLockThread : public BackgroundJob {
public:
    FSyncLockThread(ServiceContext* serviceContext, bool allowFsyncFailure)
        : BackgroundJob(false),
          _serviceContext(serviceContext),
          _allowFsyncFailure(allowFsyncFailure) {}

    std::string name() const override {
        return "FSyncLockThread";
    }

    void run() override;

    /**
     * Releases the fsync lock for shutdown.
     */
    void shutdown(stdx::unique_lock<Latch>& lk);

private:
    ServiceContext* const _serviceContext;
    bool _allowFsyncFailure;
};

/**
 * Allows holders to block on an active fsyncLock.
 */
extern SimpleMutex filesLockedFsync;

/**
 * Must be taken before accessing globalFsyncLockThread below.
 */
extern Mutex fsyncStateMutex;

/**
 * The FSyncLockThread must be external available for interruption during shutdown.
 * Must lock the 'fsyncStateMutex' before accessing.
 *
 * TODO (SERVER-76131): consider whether this should decorate the service context.
 */
extern std::unique_ptr<FSyncLockThread> globalFsyncLockThread;

}  // namespace mongo
