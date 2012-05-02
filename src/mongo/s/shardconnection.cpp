// shardconnection.cpp

/**
*    Copyright (C) 2008 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "pch.h"
#include "shard.h"
#include "config.h"
#include "request.h"
#include <set>

namespace mongo {

    DBConnectionPool shardConnectionPool;

    /**
     * holds all the actual db connections for a client to various servers
     * 1 per thread, so doesn't have to be thread safe
     */
    class ClientConnections : boost::noncopyable {
    public:
        struct Status : boost::noncopyable {
            Status() : created(0), avail(0) {}

            long long created;
            DBClientBase* avail;
        };


        ClientConnections() {}

        ~ClientConnections() {
            for ( HostMap::iterator i=_hosts.begin(); i!=_hosts.end(); ++i ) {
                string addr = i->first;
                Status* ss = i->second;
                verify( ss );
                if ( ss->avail ) {
                    /* if we're shutting down, don't want to initiate release mechanism as it is slow,
                       and isn't needed since all connections will be closed anyway */
                    if ( inShutdown() ) {
                        if( versionManager.isVersionableCB( ss->avail ) ) versionManager.resetShardVersionCB( ss->avail );
                        delete ss->avail;
                    }
                    else
                        release( addr , ss->avail );
                    ss->avail = 0;
                }
                delete ss;
            }
            _hosts.clear();
        }

        DBClientBase * get( const string& addr , const string& ns ) {
            _check( ns );

            Status* &s = _hosts[addr];
            if ( ! s )
                s = new Status();

            if ( s->avail ) {
                DBClientBase* c = s->avail;
                s->avail = 0;
                try {
                    shardConnectionPool.onHandedOut( c );
                }
                catch ( std::exception& ) {
                    delete c;
                    throw;
                }
                return c;
            }

            s->created++;
            return shardConnectionPool.get( addr );
        }

        void done( const string& addr , DBClientBase* conn ) {
            Status* s = _hosts[addr];
            verify( s );
            if ( s->avail ) {
                release( addr , conn );
                return;
            }
            s->avail = conn;
        }

        void sync() {
            for ( HostMap::iterator i=_hosts.begin(); i!=_hosts.end(); ++i ) {
                string addr = i->first;
                Status* ss = i->second;
                if ( ss->avail )
                    ss->avail->getLastError();
                
            }
        }

        void checkVersions( const string& ns ) {

            vector<Shard> all;
            Shard::getAllShards( all );

            // Now only check top-level shard connections
            for ( unsigned i=0; i<all.size(); i++ ) {

                string sconnString = all[i].getConnString();
                Status* &s = _hosts[sconnString];

                if ( ! s ){
                    s = new Status();
                }

                if( ! s->avail )
                    s->avail = shardConnectionPool.get( sconnString );

                versionManager.checkShardVersionCB( s->avail, ns, false, 1 );

            }
        }

        void release( const string& addr , DBClientBase * conn ) {
            shardConnectionPool.release( addr , conn );
        }

        void _check( const string& ns ) {
            if ( ns.size() == 0 || _seenNS.count( ns ) )
                return;
            _seenNS.insert( ns );
            checkVersions( ns );
        }
        
        typedef map<string,Status*,DBConnectionPool::serverNameCompare> HostMap;
        HostMap _hosts;
        set<string> _seenNS;
        // -----

        static thread_specific_ptr<ClientConnections> _perThread;

        static ClientConnections* threadInstance() {
            ClientConnections* cc = _perThread.get();
            if ( ! cc ) {
                cc = new ClientConnections();
                _perThread.reset( cc );
            }
            return cc;
        }
    };

    thread_specific_ptr<ClientConnections> ClientConnections::_perThread;

    ShardConnection::ShardConnection( const Shard * s , const string& ns, ChunkManagerPtr manager )
        : _addr( s->getConnString() ) , _ns( ns ), _manager( manager ) {
        _init();
    }

    ShardConnection::ShardConnection( const Shard& s , const string& ns, ChunkManagerPtr manager )
        : _addr( s.getConnString() ) , _ns( ns ), _manager( manager ) {
        _init();
    }

    ShardConnection::ShardConnection( const string& addr , const string& ns, ChunkManagerPtr manager )
        : _addr( addr ) , _ns( ns ), _manager( manager ) {
        _init();
    }

    void ShardConnection::_init() {
        verify( _addr.size() );
        _conn = ClientConnections::threadInstance()->get( _addr , _ns );
        _finishedInit = false;
    }

    void ShardConnection::_finishInit() {
        if ( _finishedInit )
            return;
        _finishedInit = true;

        if ( _ns.size() && versionManager.isVersionableCB( _conn ) ) {
            // Make sure we specified a manager for the correct namespace
            if( _manager ) verify( _manager->getns() == _ns );
            _setVersion = versionManager.checkShardVersionCB( this , false , 1 );
        }
        else {
            // Make sure we didn't specify a manager for an empty namespace
            verify( ! _manager );
            _setVersion = false;
        }

    }

    void ShardConnection::done() {
        if ( _conn ) {
            ClientConnections::threadInstance()->done( _addr , _conn );
            _conn = 0;
            _finishedInit = true;
        }
    }

    void ShardConnection::kill() {
        if ( _conn ) {
            if( versionManager.isVersionableCB( _conn ) ) versionManager.resetShardVersionCB( _conn );
            delete _conn;
            _conn = 0;
            _finishedInit = true;
        }
    }

    void ShardConnection::sync() {
        ClientConnections::threadInstance()->sync();
    }

    bool ShardConnection::runCommand( const string& db , const BSONObj& cmd , BSONObj& res ) {
        verify( _conn );
        bool ok = _conn->runCommand( db , cmd , res );
        if ( ! ok ) {
            if ( res["code"].numberInt() == SendStaleConfigCode ) {
                done();
                throw RecvStaleConfigException( res["errmsg"].String(), res );
            }
        }
        return ok;
    }

    void ShardConnection::checkMyConnectionVersions( const string & ns ) {
        ClientConnections::threadInstance()->checkVersions( ns );
    }

    ShardConnection::~ShardConnection() {
        if ( _conn ) {
            if ( ! _conn->isFailed() ) {
                /* see done() comments above for why we log this line */
                log() << "sharded connection to " << _conn->getServerAddress() << " not being returned to the pool" << endl;
            }
            kill();
        }
    }
}
