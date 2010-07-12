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

#include "../pch.h"
#include "dbclient.h"
#include "connpool.h"
#include "redef_macros.h"
#include "syncclusterconnection.h"

namespace mongo {

    extern string ourHostname;
    
    class DistributedLock {
    public:

        DistributedLock( const ConnectionString& conn , const string& name )
            : _conn(conn),_name(name),_myid(""){
            _id = BSON( "_id" << name );
            _ns = "config.locks";
        }

        int getState(){
            return _state.get();
        }

        bool isLocked(){
            return _state.get() != 0;
        }
        
        bool lock_try( string why , BSONObj * other = 0 );
        void unlock();

        string myid(){
            string s = _myid.get();
            if ( s.empty() ){
                stringstream ss;
                ss << ourHostname << ":" << time(0) << ":" << rand();
                s = ss.str();
                _myid.set( s );
            }

            return s;
        }
        
    private:
        ConnectionString _conn;
        string _ns;
        string _name;
        BSONObj _id;
        
        ThreadLocalValue<int> _state;
        ThreadLocalValue<string> _myid;
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

