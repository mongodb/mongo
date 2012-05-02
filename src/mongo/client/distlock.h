// distlock.h

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

#pragma once

#include "../pch.h"
#include "connpool.h"
#include "syncclusterconnection.h"

#define LOCK_TIMEOUT (15 * 60 * 1000)
#define LOCK_SKEW_FACTOR (30)
#define LOCK_PING (LOCK_TIMEOUT / LOCK_SKEW_FACTOR)
#define MAX_LOCK_NET_SKEW (LOCK_TIMEOUT / LOCK_SKEW_FACTOR)
#define MAX_LOCK_CLOCK_SKEW (LOCK_TIMEOUT / LOCK_SKEW_FACTOR)
#define NUM_LOCK_SKEW_CHECKS (3)

// The maximum clock skew we need to handle between config servers is
// 2 * MAX_LOCK_NET_SKEW + MAX_LOCK_CLOCK_SKEW.

// Net effect of *this* clock being slow is effectively a multiplier on the max net skew
// and a linear increase or decrease of the max clock skew.

namespace mongo {

    /**
     * Exception class to encapsulate exceptions while managing distributed locks
     */
    class LockException : public DBException {
    public:
    	LockException( const char * msg , int code ) : DBException( msg, code ) {}
    	LockException( const string& msg, int code ) : DBException( msg, code ) {}
    	virtual ~LockException() throw() { }
    };

    /**
     * Indicates an error in retrieving time values from remote servers.
     */
    class TimeNotFoundException : public LockException {
    public:
        TimeNotFoundException( const char * msg , int code ) : LockException( msg, code ) {}
        TimeNotFoundException( const string& msg, int code ) : LockException( msg, code ) {}
        virtual ~TimeNotFoundException() throw() { }
    };

    /**
     * The distributed lock is a configdb backed way of synchronizing system-wide tasks. A task must be identified by a
     * unique name across the system (e.g., "balancer"). A lock is taken by writing a document in the configdb's locks
     * collection with that name.
     *
     * To be maintained, each taken lock needs to be revalidaded ("pinged") within a pre-established amount of time. This
     * class does this maintenance automatically once a DistributedLock object was constructed.
     */
    class DistributedLock {
    public:

    	static LabeledLevel logLvl;

        struct PingData {
            
            PingData( const string& _id , Date_t _lastPing , Date_t _remote , OID _ts ) 
                : id(_id), lastPing(_lastPing), remote(_remote), ts(_ts){
            }

            PingData()
                : id(""), lastPing(0), remote(0), ts(){
            }
            
            string id;
            Date_t lastPing;
            Date_t remote;
            OID ts;
        };
        
    	class LastPings {
    	public:
    	    LastPings() : _mutex( "DistributedLock::LastPings" ) {}
    	    ~LastPings(){}

    	    PingData getLastPing( const ConnectionString& conn, const string& lockName );
    	    void setLastPing( const ConnectionString& conn, const string& lockName, const PingData& pd );

    	    mongo::mutex _mutex;
    	    map< std::pair<string, string>, PingData > _lastPings;
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
        DistributedLock( const ConnectionString& conn , const string& name , unsigned long long lockTimeout = 0, bool asProcess = false );
        ~DistributedLock(){};

        /**
         * Attempts to acquire 'this' lock, checking if it could or should be stolen from the previous holder. Please
         * consider using the dist_lock_try construct to acquire this lock in an exception safe way.
         *
         * @param why human readable description of why the lock is being taken (used to log)
         * @param whether this is a lock re-entry or a new lock
         * @param other configdb's lock document that is currently holding the lock, if lock is taken, or our own lock
         * details if not
         * @return true if it managed to grab the lock
         */
        bool lock_try( const string& why , bool reenter = false, BSONObj * other = 0 );

        /**
         * Releases a previously taken lock.
         */
        void unlock( BSONObj* oldLockPtr = NULL );

        Date_t getRemoteTime();

        bool isRemoteTimeSkewed();

        const string& getProcessId();

        const ConnectionString& getRemoteConnection();

        /**
         * Check the skew between a cluster of servers
         */
        static bool checkSkew( const ConnectionString& cluster, unsigned skewChecks = NUM_LOCK_SKEW_CHECKS, unsigned long long maxClockSkew = MAX_LOCK_CLOCK_SKEW, unsigned long long maxNetSkew = MAX_LOCK_NET_SKEW );

        /**
         * Get the remote time from a server or cluster
         */
        static Date_t remoteTime( const ConnectionString& cluster, unsigned long long maxNetSkew = MAX_LOCK_NET_SKEW );

        static bool killPinger( DistributedLock& lock );

        /**
         * Namespace for lock pings
         */
        static const string lockPingNS;

        /**
         * Namespace for locks
         */
        static const string locksNS;

        const ConnectionString _conn;
        const string _name;
        const BSONObj _id;
        const string _processId;

        // Timeout for lock, usually LOCK_TIMEOUT
        const unsigned long long _lockTimeout;
        const unsigned long long _maxClockSkew;
        const unsigned long long _maxNetSkew;
        const unsigned long long _lockPing;

    private:

        void resetLastPing(){ lastPings.setLastPing( _conn, _name, PingData() ); }
        void setLastPing( const PingData& pd ){ lastPings.setLastPing( _conn, _name, pd ); }
        PingData getLastPing(){ return lastPings.getLastPing( _conn, _name ); }

        // May or may not exist, depending on startup
        mongo::mutex _mutex;
        string _threadId;

    };

    class dist_lock_try {
    public:

    	dist_lock_try() : _lock(NULL), _got(false) {}

    	dist_lock_try( const dist_lock_try& that ) : _lock(that._lock), _got(that._got), _other(that._other) {
    		_other.getOwned();

    		// Make sure the lock ownership passes to this object,
    		// so we only unlock once.
    		((dist_lock_try&) that)._got = false;
    		((dist_lock_try&) that)._lock = NULL;
    		((dist_lock_try&) that)._other = BSONObj();
    	}

    	// Needed so we can handle lock exceptions in context of lock try.
    	dist_lock_try& operator=( const dist_lock_try& that ){

    	    if( this == &that ) return *this;

    	    _lock = that._lock;
    	    _got = that._got;
    	    _other = that._other;
    	    _other.getOwned();
    	    _why = that._why;

    	    // Make sure the lock ownership passes to this object,
    	    // so we only unlock once.
    	    ((dist_lock_try&) that)._got = false;
    	    ((dist_lock_try&) that)._lock = NULL;
    	    ((dist_lock_try&) that)._other = BSONObj();

    	    return *this;
    	}

        dist_lock_try( DistributedLock * lock , string why )
            : _lock(lock), _why(why) {
            _got = _lock->lock_try( why , false , &_other );
        }

        ~dist_lock_try() {
            if ( _got ) {
                verify( ! _other.isEmpty() );
                _lock->unlock( &_other );
            }
        }

        bool reestablish(){
            return retry();
        }

        bool retry() {
            verify( _lock );
            verify( _got );
            verify( ! _other.isEmpty() );

            return _got = _lock->lock_try( _why , true, &_other );
        }

        bool got() const { return _got; }
        BSONObj other() const { return _other; }

    private:
        DistributedLock * _lock;
        bool _got;
        BSONObj _other;
        string _why;
    };

}

