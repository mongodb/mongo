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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/catalog/legacy/distlock.h"

#include "mongo/db/server_options.h"
#include "mongo/client/dbclientcursor.h"
#include "mongo/client/connpool.h"
#include "mongo/s/type_locks.h"
#include "mongo/s/type_lockpings.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/concurrency/threadlocal.h"
#include "mongo/util/log.h"
#include "mongo/util/timer.h"

namespace mongo {

    using std::endl;
    using std::list;
    using std::set;
    using std::string;
    using std::stringstream;
    using std::unique_ptr;
    using std::vector;

    LabeledLevel DistributedLock::logLvl( 1 );
    DistributedLock::LastPings DistributedLock::lastPings;

    ThreadLocalValue<string> distLockIds("");

    /* ==================
     * Module initialization
     */

    static SimpleMutex _cachedProcessMutex;
    static string* _cachedProcessString = NULL;

    static void initModule() {
        stdx::lock_guard<SimpleMutex> lk(_cachedProcessMutex);
        if (_cachedProcessString) {
            // someone got the lock before us
            return;
        }

        // cache process string
        stringstream ss;
        ss << getHostName() << ":" << serverGlobalParams.port << ":" << time(0) << ":" << rand();
        _cachedProcessString = new string( ss.str() );
    }

    /* =================== */

    string getDistLockProcess() {
        if (!_cachedProcessString)
            initModule();
        verify( _cachedProcessString );
        return *_cachedProcessString;
    }

    string getDistLockId() {
        string s = distLockIds.get();
        if ( s.empty() ) {
            stringstream ss;
            ss << getDistLockProcess() << ":" << getThreadName() << ":" << rand();
            s = ss.str();
            distLockIds.set( s );
        }
        return s;
    }

    LockException::LockException(StringData msg, int code): LockException(msg, code, OID()) {
    }

    LockException::LockException(StringData msg, int code, DistLockHandle lockID):
        DBException(msg.toString(), code),
        _mustUnlockID(lockID) {
    }

    DistLockHandle LockException::getMustUnlockID() const {
        return _mustUnlockID;
    }

    /**
     * Create a new distributed lock, potentially with a custom sleep and takeover time.  If a custom sleep time is
     * specified (time between pings)
     */
    DistributedLock::DistributedLock( const ConnectionString& conn , const string& name , unsigned long long lockTimeout, bool asProcess )
        : _conn(conn), _name(name),
          _processId( asProcess ? getDistLockId() : getDistLockProcess() ),
          _lockTimeout( lockTimeout == 0 ? LOCK_TIMEOUT : lockTimeout ),
          _maxClockSkew( _lockTimeout / LOCK_SKEW_FACTOR ), _maxNetSkew( _maxClockSkew ),
          _lockPing( _maxClockSkew )
    {
        LOG( logLvl ) << "created new distributed lock for " << name << " on " << conn
                      << " ( lock timeout : " << _lockTimeout
                      << ", ping interval : " << _lockPing << ", process : " << asProcess << " )" << endl;


    }

    DistLockPingInfo DistributedLock::LastPings::getLastPing(const ConnectionString& conn,
                                                             const string& lockName) {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        return _lastPings[std::make_pair(conn.toString(), lockName)];
    }

    void DistributedLock::LastPings::setLastPing(const ConnectionString& conn,
                                                 const string& lockName,
                                                 const DistLockPingInfo& pd) {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        _lastPings[std::make_pair(conn.toString(), lockName)] = pd;
    }

    Date_t DistributedLock::getRemoteTime() const {
        return DistributedLock::remoteTime(_conn, _maxNetSkew);
    }

    bool DistributedLock::isRemoteTimeSkewed() const {
        return !DistributedLock::checkSkew(_conn,
                                           NUM_LOCK_SKEW_CHECKS,
                                           _maxClockSkew,
                                           _maxNetSkew);
    }

    const ConnectionString& DistributedLock::getRemoteConnection() const {
        return _conn;
    }

    const string& DistributedLock::getProcessId() const {
        return _processId;
    }

