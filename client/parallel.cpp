// parallel.cpp
/*
 *    Copyright 2010 10gen Inc.
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


#include "stdafx.h"
#include "parallel.h"
#include "connpool.h"
#include "../db/queryutil.h"
#include "../db/dbmessage.h"
#include "../s/util.h"

namespace mongo {
    
    // --------  ClusteredCursor -----------
    
    ClusteredCursor::ClusteredCursor( QueryMessage& q ){
        _ns = q.ns;
        _query = q.query.copy();
        _options = q.queryOptions;
        _fields = q.fields;
        _done = false;
    }

    ClusteredCursor::ClusteredCursor( const string& ns , const BSONObj& q , int options , const BSONObj& fields ){
        _ns = ns;
        _query = q.getOwned();
        _options = options;
        _fields = fields.getOwned();
        _done = false;
    }

    ClusteredCursor::~ClusteredCursor(){
        _done = true; // just in case
    }
    
    auto_ptr<DBClientCursor> ClusteredCursor::query( const string& server , int num , BSONObj extra ){
        uassert( 10017 ,  "cursor already done" , ! _done );
        
        BSONObj q = _query;
        if ( ! extra.isEmpty() ){
            q = concatQuery( q , extra );
        }

        ScopedDbConnection conn( server );
        checkShardVersion( conn.conn() , _ns );

        if ( logLevel >= 5 ){
            log(5) << "ClusteredCursor::query (" << type() << ") server:" << server 
                   << " ns:" << _ns << " query:" << q << " num:" << num << 
                " _fields:" << _fields << " options: " << _options << endl;
        }
        
        auto_ptr<DBClientCursor> cursor = 
            conn->query( _ns.c_str() , q , num , 0 , ( _fields.isEmpty() ? 0 : &_fields ) , _options );
        
        if ( cursor->hasResultFlag( QueryResult::ResultFlag_ShardConfigStale ) )
            throw StaleConfigException( _ns , "ClusteredCursor::query" );

        conn.done();
        return cursor;
    }

    BSONObj ClusteredCursor::concatQuery( const BSONObj& query , const BSONObj& extraFilter ){
        if ( ! query.hasField( "query" ) )
            return _concatFilter( query , extraFilter );
        
        BSONObjBuilder b;
        BSONObjIterator i( query );
        while ( i.more() ){
            BSONElement e = i.next();

            if ( strcmp( e.fieldName() , "query" ) ){
                b.append( e );
                continue;
            }
            
            b.append( "query" , _concatFilter( e.embeddedObjectUserCheck() , extraFilter ) );
        }
        return b.obj();
    }
    
    BSONObj ClusteredCursor::_concatFilter( const BSONObj& filter , const BSONObj& extra ){
        BSONObjBuilder b;
        b.appendElements( filter );
        b.appendElements( extra );
        return b.obj();
        // TODO: should do some simplification here if possibl ideally
    }

    
    // --------  FilteringClientCursor -----------
    FilteringClientCursor::FilteringClientCursor( const BSONObj filter )
        : _matcher( filter ) , _done( false ){
    }

    FilteringClientCursor::FilteringClientCursor( auto_ptr<DBClientCursor> cursor , const BSONObj filter )
        : _matcher( filter ) , _cursor( cursor ) , _done( cursor.get() == 0 ){
    }
    
    FilteringClientCursor::~FilteringClientCursor(){
    }
        
    void FilteringClientCursor::reset( auto_ptr<DBClientCursor> cursor ){
        _cursor = cursor;
        _next = BSONObj();
        _done = _cursor.get() == 0;
    }

    bool FilteringClientCursor::more(){
        if ( ! _next.isEmpty() )
            return true;
        
        if ( _done )
            return false;
        
        _advance();
        return ! _next.isEmpty();
    }
    
    BSONObj FilteringClientCursor::next(){
        assert( ! _next.isEmpty() );
        assert( ! _done );

        BSONObj ret = _next;
        _next = BSONObj();
        _advance();
        return ret;
    }
    
    void FilteringClientCursor::_advance(){
        assert( _next.isEmpty() );
        if ( ! _cursor.get() )
            return;
        
        while ( _cursor->more() ){
            _next = _cursor->next();
            if ( _matcher.matches( _next ) ){
                if ( ! _cursor->moreInCurrentBatch() )
                    _next = _next.getOwned();
                return;
            }
            _next = BSONObj();
        }
        _done = true;
    }
    
    // --------  SerialServerClusteredCursor -----------
    
    SerialServerClusteredCursor::SerialServerClusteredCursor( const set<ServerAndQuery>& servers , QueryMessage& q , int sortOrder) : ClusteredCursor( q ){
        for ( set<ServerAndQuery>::const_iterator i = servers.begin(); i!=servers.end(); i++ )
            _servers.push_back( *i );
        
        if ( sortOrder > 0 )
            sort( _servers.begin() , _servers.end() );
        else if ( sortOrder < 0 )
            sort( _servers.rbegin() , _servers.rend() );
        
        _serverIndex = 0;
    }
    
    bool SerialServerClusteredCursor::more(){
        if ( _current.more() )
            return true;
        
        if ( _serverIndex >= _servers.size() ){
            return false;
        }
        
        ServerAndQuery& sq = _servers[_serverIndex++];

        _current.reset( query( sq._server , 0 , sq._extra ) );
        if ( _current.more() )
            return true;
        
        // this sq has nothing, so keep looking
        return more();
    }
    
    BSONObj SerialServerClusteredCursor::next(){
        uassert( 10018 ,  "no more items" , more() );
        return _current.next();
    }

    // --------  ParallelSortClusteredCursor -----------
    
    ParallelSortClusteredCursor::ParallelSortClusteredCursor( const set<ServerAndQuery>& servers , QueryMessage& q , 
                                                              const BSONObj& sortKey ) 
        : ClusteredCursor( q ) , _servers( servers ){
        _sortKey = sortKey.getOwned();
        _init();
    }

    ParallelSortClusteredCursor::ParallelSortClusteredCursor( const set<ServerAndQuery>& servers , const string& ns , 
                                                              const Query& q , 
                                                              int options , const BSONObj& fields  )
        : ClusteredCursor( ns , q.obj , options , fields ) , _servers( servers ){
        _sortKey = q.getSort().copy();
        _init();
    }

    void ParallelSortClusteredCursor::_init(){
        _numServers = _servers.size();
        _cursors = new FilteringClientCursor[_numServers];
        _nexts = new BSONObj[_numServers];
            
        // TODO: parellize
        int num = 0;
        for ( set<ServerAndQuery>::iterator i = _servers.begin(); i!=_servers.end(); i++ ){
            const ServerAndQuery& sq = *i;
            _cursors[num++].reset( query( sq._server , 0 , sq._extra ) );
        }
            
    }
    
    ParallelSortClusteredCursor::~ParallelSortClusteredCursor(){
        delete [] _cursors;
        delete [] _nexts;
    }

    bool ParallelSortClusteredCursor::more(){
        for ( int i=0; i<_numServers; i++ ){
            if ( ! _nexts[i].isEmpty() )
                return true;

            if ( _cursors[i].more() )
                return true;
        }
        return false;
    }
        
    BSONObj ParallelSortClusteredCursor::next(){
        advance();
            
        BSONObj best = BSONObj();
        int bestFrom = -1;
            
        for ( int i=0; i<_numServers; i++){
            if ( _nexts[i].isEmpty() )
                continue;

            if ( best.isEmpty() ){
                best = _nexts[i];
                bestFrom = i;
                continue;
            }
                
            int comp = best.woSortOrder( _nexts[i] , _sortKey );
            if ( comp < 0 )
                continue;
                
            best = _nexts[i];
            bestFrom = i;
        }
            
        uassert( 10019 ,  "no more elements" , ! best.isEmpty() );
        _nexts[bestFrom] = BSONObj();
            
        return best;
    }

    void ParallelSortClusteredCursor::advance(){
        for ( int i=0; i<_numServers; i++ ){

            if ( ! _nexts[i].isEmpty() ){
                // already have a good object there
                continue;
            }
                
            if ( ! _cursors[i].more() ){
                // cursor is dead, oh well
                continue;
            }

            _nexts[i] = _cursors[i].next();
        }
            
    }

    // -----------------
    // ---- Future -----
    // -----------------

    Future::CommandResult::CommandResult( const string& server , const string& db , const BSONObj& cmd ){
        _server = server;
        _db = db;
        _cmd = cmd;
        _done = false;
    }

    bool Future::CommandResult::join(){
        while ( ! _done )
            sleepmicros( 50 );
        return _ok;
    }

    void Future::commandThread(){
        assert( _grab );
        shared_ptr<CommandResult> res = *_grab;
        _grab = 0;
        
        ScopedDbConnection conn( res->_server );
        res->_ok = conn->runCommand( res->_db , res->_cmd , res->_res );
        res->_done = true;
    }

    shared_ptr<Future::CommandResult> Future::spawnCommand( const string& server , const string& db , const BSONObj& cmd ){
        shared_ptr<Future::CommandResult> res;
        res.reset( new Future::CommandResult( server , db , cmd ) );
        
        _grab = &res;
        
        boost::thread thr( Future::commandThread );

        while ( _grab )
            sleepmicros(2);

        return res;
    }

    shared_ptr<Future::CommandResult> * Future::_grab;
    
    
}
