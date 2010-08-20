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


#include "pch.h"
#include "parallel.h"
#include "connpool.h"
#include "../db/queryutil.h"
#include "../db/dbmessage.h"
#include "../s/util.h"
#include "../s/shard.h"

namespace mongo {
    
    // --------  ClusteredCursor -----------
    
    ClusteredCursor::ClusteredCursor( QueryMessage& q ){
        _ns = q.ns;
        _query = q.query.copy();
        _options = q.queryOptions;
        _fields = q.fields.copy();
        _batchSize = q.ntoreturn;
        if ( _batchSize == 1 )
            _batchSize = 2;

        _done = false;
        _didInit = false;
    }

    ClusteredCursor::ClusteredCursor( const string& ns , const BSONObj& q , int options , const BSONObj& fields ){
        _ns = ns;
        _query = q.getOwned();
        _options = options;
        _fields = fields.getOwned();
        _batchSize = 0;

        _done = false;
        _didInit = false;
    }

    ClusteredCursor::~ClusteredCursor(){
        _done = true; // just in case
    }

    void ClusteredCursor::init(){
        if ( _didInit )
            return;
        _didInit = true;
        _init();
    }
    
    auto_ptr<DBClientCursor> ClusteredCursor::query( const string& server , int num , BSONObj extra , int skipLeft ){
        uassert( 10017 ,  "cursor already done" , ! _done );
        assert( _didInit );
        
        BSONObj q = _query;
        if ( ! extra.isEmpty() ){
            q = concatQuery( q , extra );
        }

        ShardConnection conn( server , _ns );
        
        if ( conn.setVersion() ){
            conn.done();
            throw StaleConfigException( _ns , "ClusteredCursor::query ShardConnection had to change" , true );
        }

        if ( logLevel >= 5 ){
            log(5) << "ClusteredCursor::query (" << type() << ") server:" << server 
                   << " ns:" << _ns << " query:" << q << " num:" << num 
                   << " _fields:" << _fields << " options: " << _options << endl;
        }
        
        auto_ptr<DBClientCursor> cursor = 
            conn->query( _ns , q , num , 0 , ( _fields.isEmpty() ? 0 : &_fields ) , _options , _batchSize == 0 ? 0 : _batchSize + skipLeft );

        assert( cursor.get() );
        
        if ( cursor->hasResultFlag( ResultFlag_ShardConfigStale ) ){
            conn.done();
            throw StaleConfigException( _ns , "ClusteredCursor::query" );
        }
        
        if ( cursor->hasResultFlag( ResultFlag_ErrSet ) ){
            conn.done();
            BSONObj o = cursor->next();
            throw UserException( o["code"].numberInt() , o["$err"].String() );
        }


        cursor->attach( &conn );

        conn.done();
        return cursor;
    }

