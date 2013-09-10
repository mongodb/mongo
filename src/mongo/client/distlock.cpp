// @file distlock.h

/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "mongo/pch.h"

#include "mongo/client/distlock.h"

#include <boost/thread/thread.hpp>

#include "mongo/client/dbclientcursor.h"
#include "mongo/s/type_locks.h"
#include "mongo/s/type_lockpings.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/timer.h"

namespace mongo {

    LabeledLevel DistributedLock::logLvl( 1 );
    DistributedLock::LastPings DistributedLock::lastPings;

    ThreadLocalValue<string> distLockIds("");

    /* ==================
     * Module initialization
     */

    static SimpleMutex _cachedProcessMutex("distlock_initmodule");
    static string* _cachedProcessString = NULL;

    static void initModule() {
        SimpleMutex::scoped_lock lk(_cachedProcessMutex);
        if (_cachedProcessString) {
            // someone got the lock before us
            return;
        }

        // cache process string
        stringstream ss;
        ss << getHostName() << ":" << cmdLine.port << ":" << time(0) << ":" << rand();
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

    // Used for disabling the lock pinger during tests
    // Should *always* be true otherwise.
    static bool lockPingerEnabled = true;

    bool isLockPingerEnabled() { return lockPingerEnabled; }
    void setLockPingerEnabled(bool enabled) { lockPingerEnabled = enabled; }

    class DistributedLockPinger {
    public:

        DistributedLockPinger()
            : _mutex( "DistributedLockPinger" ) {
        }

        void _distLockPingThread( ConnectionString addr,
                                  const std::string& process,
                                  unsigned long long sleepTime ) {

            setThreadName( "LockPinger" );

            string pingId = pingThreadId( addr, process );

            LOG( DistributedLock::logLvl - 1 ) << "creating distributed lock ping thread for " << addr
                                               << " and process " << process
                                               << " (sleeping for " << sleepTime << "ms)" << endl;

            static int loops = 0;
            while( ! inShutdown() && ! shouldKill( addr, process ) ) {

                LOG( DistributedLock::logLvl + 2 ) << "distributed lock pinger '" << pingId << "' about to ping." << endl;

                Date_t pingTime;

                try {
                    ScopedDbConnection conn(addr.toString(), 30.0);

                    pingTime = jsTime();

                    // refresh the entry corresponding to this process in the lockpings collection
                    conn->update( LockpingsType::ConfigNS,
                                  BSON( LockpingsType::process(process) ),
                                  BSON( "$set" << BSON( LockpingsType::ping(pingTime) ) ),
                                  true );

                    string err = conn->getLastError();
                    if ( ! err.empty() ) {
                        warning() << "pinging failed for distributed lock pinger '" << pingId << "'."
                                  << causedBy( err ) << endl;
                        conn.done();

                        // Sleep for normal ping time
                        sleepmillis(sleepTime);
                        continue;
                    }

                    // remove really old entries from the lockpings collection if they're not holding a lock
                    // (this may happen if an instance of a process was taken down and no new instance came up to
                    // replace it for a quite a while)
                    // if the lock is taken, the take-over mechanism should handle the situation
                    auto_ptr<DBClientCursor> c = conn->query( LocksType::ConfigNS , BSONObj() );
                    // TODO:  Would be good to make clear whether query throws or returns empty on errors
                    uassert( 16060, str::stream() << "cannot query locks collection on config server " << conn.getHost(), c.get() );

                    set<string> pids;
                    while ( c->more() ) {
                        BSONObj lock = c->next();
                        if ( ! lock[LocksType::process()].eoo() ) {
                            pids.insert( lock[LocksType::process()].valuestrsafe() );
                        }
                    }

                    Date_t fourDays = pingTime - ( 4 * 86400 * 1000 ); // 4 days
                    conn->remove( LockpingsType::ConfigNS,
                                  BSON( LockpingsType::process() << NIN << pids <<
                                        LockpingsType::ping() << LT << fourDays ) );
                    err = conn->getLastError();
                    if ( ! err.empty() ) {
                        warning() << "ping cleanup for distributed lock pinger '" << pingId << " failed."
                                  << causedBy( err ) << endl;
                        conn.done();

                        // Sleep for normal ping time
                        sleepmillis(sleepTime);
                        continue;
                    }

                    // create index so remove is fast even with a lot of servers
                    if ( loops++ == 0 ) {
                        conn->ensureIndex( LockpingsType::ConfigNS, BSON( LockpingsType::ping() << 1 ) );
                    }

                    LOG( DistributedLock::logLvl - ( loops % 10 == 0 ? 1 : 0 ) ) << "cluster " << addr << " pinged successfully at " << pingTime
                            << " by distributed lock pinger '" << pingId
                            << "', sleeping for " << sleepTime << "ms" << endl;

                    // Remove old locks, if possible
                    // Make sure no one else is adding to this list at the same time
                    scoped_lock lk( _mutex );

                    int numOldLocks = _oldLockOIDs.size();
                    if( numOldLocks > 0 ) {
                        LOG( DistributedLock::logLvl - 1 ) << "trying to delete " << _oldLockOIDs.size() << " old lock entries for process " << process << endl;
                    }

                    bool removed = false;
                    for( list<OID>::iterator i = _oldLockOIDs.begin(); i != _oldLockOIDs.end();
                            i = ( removed ? _oldLockOIDs.erase( i ) : ++i ) ) {
                        removed = false;
                        try {
                            // Got OID from lock with id, so we don't need to specify id again
                            conn->update( LocksType::ConfigNS ,
                                          BSON( LocksType::lockID(*i) ),
                                          BSON( "$set" << BSON( LocksType::state(0) ) ) );

                            // Either the update went through or it didn't, either way we're done trying to
                            // unlock
                            LOG( DistributedLock::logLvl - 1 ) << "handled late remove of old distributed lock with ts " << *i << endl;
                            removed = true;
                        }
                        catch( UpdateNotTheSame& ) {
                            LOG( DistributedLock::logLvl - 1 ) << "partially removed old distributed lock with ts " << *i << endl;
                            removed = true;
                        }
                        catch ( std::exception& e) {
                            warning() << "could not remove old distributed lock with ts " << *i
                                      << causedBy( e ) <<  endl;
                        }

                    }

                    if( numOldLocks > 0 && _oldLockOIDs.size() > 0 ){
                        LOG( DistributedLock::logLvl - 1 ) << "not all old lock entries could be removed for process " << process << endl;
                    }

                    conn.done();

                }
                catch ( std::exception& e ) {
                    warning() << "distributed lock pinger '" << pingId << "' detected an exception while pinging."
                              << causedBy( e ) << endl;
                }

                sleepmillis(sleepTime);
            }

            warning() << "removing distributed lock ping thread '" << pingId << "'" << endl;


            if( shouldKill( addr, process ) )
                finishKill( addr, process );

        }

        void distLockPingThread( ConnectionString addr,
                                 long long clockSkew,
                                 const std::string& processId,
                                 unsigned long long sleepTime ) {
            try {
                jsTimeVirtualThreadSkew( clockSkew );
                _distLockPingThread( addr, processId, sleepTime );
            }
            catch ( std::exception& e ) {
                error() << "unexpected error while running distributed lock pinger for " << addr << ", process " << processId << causedBy( e ) << endl;
            }
            catch ( ... ) {
                error() << "unknown error while running distributed lock pinger for " << addr << ", process " << processId << endl;
            }
        }

        string pingThreadId( const ConnectionString& conn, const string& processId ) {
            return conn.toString() + "/" + processId;
        }

        string got( DistributedLock& lock, unsigned long long sleepTime ) {

            if (!lockPingerEnabled) return "";

            // Make sure we don't start multiple threads for a process id
            scoped_lock lk( _mutex );

            const ConnectionString& conn = lock.getRemoteConnection();
            const string& processId = lock.getProcessId();
            string s = pingThreadId( conn, processId );

            // Ignore if we already have a pinging thread for this process.
            if ( _seen.count( s ) > 0 ) return s;

            // Check our clock skew
            try {
                if( lock.isRemoteTimeSkewed() ) {
                    throw LockException( str::stream() << "clock skew of the cluster " << conn.toString() << " is too far out of bounds to allow distributed locking." , 13650 );
                }
            }
            catch( LockException& e) {
                throw LockException( str::stream() << "error checking clock skew of cluster " << conn.toString() << causedBy( e ) , 13651);
            }

            boost::thread t( boost::bind( &DistributedLockPinger::distLockPingThread, this, conn, getJSTimeVirtualThreadSkew(), processId, sleepTime) );

            _seen.insert( s );

            return s;
        }

        void addUnlockOID( const OID& oid ) {
            // Modifying the lock from some other thread
            scoped_lock lk( _mutex );
            _oldLockOIDs.push_back( oid );
        }

        bool willUnlockOID( const OID& oid ) {
            scoped_lock lk( _mutex );
            return find( _oldLockOIDs.begin(), _oldLockOIDs.end(), oid ) != _oldLockOIDs.end();
        }

        void kill( const ConnectionString& conn, const string& processId ) {
            // Make sure we're in a consistent state before other threads can see us
            scoped_lock lk( _mutex );

            string pingId = pingThreadId( conn, processId );

            verify( _seen.count( pingId ) > 0 );
            _kill.insert( pingId );

        }

        bool shouldKill( const ConnectionString& conn, const string& processId ) {
            scoped_lock lk( _mutex );
            return _kill.count( pingThreadId( conn, processId ) ) > 0;
        }

        void finishKill( const ConnectionString& conn, const string& processId ) {
            // Make sure we're in a consistent state before other threads can see us
            scoped_lock lk( _mutex );

            string pingId = pingThreadId( conn, processId );

            _kill.erase( pingId );
            _seen.erase( pingId );

        }

    private:
        // Protects all of the members below.
        mongo::mutex _mutex;
        set<string> _kill;
        set<string> _seen;
        list<OID> _oldLockOIDs;

    } distLockPinger;

    /**
     * Create a new distributed lock, potentially with a custom sleep and takeover time.  If a custom sleep time is
     * specified (time between pings)
     */
    DistributedLock::DistributedLock( const ConnectionString& conn , const string& name , unsigned long long lockTimeout, bool asProcess )
        : _conn(conn), _name(name),
          _processId( asProcess ? getDistLockId() : getDistLockProcess() ),
          _lockTimeout( lockTimeout == 0 ? LOCK_TIMEOUT : lockTimeout ),
          _maxClockSkew( _lockTimeout / LOCK_SKEW_FACTOR ), _maxNetSkew( _maxClockSkew ),
          _lockPing( _maxClockSkew ), _mutex( "DistributedLock" )
    {
        LOG( logLvl ) << "created new distributed lock for " << name << " on " << conn
                      << " ( lock timeout : " << _lockTimeout
                      << ", ping interval : " << _lockPing << ", process : " << asProcess << " )" << endl;


    }

    DistributedLock::PingData DistributedLock::LastPings::getLastPing( const ConnectionString& conn, const string& lockName ){
        scoped_lock lock( _mutex );
        return _lastPings[ std::pair< string, string >( conn.toString(), lockName ) ];
    }

    void DistributedLock::LastPings::setLastPing( const ConnectionString& conn, const string& lockName, const PingData& pd ){
        scoped_lock lock( _mutex );
        _lastPings[ std::pair< string, string >( conn.toString(), lockName ) ] = pd;
    }

    Date_t DistributedLock::getRemoteTime() {
        return DistributedLock::remoteTime( _conn, _maxNetSkew );
    }

    bool DistributedLock::isRemoteTimeSkewed() {
        return !DistributedLock::checkSkew( _conn, NUM_LOCK_SKEW_CHECKS, _maxClockSkew, _maxNetSkew );
    }

    const ConnectionString& DistributedLock::getRemoteConnection() {
        return _conn;
    }

    const string& DistributedLock::getProcessId() {
        return _processId;
    }

    /**
     * Returns the remote time as reported by the cluster or server.  The maximum difference between the reported time
     * and the actual time on the remote server (at the completion of the function) is the maxNetSkew
     */
    Date_t DistributedLock::remoteTime( const ConnectionString& cluster, unsigned long long maxNetSkew ) {

        ConnectionString server( *cluster.getServers().begin() );
        ScopedDbConnection conn(server.toString());

        BSONObj result;
        long long delay;

        try {
            Date_t then = jsTime();
            bool success = conn->runCommand( string("admin"),
                                             BSON( "serverStatus" << 1 ),
                                             result );
            delay = jsTime() - then;

            if( !success )
                throw TimeNotFoundException( str::stream() << "could not get status from server "
                                             << server.toString() << " in cluster " << cluster.toString()
                                             << " to check time", 13647 );

            // Make sure that our delay is not more than 2x our maximum network skew, since this is the max our remote
            // time value can be off by if we assume a response in the middle of the delay.
            if( delay > (long long) (maxNetSkew * 2) )
                throw TimeNotFoundException( str::stream() << "server " << server.toString()
                                             << " in cluster " << cluster.toString()
                                             << " did not respond within max network delay of "
                                             << maxNetSkew << "ms", 13648 );
        }
        catch(...) {
            conn.done();
            throw;
        }

        conn.done();

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
                avgSkews[s] += (long long) (remote - local);

                LOG( logLvl + 1 ) << "skew from remote server " << server << " found: " << (long long) (remote - local) << endl;

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

    // For use in testing, ping thread should run indefinitely in practice.
    bool DistributedLock::killPinger( DistributedLock& lock ) {
        if( lock._threadId == "") return false;

        distLockPinger.kill( lock._conn, lock._processId );
        return true;
    }


    bool DistributedLock::isLockHeld( double timeout, string* errMsg ) {
        ScopedDbConnection conn(_conn.toString(), timeout );

        BSONObj lockObj;
        try {
            lockObj = conn->findOne( LocksType::ConfigNS,
                                     BSON( LocksType::name(_name) ) ).getOwned();
        }
        catch ( DBException& e ) {
            *errMsg = str::stream() << "error checking whether lock " << _name << " is held "
                                    << causedBy( e );
            return false;
        }
        conn.done();

        if ( lockObj.isEmpty() ) {
            *errMsg = str::stream() << "no lock for " << _name << " exists in the locks collection";
            return false;
        }

        if ( lockObj[LocksType::state()].numberInt() < 2 ) {
            *errMsg = str::stream() << "lock " << _name << " current state is not held ("
                                    << lockObj[LocksType::state()].numberInt() << ")";
            return false;
        }

        if ( lockObj[LocksType::process()].String() != _processId ) {
            *errMsg = str::stream() << "lock " << _name << " is currently being held by "
                                    << "another process ("
                                    << lockObj[LocksType::process()].String() << ")";
            return false;
        }

        if ( distLockPinger.willUnlockOID( lockObj[LocksType::lockID()].OID() ) ) {
            *errMsg = str::stream() << "lock " << _name << " is not held and is currently being "
                                    << "scheduled for lazy unlock by "
                                    << lockObj[LocksType::lockID()].OID();
            return false;
        }

        return true;
    }

    static void logErrMsgOrWarn(const StringData& messagePrefix,
                                const StringData& lockName,
                                const StringData& errMsg,
                                const StringData& altErrMsg) {

        if (errMsg.empty()) {
            LOG(DistributedLock::logLvl - 1) << messagePrefix << " '" << lockName << "' " <<
                altErrMsg << std::endl;
        }
        else {
            warning() << messagePrefix << " '" << lockName << "' " << causedBy(errMsg.toString());
        }
    }

    // Semantics of this method are basically that if the lock cannot be acquired, returns false, can be retried.
    // If the lock should not be tried again (some unexpected error) a LockException is thrown.
    // If we are only trying to re-enter a currently held lock, reenter should be true.
    // Note:  reenter doesn't actually make this lock re-entrant in the normal sense, since it can still only
    // be unlocked once, instead it is used to verify that the lock is already held.
    bool DistributedLock::lock_try( const string& why , bool reenter, BSONObj * other, double timeout ) {

        // TODO:  Start pinging only when we actually get the lock?
        // If we don't have a thread pinger, make sure we shouldn't have one
        if( _threadId == "" ){
            scoped_lock lk( _mutex );
            _threadId = distLockPinger.got( *this, _lockPing );
        }

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
                    conn->insert( LocksType::ConfigNS , BSON( LocksType::name(_name) << LocksType::state(0) << LocksType::who("") ) );
                }
                catch ( UserException& e ) {
                    warning() << "could not insert initial doc for distributed lock " << _name << causedBy( e ) << endl;
                }
            }

            // Case 2: A set lock that we might be able to force
            else if ( o[LocksType::state()].numberInt() > 0 ) {

                string lockName = o[LocksType::name()].String() + string("/") + o[LocksType::process()].String();

                bool canReenter = reenter &&
                                  o[LocksType::process()].String() == _processId &&
                                  !distLockPinger.willUnlockOID( o[LocksType::lockID()].OID() ) &&
                                  o[LocksType::state()].numberInt() == 2;
                if( reenter && ! canReenter ) {

                    LOG( logLvl - 1 ) << "not re-entering distributed lock " << lockName;
                    if ( o[LocksType::process()].String() != _processId ) {
                        LOG( logLvl - 1 ) << ", different process " << _processId << endl;
                    }
                    else if ( o[LocksType::state()].numberInt() == 2 ) {
                        LOG( logLvl - 1 ) << ", state not finalized" << endl;
                    }
                    else {
                        LOG( logLvl - 1 ) << ", ts " << o[LocksType::lockID()].OID() << " scheduled for late unlock" << endl;
                    }

                    // reset since we've been bounced by a previous lock not being where we thought it was,
                    // and should go through full forcing process if required.
                    // (in theory we should never see a ping here if used correctly)
                    *other = o; other->getOwned(); conn.done(); resetLastPing();
                    return false;
                }

                BSONObj lastPing = conn->findOne( LockpingsType::ConfigNS, o[LocksType::process()].wrap( LockpingsType::process() ) );
                if ( lastPing.isEmpty() ) {
                    LOG( logLvl ) << "empty ping found for process in lock '" << lockName << "'" << endl;
                    // TODO:  Using 0 as a "no time found" value Will fail if dates roll over, but then, so will a lot.
                    lastPing = BSON( LockpingsType::process(o[LocksType::process()].String()) <<
                                     LockpingsType::ping((Date_t) 0) );
                }

                unsigned long long elapsed = 0;
                unsigned long long takeover = _lockTimeout;
                PingData _lastPingCheck = getLastPing();

                LOG( logLvl ) << "checking last ping for lock '" << lockName << "'" << " against process " << _lastPingCheck.id << " and ping " << _lastPingCheck.lastPing << endl;

                try {

                    Date_t remote = remoteTime( _conn );

                    // Timeout the elapsed time using comparisons of remote clock
                    // For non-finalized locks, timeout 15 minutes since last seen (ts)
                    // For finalized locks, timeout 15 minutes since last ping
                    bool recPingChange = o[LocksType::state()].numberInt() == 2 &&
                                         ( _lastPingCheck.id != lastPing[LockpingsType::process()].String() ||
                                           _lastPingCheck.lastPing != lastPing[LockpingsType::ping()].Date() );
                    bool recTSChange = _lastPingCheck.ts != o[LocksType::lockID()].OID();

                    if( recPingChange || recTSChange ) {
                        // If the ping has changed since we last checked, mark the current date and time
                        setLastPing( PingData( lastPing[LockpingsType::process()].String().c_str(),
                                               lastPing[LockpingsType::ping()].Date(),
                                               remote, o[LocksType::lockID()].OID() ) );
                    }
                    else {

                        // GOTCHA!  Due to network issues, it is possible that the current time
                        // is less than the remote time.  We *have* to check this here, otherwise
                        // we overflow and our lock breaks.
                        if(_lastPingCheck.remote >= remote)
                            elapsed = 0;
                        else
                            elapsed = remote - _lastPingCheck.remote;
                    }
                }
                catch( LockException& e ) {

                    // Remote server cannot be found / is not responsive
                    warning() << "Could not get remote time from " << _conn << causedBy( e );
                    // If our config server is having issues, forget all the pings until we can see it again
                    resetLastPing();

                }

                if ( elapsed <= takeover && ! canReenter ) {
                    LOG( logLvl ) << "could not force lock '" << lockName << "' because elapsed time " << elapsed << " <= takeover time " << takeover << endl;
                    *other = o; other->getOwned(); conn.done();
                    return false;
                }
                else if( elapsed > takeover && canReenter ) {
                    LOG( logLvl - 1 ) << "not re-entering distributed lock " << lockName << "' because elapsed time " << elapsed << " > takeover time " << takeover << endl;
                    *other = o; other->getOwned(); conn.done();
                    return false;
                }

                LOG( logLvl - 1 ) << ( canReenter ? "re-entering" : "forcing" ) << " lock '" << lockName << "' because "
                                  << ( canReenter ? "re-entering is allowed, " : "" )
                                  << "elapsed time " << elapsed << " > takeover time " << takeover << endl;

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
                        uassert( 14023, str::stream() << "remote time in cluster " << _conn.toString() << " is now skewed, cannot force lock.", !isRemoteTimeSkewed() );

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
                    catch( std::exception& e ) {
                        conn.done();
                        throw LockException( str::stream() << "exception forcing distributed lock "
                                             << lockName << causedBy( e ), 13660);
                    }

                }
                else {

                    verify( canReenter );

                    // Lock may be re-entered, reset our timer if succeeds or fails
                    // Not strictly necessary, but helpful for small timeouts where thread scheduling is significant.
                    // This ensures that two attempts are still required for a force if not acquired, and resets our
                    // state if we are acquired.
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
                        throw LockException( str::stream() << "exception re-entering distributed lock "
                                             << lockName << causedBy( e ), 13660);
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
                << "when" << jsTime()
                << LocksType::why(why)
                << LocksType::lockID(OID::gen()) );
        BSONObj whatIWant = BSON( "$set" << lockDetails );

        BSONObj query = queryBuilder.obj();

        string lockName = _name + string("/") + _processId;

        try {

            // Main codepath to acquire lock

            LOG( logLvl ) << "about to acquire distributed lock '" << lockName << ":\n"
                          <<  lockDetails.jsonString(Strict, true) << "\n"
                          << query.jsonString(Strict, true) << endl;

            conn->update( LocksType::ConfigNS , query , whatIWant );

            BSONObj err = conn->getLastErrorDetailed();
            string errMsg = DBClientWithCommands::getLastErrorString(err);

            currLock = conn->findOne( LocksType::ConfigNS , BSON( LocksType::name(_name) ) );

            if ( !errMsg.empty() || !err["n"].type() || err["n"].numberInt() < 1 ) {
                logErrMsgOrWarn("could not acquire lock", lockName, errMsg, "(another update won");
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
                    throw LockException( str::stream() << "distributed lock " << lockName
                                         << " had errors communicating with individual server "
                                         << up[1].first << causedBy( e ), 13661 );
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

                // Register the lock for deletion, to speed up failover
                // Not strictly necessary, but helpful
                distLockPinger.addUnlockOID( lockDetails[LocksType::lockID()].OID() );

                gotLock = false;
            }
        }
        catch( std::exception& e ) {
            conn.done();
            throw LockException( str::stream() << "exception creating distributed lock "
                                 << lockName << causedBy( e ), 13663 );
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

                // Register the bad final lock for deletion, in case it exists
                distLockPinger.addUnlockOID( lockDetails[LocksType::lockID()].OID() );

                throw LockException( str::stream() << "exception finalizing winning lock"
                                     << causedBy( e ), 13662 );
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

    // Unlock now takes an optional pointer to the lock, so you can be specific about which
    // particular lock you want to unlock.  This is required when the config server is down,
    // and so cannot tell you what lock ts you should try later.
    void DistributedLock::unlock( BSONObj* oldLockPtr ) {

        verify( _name != "" );

        string lockName = _name + string("/") + _processId;

        const int maxAttempts = 3;
        int attempted = 0;

        BSONObj oldLock;
        if( oldLockPtr ) oldLock = *oldLockPtr;

        while ( ++attempted <= maxAttempts ) {

            ScopedDbConnection conn(_conn.toString());

            try {

                if( oldLock.isEmpty() )
                    oldLock = conn->findOne( LocksType::ConfigNS, BSON( LocksType::name(_name) ) );

                if( oldLock[LocksType::state()].eoo() ||
                    oldLock[LocksType::state()].numberInt() != 2 ||
                    oldLock[LocksType::lockID()].eoo() ) {
                    warning() << "cannot unlock invalid distributed lock " << oldLock << endl;
                    conn.done();
                    break;
                }

                // Use ts when updating lock, so that new locks can be sure they won't get trampled.
                conn->update( LocksType::ConfigNS ,
                              BSON( LocksType::name(_name) <<
                                    LocksType::lockID(oldLock[LocksType::lockID()].OID()) ),
                              BSON( "$set" << BSON( LocksType::state(0) ) ) );

                // Check that the lock was actually unlocked... if not, try again
                BSONObj err = conn->getLastErrorDetailed();
                string errMsg = DBClientWithCommands::getLastErrorString(err);

                if ( !errMsg.empty() || !err["n"].type() || err["n"].numberInt() < 1 ){
                    warning() << "distributed lock unlock update failed, retrying "
                              << ( errMsg.empty() ? causedBy( "( update not registered )" ) : causedBy( errMsg ) ) << endl;
                    conn.done();
                    continue;
                }

                LOG( logLvl - 1 ) << "distributed lock '" << lockName << "' unlocked. " << endl;
                conn.done();
                return;
            }
            catch( UpdateNotTheSame& ) {
                LOG( logLvl - 1 ) << "distributed lock '" << lockName << "' unlocked (messily). " << endl;
                conn.done();
                break;
            }
            catch ( std::exception& e) {
                warning() << "distributed lock '" << lockName << "' failed unlock attempt."
                          << causedBy( e ) <<  endl;

                conn.done();
                // TODO:  If our lock timeout is small, sleeping this long may be unsafe.
                if( attempted != maxAttempts) sleepsecs(1 << attempted);
            }
        }

        if( attempted > maxAttempts && ! oldLock.isEmpty() && ! oldLock[LocksType::lockID()].eoo() ) {

            LOG( logLvl - 1 ) << "could not unlock distributed lock with ts " <<
                oldLock[LocksType::lockID()].OID() << ", will attempt again later" << endl;

            // We couldn't unlock the lock at all, so try again later in the pinging thread...
            distLockPinger.addUnlockOID( oldLock[LocksType::lockID()].OID() );
        }
        else if( attempted > maxAttempts ) {
            warning() << "could not unlock untracked distributed lock, a manual force may be required" << endl;
        }

        warning() << "distributed lock '" << lockName << "' couldn't consummate unlock request. "
                  << "lock may be taken over after " << ( _lockTimeout / (60 * 1000) )
                  << " minutes timeout." << endl;
    }

    ScopedDistributedLock::ScopedDistributedLock(const ConnectionString& conn, const string& name) :
            _lock(conn, name), _why(""), _lockTryIntervalMillis(1000), _acquired(false)
    {
    }

    ScopedDistributedLock::~ScopedDistributedLock() {
        if (_acquired) {
            unlock();
        }
    }

    bool ScopedDistributedLock::tryAcquire(string* errMsg) {
        try {
            _acquired = _lock.lock_try(_why, false, &_other);
        }
        catch (const DBException& e) {

            *errMsg = str::stream() << "error acquiring distributed lock " << _lock._name << " for "
                                    << _why << causedBy(e);

            return false;
        }

        return _acquired;
    }

    void ScopedDistributedLock::unlock() {
        _lock.unlock(&_other);
    }

    bool ScopedDistributedLock::acquire(long long waitForMillis, string* errMsg) {

        string dummy;
        if (!errMsg) errMsg = &dummy;

        Timer timer;
        Timer msgTimer;

        while (!_acquired && (waitForMillis <= 0 || timer.millis() < waitForMillis)) {

            string acquireErrMsg;
            _acquired = tryAcquire(&acquireErrMsg);

            if (_acquired) break;

            // Set our error message to the last error, in case we break with !_acquired
            *errMsg = acquireErrMsg;

            if (waitForMillis == 0) break;

            // Periodically message for debugging reasons
            if (msgTimer.seconds() > 10) {

                log() << "waited " << timer.seconds() << "s for distributed lock " << _lock._name
                      << " for " << _why << endl;

                msgTimer.reset();
            }

            long long timeRemainingMillis = std::max(0LL, waitForMillis - timer.millis());
            sleepmillis(std::min(_lockTryIntervalMillis, timeRemainingMillis));
        }

        if (_acquired) {
            verify(!_other.isEmpty());
            return true;
        }

        *errMsg = str::stream() << "could not acquire distributed lock " << _lock._name << " for "
                                << _why << " after " << timer.seconds()
                                << "s, other lock may be held: " << _other << causedBy(errMsg);

        return false;
    }

}
