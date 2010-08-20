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

#include "pch.h"

#include "boost/thread/once.hpp"

#include "dbclient.h"
#include "distlock.h"

namespace mongo {

    string lockPingNS = "config.lockpings";

    ThreadLocalValue<string> distLockIds("");

    /* ==================
     * Module initialization
     */

    boost::once_flag _init = BOOST_ONCE_INIT;
    static string* _cachedProcessString = NULL;

    static void initModule() {
        // cache process string
        stringstream ss;
        ss << getHostName() << ":" << time(0) << ":" << rand();
        _cachedProcessString = new string( ss.str() );
    }

    /* =================== */

    string getDistLockProcess(){
        boost::call_once( initModule, _init );
        assert( _cachedProcessString );
        return *_cachedProcessString;
    }

    string getDistLockId(){
        string s = distLockIds.get();
        if ( s.empty() ){
            stringstream ss;
            ss << getDistLockProcess() << ":" << getThreadName() << ":" << rand();
            s = ss.str();
            distLockIds.set( s );
        }
        return s;
    }
    
    void distLockPingThread( ConnectionString addr ){
        setThreadName( "LockPinger" );

        static int loops = 0;
        while( ! inShutdown() ){

            string process = getDistLockProcess();
            log(4) << "dist_lock about to ping for: " << process << endl;

            try {
                ScopedDbConnection conn( addr );
                
                // do ping
                conn->update( lockPingNS , 
                              BSON( "_id" << process ) , 
                              BSON( "$set" << BSON( "ping" << DATENOW ) ) ,
                              true );
                
                
                // remove really old entries
                BSONObjBuilder f;
                f.appendDate( "$lt" , jsTime() - ( 4 * 86400 * 1000 ) );
                BSONObj r = BSON( "ping" << f.obj() );
                conn->remove( lockPingNS , r );
                
                // create index so remove is fast even with a lot of servers
                if ( loops++ == 0 ){
                    conn->ensureIndex( lockPingNS , BSON( "ping" << 1 ) );
                }
                
                conn.done();
            }
            catch ( std::exception& e ){
                log( LL_WARNING ) << "dist_lock couldn't ping: " << e.what() << endl;
            }

            log(4) << "dist_lock pinged successfully for: " << process << endl;
            sleepsecs(30);
        }
    }
        
    
    class DistributedLockPinger {
    public:
        DistributedLockPinger()
            : _mutex( "DistributedLockPinger" ){
        }
        
        void got( const ConnectionString& conn ){
            string s = conn.toString();
            scoped_lock lk( _mutex );
            if ( _seen.count( s ) > 0 )
                return;
            boost::thread t( boost::bind( &distLockPingThread , conn ) );
            _seen.insert( s );
        }
        
        set<string> _seen;
        mongo::mutex _mutex;
        
    } distLockPinger;
    
