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
#include "dbclient.h"
#include "connpool.h"
#include "redef_macros.h"
#include "syncclusterconnection.h"

namespace mongo {

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

        /**
         * The constructor does not connect to the configdb yet and constructing does not mean the lock was acquired.
         * Construction does trigger a lock "pinging" mechanism, though.
         *
         * @param conn address of config(s) server(s)
         * @param name identifier for the lock
         * @param takeoverMinutes how long can the log go "unpinged" before a new attempt to lock steals it (in minutes)
         */
        DistributedLock( const ConnectionString& conn , const string& name , unsigned takeoverMinutes = 15 );

        /**
         * Attempts to aquire 'this' lock, checking if it could or should be stolen from the previous holder. Please
         * consider using the dist_lock_try construct to acquire this lock in an exception safe way.
         *
         * @param why human readable description of why the lock is being taken (used to log)
         * @param other configdb's lock document that is currently holding the lock, if lock is taken
         * @return true if it managed to grab the lock
         */
        bool lock_try( string why , BSONObj * other = 0 );

        /**
         * Releases a previously taken lock.
         */
        void unlock();

    private:
        ConnectionString _conn;
        string _name;
        unsigned _takeoverMinutes;

        string _ns;
        BSONObj _id;
    };

    class dist_lock_try {
    public:
        dist_lock_try( DistributedLock * lock , string why )
            : _lock(lock) {
            _got = _lock->lock_try( why , &_other );
        }

        ~dist_lock_try() {
            if ( _got ) {
                _lock->unlock();
            }
        }

        bool got() const { return _got; }
        BSONObj other() const { return _other; }

    private:
        DistributedLock * _lock;
        bool _got;
        BSONObj _other;
    };

}

