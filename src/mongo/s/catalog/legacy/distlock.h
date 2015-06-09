// distlock.h

/*    Copyright 2009 10gen Inc.
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

#pragma once

#include "mongo/client/connpool.h"
#include "mongo/client/syncclusterconnection.h"
#include "mongo/logger/labeled_level.h"
#include "mongo/s/catalog/dist_lock_manager.h"
#include "mongo/s/catalog/dist_lock_ping_info.h"

namespace mongo {

    namespace {

        enum TimeConstants {
            LOCK_TIMEOUT = 15 * 60 * 1000,
            LOCK_SKEW_FACTOR = 30,
            LOCK_PING = LOCK_TIMEOUT / LOCK_SKEW_FACTOR,
            MAX_LOCK_NET_SKEW = LOCK_TIMEOUT / LOCK_SKEW_FACTOR,
            MAX_LOCK_CLOCK_SKEW = LOCK_TIMEOUT / LOCK_SKEW_FACTOR,
            NUM_LOCK_SKEW_CHECKS = 3,
        };

        // The maximum clock skew we need to handle between config servers is
        // 2 * MAX_LOCK_NET_SKEW + MAX_LOCK_CLOCK_SKEW.

        // Net effect of *this* clock being slow is effectively a multiplier on the max net skew
        // and a linear increase or decrease of the max clock skew.
    }

    /**
     * Exception class to encapsulate exceptions while managing distributed locks
     */
    class LockException : public DBException {
    public:
        LockException(StringData msg, int code);

        /**
         * Use this to signal that a lock with lockID needs to be unlocked. For example, in cases
         * where the final lock acquisition was not propagated properly to all config servers.
         */
        LockException(StringData msg, int code, DistLockHandle lockID);

        /**
         * Returns the OID of the lock that needs to be unlocked.
         */
        DistLockHandle getMustUnlockID() const;

        virtual ~LockException() = default;

    private:
        // The identifier of a lock that needs to be unlocked.
        DistLockHandle _mustUnlockID;
    };

    /**
     * Indicates an error in retrieving time values from remote servers.
     */
    class TimeNotFoundException : public LockException {
    public:
        TimeNotFoundException( const char * msg , int code ) : LockException( msg, code ) {}
        TimeNotFoundException( const std::string& msg, int code ) : LockException( msg, code ) {}
        virtual ~TimeNotFoundException() = default;
    };

    /**
     * The distributed lock is a configdb backed way of synchronizing system-wide tasks. A task
     * must be identified by a unique name across the system (e.g., "balancer"). A lock is taken
     * by writing a document in the configdb's locks collection with that name.
     *
     * To be maintained, each taken lock needs to be revalidated ("pinged") within a
     * pre-established amount of time. This class does this maintenance automatically once a
     * DistributedLock object was constructed. The ping procedure records the local time to
     * the ping document, but that time is untrusted and is only used as a point of reference
     * of whether the ping was refreshed or not. Ultimately, the clock a configdb is the source
     * of truth when determining whether a ping is still fresh or not. This is achieved by
     * (1) remembering the ping document time along with config server time when unable to
     * take a lock, and (2) ensuring all config servers report similar times and have similar
     * time rates (the difference in times must start and stay small).
     *
     * Lock states include:
     * 0: unlocked
     * 1: about to be locked
     * 2: locked
     *
     * Valid state transitions:
     * 0 -> 1
     * 1 -> 2
     * 2 -> 0
     *
     * Note that at any point in time, a lock can be force unlocked if the ping for the lock
     * becomes too stale.
     */
    class DistributedLock {
    public:

        static logger::LabeledLevel logLvl;

        class LastPings {
        public:
            DistLockPingInfo getLastPing(const ConnectionString& conn,
                                         const std::string& lockName);
            void setLastPing(const ConnectionString& conn,
                             const std::string& lockName,
                             const DistLockPingInfo& pd);

            mongo::mutex _mutex;
            std::map< std::pair<std::string, std::string>, DistLockPingInfo > _lastPings;
        };

        static LastPings lastPings;

        /**
         * The constructor does not connect to the configdb yet and constructing does not mean the lock was acquired.
         * Construction does trigger a lock "pinging" mechanism, though.
         *
         * @param conn address of config(s) server(s)
         * @param name identifier for the lock
         * @param lockTimeout how long can the log go "unpinged" before a new attempt to lock steals it (in minutes).
         * @param lockPing how long to wait between lock pings
         * @param legacy use legacy logic
         *
         */
        DistributedLock( const ConnectionString& conn , const std::string& name , unsigned long long lockTimeout = 0, bool asProcess = false );
        ~DistributedLock(){};

        /**
         * Attempts to acquire 'this' lock, checking if it could or should be stolen from the previous holder. Please
         * consider using the dist_lock_try construct to acquire this lock in an exception safe way.
         *
         * @param why human readable description of why the lock is being taken (used to log)
         * @param other configdb's lock document that is currently holding the lock, if lock is taken, or our own lock
         * details if not
         * @return true if it managed to grab the lock
         */
        bool lock_try(const std::string& why, BSONObj* other = 0, double timeout = 0.0);

        /**
         * Returns OK if this lock is held (but does not guarantee that this owns it) and
         * it was possible to confirm that, within 'timeout' seconds, if provided, with the
         * config servers.
         */
        Status checkStatus(double timeout);

        /**
         * Releases a previously taken lock. Returns true on success.
         */
        bool unlock(const OID& lockID);

        Date_t getRemoteTime() const;

        bool isRemoteTimeSkewed() const;

        const std::string& getProcessId() const;

        const ConnectionString& getRemoteConnection() const;

        /**
         * Checks the skew among a cluster of servers and returns true if the min and max clock
         * times among the servers are within maxClockSkew.
         */
        static bool checkSkew( const ConnectionString& cluster,
                               unsigned skewChecks = NUM_LOCK_SKEW_CHECKS,
                               unsigned long long maxClockSkew = MAX_LOCK_CLOCK_SKEW,
                               unsigned long long maxNetSkew = MAX_LOCK_NET_SKEW );

        /**
         * Get the remote time from a server or cluster
         */
        static Date_t remoteTime( const ConnectionString& cluster, unsigned long long maxNetSkew = MAX_LOCK_NET_SKEW );

        /**
         * Namespace for lock pings
         */
        static const std::string lockPingNS;

        /**
         * Namespace for locks
         */
        static const std::string locksNS;

        const ConnectionString _conn;
        const std::string _name;
        const std::string _processId;

        // Timeout for lock, usually LOCK_TIMEOUT
        const unsigned long long _lockTimeout;
        const unsigned long long _maxClockSkew;
        const unsigned long long _maxNetSkew;
        const unsigned long long _lockPing;

    private:

        void resetLastPing() {
            lastPings.setLastPing(_conn, _name, DistLockPingInfo());
        }

        void setLastPing(const DistLockPingInfo& pd) {
            lastPings.setLastPing(_conn, _name, pd);
        }

        DistLockPingInfo getLastPing() {
            return lastPings.getLastPing(_conn, _name);
        }
    };

}