    DistributedLock::DistributedLock( const ConnectionString& conn , const string& name , unsigned takeoverMinutes )
        : _conn(conn),_name(name),_takeoverMinutes(takeoverMinutes){
        _id = BSON( "_id" << name );
        _ns = "config.locks";
        distLockPinger.got( conn );
    }

       
    bool DistributedLock::lock_try( string why , BSONObj * other ){
        ScopedDbConnection conn( _conn );
            
        BSONObjBuilder queryBuilder;
        queryBuilder.appendElements( _id );
        queryBuilder.append( "state" , 0 );            

        { // make sure its there so we can use simple update logic below
            BSONObj o = conn->findOne( _ns , _id );
            if ( o.isEmpty() ){
                try {
                    log(4) << "dist_lock inserting initial doc in " << _ns << " for lock " << _name << endl;
                    conn->insert( _ns , BSON( "_id" << _name << "state" << 0 << "who" << "" ) );
                }
                catch ( UserException& e ){
                    log() << "dist_lock could not insert initial doc: " << e << endl;
                }
            }

            else if ( o["state"].numberInt() > 0 ){
                BSONObj lastPing = conn->findOne( lockPingNS , o["process"].wrap( "_id" ) );
                if ( lastPing.isEmpty() ){
                    // TODO: maybe this should clear, not sure yet
                    log() << "lastPing is empty! this could be bad: " << o << endl;
                    conn.done();
                    return false;
                }

                unsigned long long elapsed = jsTime() - lastPing["ping"].Date(); // in ms
                elapsed = elapsed / ( 1000 * 60 ); // convert to minutes

                if ( elapsed <= _takeoverMinutes ){
                    log(1) << "dist_lock lock failed because taken by: " << o << " elapsed minutes: " << elapsed << endl;
                    conn.done();
                    return false;
                }
                
                log() << "dist_lock forcefully taking over from: " << o << " elapsed minutes: " << elapsed << endl;
                conn->update( _ns , _id , BSON( "$set" << BSON( "state" << 0 ) ) );
            }
            else if ( o["ts"].type() ){
                queryBuilder.append( o["ts"] );
            }
        }
        
        OID ts;
        ts.init();

        bool gotLock = false;
        BSONObj now;

        BSONObj lockDetails = BSON( "state" << 1 << "who" << getDistLockId() << "process" << getDistLockProcess() <<
                                    "when" << DATENOW << "why" << why << "ts" << ts );
        BSONObj whatIWant = BSON( "$set" << lockDetails );
        try {
            log(4) << "dist_lock about to aquire lock: " << lockDetails << endl;

            conn->update( _ns , queryBuilder.obj() , whatIWant );
                
            BSONObj o = conn->getLastErrorDetailed();
            now = conn->findOne( _ns , _id );
                
            if ( o["n"].numberInt() == 0 ){
                if ( other ){
                    *other = now;
                    other->getOwned();
                }
                log() << "dist_lock error trying to aquire lock: " << lockDetails << " error: " << o << endl;
                gotLock = false;
            }
            else {
                gotLock = true;
            }

        }
        catch ( UpdateNotTheSame& up ){
            // this means our update got through on some, but not others
            log(4) << "dist_lock lock did not propagate properly" << endl;

            for ( unsigned i=0; i<up.size(); i++ ){
                ScopedDbConnection temp( up[i].first );
                BSONObj temp2 = temp->findOne( _ns , _id );

                if ( now.isEmpty() || now["ts"] < temp2["ts"] ){
                    now = temp2.getOwned();
                }

                temp.done();
            }

            if ( now["ts"].OID() == ts ){
                log(4) << "dist_lock completed lock propagation" << endl;
                gotLock = true;
                conn->update( _ns , _id , whatIWant );
            }
            else {
                log() << "dist_lock error trying to complete propagation" << endl;
                gotLock = false;
            }
        }
            
        conn.done();
            
        log(1) << "dist_lock lock gotLock: " << gotLock << " now: " << now << endl;

        return gotLock;
    }

    void DistributedLock::unlock(){
        const int maxAttempts = 3;
        int attempted = 0;
        while ( ++attempted <= maxAttempts ) {

            try {
                ScopedDbConnection conn( _conn );
                conn->update( _ns , _id, BSON( "$set" << BSON( "state" << 0 ) ) );
                log(1) << "dist_lock unlock: " << conn->findOne( _ns , _id ) << endl;
                conn.done();

                return;

            
            } catch ( std::exception& e) {
                log( LL_WARNING ) << "dist_lock  " << _name << " failed to contact config server in unlock attempt " 
                                  << attempted << ": " << e.what() <<  endl;

                sleepsecs(1 << attempted);
            }
        }

        log( LL_WARNING ) << "dist_lock couldn't consumate unlock request. " << "Lock " << _name 
                              << " will be taken over after " <<  _takeoverMinutes << " minutes timeout" << endl;
    }

}