    /**
     * Returns the remote time as reported by the cluster or server.  The maximum difference between the reported time
     * and the actual time on the remote server (at the completion of the function) is the maxNetSkew
     */
    Date_t DistributedLock::remoteTime( const ConnectionString& cluster, unsigned long long maxNetSkew ) {

        ConnectionString server( *cluster.getServers().begin() );

        // Get result and delay if successful, errMsg if not
        bool success = false;
        BSONObj result;
        string errMsg;
        Milliseconds delay{0};

        unique_ptr<ScopedDbConnection> connPtr;
        try {
            connPtr.reset( new ScopedDbConnection( server.toString() ) );
            ScopedDbConnection& conn = *connPtr;

            Date_t then = jsTime();
            success = conn->runCommand( string( "admin" ), BSON( "serverStatus" << 1 ), result );
            delay = jsTime() - then;

            if ( !success ) errMsg = result.toString();
            conn.done();
        }
        catch ( const DBException& ex ) {

            if ( connPtr && connPtr->get()->isFailed() ) {
                // Return to the pool so the pool knows about the failure
                connPtr->done();
            }

            success = false;
            errMsg = ex.toString();
        }
        
        if( !success ) {
            throw TimeNotFoundException( str::stream() << "could not get status from server "
                                                       << server.toString() << " in cluster "
                                                       << cluster.toString() << " to check time"
                                                       << causedBy( errMsg ),
                                         13647 );
        }

        // Make sure that our delay is not more than 2x our maximum network skew, since this is the max our remote
        // time value can be off by if we assume a response in the middle of the delay.
        if (delay > Milliseconds(maxNetSkew * 2)) {
            throw TimeNotFoundException( str::stream()
                                             << "server " << server.toString() << " in cluster "
                                             << cluster.toString()
                                             << " did not respond within max network delay of "
                                             << maxNetSkew << "ms",
                                         13648 );
        }

        return result["localTime"].Date() - (delay / 2);
    }

    bool DistributedLock::checkSkew( const ConnectionString& cluster, unsigned skewChecks, unsigned long long maxClockSkew, unsigned long long maxNetSkew ) {

        vector<HostAndPort> servers = cluster.getServers();

        if(servers.size() < 1) return true;

        vector<long long> avgSkews;

        for(unsigned i = 0; i < skewChecks; i++) {

            // Find the average skew for each server
            unsigned s = 0;
            for(vector<HostAndPort>::iterator si = servers.begin(); si != servers.end(); ++si,s++) {

                if(i == 0) avgSkews.push_back(0);

                // Could check if this is self, but shouldn't matter since local network connection should be fast.
                ConnectionString server( *si );

                vector<long long> skew;

                BSONObj result;

                Date_t remote = remoteTime( server, maxNetSkew );
                Date_t local = jsTime();

                // Remote time can be delayed by at most MAX_NET_SKEW

                // Skew is how much time we'd have to add to local to get to remote
                avgSkews[s] += (remote - local).count();

                LOG( logLvl + 1 ) << "skew from remote server " << server << " found: "
                                  << (remote - local).count();

            }
        }

        // Analyze skews

        long long serverMaxSkew = 0;
        long long serverMinSkew = 0;

        for(unsigned s = 0; s < avgSkews.size(); s++) {

            long long avgSkew = (avgSkews[s] /= skewChecks);

            // Keep track of max and min skews
            if(s == 0) {
                serverMaxSkew = avgSkew;
                serverMinSkew = avgSkew;
            }
            else {
                if(avgSkew > serverMaxSkew)
                    serverMaxSkew = avgSkew;
                if(avgSkew < serverMinSkew)
                    serverMinSkew = avgSkew;
            }

        }

        long long totalSkew = serverMaxSkew - serverMinSkew;

        // Make sure our max skew is not more than our pre-set limit
        if(totalSkew > (long long) maxClockSkew) {
            LOG( logLvl + 1 ) << "total clock skew of " << totalSkew << "ms for servers " << cluster << " is out of " << maxClockSkew << "ms bounds." << endl;
            return false;
        }

        LOG( logLvl + 1 ) << "total clock skew of " << totalSkew << "ms for servers " << cluster << " is in " << maxClockSkew << "ms bounds." << endl;
        return true;
    }

