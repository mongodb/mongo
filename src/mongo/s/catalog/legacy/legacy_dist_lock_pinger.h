/**
 *    Copyright (C) 2015 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <list>
#include <set>
#include <string>

#include "mongo/base/status.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/s/catalog/dist_lock_manager.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/time_support.h"

namespace mongo {

class DistributedLock;

class LegacyDistLockPinger {
public:
    LegacyDistLockPinger() = default;

    /**
     * Starts pinging the process id for the given lock.
     */
    Status startPing(const DistributedLock& lock, Milliseconds sleepTime);

    /**
     * Adds a distributed lock that has the given id to the unlock list. The unlock list
     * contains the list of locks that this pinger will repeatedly attempt to unlock until
     * it succeeds.
     */
    void addUnlockOID(const DistLockHandle& lockID);

    /**
     * Returns true if the given lock id is currently in the unlock queue.
     */
    bool willUnlockOID(const DistLockHandle& lockID);

    /**
     * For testing only: non-blocking call to stop pinging the given process id.
     */
    void stopPing(const ConnectionString& conn, const std::string& processId);

    /**
     * Kills all ping threads and wait for them to cleanup.
     */
    void shutdown();

private:
    /**
     * Helper method for calling _distLockPingThread.
     */
    void distLockPingThread(ConnectionString addr,
                            long long clockSkew,
                            const std::string& processId,
                            Milliseconds sleepTime);

    /**
     * Function for repeatedly pinging the process id. Also attempts to unlock all the
     * locks in the unlock list.
     */
    void _distLockPingThread(ConnectionString addr,
                             const std::string& process,
                             Milliseconds sleepTime);

    /**
     * Returns true if a request has been made to stop pinging the give process id.
     */
    bool shouldStopPinging(const ConnectionString& conn, const std::string& processId);

    /**
     * Acknowledge the stop ping request and performs the necessary cleanup.
     */
    void acknowledgeStopPing(const ConnectionString& conn, const std::string& processId);

    /**
     * Blocks until duration has elapsed or if the ping thread is interrupted.
     */
    void waitTillNextPingTime(Milliseconds duration);

    //
    // All member variables are labeled with one of the following codes indicating the
    // synchronization rules for accessing them.
    //
    // (M)  Must hold _mutex for access.

    stdx::mutex _mutex;

    // Triggered everytime a pinger thread is stopped.
    stdx::condition_variable _pingStoppedCV;  // (M)

    // pingID -> thread
    // This can contain multiple elements in tests, but in tne normal case, this will
    // contain only a single element.
    // Note: can be safely read when _inShutdown is true.
    std::map<std::string, stdx::thread> _pingThreads;  // (M*)

    // Contains the list of process id to stopPing.
    std::set<std::string> _kill;  // (M)

    // Contains all of the process id to ping.
    std::set<std::string> _seen;  // (M)

    // Contains all lock ids to keeping on retrying to unlock until success.
    std::list<DistLockHandle> _unlockList;  // (M)

    bool _inShutdown = false;  // (M)
};
}
