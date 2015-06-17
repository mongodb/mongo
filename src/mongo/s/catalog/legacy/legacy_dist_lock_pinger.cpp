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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/catalog/legacy/legacy_dist_lock_pinger.h"

#include "mongo/client/connpool.h"
#include "mongo/s/catalog/legacy/distlock.h"
#include "mongo/s/type_lockpings.h"
#include "mongo/s/type_locks.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/exit.h"
#include "mongo/util/log.h"

namespace mongo {

    using std::string;

namespace {
    string pingThreadId(const ConnectionString& conn, const string& processId) {
        return conn.toString() + "/" + processId;
    }
}

    void LegacyDistLockPinger::_distLockPingThread(ConnectionString addr,
                                                   const string& process,
                                                   Milliseconds sleepTime) {
        setThreadName("LockPinger");

        string pingId = pingThreadId(addr, process);

        LOG(0) << "creating distributed lock ping thread for " << addr
               << " and process " << process << " (sleeping for " << sleepTime.count() << "ms)";

        static int loops = 0;
        Date_t lastPingTime = jsTime();
        while (!shouldStopPinging(addr, process)) {

            LOG(3) << "distributed lock pinger '" << pingId << "' about to ping.";

            Date_t pingTime;

            try {
                ScopedDbConnection conn(addr.toString(), 30.0);

                pingTime = jsTime();
                const auto  elapsed = pingTime - lastPingTime;
                if (elapsed > 10 * sleepTime) {
                    warning() << "Lock pinger for addr: " << addr
                              << ", proc: " << process
                              << " was inactive for " << elapsed;
                }

                lastPingTime = pingTime;

                // Refresh the entry corresponding to this process in the lockpings collection.
                conn->update(LockpingsType::ConfigNS,
                             BSON(LockpingsType::process(process)),
                             BSON("$set" << BSON(LockpingsType::ping(pingTime))),
                             true);

                string err = conn->getLastError();
                if (!err.empty()) {
                    warning() << "pinging failed for distributed lock pinger '" << pingId << "'."
                              << causedBy(err);
                    conn.done();

                    if (!shouldStopPinging(addr, process)) {
                        waitTillNextPingTime(sleepTime);
                    }
                    continue;
                }

                // Remove really old entries from the lockpings collection if they're not
                // holding a lock. This may happen if an instance of a process was taken down
                // and no new instance came up to replace it for a quite a while.
                // NOTE this is NOT the same as the standard take-over mechanism, which forces
                // the lock entry.
                BSONObj fieldsToReturn = BSON(LocksType::state() << 1
                                              << LocksType::process() << 1);
                auto activeLocks =
                        conn->query(LocksType::ConfigNS,
                                    BSON(LocksType::state() << NE << LocksType::UNLOCKED));

                uassert(16060,
                        str::stream() << "cannot query locks collection on config server "
                                      << conn.getHost(),
                        activeLocks.get());

                std::set<string> pids;
                while (activeLocks->more()) {
                    BSONObj lock = activeLocks->nextSafe();

                    if (!lock[LocksType::process()].eoo()) {
                        pids.insert(lock[LocksType::process()].str());
                    }
                    else {
                        warning() << "found incorrect lock document during lock ping cleanup: "
                                  << lock.toString();
                    }
                }

                // This can potentially delete ping entries that are actually active (if the clock
                // of another pinger is too skewed). This is still fine as the lock logic only
                // checks if there is a change in the ping document and the document going away
                // is a valid change.
                Date_t fourDays = pingTime - stdx::chrono::hours{4 * 24};
                conn->remove(LockpingsType::ConfigNS,
                             BSON(LockpingsType::process() << NIN << pids
                                  << LockpingsType::ping() << LT << fourDays));
                err = conn->getLastError();

                if (!err.empty()) {
                    warning() << "ping cleanup for distributed lock pinger '" << pingId
                              << " failed." << causedBy(err);
                    conn.done();

                    if (!shouldStopPinging(addr, process)) {
                        waitTillNextPingTime(sleepTime);
                    }
                    continue;
                }

                LOG(1 - (loops % 10 == 0 ? 1 : 0)) << "cluster " << addr
                        << " pinged successfully at " << pingTime
                        << " by distributed lock pinger '" << pingId
                        << "', sleeping for " << sleepTime.count() << "ms";

                // Remove old locks, if possible
                // Make sure no one else is adding to this list at the same time
                stdx::lock_guard<stdx::mutex> lk(_mutex);

                int numOldLocks = _unlockList.size();
                if (numOldLocks > 0) {
                    LOG(0) << "trying to delete " << _unlockList.size()
                           << " old lock entries for process " << process;
                }

                bool removed = false;
                for (auto iter = _unlockList.begin(); iter != _unlockList.end();
                        iter = (removed ? _unlockList.erase(iter) : ++iter)) {
                    removed = false;
                    try {
                        // Got DistLockHandle from lock, so we don't need to specify _id again
                        conn->update(LocksType::ConfigNS,
                                     BSON(LocksType::lockID(*iter)),
                                     BSON("$set" << BSON( LocksType::state(LocksType::UNLOCKED))));

                        // Either the update went through or it didn't,
                        // either way we're done trying to unlock.
                        LOG(0) << "handled late remove of old distributed lock with ts " << *iter;
                        removed = true;
                    }
                    catch (UpdateNotTheSame&) {
                        LOG(0) << "partially removed old distributed lock with ts " << *iter;
                        removed = true;
                    }
                    catch (std::exception& e) {
                        warning() << "could not remove old distributed lock with ts " << *iter
                                  << causedBy(e);
                    }

                }

                if (numOldLocks > 0 && _unlockList.size() > 0) {
                    LOG(0) << "not all old lock entries could be removed for process " << process;
                }

                conn.done();

            }
            catch (std::exception& e) {
                warning() << "distributed lock pinger '" << pingId
                          << "' detected an exception while pinging." << causedBy(e);
            }

            if (!shouldStopPinging(addr, process)) {
                waitTillNextPingTime(sleepTime);
            }
        }

        warning() << "removing distributed lock ping thread '" << pingId << "'";

        if (shouldStopPinging(addr, process)) {
            acknowledgeStopPing(addr, process);
        }
    }