    Status DistributedLock::checkStatus(double timeout) {

        BSONObj lockObj;
        try {
            ScopedDbConnection conn(_conn.toString(), timeout );
            lockObj = conn->findOne( LocksType::ConfigNS,
                                     BSON( LocksType::name(_name) ) ).getOwned();
            conn.done();
        }
        catch ( DBException& e ) {
            return e.toStatus();
        }

        if ( lockObj.isEmpty() ) {
            return Status(ErrorCodes::LockFailed,
                    str::stream() << "no lock for " << _name << " exists in the locks collection");
        }

        if ( lockObj[LocksType::state()].numberInt() < 2 ) {
            return Status(ErrorCodes::LockFailed,
                          str::stream() << "lock " << _name << " current state is not held ("
                                        << lockObj[LocksType::state()].numberInt() << ")");
        }

        if ( lockObj[LocksType::process()].String() != _processId ) {
            return Status(ErrorCodes::LockFailed,
                          str::stream() << "lock " << _name << " is currently being held by "
                                        << "another process ("
                                        << lockObj[LocksType::process()].String() << ")");
        }

        return Status::OK();
    }

    static void logErrMsgOrWarn(StringData messagePrefix,
                                StringData lockName,
                                StringData errMsg,
                                StringData altErrMsg) {

        if (errMsg.empty()) {
            LOG(DistributedLock::logLvl - 1) << messagePrefix << " '" << lockName << "' " <<
                altErrMsg << std::endl;
        }
        else {
            warning() << messagePrefix << " '" << lockName << "' " << causedBy(errMsg.toString());
        }
    }

