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

    // ------ PoolForHost ------

    PoolForHost::~PoolForHost() {
        while ( ! _pool.empty() ) {
            StoredConnection sc = _pool.top();
            delete sc.conn;
            _pool.pop();
        }
    }

    void PoolForHost::done( DBClientBase * c ) {
        if ( _pool.size() >= _maxPerHost ) {
            delete c;
        }
        else {
            _pool.push(c);
        }
    }

    DBClientBase * PoolForHost::get() {

        time_t now = time(0);

        while ( ! _pool.empty() ) {
            StoredConnection sc = _pool.top();
            _pool.pop();
            if ( sc.ok( now ) )
                return sc.conn;
            delete sc.conn;
        }

        return NULL;
    }

    void PoolForHost::flush() {
        vector<StoredConnection> all;
        while ( ! _pool.empty() ) {
            StoredConnection c = _pool.top();
            _pool.pop();
            all.push_back( c );
            bool res;
            c.conn->isMaster( res );
        }

        for ( vector<StoredConnection>::iterator i=all.begin(); i != all.end(); ++i ) {
            _pool.push( *i );
        }
    }

    PoolForHost::StoredConnection::StoredConnection( DBClientBase * c ) {
        conn = c;
        when = time(0);
    }

    bool PoolForHost::StoredConnection::ok( time_t now ) {
        // if connection has been idle for an hour, kill it
        return ( now - when ) < 3600;
    }

    void PoolForHost::createdOne( DBClientBase * base) {
        if ( _created == 0 )
            _type = base->type();
        _created++;
    }

    unsigned PoolForHost::_maxPerHost = 50;

    // ------ DBConnectionPool ------

    DBConnectionPool pool;

    DBClientBase* DBConnectionPool::_get(const string& ident) {
        scoped_lock L(_mutex);
        PoolForHost& p = _pools[ident];
        return p.get();
    }

    DBClientBase* DBConnectionPool::_finishCreate( const string& host , DBClientBase* conn ) {
        {
            scoped_lock L(_mutex);
            PoolForHost& p = _pools[host];
            p.createdOne( conn );
        }

        onCreate( conn );
        onHandedOut( conn );

        return conn;
    }

    DBClientBase* DBConnectionPool::get(const ConnectionString& url) {
        DBClientBase * c = _get( url.toString() );
        if ( c ) {
            onHandedOut( c );
            return c;
        }

        string errmsg;
        c = url.connect( errmsg );
        uassert( 13328 ,  _name + ": connect failed " + url.toString() + " : " + errmsg , c );

        return _finishCreate( url.toString() , c );
    }

    DBClientBase* DBConnectionPool::get(const string& host) {
        DBClientBase * c = _get( host );
        if ( c ) {
            onHandedOut( c );
            return c;
        }

        string errmsg;
        ConnectionString cs = ConnectionString::parse( host , errmsg );
        uassert( 13071 , (string)"invalid hostname [" + host + "]" + errmsg , cs.isValid() );

        c = cs.connect( errmsg );
        if ( ! c )
            throw SocketException( SocketException::CONNECT_ERROR , host , 11002 , str::stream() << _name << " error: " << errmsg );
        return _finishCreate( host , c );
    }

    DBConnectionPool::~DBConnectionPool() {
        // connection closing is handled by ~PoolForHost
    }

    void DBConnectionPool::flush() {
        scoped_lock L(_mutex);
        for ( PoolMap::iterator i = _pools.begin(); i != _pools.end(); i++ ) {
            PoolForHost& p = i->second;
            p.flush();
        }
    }

    void DBConnectionPool::addHook( DBConnectionHook * hook ) {
        _hooks.push_back( hook );
    }

    void DBConnectionPool::onCreate( DBClientBase * conn ) {
        if ( _hooks.size() == 0 )
            return;

        for ( list<DBConnectionHook*>::iterator i = _hooks.begin(); i != _hooks.end(); i++ ) {
            (*i)->onCreate( conn );
        }
    }

    void DBConnectionPool::onHandedOut( DBClientBase * conn ) {
        if ( _hooks.size() == 0 )
            return;

        for ( list<DBConnectionHook*>::iterator i = _hooks.begin(); i != _hooks.end(); i++ ) {
            (*i)->onHandedOut( conn );
        }
    }

    void DBConnectionPool::appendInfo( BSONObjBuilder& b ) {
        BSONObjBuilder bb( b.subobjStart( "hosts" ) );
        int avail = 0;
        long long created = 0;


        map<ConnectionString::ConnectionType,long long> createdByType;

        {
            scoped_lock lk( _mutex );
            for ( PoolMap::iterator i=_pools.begin(); i!=_pools.end(); ++i ) {
                string s = i->first;
                BSONObjBuilder temp( bb.subobjStart( s ) );
                temp.append( "available" , i->second.numAvailable() );
                temp.appendNumber( "created" , i->second.numCreated() );
                temp.done();

                avail += i->second.numAvailable();
                created += i->second.numCreated();

                long long& x = createdByType[i->second.type()];
                x += i->second.numCreated();
            }
        }
        bb.done();

        {
            BSONObjBuilder temp( bb.subobjStart( "createdByType" ) );
            for ( map<ConnectionString::ConnectionType,long long>::iterator i=createdByType.begin(); i!=createdByType.end(); ++i ) {
                temp.appendNumber( ConnectionString::typeToString( i->first ) , i->second );
            }
            temp.done();
        }

        b.append( "totalAvailable" , avail );
        b.appendNumber( "totalCreated" , created );
    }

    bool DBConnectionPool::serverNameCompare::operator()( const string& a , const string& b ) const{
        string ap = str::before( a , "/" );
        string bp = str::before( b , "/" );
        
        return ap < bp;
    }

    // ------ ScopedDbConnection ------

    ScopedDbConnection * ScopedDbConnection::steal() {
        assert( _conn );
        ScopedDbConnection * n = new ScopedDbConnection( _host , _conn );
        _conn = 0;
        return n;
    }

    ScopedDbConnection::~ScopedDbConnection() {
        if ( _conn ) {
            if ( ! _conn->isFailed() ) {
                /* see done() comments above for why we log this line */
                log() << "~ScopedDbConnection: _conn != null" << endl;
            }
            kill();
        }
    }

    ScopedDbConnection::ScopedDbConnection(const Shard& shard )
        : _host( shard.getConnString() ) , _conn( pool.get(_host) ) {
    }

    ScopedDbConnection::ScopedDbConnection(const Shard* shard )
        : _host( shard->getConnString() ) , _conn( pool.get(_host) ) {
    }


    class PoolFlushCmd : public Command {
    public:
        PoolFlushCmd() : Command( "connPoolSync" , false , "connpoolsync" ) {}
        virtual void help( stringstream &help ) const { help<<"internal"; }
        virtual LockType locktype() const { return NONE; }
        virtual bool run(const string&, mongo::BSONObj&, std::string&, mongo::BSONObjBuilder& result, bool) {
            pool.flush();
            return true;
        }
        virtual bool slaveOk() const {
            return true;
        }

    } poolFlushCmd;

    class PoolStats : public Command {
    public:
        PoolStats() : Command( "connPoolStats" ) {}
        virtual void help( stringstream &help ) const { help<<"stats about connection pool"; }
        virtual LockType locktype() const { return NONE; }
        virtual bool run(const string&, mongo::BSONObj&, std::string&, mongo::BSONObjBuilder& result, bool) {
            pool.appendInfo( result );
            result.append( "numDBClientConnection" , DBClientConnection::getNumConnections() );
            result.append( "numAScopedConnection" , AScopedConnection::getNumConnections() );
            return true;
        }
        virtual bool slaveOk() const {
            return true;
        }

    } poolStatsCmd;

    AtomicUInt AScopedConnection::_numConnections;

} // namespace mongo
