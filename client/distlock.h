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


/**
 * distributed locking mechanism
 */

#pragma once

#include "../pch.h"
#include "dbclient.h"
#include "connpool.h"
#include "redef_macros.h"
#include "syncclusterconnection.h"

namespace mongo {

    class DistributedLock {
    public:

        /**
         * @param takeoverMinutes how long before we steal lock in minutes
         */
        DistributedLock( const ConnectionString& conn , const string& name , unsigned takeoverMinutes = 10 );

        bool lock_try( string why , BSONObj * other = 0 );
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
            : _lock(lock){
            _got = _lock->lock_try( why , &_other );
        }

        ~dist_lock_try(){
            if ( _got ){
                _lock->unlock();
            }
        }

        bool got() const {
            return _got;
        }

        BSONObj other() const {
            return _other;
        }
 
    private:
        DistributedLock * _lock;
        bool _got;
        BSONObj _other;
        
    };

}