    // Semantics of this method are basically that if the lock cannot be acquired, returns false,
    // can be retried. If the lock should not be tried again (some unexpected error),
    // a LockException is thrown.
    bool DistributedLock::lock_try(const string& why, BSONObj* other, double timeout) {
        // This should always be true, if not, we are using the lock incorrectly.
        verify( _name != "" );

        LOG( logLvl ) << "trying to acquire new distributed lock for " << _name << " on " << _conn
                      << " ( lock timeout : " << _lockTimeout
                      << ", ping interval : " << _lockPing << ", process : " << _processId << " )"
                      << endl;

        // write to dummy if 'other' is null
        BSONObj dummyOther;
        if ( other == NULL )
            other = &dummyOther;

        ScopedDbConnection conn(_conn.toString(), timeout );

        BSONObjBuilder queryBuilder;
        queryBuilder.append( LocksType::name() , _name );
        queryBuilder.append( LocksType::state() , 0 );

        {
            // make sure its there so we can use simple update logic below
            BSONObj o = conn->findOne( LocksType::ConfigNS , BSON( LocksType::name(_name) ) ).getOwned();

            // Case 1: No locks
            if ( o.isEmpty() ) {
                try {
                    LOG( logLvl ) << "inserting initial doc in " << LocksType::ConfigNS << " for lock " << _name << endl;
                    conn->insert( LocksType::ConfigNS,
                                  BSON( LocksType::name(_name)
                                        << LocksType::state(0)
                                        << LocksType::who("")
                                        << LocksType::lockID(OID()) ));
                }
                catch ( UserException& e ) {
                    warning() << "could not insert initial doc for distributed lock " << _name << causedBy( e ) << endl;
                }
            }

            // Case 2: A set lock that we might be able to force
            else if ( o[LocksType::state()].numberInt() > 0 ) {

                string lockName = o[LocksType::name()].String() + string("/") + o[LocksType::process()].String();

                BSONObj lastPing = conn->findOne( LockpingsType::ConfigNS, o[LocksType::process()].wrap( LockpingsType::process() ) );
                if ( lastPing.isEmpty() ) {
                    LOG( logLvl ) << "empty ping found for process in lock '" << lockName << "'" << endl;
                    // TODO:  Using 0 as a "no time found" value Will fail if dates roll over, but then, so will a lot.
                    lastPing = BSON( LockpingsType::process(o[LocksType::process()].String()) <<
                                     LockpingsType::ping(Date_t()) );
                }

                unsigned long long elapsed = 0;
                unsigned long long takeover = _lockTimeout;
                DistLockPingInfo lastPingEntry = getLastPing();

                LOG(logLvl) << "checking last ping for lock '" << lockName
                            << "' against process " << lastPingEntry.processId
                            << " and ping " << lastPingEntry.lastPing;

                try {

                    Date_t remote = remoteTime( _conn );

                    auto pingDocProcessId = lastPing[LockpingsType::process()].String();
                    auto pingDocPingValue = lastPing[LockpingsType::ping()].Date();

                    // Timeout the elapsed time using comparisons of remote clock
                    // For non-finalized locks, timeout 15 minutes since last seen (ts)
                    // For finalized locks, timeout 15 minutes since last ping
                    bool recPingChange =
                            o[LocksType::state()].numberInt() == 2 &&
                            (lastPingEntry.processId != pingDocProcessId ||
                                    lastPingEntry.lastPing != pingDocPingValue);
                    bool recTSChange = lastPingEntry.lockSessionId != o[LocksType::lockID()].OID();

                    if (recPingChange || recTSChange) {
                        // If the ping has changed since we last checked, mark the current date and time
                        setLastPing(DistLockPingInfo(pingDocProcessId,
                                                     pingDocPingValue,
                                                     remote,
                                                     o[LocksType::lockID()].OID(),
                                                     OID()));
                    }
                    else {

                        // GOTCHA!  Due to network issues, it is possible that the current time
                        // is less than the remote time.  We *have* to check this here, otherwise
                        // we overflow and our lock breaks.
                        if (lastPingEntry.configLocalTime >= remote)
                            elapsed = 0;
                        else
                            elapsed = (remote - lastPingEntry.configLocalTime).count();
                    }
                }
                catch( LockException& e ) {

                    // Remote server cannot be found / is not responsive
                    warning() << "Could not get remote time from " << _conn << causedBy( e );
                    // If our config server is having issues, forget all the pings until we can see it again
                    resetLastPing();

                }

                if (elapsed <= takeover) {
                    LOG(1) << "could not force lock '" << lockName
                           << "' because elapsed time " << elapsed
                           << " <= takeover time " << takeover;
                    *other = o; other->getOwned(); conn.done();
                    return false;
                }

                LOG(0) << "forcing lock '" << lockName
                       << "' because elapsed time " << elapsed
                       << " > takeover time " << takeover;

                if( elapsed > takeover ) {

                    // Lock may forced, reset our timer if succeeds or fails
                    // Ensures that another timeout must happen if something borks up here, and resets our pristine
                    // ping state if acquired.
                    resetLastPing();

                    try {

                        // Check the clock skew again.  If we check this before we get a lock
                        // and after the lock times out, we can be pretty sure the time is
                        // increasing at the same rate on all servers and therefore our
                        // timeout is accurate
                        if (isRemoteTimeSkewed()) {
                            string msg(str::stream() << "remote time in cluster "
                                                     << _conn.toString()
                                                     << " is now skewed, cannot force lock.");
                            throw LockException(msg, ErrorCodes::DistributedClockSkewed);
                        }

                        // Make sure we break the lock with the correct "ts" (OID) value, otherwise
                        // we can overwrite a new lock inserted in the meantime.
                        conn->update( LocksType::ConfigNS,
                                      BSON( LocksType::name(_name) <<
                                            LocksType::state(o[LocksType::state()].numberInt()) <<
                                            LocksType::lockID(o[LocksType::lockID()].OID()) ),
                                      BSON( "$set" << BSON( LocksType::state(0) ) ) );

                        BSONObj err = conn->getLastErrorDetailed();
                        string errMsg = DBClientWithCommands::getLastErrorString(err);

                        // TODO: Clean up all the extra code to exit this method, probably with a refactor
                        if ( !errMsg.empty() || !err["n"].type() || err["n"].numberInt() < 1 ) {
                            logErrMsgOrWarn("Could not force lock", lockName, errMsg, "(another force won");
                            *other = o; other->getOwned(); conn.done();
                            return false;
                        }

                    }
                    catch( UpdateNotTheSame& ) {
                        // Ok to continue since we know we forced at least one lock document, and all lock docs
                        // are required for a lock to be held.
                        warning() << "lock forcing " << lockName << " inconsistent" << endl;
                    }
                    catch (const LockException& ) {
                        // Let the exception go up and don't repackage the exception.
                        throw;
                    }
                    catch( std::exception& e ) {
                        conn.done();
                        string msg(str::stream() << "exception forcing distributed lock "
                                                 << lockName << causedBy(e));
                        throw LockException(msg, 13660);
                    }

                }
                else {
                    // Not strictly necessary, but helpful for small timeouts where thread
                    // scheduling is significant. This ensures that two attempts are still
                    // required for a force if not acquired, and resets our state if we
                    // are acquired.
                    resetLastPing();

                    // Test that the lock is held by trying to update the finalized state of the lock to the same state
                    // if it does not update or does not update on all servers, we can't re-enter.
                    try {

                        // Test the lock with the correct "ts" (OID) value
                        conn->update( LocksType::ConfigNS,
                                      BSON( LocksType::name(_name) <<
                                            LocksType::state(2) <<
                                            LocksType::lockID(o[LocksType::lockID()].OID()) ),
                                      BSON( "$set" << BSON( LocksType::state(2) ) ) );

                        BSONObj err = conn->getLastErrorDetailed();
                        string errMsg = DBClientWithCommands::getLastErrorString(err);

                        // TODO: Clean up all the extra code to exit this method, probably with a refactor
                        if ( ! errMsg.empty() || ! err["n"].type() || err["n"].numberInt() < 1 ) {
                            logErrMsgOrWarn("Could not re-enter lock", lockName, errMsg, "(not sure lock is held");
                            *other = o; other->getOwned(); conn.done();
                            return false;
                        }

                    }
                    catch( UpdateNotTheSame& ) {
                        // NOT ok to continue since our lock isn't held by all servers, so isn't valid.
                        warning() << "inconsistent state re-entering lock, lock " << lockName << " not held" << endl;
                        *other = o; other->getOwned(); conn.done();
                        return false;
                    }
                    catch( std::exception& e ) {
                        conn.done();
                        string msg(str::stream() << "exception re-entering distributed lock "
                                                 << lockName << causedBy(e));
                        throw LockException(msg, 13660);
                    }

                    LOG( logLvl - 1 ) << "re-entered distributed lock '" << lockName << "'" << endl;
                    *other = o.getOwned(); 
                    conn.done();
                    return true;

                }

                LOG( logLvl - 1 ) << "lock '" << lockName << "' successfully forced" << endl;

                // We don't need the ts value in the query, since we will only ever replace locks with state=0.
            }
            // Case 3: We have an expired lock
            else if ( o[LocksType::lockID()].type() ) {
                queryBuilder.append( o[LocksType::lockID()] );
            }
        }

        // Always reset our ping if we're trying to get a lock, since getting a lock implies the lock state is open
        // and no locks need to be forced.  If anything goes wrong, we don't want to remember an old lock.
        resetLastPing();

        bool gotLock = false;
        BSONObj currLock;

        BSONObj lockDetails = BSON( LocksType::state(1)
                << LocksType::who(getDistLockId())
                << LocksType::process(_processId)
                << LocksType::when(jsTime())
                << LocksType::why(why)
                << LocksType::lockID(OID::gen()) );
        BSONObj whatIWant = BSON( "$set" << lockDetails );

        BSONObj query = queryBuilder.obj();

        string lockName = _name + string("/") + _processId;

        try {

            // Main codepath to acquire lock

            LOG( logLvl ) << "about to acquire distributed lock '" << lockName << "'";

            LOG( logLvl + 1 ) << "trying to acquire lock " << query.toString( false, true )
                              << " with details " << lockDetails.toString( false, true ) << endl;

            conn->update( LocksType::ConfigNS , query , whatIWant );

            BSONObj err = conn->getLastErrorDetailed();
            string errMsg = DBClientWithCommands::getLastErrorString(err);

            currLock = conn->findOne( LocksType::ConfigNS , BSON( LocksType::name(_name) ) );

            if ( !errMsg.empty() || !err["n"].type() || err["n"].numberInt() < 1 ) {
                logErrMsgOrWarn("could not acquire lock", lockName, errMsg, "(another update won)");
                *other = currLock;
                other->getOwned();
                gotLock = false;
            }
            else {
                gotLock = true;
            }

        }
        catch ( UpdateNotTheSame& up ) {

            // this means our update got through on some, but not others
            warning() << "distributed lock '" << lockName << " did not propagate properly." << causedBy( up ) << endl;

            // Overall protection derives from:
            // All unlocking updates use the ts value when setting state to 0
            //   This ensures that during locking, we can override all smaller ts locks with
            //   our own safe ts value and not be unlocked afterward.
            for ( unsigned i = 0; i < up.size(); i++ ) {

                ScopedDbConnection indDB(up[i].first);
                BSONObj indUpdate;

                try {

                    indUpdate = indDB->findOne( LocksType::ConfigNS , BSON( LocksType::name(_name) ) );

                    // If we override this lock in any way, grab and protect it.
                    // We assume/ensure that if a process does not have all lock documents, it is no longer
                    // holding the lock.
                    // Note - finalized locks may compete too, but we know they've won already if competing
                    // in this round.  Cleanup of crashes during finalizing may take a few tries.
                    if( indUpdate[LocksType::lockID()] < lockDetails[LocksType::lockID()] || indUpdate[LocksType::state()].numberInt() == 0 ) {

                        BSONObj grabQuery = BSON( LocksType::name(_name)
                                << LocksType::lockID(indUpdate[LocksType::lockID()].OID()) );

                        // Change ts so we won't be forced, state so we won't be relocked
                        BSONObj grabChanges = BSON( LocksType::lockID(lockDetails[LocksType::lockID()].OID())
                                << LocksType::state(1) );

                        // Either our update will succeed, and we'll grab the lock, or it will fail b/c some other
                        // process grabbed the lock (which will change the ts), but the lock will be set until forcing
                        indDB->update( LocksType::ConfigNS, grabQuery, BSON( "$set" << grabChanges ) );

                        indUpdate = indDB->findOne( LocksType::ConfigNS, BSON( LocksType::name(_name) ) );

                        // The tournament was interfered and it is not safe to proceed further.
                        // One case this could happen is when the LockPinger processes old
                        // entries from addUnlockOID. See SERVER-10688 for more detailed
                        // description of race.
                        if ( indUpdate[LocksType::state()].numberInt() <= 0 ) {
                            LOG( logLvl - 1 ) << "lock tournament interrupted, "
                                              << "so no lock was taken; "
                                              << "new state of lock: " << indUpdate << endl;

                            // We now break and set our currLock lockID value to zero, so that
                            // we know that we did not acquire the lock below. Later code will
                            // cleanup failed entries.
                            currLock = BSON(LocksType::lockID(OID()));
                            indDB.done();
                            break;
                        }
                    }
                    // else our lock is the same, in which case we're safe, or it's a bigger lock,
                    // in which case we won't need to protect anything since we won't have the lock.

                }
                catch( std::exception& e ) {
                    conn.done();
                    string msg(str::stream() << "distributed lock " << lockName
                                             << " had errors communicating with individual server "
                                             << up[1].first << causedBy(e));
                    throw LockException(msg, 13661);
                }

                verify( !indUpdate.isEmpty() );

                // Find max TS value
                if ( currLock.isEmpty() || currLock[LocksType::lockID()] < indUpdate[LocksType::lockID()] ) {
                    currLock = indUpdate.getOwned();
                }

                indDB.done();

            }

            // Locks on all servers are now set and safe until forcing

            if ( currLock[LocksType::lockID()] == lockDetails[LocksType::lockID()] ) {
                LOG( logLvl - 1 ) << "lock update won, completing lock propagation for '" << lockName << "'" << endl;
                gotLock = true;
            }
            else {
                LOG( logLvl - 1 ) << "lock update lost, lock '" << lockName << "' not propagated." << endl;
                gotLock = false;
            }
        }
        catch( std::exception& e ) {
            conn.done();
            string msg(str::stream() << "exception creating distributed lock "
                                     << lockName << causedBy(e));
            throw LockException(msg, 13663 );
        }

        // Complete lock propagation
        if( gotLock ) {

            // This is now safe, since we know that no new locks will be placed on top of the ones we've checked for at
            // least 15 minutes.  Sets the state = 2, so that future clients can determine that the lock is truly set.
            // The invariant for rollbacks is that we will never force locks with state = 2 and active pings, since that
            // indicates the lock is active, but this means the process creating/destroying them must explicitly poll
            // when something goes wrong.
            try {

                BSONObjBuilder finalLockDetails;
                BSONObjIterator bi( lockDetails );
                while( bi.more() ) {
                    BSONElement el = bi.next();
                    if( (string) ( el.fieldName() ) == LocksType::state() )
                        finalLockDetails.append( LocksType::state(), 2 );
                    else finalLockDetails.append( el );
                }

                conn->update( LocksType::ConfigNS , BSON( LocksType::name(_name) ) , BSON( "$set" << finalLockDetails.obj() ) );

                BSONObj err = conn->getLastErrorDetailed();
                string errMsg = DBClientWithCommands::getLastErrorString(err);

                currLock = conn->findOne( LocksType::ConfigNS , BSON( LocksType::name(_name) ) );

                if ( !errMsg.empty() || !err["n"].type() || err["n"].numberInt() < 1 ) {
                    warning() << "could not finalize winning lock " << lockName
                              << ( !errMsg.empty() ? causedBy( errMsg ) : " (did not update lock) " ) << endl;
                    gotLock = false;
                }
                else {
                    // SUCCESS!
                    gotLock = true;
                }

            }
            catch( std::exception& e ) {
                conn.done();
                string msg(str::stream() << "exception finalizing winning lock" << causedBy(e));
                // Inform caller about the potential orphan lock.
                throw LockException(msg,
                                    13662,
                                    lockDetails[LocksType::lockID()].OID());
            }

        }

        *other = currLock;
        other->getOwned();

        // Log our lock results
        if(gotLock)
            LOG( logLvl - 1 ) << "distributed lock '" << lockName <<
                "' acquired, ts : " << currLock[LocksType::lockID()].OID() << endl;
        else
            LOG( logLvl - 1 ) << "distributed lock '" << lockName << "' was not acquired." << endl;

        conn.done();

        return gotLock;
    }

