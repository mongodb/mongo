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

#include <queue>
#include <sqlite3.h>
#include <string>
#include <vector>

#include "mongo/db/operation_context.h"
#include "mongo/db/storage/mobile/mobile_options.h"
#include "mongo/db/storage/mobile/mobile_session.h"
#include "mongo/stdx/mutex.h"

namespace mongo {
class MobileSession;

/**
 * This class manages a queue of operations delayed for some reason
 */
class MobileDelayedOpQueue final {
    MobileDelayedOpQueue(const MobileDelayedOpQueue&) = delete;
    MobileDelayedOpQueue& operator=(const MobileDelayedOpQueue&) = delete;

public:
    MobileDelayedOpQueue();
    void enqueueOp(std::string& opQuery);
    void execAndDequeueOp(MobileSession* session);
    void execAndDequeueAllOps(MobileSession* session);
    bool isEmpty();

private:
    AtomicWord<bool> _isEmpty;
    stdx::mutex _queueMutex;
    std::queue<std::string> _opQueryQueue;
};

/**
 * This class manages a pool of open sqlite3* objects.
 */
class MobileSessionPool final {
    MobileSessionPool(const MobileSessionPool&) = delete;
    MobileSessionPool& operator=(const MobileSessionPool&) = delete;

public:
    MobileSessionPool(const std::string& path,
                      const embedded::MobileOptions& options,
                      std::uint64_t maxPoolSize = 80);

    ~MobileSessionPool();

    /**
     * Returns a smart pointer to a previously released session for reuse, or creates a new session.
     */
    std::unique_ptr<MobileSession> getSession(OperationContext* opCtx);

    /**
     * Returns a session to the pool for later reuse.
     */
    void releaseSession(MobileSession* session);

    /**
     * Transitions the pool to shutting down mode. It waits until all sessions are released back
     * into the pool and closes all open sessions.
     */
    void shutDown();

    // Failed drops get queued here and get re-tried periodically
    MobileDelayedOpQueue failedDropsQueue;

    // Returns the mobile options associated with this storage engine instance
    const embedded::MobileOptions& getOptions() const {
        return _options;
    }

private:
    /**
        * Gets the front element from _sessions and then pops it off the queue.
        */
    sqlite3* _popSession_inlock();

    // This is used to lock the _sessions vector.
    stdx::mutex _mutex;
    stdx::condition_variable _releasedSessionNotifier;

    std::string _path;
    const embedded::MobileOptions& _options;

    /**
     * PoolSize is the number of open sessions associated with the session pool.
     */
    std::uint64_t _maxPoolSize = 80;
    std::uint64_t _curPoolSize = 0;
    bool _shuttingDown = false;

    using SessionPool = std::vector<sqlite3*>;
    SessionPool _sessions;
};
}  // namespace mongo
