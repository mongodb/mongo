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
 * distribuetd locking mechanism
 */

#include "../pch.h"
#include "dbclient.h"
#include "redef_macros.h"

namespace mongo {

    extern string ourHostname;

    class DistributedLock {
    public:
        DistributedLock( const ConnectionString& conn , const string& ns , const BSONObj& key , const string& field )
            : _conn(conn), _ns(ns), _key(key.getOwned()), _field(field){
        }

        int getState(){
            return _state.get();
        }

        bool isLocked(){
            return _state.get() != 0;
        }
        
        bool lock_try(){
            // recursive
            if ( getState() > 0 )
                return true;

            ScopedDbConnection conn( _conn );
            
            { // make sure its there so we can use simple update logic below
                BSONObj o = conn->findOne( _ns , _key );
                if ( o.isEmpty() ){
                    conn->update( _ns , _key , BSON( "$set" << BSON( _field << BSON( "state" << 0 ) ) ) , 1 );
                }
            }


            BSONObjBuilder b;
            b.appendElements( _key );
            b.append( _field + ".state" , 0 );
            

            conn->update( _ns , b.obj() , BSON( "$set" << BSON( _field + ".state" << 1 << "who" << myid() ) ) );
            assert(0);
            conn.done();
        }

        void unlock(){
            ScopedDbConnection conn( _conn );
            conn->update( _ns , _key, BSON( "$set" << BSON( _field + ".state" << 0 ) ) );
            conn.done();

            _state.set( 0 );
        }

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
        BSONObj _key;
        string _field;
        ThreadLocalValue<int> _state;
        ThreadLocalValue<string> _myid;
    };
    
    class dist_lock_try {
    public:

        dist_lock_try( DistributedLock * lock )
            : _lock(lock){
            _got = _lock->lock_try();
        }

        ~dist_lock_try(){
            if ( _got ){
                _lock->unlock();
            }
        }

        bool got() const {
            return _got;
        }

    private:
        DistributedLock * _lock;
        bool _got;
        
    };

}

