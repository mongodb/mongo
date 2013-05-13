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

#include "mongo/pch.h"

#include "mongo/client/connpool.h"
#include "mongo/client/dbclient_rs.h"
#include "mongo/client/syncclusterconnection.h"
#include "mongo/s/shard.h"

namespace mongo {

    // ------ PoolForHost ------

    PoolForHost::~PoolForHost() {
        clear();
    }

    void PoolForHost::clear() {
        while ( ! _pool.empty() ) {
            StoredConnection sc = _pool.top();
            delete sc.conn;
            _pool.pop();
        }
    }

    void PoolForHost::done( DBConnectionPool * pool, DBClientBase * c ) {
        if (c->isFailed()) {
            reportBadConnectionAt(c->getSockCreationMicroSec());
            pool->onDestroy(c);
            delete c;
        }
        else if (_pool.size() >= _maxPerHost ||
                c->getSockCreationMicroSec() < _minValidCreationTimeMicroSec) {
            pool->onDestroy(c);
            delete c;
        }
        else {
            _pool.push(c);
        }
    }

    void PoolForHost::reportBadConnectionAt(uint64_t microSec) {
        if (microSec != DBClientBase::INVALID_SOCK_CREATION_TIME &&
                microSec > _minValidCreationTimeMicroSec) {
            _minValidCreationTimeMicroSec = microSec;
            log() << "Detected bad connection created at " << _minValidCreationTimeMicroSec
                    << " microSec, clearing pool for " << _hostName
                    << " of " << _pool.size() << " connections" << endl;
            clear();
        }
    }

    bool PoolForHost::isBadSocketCreationTime(uint64_t microSec) {
        return microSec != DBClientBase::INVALID_SOCK_CREATION_TIME &&
                microSec <= _minValidCreationTimeMicroSec;
    }

    DBClientBase * PoolForHost::get( DBConnectionPool * pool , double socketTimeout ) {

        time_t now = time(0);
        
        while ( ! _pool.empty() ) {
            StoredConnection sc = _pool.top();
            _pool.pop();
            
            if ( ! sc.ok( now ) )  {
                pool->onDestroy( sc.conn );
                delete sc.conn;
                continue;
            }
            
            verify( sc.conn->getSoTimeout() == socketTimeout );

            return sc.conn;

        }

        return NULL;
    }

    void PoolForHost::flush() {
        vector<StoredConnection> all;
        while ( ! _pool.empty() ) {
            StoredConnection c = _pool.top();
            _pool.pop();
            bool res;
            bool alive = false;
            try {
                c.conn->isMaster( res );
                alive = true;
            } catch ( const DBException e ) {
                // There's something wrong with this connection, swallow the exception and do not
                // put the connection back in the pool.
                LOG(1) << "Exception thrown when checking pooled connection to " <<
                    c.conn->getServerAddress() << ": " << causedBy(e) << endl;
                delete c.conn;
                c.conn = NULL;
            }
            if ( alive ) {
                all.push_back( c );
            }
        }

        for ( vector<StoredConnection>::iterator i=all.begin(); i != all.end(); ++i ) {
            _pool.push( *i );
        }
    }

    void PoolForHost::getStaleConnections( vector<DBClientBase*>& stale ) {
        time_t now = time(0);

        vector<StoredConnection> all;
        while ( ! _pool.empty() ) {
            StoredConnection c = _pool.top();
            _pool.pop();
            
            if ( c.ok( now ) )
                all.push_back( c );
            else
                stale.push_back( c.conn );
        }

        for ( size_t i=0; i<all.size(); i++ ) {
            _pool.push( all[i] );
        }
    }


    PoolForHost::StoredConnection::StoredConnection( DBClientBase * c ) {
        conn = c;
        when = time(0);
    }

    bool PoolForHost::StoredConnection::ok( time_t now ) {
        // Poke the connection to see if we're still ok
        return conn->isStillConnected();
    }

    void PoolForHost::createdOne( DBClientBase * base) {
        if ( _created == 0 )
            _type = base->type();
        _created++;
    }

    void PoolForHost::initializeHostName(const std::string& hostName) {
        if (_hostName.empty()) {
            _hostName = hostName;
        }
    }

    unsigned PoolForHost::_maxPerHost = 50;

    // ------ DBConnectionPool ------

    DBConnectionPool pool;