    // This function *must not* throw exceptions, since it can be used in destructors - failure
    // results in queuing and trying again later.
    bool DistributedLock::unlock(const DistLockHandle& lockID) {

        verify(_name != "");
        string lockName = _name + string("/") + _processId;

        const int maxAttempts = 3;
        int attempted = 0;

        while ( ++attempted <= maxAttempts ) {

            // Awkward, but necessary since the constructor itself throws exceptions
            unique_ptr<ScopedDbConnection> connPtr;

            try {

                connPtr.reset( new ScopedDbConnection( _conn.toString() ) );
                ScopedDbConnection& conn = *connPtr;

                // Use ts when updating lock, so that new locks can be sure they won't get trampled.
                conn->update(LocksType::ConfigNS,
                             BSON(LocksType::name(_name)
                                  << LocksType::lockID(lockID)),
                             BSON("$set" << BSON(LocksType::state(0))));

                // Check that the lock was actually unlocked... if not, try again
                BSONObj err = conn->getLastErrorDetailed();
                string errMsg = DBClientWithCommands::getLastErrorString(err);

                if ( !errMsg.empty() || !err["n"].type() || err["n"].numberInt() < 1 ){
                    warning() << "distributed lock unlock update failed, retrying "
                              << ( errMsg.empty() ? causedBy( "( update not registered )" ) : causedBy( errMsg ) ) << endl;
                    conn.done();
                    continue;
                }

                LOG(0) << "distributed lock '" << lockName << "' unlocked. ";
                conn.done();
                return true;
            }
            catch (const UpdateNotTheSame&) {
                LOG(0) << "distributed lock '" << lockName << "' unlocked (messily). ";
                // This isn't a connection problem, so don't throw away the conn
                connPtr->done();
                return true;
            }
            catch ( std::exception& e) {
                warning() << "distributed lock '" << lockName << "' failed unlock attempt."
                          << causedBy( e ) <<  endl;

                // TODO:  If our lock timeout is small, sleeping this long may be unsafe.
                if( attempted != maxAttempts) sleepsecs(1 << attempted);
            }
        }

        return false;
    }

}
