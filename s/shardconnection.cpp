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
    
    /**
     * holds all the actual db connections for a client to various servers
     * 1 pre thread, so don't have to worry about thread safety
     */
    class ClientConnections : boost::noncopyable {
    public:
        struct Status : boost::noncopyable {
            Status() : created(0), avail(0){}

            long long created;            
            DBClientBase* avail;
        };


        ClientConnections(){}
        
        ~ClientConnections(){
            for ( map<string,Status*>::iterator i=_hosts.begin(); i!=_hosts.end(); ++i ){
                string addr = i->first;
                Status* ss = i->second;
                assert( ss );
                if ( ss->avail ){
                    /* if we're shutting down, don't want to initiate release mechanism as it is slow, 
                       and isn't needed since all connections will be closed anyway */
                    if ( inShutdown() )
                        delete ss->avail;
                    else
                        release( addr , ss->avail );
                    ss->avail = 0;
                }
                delete ss;
            }
            _hosts.clear();
        }
        
        DBClientBase * get( const string& addr , const string& ns ){
            _check( ns );

            Status* &s = _hosts[addr];
            if ( ! s )
                s = new Status();
            
            if ( s->avail ){
                DBClientBase* c = s->avail;
                s->avail = 0;
                pool.onHandedOut( c );
                return c;
            }

            s->created++;
            return pool.get( addr );
        }
        
        void done( const string& addr , DBClientBase* conn ){
            Status* s = _hosts[addr];
            assert( s );
            if ( s->avail ){
                release( addr , conn );
                return;
            }
            s->avail = conn;
        }
        
        void sync(){
            for ( map<string,Status*>::iterator i=_hosts.begin(); i!=_hosts.end(); ++i ){
                string addr = i->first;
                Status* ss = i->second;

                if ( ss->avail ){
                    ss->avail->getLastError();
                    release( addr , ss->avail );
                    ss->avail = 0;
                }
                delete ss;
            }
            _hosts.clear();
        }

        void checkVersions( const string& ns ){
            vector<Shard> all;
            Shard::getAllShards( all );
            for ( unsigned i=0; i<all.size(); i++ ){
                Status* &s = _hosts[all[i].getConnString()];
                if ( ! s )
                    s = new Status();
            }

            for ( map<string,Status*>::iterator i=_hosts.begin(); i!=_hosts.end(); ++i ){
                if ( ! Shard::isAShard( i->first ) )
                    continue;
                Status* ss = i->second;
                assert( ss );
                if ( ! ss->avail )
                    ss->avail = pool.get( i->first );
                checkShardVersion( *ss->avail , ns );
            }
        }

        void release( const string& addr , DBClientBase * conn ){
            resetShardVersion( conn );
            BSONObj res;
            
            try {
                if ( conn->simpleCommand( "admin" , &res , "unsetSharding" ) ){
                    pool.release( addr , conn );
                }
                else {
                    log(LL_ERROR) << " couldn't unset sharding :( " << res << endl;
                    delete conn;
                }
            }
            catch ( std::exception& e ){
                log(LL_ERROR) << "couldn't unsert sharding : " << e.what() << endl;
                delete conn;
            }
        }
        
        void _check( const string& ns ){
            if ( ns.size() == 0 || _seenNS.count( ns ) )
                return;
            _seenNS.insert( ns );
            checkVersions( ns );
        }

        map<string,Status*> _hosts;
        set<string> _seenNS;
        // -----
        
        static thread_specific_ptr<ClientConnections> _perThread;

        static ClientConnections* threadInstance(){
            ClientConnections* cc = _perThread.get();
            if ( ! cc ){
                cc = new ClientConnections();
                _perThread.reset( cc );
            }
            return cc;
        }
    };

    thread_specific_ptr<ClientConnections> ClientConnections::_perThread;

    ShardConnection::ShardConnection( const Shard * s , const string& ns )
        : _addr( s->getConnString() ) , _ns( ns ) {
        _init();
    }

    ShardConnection::ShardConnection( const Shard& s , const string& ns )
        : _addr( s.getConnString() ) , _ns( ns ) {
        _init();
    }
    
    ShardConnection::ShardConnection( const string& addr , const string& ns )
        : _addr( addr ) , _ns( ns ) {
        _init();
    }
    
    void ShardConnection::_init(){
        assert( _addr.size() );
        _conn = ClientConnections::threadInstance()->get( _addr , _ns );
        _finishedInit = false;
    }

    void ShardConnection::_finishInit(){
        if ( _finishedInit )
            return;
        _finishedInit = true;
        
        if ( _ns.size() ){
            _setVersion = checkShardVersion( *_conn , _ns );
        }
        else {
            _setVersion = false;
        }
        
    }

    void ShardConnection::done(){
        if ( _conn ){
            ClientConnections::threadInstance()->done( _addr , _conn );
            _conn = 0;
            _finishedInit = true;
        }
    }

    void ShardConnection::kill(){
        if ( _conn ){
            delete _conn;
            _conn = 0;
            _finishedInit = true;
        }
    }

    void ShardConnection::sync(){
        ClientConnections::threadInstance()->sync();
    }

    bool ShardConnection::runCommand( const string& db , const BSONObj& cmd , BSONObj& res ){
        assert( _conn );
        bool ok = _conn->runCommand( db , cmd , res );
        if ( ! ok ){
            if ( res["code"].numberInt() == StaleConfigInContextCode ){
                string big = res["errmsg"].String();
                string ns,raw;
                massert( 13409 , (string)"can't parse ns from: " + big  , StaleConfigException::parse( big , ns , raw ) );
                done();
                throw StaleConfigException( ns , raw );
            }
        }
        return ok;
    }

    void ShardConnection::checkMyConnectionVersions( const string & ns ){
        ClientConnections::threadInstance()->checkVersions( ns );
    }

    ShardConnection::~ShardConnection() {
        if ( _conn ){
            if ( ! _conn->isFailed() ) {
                /* see done() comments above for why we log this line */
                log() << "~ScopedDBConnection: _conn != null" << endl;
            }
            kill();
        }
    }
}