    void LegacyDistLockPinger::distLockPingThread(ConnectionString addr,
                                                  long long clockSkew,
                                                  const std::string& processId,
                                                  stdx::chrono::milliseconds sleepTime) {
        try {
            jsTimeVirtualThreadSkew(clockSkew);
            _distLockPingThread(addr, processId, sleepTime);
        }
        catch (std::exception& e) {
            error() << "unexpected error while running distributed lock pinger for " << addr
                    << ", process " << processId << causedBy(e);
        }
        catch (...) {
            error() << "unknown error while running distributed lock pinger for " << addr
                    << ", process " << processId;
        }
    }

    Status LegacyDistLockPinger::startPing(const DistributedLock& lock,
                                           stdx::chrono::milliseconds sleepTime) {
        const ConnectionString& conn = lock.getRemoteConnection();
        const string& processId = lock.getProcessId();
        string pingID = pingThreadId(conn, processId);

        {
            // Make sure we don't start multiple threads for a process id.
            stdx::lock_guard<stdx::mutex> lk(_mutex);

            if (_inShutdown) {
                return Status(ErrorCodes::ShutdownInProgress,
                              "shutting down, will not start ping");
            }

            // Ignore if we already have a pinging thread for this process.
            if (_seen.count(pingID) > 0) {
                return Status::OK();
            }

            // Check the config server clock skew.
            if (lock.isRemoteTimeSkewed()) {
                return Status(ErrorCodes::DistributedClockSkewed,
                              str::stream() << "clock skew of the cluster " << conn.toString()
                                            << " is too far out of bounds "
                                            << "to allow distributed locking.");
            }
        }

        {
            stdx::lock_guard<stdx::mutex> lk(_mutex);
            stdx::thread thread(stdx::bind(&LegacyDistLockPinger::distLockPingThread,
                                            this,
                                            conn,
                                            getJSTimeVirtualThreadSkew(),
                                            processId,
                                            sleepTime));
            _pingThreads.insert(std::make_pair(pingID, std::move(thread)));

            _seen.insert(pingID);
        }

        return Status::OK();
    }

    void LegacyDistLockPinger::addUnlockOID(const DistLockHandle& lockID) {
        // Modifying the lock from some other thread
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _unlockList.push_back(lockID);
    }

    bool LegacyDistLockPinger::willUnlockOID(const DistLockHandle& lockID) {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        return find(_unlockList.begin(), _unlockList.end(), lockID) != _unlockList.end();
    }

    void LegacyDistLockPinger::stopPing(const ConnectionString& conn, const string& processId) {
        {
            stdx::lock_guard<stdx::mutex> lk(_mutex);

            string pingId = pingThreadId(conn, processId);

            verify(_seen.count(pingId) > 0);
            _kill.insert(pingId);
            _pingStoppedCV.notify_all();
        }
    }

    void LegacyDistLockPinger::shutdown() {
        {
            stdx::lock_guard<stdx::mutex> lk(_mutex);
            _inShutdown = true;
            _pingStoppedCV.notify_all();
        }

        // Don't grab _mutex, otherwise will deadlock trying to join. Safe to read
        // _pingThreads since it cannot be modified once _shutdown is true.
        for (auto& idToThread : _pingThreads) {
            if (idToThread.second.joinable()) {
                idToThread.second.join();
            }
        }
    }

    bool LegacyDistLockPinger::shouldStopPinging(const ConnectionString& conn,
                                                 const string& processId) {
        if (inShutdown()) {
            return true;
        }

        stdx::lock_guard<stdx::mutex> lk(_mutex);

        if (_inShutdown) {
            return true;
        }

        return _kill.count(pingThreadId(conn, processId)) > 0;
    }

    void LegacyDistLockPinger::acknowledgeStopPing(const ConnectionString& addr,
                                                   const string& processId) {
        {
            stdx::lock_guard<stdx::mutex> lk(_mutex);

            string pingId = pingThreadId(addr, processId);

            _kill.erase(pingId);
            _seen.erase(pingId);
        }

        try {
            ScopedDbConnection conn(addr.toString(), 30.0);
            conn->remove(LockpingsType::ConfigNS, BSON(LockpingsType::process(processId)));
        }
        catch (const DBException& ex) {
            warning() << "Error encountered while stopping ping on " << processId
                      << causedBy(ex);
        }
    }

    void LegacyDistLockPinger::waitTillNextPingTime(stdx::chrono::milliseconds duration) {
        stdx::unique_lock<stdx::mutex> lk(_mutex);
        _pingStoppedCV.wait_for(lk, duration);
    }
}
