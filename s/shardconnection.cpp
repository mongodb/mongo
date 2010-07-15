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
     */
    class ClientConnections : boost::noncopyable {
    public:
        struct Status : boost::noncopyable {
            Status() : created(0){}
            
            std::stack<DBClientBase*> avail;
            long long created;
        };


        Nullstream& debug( Status * s = 0 , const string& addr = "" ){
            static int ll = 9;

            if ( logLevel < ll )
                return nullstream;
            Nullstream& l = log(ll);
            
            l << "ClientConnections DEBUG " << this << " ";
            if ( s ){
                l << "s: " << s << " addr: " << addr << " ";
            }
            return l;
        }
        
        ClientConnections() : _mutex("ClientConnections") {
            debug() << " NEW  " << endl;
        }
        
        ~ClientConnections(){
            debug() << " KILLING  " << endl;
            for ( map<string,Status*>::iterator i=_hosts.begin(); i!=_hosts.end(); ++i ){
                string addr = i->first;
                Status* ss = i->second;
                assert( ss );
                std::stack<DBClientBase*>& s = ss->avail;
                while ( ! s.empty() ){
                    pool.release( addr , s.top() );
                    s.pop();
                }
                delete ss;
            }
            _hosts.clear();
        }
        
        DBClientBase * get( const string& addr ){
            scoped_lock lk( _mutex );
            Status* &s = _hosts[addr];
            if ( ! s )
                s = new Status();
            
            debug() << "WANT ONE pool empty: " << s->avail.empty() << endl;
            
            if ( ! s->avail.empty() ){
                DBClientBase* c = s->avail.top();
                s->avail.pop();
                debug( s , addr ) << "GOT  " << c << endl;
                pool.onHandedOut( c );
                return c;
            }

            debug() << "CREATING NEW CONNECTION" << endl;
            s->created++;
            return pool.get( addr );
        }
        
        void done( const string& addr , DBClientBase* conn ){
            scoped_lock lk( _mutex );
            Status* s = _hosts[addr];
            assert( s );
            if ( s->avail.size() > 0 ){
                delete conn;
                return;
            }
            s->avail.push( conn );
            debug( s , addr ) << "PUSHING: " << conn << endl;
        }
        
        void sync(){
            scoped_lock lk( _mutex );
            for ( map<string,Status*>::iterator i=_hosts.begin(); i!=_hosts.end(); ++i ){
                string addr = i->first;
                Status* ss = i->second;
                assert( ss );
                std::stack<DBClientBase*>& s = ss->avail;
                while ( ! s.empty() ){
                    DBClientBase* conn = s.top();
                    conn->getLastError();
                    pool.release( addr , conn );
                    s.pop();
                }
                delete ss;
            }
            _hosts.clear();
        }
        
        map<string,Status*> _hosts;
        mongo::mutex _mutex;

        // -----
        
        static thread_specific_ptr<ClientConnections> _perThread;

        static ClientConnections* get(){
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
        _conn = ClientConnections::get()->get( _addr );
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
            ClientConnections::get()->done( _addr , _conn );
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
        ClientConnections::get()->sync();
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