    DBConnectionPool::DBConnectionPool() 
        : _mutex("DBConnectionPool") , 
          _name( "dbconnectionpool" ) , 
          _hooks( new list<DBConnectionHook*>() ) { 
    }

    DBClientBase* DBConnectionPool::_get(const string& ident , double socketTimeout ) {
        verify( ! inShutdown() );
        scoped_lock L(_mutex);
        PoolForHost& p = _pools[PoolKey(ident,socketTimeout)];
        p.initializeHostName(ident);
        return p.get( this , socketTimeout );
    }

    DBClientBase* DBConnectionPool::_finishCreate( const string& host , double socketTimeout , DBClientBase* conn ) {
        {
            scoped_lock L(_mutex);
            PoolForHost& p = _pools[PoolKey(host,socketTimeout)];
            p.initializeHostName(host);
            p.createdOne( conn );
        }
        
        try {
            onCreate( conn );
            onHandedOut( conn );
        }
        catch ( std::exception & ) {
            delete conn;
            throw;
        }

        return conn;
    }

    DBClientBase* DBConnectionPool::get(const ConnectionString& url, double socketTimeout) {
        DBClientBase * c = _get( url.toString() , socketTimeout );
        if ( c ) {
            try {
                onHandedOut( c );
            }
            catch ( std::exception& ) {
                delete c;
                throw;
            }
            return c;
        }

        string errmsg;
        c = url.connect( errmsg, socketTimeout );
        uassert( 13328 ,  _name + ": connect failed " + url.toString() + " : " + errmsg , c );

        return _finishCreate( url.toString() , socketTimeout , c );
    }

    DBClientBase* DBConnectionPool::get(const string& host, double socketTimeout) {
        DBClientBase * c = _get( host , socketTimeout );
        if ( c ) {
            try {
                onHandedOut( c );
            }
            catch ( std::exception& ) {
                delete c;
                throw;
            }
            return c;
        }

        string errmsg;
        ConnectionString cs = ConnectionString::parse( host , errmsg );
        uassert( 13071 , (string)"invalid hostname [" + host + "]" + errmsg , cs.isValid() );

        c = cs.connect( errmsg, socketTimeout );
        if ( ! c )
            throw SocketException( SocketException::CONNECT_ERROR , host , 11002 , str::stream() << _name << " error: " << errmsg );
        return _finishCreate( host , socketTimeout , c );
    }

