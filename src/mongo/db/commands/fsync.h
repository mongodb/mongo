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
#include "mongo/stdx/mutex.h"
#include "mongo/util/background.h"

#include <memory>
#include <string>

namespace mongo {

/**
 * Maintains a global read lock while mongod is fsyncLocked.
 */
class FSyncLockThread : public BackgroundJob {
public:
    FSyncLockThread(ServiceContext* serviceContext,
                    bool allowFsyncFailure,
                    const Milliseconds deadline)
        : BackgroundJob(false),
          _serviceContext(serviceContext),
          _allowFsyncFailure(allowFsyncFailure),
          _deadline(deadline) {}

    std::string name() const override {
        return "FSyncLockThread";
    }

    void run() override;

    /**
     * Releases the fsync lock for shutdown.
     */
    void shutdown(stdx::unique_lock<stdx::mutex>& lk);

private:
    /**
     * Wait lastApplied to catch lastWritten so we won't write/apply any oplog when fsync locked.
     */
    void _waitUntilLastAppliedCatchupLastWritten();

private:
    ServiceContext* const _serviceContext;
    bool _allowFsyncFailure;
    const Milliseconds _deadline;
};

/**
 * This is used to block oplogWriter and should never be acquired by others.
 */
extern stdx::mutex oplogWriterLockedFsync;

/**
 * This is used to block oplogApplier and should never be acquired by others.
 */
extern stdx::mutex oplogApplierLockedFsync;

/**
 * Must be taken before accessing globalFsyncLockThread below.
 */
extern stdx::mutex fsyncStateMutex;

/**
 * The FSyncLockThread must be external available for interruption during shutdown.
 * Must lock the 'fsyncStateMutex' before accessing.
 */
extern std::unique_ptr<FSyncLockThread> globalFsyncLockThread;

}  // namespace mongo