    BSONObj ClusteredCursor::explain( const string& server , BSONObj extra ){
        BSONObj q = _query;
        if ( ! extra.isEmpty() ){
            q = concatQuery( q , extra );
        }

        ShardConnection conn( server , _ns );
        BSONObj o = conn->findOne( _ns , Query( q ).explain() );
        conn.done();
        return o;
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

    BSONObj ClusteredCursor::explain(){
        BSONObjBuilder b;
        b.append( "clusteredType" , type() );

        long long nscanned = 0;
        long long nscannedObjects = 0;
        long long n = 0;
        long long millis = 0;
        double numExplains = 0;
        
        map<string,list<BSONObj> > out;
        {
            _explain( out );
            
            BSONObjBuilder x( b.subobjStart( "shards" ) );
            for ( map<string,list<BSONObj> >::iterator i=out.begin(); i!=out.end(); ++i ){
                string shard = i->first;
                list<BSONObj> l = i->second;
                BSONArrayBuilder y( x.subarrayStart( shard ) );
                for ( list<BSONObj>::iterator j=l.begin(); j!=l.end(); ++j ){
                    BSONObj temp = *j;
                    y.append( temp );

                    nscanned += temp["nscanned"].numberLong();
                    nscannedObjects += temp["nscannedObjects"].numberLong();
                    n += temp["n"].numberLong();
                    millis += temp["millis"].numberLong();
                    numExplains++;
                }
                y.done();
            }
            x.done();
        }

        b.appendNumber( "nscanned" , nscanned );
        b.appendNumber( "nscannedObjects" , nscannedObjects );
        b.appendNumber( "n" , n );
        b.appendNumber( "millisTotal" , millis );
        b.append( "millisAvg" , (int)((double)millis / numExplains ) );
        b.append( "numQueries" , (int)numExplains );
        b.append( "numShards" , (int)out.size() );

        return b.obj();
    }
    
    // --------  FilteringClientCursor -----------
    FilteringClientCursor::FilteringClientCursor( const BSONObj filter )
        : _matcher( filter ) , _done( true ){
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

    BSONObj FilteringClientCursor::peek(){
        if ( _next.isEmpty() )
            _advance();
        return _next;
    }
    
    void FilteringClientCursor::_advance(){
        assert( _next.isEmpty() );
        if ( ! _cursor.get() || _done )
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

        _needToSkip = q.ntoskip;
    }
    
    bool SerialServerClusteredCursor::more(){
        
        // TODO: optimize this by sending on first query and then back counting
        //       tricky in case where 1st server doesn't have any after
        //       need it to send n skipped
        while ( _needToSkip > 0 && _current.more() ){
            _current.next();
            _needToSkip--;
        }
        
        if ( _current.more() )
            return true;
        
        if ( _serverIndex >= _servers.size() ){
            return false;
        }
        
        ServerAndQuery& sq = _servers[_serverIndex++];

        _current.reset( query( sq._server , 0 , sq._extra ) );
        return more();
    }
    
    BSONObj SerialServerClusteredCursor::next(){
        uassert( 10018 ,  "no more items" , more() );
        return _current.next();
    }

    void SerialServerClusteredCursor::_explain( map< string,list<BSONObj> >& out ){
        for ( unsigned i=0; i<_servers.size(); i++ ){
            ServerAndQuery& sq = _servers[i];
            list<BSONObj> & l = out[sq._server];
            l.push_back( explain( sq._server , sq._extra ) );
        }
    }

    // --------  ParallelSortClusteredCursor -----------
    
    ParallelSortClusteredCursor::ParallelSortClusteredCursor( const set<ServerAndQuery>& servers , QueryMessage& q , 
                                                              const BSONObj& sortKey ) 
        : ClusteredCursor( q ) , _servers( servers ){
        _sortKey = sortKey.getOwned();
        _needToSkip = q.ntoskip;
        _finishCons();
    }

    ParallelSortClusteredCursor::ParallelSortClusteredCursor( const set<ServerAndQuery>& servers , const string& ns , 
                                                              const Query& q , 
                                                              int options , const BSONObj& fields  )
        : ClusteredCursor( ns , q.obj , options , fields ) , _servers( servers ){
        _sortKey = q.getSort().copy();
        _needToSkip = 0;
        _finishCons();
    }

    void ParallelSortClusteredCursor::_finishCons(){
        _numServers = _servers.size();
        _cursors = 0;

        if ( ! _sortKey.isEmpty() && ! _fields.isEmpty() ){
            // we need to make sure the sort key is in the project

            set<string> sortKeyFields;
            _sortKey.getFieldNames(sortKeyFields);

            BSONObjBuilder b;
            bool isNegative = false;
            {
                BSONObjIterator i( _fields );
                while ( i.more() ){
                    BSONElement e = i.next();
                    b.append( e );

                    string fieldName = e.fieldName();

                    // exact field
                    bool found = sortKeyFields.erase(fieldName);

                    // subfields
                    set<string>::const_iterator begin = sortKeyFields.lower_bound(fieldName + ".\x00");
                    set<string>::const_iterator end   = sortKeyFields.lower_bound(fieldName + ".\xFF");
                    sortKeyFields.erase(begin, end);

                    if ( ! e.trueValue() ) {
                        uassert( 13431 , "have to have sort key in projection and removing it" , !found && begin == end );
                    } else if (!e.isABSONObj()) {
                        isNegative = true;
                    }
                }
            }                    
            
            if (isNegative){
                for (set<string>::const_iterator it(sortKeyFields.begin()), end(sortKeyFields.end()); it != end; ++it){
                    b.append(*it, 1);
                }
            }
            
            _fields = b.obj();
        }
    }
    
    void ParallelSortClusteredCursor::_init(){
        assert( ! _cursors );
        _cursors = new FilteringClientCursor[_numServers];
            
        // TODO: parellize
        int num = 0;
        for ( set<ServerAndQuery>::iterator i = _servers.begin(); i!=_servers.end(); ++i ){
            const ServerAndQuery& sq = *i;
            _cursors[num++].reset( query( sq._server , 0 , sq._extra , _needToSkip ) );
        }
            
    }
    
    ParallelSortClusteredCursor::~ParallelSortClusteredCursor(){
        delete [] _cursors;
        _cursors = 0;
    }

    bool ParallelSortClusteredCursor::more(){

        if ( _needToSkip > 0 ){
            int n = _needToSkip;
            _needToSkip = 0;

            while ( n > 0 && more() ){
                BSONObj x = next();
                n--;
            }

            _needToSkip = n;
        }
        
        for ( int i=0; i<_numServers; i++ ){
            if ( _cursors[i].more() )
                return true;
        }
        return false;
    }
        
    BSONObj ParallelSortClusteredCursor::next(){
        BSONObj best = BSONObj();
        int bestFrom = -1;
            
        for ( int i=0; i<_numServers; i++){
            if ( ! _cursors[i].more() )
                continue;
            
            BSONObj me = _cursors[i].peek();

            if ( best.isEmpty() ){
                best = me;
                bestFrom = i;
                continue;
            }
                
            int comp = best.woSortOrder( me , _sortKey , true );
            if ( comp < 0 )
                continue;
                
            best = me;
            bestFrom = i;
        }
        
        uassert( 10019 ,  "no more elements" , ! best.isEmpty() );
        _cursors[bestFrom].next();
            
        return best;
    }

    void ParallelSortClusteredCursor::_explain( map< string,list<BSONObj> >& out ){
        for ( set<ServerAndQuery>::iterator i=_servers.begin(); i!=_servers.end(); ++i ){
            const ServerAndQuery& sq = *i;
            list<BSONObj> & l = out[sq._server];
            l.push_back( explain( sq._server , sq._extra ) );
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
        conn.done();
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