    void DBConnectionPool::release(const string& host, DBClientBase *c) {
        scoped_lock L(_mutex);
        _pools[PoolKey(host,c->getSoTimeout())].done(this,c);
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

    void DBConnectionPool::clear() {
        scoped_lock L(_mutex);
        LOG(2) << "Removing connections on all pools owned by " << _name  << endl;
        for (PoolMap::iterator iter = _pools.begin(); iter != _pools.end(); ++iter) {
            iter->second.clear();
        }
    }

    void DBConnectionPool::removeHost( const string& host ) {
        scoped_lock L(_mutex);
        LOG(2) << "Removing connections from all pools for host: " << host << endl;
        for ( PoolMap::iterator i = _pools.begin(); i != _pools.end(); ++i ) {
            const string& poolHost = i->first.ident;
            if ( !serverNameCompare()(host, poolHost) && !serverNameCompare()(poolHost, host) ) {
                // hosts are the same
                i->second.clear();
            }
        }
    }

    void DBConnectionPool::addHook( DBConnectionHook * hook ) {
        _hooks->push_back( hook );
    }

    void DBConnectionPool::onCreate( DBClientBase * conn ) {
        if ( _hooks->size() == 0 )
            return;

        for ( list<DBConnectionHook*>::iterator i = _hooks->begin(); i != _hooks->end(); i++ ) {
            (*i)->onCreate( conn );
        }
    }

    void DBConnectionPool::onHandedOut( DBClientBase * conn ) {
        if ( _hooks->size() == 0 )
            return;

        for ( list<DBConnectionHook*>::iterator i = _hooks->begin(); i != _hooks->end(); i++ ) {
            (*i)->onHandedOut( conn );
        }
    }

    void DBConnectionPool::onDestroy( DBClientBase * conn ) {
        if ( _hooks->size() == 0 )
            return;

        for ( list<DBConnectionHook*>::iterator i = _hooks->begin(); i != _hooks->end(); i++ ) {
            (*i)->onDestroy( conn );
        }
    }

    void DBConnectionPool::appendInfo( BSONObjBuilder& b ) {

        int avail = 0;
        long long created = 0;


        map<ConnectionString::ConnectionType,long long> createdByType;

        set<string> replicaSets;
        
        BSONObjBuilder bb( b.subobjStart( "hosts" ) );
        {
            scoped_lock lk( _mutex );
            for ( PoolMap::iterator i=_pools.begin(); i!=_pools.end(); ++i ) {
                if ( i->second.numCreated() == 0 )
                    continue;

                string s = str::stream() << i->first.ident << "::" << i->first.timeout;

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
        
        // Always report all replica sets being tracked
        ReplicaSetMonitor::getAllTrackedSets(&replicaSets);
        
        BSONObjBuilder setBuilder( b.subobjStart( "replicaSets" ) );
        for ( set<string>::iterator i=replicaSets.begin(); i!=replicaSets.end(); ++i ) {
            string rs = *i;
            ReplicaSetMonitorPtr m = ReplicaSetMonitor::get( rs );
            if ( ! m ) {
                warning() << "no monitor for set: " << rs << endl;
                continue;
            }
            
            BSONObjBuilder temp( setBuilder.subobjStart( rs ) );
            m->appendInfo( temp );
            temp.done();
        }
        setBuilder.done();

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
        const char* ap = a.c_str();
        const char* bp = b.c_str();
       
        while (true){
            if (*ap == '\0' || *ap == '/'){
                if (*bp == '\0' || *bp == '/')
                    return false; // equal strings
                else
                    return true; // a is shorter
            }

            if (*bp == '\0' || *bp == '/')
                return false; // b is shorter
            
            if ( *ap < *bp)
                return true;
            else if (*ap > *bp)
                return false;

            ++ap;
            ++bp;
        }
        verify(false);
    }
    
    bool DBConnectionPool::poolKeyCompare::operator()( const PoolKey& a , const PoolKey& b ) const {
        if (DBConnectionPool::serverNameCompare()( a.ident , b.ident ))
            return true;
        
        if (DBConnectionPool::serverNameCompare()( b.ident , a.ident ))
            return false;

        return a.timeout < b.timeout;
    }

    bool DBConnectionPool::isConnectionGood(const string& hostName, DBClientBase* conn) {
        if (conn == NULL) {
            return false;
        }

        if (conn->isFailed()) {
            return false;
        }

        {
            scoped_lock sl(_mutex);
            PoolForHost& pool = _pools[PoolKey(hostName, conn->getSoTimeout())];
            if (pool.isBadSocketCreationTime(conn->getSockCreationMicroSec())) {
                return false;
            }
        }

        return true;
    }

    void DBConnectionPool::taskDoWork() { 
        vector<DBClientBase*> toDelete;
        
        {
            // we need to get the connections inside the lock
            // but we can actually delete them outside
            scoped_lock lk( _mutex );
            for ( PoolMap::iterator i=_pools.begin(); i!=_pools.end(); ++i ) {
                i->second.getStaleConnections( toDelete );
            }
        }

        for ( size_t i=0; i<toDelete.size(); i++ ) {
            try {
                onDestroy( toDelete[i] );
                delete toDelete[i];
            }
            catch ( ... ) {
                // we don't care if there was a socket error
            }
        }
    }

    // ------ ScopedDbConnection ------

    void ScopedDbConnection::_setSocketTimeout(){
        if( ! _conn ) return;
        if( _conn->type() == ConnectionString::MASTER )
            (( DBClientConnection* ) _conn)->setSoTimeout( _socketTimeout );
        else if( _conn->type() == ConnectionString::SYNC )
            (( SyncClusterConnection* ) _conn)->setAllSoTimeouts( _socketTimeout );
    }

    ScopedDbConnection::~ScopedDbConnection() {
        if ( _conn ) {
            if (_conn->isFailed()) {
                if (_conn->getSockCreationMicroSec() ==
                        DBClientBase::INVALID_SOCK_CREATION_TIME) {
                    kill();
                }
                else {
                    // The pool takes care of deleting the failed connection - this
                    // will also trigger disposal of older connections in the pool
                    done();
                }
            }
            else {
                /* see done() comments above for why we log this line */
                log() << "scoped connection to " << _conn->getServerAddress()
                        << " not being returned to the pool" << endl;
                kill();
            }
        }
    }

    void ScopedDbConnection::clearPool() {
        pool.clear();
    }

    AtomicUInt AScopedConnection::_numConnections;

} // namespace mongo
