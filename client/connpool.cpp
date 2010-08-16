/* connpool.cpp
*/

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

// _ todo: reconnect?

#include "pch.h"
#include "connpool.h"
#include "../db/commands.h"
#include "syncclusterconnection.h"
#include "../s/shard.h"

namespace mongo {

    DBConnectionPool pool;
    
    DBClientBase* DBConnectionPool::_get(const string& ident) {
        scoped_lock L(_mutex);
        
        PoolForHost& p = _pools[ident];
        if ( p.pool.empty() )
            return 0;
        
        DBClientBase *c = p.pool.top();
        p.pool.pop();
        return c;
    }

    DBClientBase* DBConnectionPool::_finishCreate( const string& host , DBClientBase* conn ){
        {
            scoped_lock L(_mutex);
            PoolForHost& p = _pools[host];
            p.created++;
        }

        onCreate( conn );
        onHandedOut( conn );
        
        return conn;
    }

    DBClientBase* DBConnectionPool::get(const ConnectionString& url) {
        DBClientBase * c = _get( url.toString() );
        if ( c ){
            onHandedOut( c );
            return c;
        }
        
        string errmsg;
        c = url.connect( errmsg );
        uassert( 13328 ,  (string)"dbconnectionpool: connect failed " + url.toString() + " : " + errmsg , c );
        
        return _finishCreate( url.toString() , c );
    }
    
    DBClientBase* DBConnectionPool::get(const string& host) {
        DBClientBase * c = _get( host );
        if ( c ){
            onHandedOut( c );
            return c;
        }
        
        string errmsg;
        ConnectionString cs = ConnectionString::parse( host , errmsg );
        uassert( 13071 , (string)"invalid hostname [" + host + "]" + errmsg , cs.isValid() );
        
        c = cs.connect( errmsg );
        uassert( 11002 ,  (string)"dbconnectionpool: connect failed " + host + " : " + errmsg , c );
        return _finishCreate( host , c );
    }

    DBConnectionPool::~DBConnectionPool(){
        for ( map<string,PoolForHost>::iterator i = _pools.begin(); i != _pools.end(); i++ ){
            PoolForHost& p = i->second;

            while ( ! p.pool.empty() ){
                DBClientBase * c = p.pool.top();
                delete c;
                p.pool.pop();
            }
        }
    }

    void DBConnectionPool::flush(){
        scoped_lock L(_mutex);
        for ( map<string,PoolForHost>::iterator i = _pools.begin(); i != _pools.end(); i++ ){
            PoolForHost& p = i->second;

            vector<DBClientBase*> all;
            while ( ! p.pool.empty() ){
                DBClientBase * c = p.pool.top();
                p.pool.pop();
                all.push_back( c );
                bool res;
                c->isMaster( res );
            }
            
            for ( vector<DBClientBase*>::iterator i=all.begin(); i != all.end(); i++ ){
                p.pool.push( *i );
            }
        }
    }

    void DBConnectionPool::addHook( DBConnectionHook * hook ){
        _hooks.push_back( hook );
    }

    void DBConnectionPool::onCreate( DBClientBase * conn ){
        if ( _hooks.size() == 0 )
            return;
        
        for ( list<DBConnectionHook*>::iterator i = _hooks.begin(); i != _hooks.end(); i++ ){
            (*i)->onCreate( conn );
        }
    }

    void DBConnectionPool::onHandedOut( DBClientBase * conn ){
        if ( _hooks.size() == 0 )
            return;
        
        for ( list<DBConnectionHook*>::iterator i = _hooks.begin(); i != _hooks.end(); i++ ){
            (*i)->onHandedOut( conn );
        }
    }

    void DBConnectionPool::appendInfo( BSONObjBuilder& b ){
        scoped_lock lk( _mutex );
        BSONObjBuilder bb( b.subobjStart( "hosts" ) );
        for ( map<string,PoolForHost>::iterator i=_pools.begin(); i!=_pools.end(); ++i ){
            string s = i->first;
            BSONObjBuilder temp( bb.subobjStart( s.c_str() ) );
            temp.append( "available" , (int)(i->second.pool.size()) );
            temp.appendNumber( "created" , i->second.created );
            temp.done();
        }
        bb.done();
    }

    ScopedDbConnection * ScopedDbConnection::steal(){
        assert( _conn );
        ScopedDbConnection * n = new ScopedDbConnection( _host , _conn );
        _conn = 0;
        return n;
    }
    
    ScopedDbConnection::~ScopedDbConnection() {
        if ( _conn ){
            if ( ! _conn->isFailed() ) {
                /* see done() comments above for why we log this line */
                log() << "~ScopedDbConnection: _conn != null" << endl;
            }
            kill();
        }
    }

    ScopedDbConnection::ScopedDbConnection(const Shard& shard )
        : _host( shard.getConnString() ) , _conn( pool.get(_host) ){
    }
    
    ScopedDbConnection::ScopedDbConnection(const Shard* shard )
        : _host( shard->getConnString() ) , _conn( pool.get(_host) ){
    }


    class PoolFlushCmd : public Command {
    public:
        PoolFlushCmd() : Command( "connPoolSync" , false , "connpoolsync" ){}
        virtual void help( stringstream &help ) const { help<<"internal"; }
        virtual LockType locktype() const { return NONE; }
        virtual bool run(const string&, mongo::BSONObj&, std::string&, mongo::BSONObjBuilder& result, bool){
            pool.flush();
            return true;
        }
        virtual bool slaveOk() const {
            return true;
        }

    } poolFlushCmd;

    class PoolStats : public Command {
    public:
        PoolStats() : Command( "connPoolStats" ){}
        virtual void help( stringstream &help ) const { help<<"stats about connection pool"; }
        virtual LockType locktype() const { return NONE; }
        virtual bool run(const string&, mongo::BSONObj&, std::string&, mongo::BSONObjBuilder& result, bool){
            pool.appendInfo( result );
            return true;
        }
        virtual bool slaveOk() const {
            return true;
        }

    } poolStatsCmd;


} // namespace mongo
