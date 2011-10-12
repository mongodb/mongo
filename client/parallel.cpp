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

    ClusteredCursor::ClusteredCursor( QueryMessage& q ) {
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

    ClusteredCursor::ClusteredCursor( const string& ns , const BSONObj& q , int options , const BSONObj& fields ) {
        _ns = ns;
        _query = q.getOwned();
        _options = options;
        _fields = fields.getOwned();
        _batchSize = 0;

        _done = false;
        _didInit = false;
    }

    ClusteredCursor::~ClusteredCursor() {
        _done = true; // just in case
    }

    void ClusteredCursor::init() {
        if ( _didInit )
            return;
        _didInit = true;
        _init();
    }

    void ClusteredCursor::_checkCursor( DBClientCursor * cursor ) {
        assert( cursor );
        
        if ( cursor->hasResultFlag( ResultFlag_ShardConfigStale ) ) {
            throw StaleConfigException( _ns , "ClusteredCursor::_checkCursor" );
        }
        
        if ( cursor->hasResultFlag( ResultFlag_ErrSet ) ) {
            BSONObj o = cursor->next();
            throw UserException( o["code"].numberInt() , o["$err"].String() );
        }
    }

    auto_ptr<DBClientCursor> ClusteredCursor::query( const string& server , int num , BSONObj extra , int skipLeft , bool lazy ) {
        uassert( 10017 ,  "cursor already done" , ! _done );
        assert( _didInit );

        BSONObj q = _query;
        if ( ! extra.isEmpty() ) {
            q = concatQuery( q , extra );
        }

        try {
            ShardConnection conn( server , _ns );
            
            if ( conn.setVersion() ) {
                conn.done();
                throw StaleConfigException( _ns , "ClusteredCursor::query" , true );
            }
            
            LOG(5) << "ClusteredCursor::query (" << type() << ") server:" << server
                   << " ns:" << _ns << " query:" << q << " num:" << num
                   << " _fields:" << _fields << " options: " << _options << endl;
        
            auto_ptr<DBClientCursor> cursor =
                conn->query( _ns , q , num , 0 , ( _fields.isEmpty() ? 0 : &_fields ) , _options , _batchSize == 0 ? 0 : _batchSize + skipLeft );
            
            if ( ! cursor.get() && _options & QueryOption_PartialResults ) {
                _done = true;
                conn.done();
                return cursor;
            }
            
            massert( 13633 , str::stream() << "error querying server: " << server  , cursor.get() );
            
            cursor->attach( &conn ); // this calls done on conn
            assert( ! conn.ok() );
            _checkCursor( cursor.get() );
            return cursor;
        }
        catch ( SocketException& e ) {
            if ( ! ( _options & QueryOption_PartialResults ) )
                throw e;
            _done = true;
            return auto_ptr<DBClientCursor>();
        }
    }

    BSONObj ClusteredCursor::explain( const string& server , BSONObj extra ) {
        BSONObj q = _query;
        if ( ! extra.isEmpty() ) {
            q = concatQuery( q , extra );
        }

        BSONObj o;

        ShardConnection conn( server , _ns );
        auto_ptr<DBClientCursor> cursor = conn->query( _ns , Query( q ).explain() , abs( _batchSize ) * -1 , 0 , _fields.isEmpty() ? 0 : &_fields );
        if ( cursor.get() && cursor->more() )
            o = cursor->next().getOwned();
        conn.done();
        return o;
    }

    BSONObj ClusteredCursor::concatQuery( const BSONObj& query , const BSONObj& extraFilter ) {
        if ( ! query.hasField( "query" ) )
            return _concatFilter( query , extraFilter );

        BSONObjBuilder b;
        BSONObjIterator i( query );
        while ( i.more() ) {
            BSONElement e = i.next();

            if ( strcmp( e.fieldName() , "query" ) ) {
                b.append( e );
                continue;
            }

            b.append( "query" , _concatFilter( e.embeddedObjectUserCheck() , extraFilter ) );
        }
        return b.obj();
    }

    BSONObj ClusteredCursor::_concatFilter( const BSONObj& filter , const BSONObj& extra ) {
        BSONObjBuilder b;
        b.appendElements( filter );
        b.appendElements( extra );
        return b.obj();
        // TODO: should do some simplification here if possibl ideally
    }

    BSONObj ClusteredCursor::explain() {
        // Note: by default we filter out allPlans and oldPlan in the shell's
        // explain() function. If you add any recursive structures, make sure to
        // edit the JS to make sure everything gets filtered.

        BSONObjBuilder b;
        b.append( "clusteredType" , type() );

        long long millis = 0;
        double numExplains = 0;

        map<string,long long> counters;

        map<string,list<BSONObj> > out;
        {
            _explain( out );

            BSONObjBuilder x( b.subobjStart( "shards" ) );
            for ( map<string,list<BSONObj> >::iterator i=out.begin(); i!=out.end(); ++i ) {
                string shard = i->first;
                list<BSONObj> l = i->second;
                BSONArrayBuilder y( x.subarrayStart( shard ) );
                for ( list<BSONObj>::iterator j=l.begin(); j!=l.end(); ++j ) {
                    BSONObj temp = *j;
                    y.append( temp );

                    BSONObjIterator k( temp );
                    while ( k.more() ) {
                        BSONElement z = k.next();
                        if ( z.fieldName()[0] != 'n' )
                            continue;
                        long long& c = counters[z.fieldName()];
                        c += z.numberLong();
                    }

                    millis += temp["millis"].numberLong();
                    numExplains++;
                }
                y.done();
            }
            x.done();
        }

        for ( map<string,long long>::iterator i=counters.begin(); i!=counters.end(); ++i )
            b.appendNumber( i->first , i->second );

        b.appendNumber( "millisTotal" , millis );
        b.append( "millisAvg" , (int)((double)millis / numExplains ) );
        b.append( "numQueries" , (int)numExplains );
        b.append( "numShards" , (int)out.size() );

        return b.obj();
    }

    // --------  FilteringClientCursor -----------
    FilteringClientCursor::FilteringClientCursor( const BSONObj filter )
        : _matcher( filter ) , _done( true ) {
    }

    FilteringClientCursor::FilteringClientCursor( auto_ptr<DBClientCursor> cursor , const BSONObj filter )
        : _matcher( filter ) , _cursor( cursor ) , _done( cursor.get() == 0 ) {
    }

    FilteringClientCursor::FilteringClientCursor( DBClientCursor* cursor , const BSONObj filter )
        : _matcher( filter ) , _cursor( cursor ) , _done( cursor == 0 ) {
    }


    FilteringClientCursor::~FilteringClientCursor() {
    }

    void FilteringClientCursor::reset( auto_ptr<DBClientCursor> cursor ) {
        _cursor = cursor;
        _next = BSONObj();
        _done = _cursor.get() == 0;
    }

    void FilteringClientCursor::reset( DBClientCursor* cursor ) {
        _cursor.reset( cursor );
        _next = BSONObj();
        _done = cursor == 0;
    }


    bool FilteringClientCursor::more() {
        if ( ! _next.isEmpty() )
            return true;

        if ( _done )
            return false;

        _advance();
        return ! _next.isEmpty();
    }

    BSONObj FilteringClientCursor::next() {
        assert( ! _next.isEmpty() );
        assert( ! _done );

        BSONObj ret = _next;
        _next = BSONObj();
        _advance();
        return ret;
    }

    BSONObj FilteringClientCursor::peek() {
        if ( _next.isEmpty() )
            _advance();
        return _next;
    }

    void FilteringClientCursor::_advance() {
        assert( _next.isEmpty() );
        if ( ! _cursor.get() || _done )
            return;

        while ( _cursor->more() ) {
            _next = _cursor->next();
            if ( _matcher.matches( _next ) ) {
                if ( ! _cursor->moreInCurrentBatch() )
                    _next = _next.getOwned();
                return;
            }
            _next = BSONObj();
        }
        _done = true;
    }

    // --------  SerialServerClusteredCursor -----------

    SerialServerClusteredCursor::SerialServerClusteredCursor( const set<ServerAndQuery>& servers , QueryMessage& q , int sortOrder) : ClusteredCursor( q ) {
        for ( set<ServerAndQuery>::const_iterator i = servers.begin(); i!=servers.end(); i++ )
            _servers.push_back( *i );

        if ( sortOrder > 0 )
            sort( _servers.begin() , _servers.end() );
        else if ( sortOrder < 0 )
            sort( _servers.rbegin() , _servers.rend() );

        _serverIndex = 0;

        _needToSkip = q.ntoskip;
    }

    bool SerialServerClusteredCursor::more() {

        // TODO: optimize this by sending on first query and then back counting
        //       tricky in case where 1st server doesn't have any after
        //       need it to send n skipped
        while ( _needToSkip > 0 && _current.more() ) {
            _current.next();
            _needToSkip--;
        }

        if ( _current.more() )
            return true;

        if ( _serverIndex >= _servers.size() ) {
            return false;
        }

        ServerAndQuery& sq = _servers[_serverIndex++];

        _current.reset( query( sq._server , 0 , sq._extra ) );
        return more();
    }

    BSONObj SerialServerClusteredCursor::next() {
        uassert( 10018 ,  "no more items" , more() );
        return _current.next();
    }

    void SerialServerClusteredCursor::_explain( map< string,list<BSONObj> >& out ) {
        for ( unsigned i=0; i<_servers.size(); i++ ) {
            ServerAndQuery& sq = _servers[i];
            list<BSONObj> & l = out[sq._server];
            l.push_back( explain( sq._server , sq._extra ) );
        }
    }

    // --------  ParallelSortClusteredCursor -----------

    ParallelSortClusteredCursor::ParallelSortClusteredCursor( const set<ServerAndQuery>& servers , QueryMessage& q ,
            const BSONObj& sortKey )
        : ClusteredCursor( q ) , _servers( servers ) {
        _sortKey = sortKey.getOwned();
        _needToSkip = q.ntoskip;
        _finishCons();
    }

    ParallelSortClusteredCursor::ParallelSortClusteredCursor( const set<ServerAndQuery>& servers , const string& ns ,
            const Query& q ,
            int options , const BSONObj& fields  )
        : ClusteredCursor( ns , q.obj , options , fields ) , _servers( servers ) {
        _sortKey = q.getSort().copy();
        _needToSkip = 0;
        _finishCons();
    }

    void ParallelSortClusteredCursor::_finishCons() {
        _numServers = _servers.size();
        _cursors = 0;

        if ( ! _sortKey.isEmpty() && ! _fields.isEmpty() ) {
            // we need to make sure the sort key is in the projection

            set<string> sortKeyFields;
            _sortKey.getFieldNames(sortKeyFields);

            BSONObjBuilder b;
            bool isNegative = false;
            {
                BSONObjIterator i( _fields );
                while ( i.more() ) {
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
                    }
                    else if (!e.isABSONObj()) {
                        isNegative = true;
                    }
                }
            }

            if (isNegative) {
                for (set<string>::const_iterator it(sortKeyFields.begin()), end(sortKeyFields.end()); it != end; ++it) {
                    b.append(*it, 1);
                }
            }

            _fields = b.obj();
        }
    }

    // TODO:  Merge with futures API?  We do a lot of error checking here that would be useful elsewhere.
    void ParallelSortClusteredCursor::_init() {

        // log() << "Starting parallel search..." << endl;

        // make sure we're not already initialized
        assert( ! _cursors );
        _cursors = new FilteringClientCursor[_numServers];

        bool returnPartial = ( _options & QueryOption_PartialResults );

        vector<ServerAndQuery> queries( _servers.begin(), _servers.end() );
        set<int> retryQueries;
        int finishedQueries = 0;

        vector< shared_ptr<ShardConnection> > conns;
        vector<string> servers;

        // Since we may get all sorts of errors, record them all as they come and throw them later if necessary
        vector<string> staleConfigExs;
        vector<string> socketExs;
        vector<string> otherExs;
        bool allConfigStale = false;

        int retries = -1;

        // Loop through all the queries until we've finished or gotten a socket exception on all of them
        // We break early for non-socket exceptions, and socket exceptions if we aren't returning partial results
        do {
            retries++;

            bool firstPass = retryQueries.size() == 0;

            if( ! firstPass ){
                log() << "retrying " << ( returnPartial ? "(partial) " : "" ) << "parallel connection to ";
                for( set<int>::iterator it = retryQueries.begin(); it != retryQueries.end(); ++it ){
                    log() << queries[*it]._server << ", ";
                }
                log() << finishedQueries << " finished queries." << endl;
            }

            size_t num = 0;
            for ( vector<ServerAndQuery>::iterator it = queries.begin(); it != queries.end(); ++it ) {
                size_t i = num++;

                const ServerAndQuery& sq = *it;

                // If we're not retrying this cursor on later passes, continue
                if( ! firstPass && retryQueries.find( i ) == retryQueries.end() ) continue;

                // log() << "Querying " << _query << " from " << _ns << " for " << sq._server << endl;

                BSONObj q = _query;
                if ( ! sq._extra.isEmpty() ) {
                    q = concatQuery( q , sq._extra );
                }

                string errLoc = " @ " + sq._server;

                if( firstPass ){

                    // This may be the first time connecting to this shard, if so we can get an error here
                    try {
                        conns.push_back( shared_ptr<ShardConnection>( new ShardConnection( sq._server , _ns ) ) );
                    }
                    catch( std::exception& e ){
                        socketExs.push_back( e.what() + errLoc );
                        if( ! returnPartial ){
                            num--;
                            break;
                        }
                        conns.push_back( shared_ptr<ShardConnection>() );
                        continue;
                    }

                    servers.push_back( sq._server );
                }

                if ( conns[i]->setVersion() ) {
                    conns[i]->done();
                    staleConfigExs.push_back( (string)"stale config detected for " + StaleConfigException( _ns , "ParallelCursor::_init" , true ).what() + errLoc );
                    break;
                }

                LOG(5) << "ParallelSortClusteredCursor::init server:" << sq._server << " ns:" << _ns
                       << " query:" << q << " _fields:" << _fields << " options: " << _options  << endl;

                if( ! _cursors[i].raw() )
                    _cursors[i].reset( new DBClientCursor( conns[i]->get() , _ns , q ,
                                                            0 , // nToReturn
                                                            0 , // nToSkip
                                                            _fields.isEmpty() ? 0 : &_fields , // fieldsToReturn
                                                            _options ,
                                                            _batchSize == 0 ? 0 : _batchSize + _needToSkip // batchSize
                                                            ) );

                try{
                    _cursors[i].raw()->initLazy( ! firstPass );
                }
                catch( SocketException& e ){
                    socketExs.push_back( e.what() + errLoc );
                    _cursors[i].reset( NULL );
                    conns[i]->done();
                    if( ! returnPartial ) break;
                }
                catch( std::exception& e){
                    otherExs.push_back( e.what() + errLoc );
                    _cursors[i].reset( NULL );
                    conns[i]->done();
                    break;
                }

            }

            // Go through all the potentially started cursors and finish initializing them or log any errors and
            // potentially retry
            // TODO:  Better error classification would make this easier, errors are indicated in all sorts of ways
            // here that we need to trap.
            for ( size_t i = 0; i < num; i++ ) {

                // log() << "Finishing query for " << cons[i].get()->getHost() << endl;
                string errLoc = " @ " + queries[i]._server;

                if( ! _cursors[i].raw() || ( ! firstPass && retryQueries.find( i ) == retryQueries.end() ) ){
                    if( conns[i] ) conns[i].get()->done();
                    continue;
                }

                assert( conns[i] );
                retryQueries.erase( i );

                bool retry = false;

                try {

                    if( ! _cursors[i].raw()->initLazyFinish( retry ) ) {

                        warning() << "invalid result from " << conns[i]->getHost() << ( retry ? ", retrying" : "" ) << endl;
                        _cursors[i].reset( NULL );

                        if( ! retry ){
                            socketExs.push_back( str::stream() << "error querying server: " << servers[i] );
                            conns[i]->done();
                        }
                        else {
                            retryQueries.insert( i );
                        }

                        continue;
                    }
                }
                catch ( MsgAssertionException& e ){
                    socketExs.push_back( e.what() + errLoc );
                    _cursors[i].reset( NULL );
                    conns[i]->done();
                    continue;
                }
                catch ( SocketException& e ) {
                    socketExs.push_back( e.what() + errLoc );
                    _cursors[i].reset( NULL );
                    conns[i]->done();
                    continue;
                }
                catch( std::exception& e ){
                    otherExs.push_back( e.what() + errLoc );
                    _cursors[i].reset( NULL );
                    conns[i]->done();
                    continue;
                }

                try {
                    _cursors[i].raw()->attach( conns[i].get() ); // this calls done on conn
                    _checkCursor( _cursors[i].raw() );

                    finishedQueries++;
                }
                catch ( StaleConfigException& e ){

                    // Our stored configuration data is actually stale, we need to reload it
                    // when we throw our exception
                    allConfigStale = true;

                    staleConfigExs.push_back( (string)"stale config detected for " + e.what() + errLoc );
                    _cursors[i].reset( NULL );
                    conns[i]->done();
                    continue;
                }
                catch( std::exception& e ){
                    otherExs.push_back( e.what() + errLoc );
                    _cursors[i].reset( NULL );
                    conns[i]->done();
                    continue;
                }
            }

            // Don't exceed our max retries, should not happen
            assert( retries < 5 );
        }
        while( retryQueries.size() > 0 /* something to retry */ &&
               ( socketExs.size() == 0 || returnPartial ) /* no conn issues */ &&
               staleConfigExs.size() == 0 /* no config issues */ &&
               otherExs.size() == 0 /* no other issues */);

        // Assert that our conns are all closed!
        for( vector< shared_ptr<ShardConnection> >::iterator i = conns.begin(); i < conns.end(); ++i ){
            assert( ! (*i) || ! (*i)->ok() );
        }

        // Handle errors we got during initialization.
        // If we're returning partial results, we can ignore socketExs, but nothing else
        // Log a warning in any case, so we don't lose these messages
        bool throwException = ( socketExs.size() > 0 && ! returnPartial ) || staleConfigExs.size() > 0 || otherExs.size() > 0;

        if( socketExs.size() > 0 || staleConfigExs.size() > 0 || otherExs.size() > 0 ) {

            vector<string> errMsgs;

            errMsgs.insert( errMsgs.end(), staleConfigExs.begin(), staleConfigExs.end() );
            errMsgs.insert( errMsgs.end(), otherExs.begin(), otherExs.end() );
            errMsgs.insert( errMsgs.end(), socketExs.begin(), socketExs.end() );

            stringstream errMsg;
            errMsg << "could not initialize cursor across all shards because : ";
            for( vector<string>::iterator i = errMsgs.begin(); i != errMsgs.end(); i++ ){
                if( i != errMsgs.begin() ) errMsg << " :: and :: ";
                errMsg << *i;
            }

            if( throwException && staleConfigExs.size() > 0 )
                throw StaleConfigException( _ns , errMsg.str() , ! allConfigStale );
            else if( throwException )
                throw DBException( errMsg.str(), 14827 );
            else
                warning() << errMsg.str() << endl;
        }

        if( retries > 0 )
            log() <<  "successfully finished parallel query after " << retries << " retries" << endl;

    }

    ParallelSortClusteredCursor::~ParallelSortClusteredCursor() {
        delete [] _cursors;
        _cursors = 0;
    }

    bool ParallelSortClusteredCursor::more() {

        if ( _needToSkip > 0 ) {
            int n = _needToSkip;
            _needToSkip = 0;

            while ( n > 0 && more() ) {
                BSONObj x = next();
                n--;
            }

            _needToSkip = n;
        }

        for ( int i=0; i<_numServers; i++ ) {
            if ( _cursors[i].more() )
                return true;
        }
        return false;
    }

    BSONObj ParallelSortClusteredCursor::next() {
        BSONObj best = BSONObj();
        int bestFrom = -1;

        for ( int i=0; i<_numServers; i++) {
            if ( ! _cursors[i].more() )
                continue;

            BSONObj me = _cursors[i].peek();

            if ( best.isEmpty() ) {
                best = me;
                bestFrom = i;
                if( _sortKey.isEmpty() ) break;
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

    void ParallelSortClusteredCursor::_explain( map< string,list<BSONObj> >& out ) {
        for ( set<ServerAndQuery>::iterator i=_servers.begin(); i!=_servers.end(); ++i ) {
            const ServerAndQuery& sq = *i;
            list<BSONObj> & l = out[sq._server];
            l.push_back( explain( sq._server , sq._extra ) );
        }

    }

    // -----------------
    // ---- Future -----
    // -----------------

    Future::CommandResult::CommandResult( const string& server , const string& db , const BSONObj& cmd , int options , DBClientBase * conn )
        :_server(server) ,_db(db) , _options(options), _cmd(cmd) ,_conn(conn) ,_done(false)
    {
        try {
            if ( ! _conn ){
                _connHolder.reset( new ScopedDbConnection( _server ) );
                _conn = _connHolder->get();
            }

            if ( _conn->lazySupported() ) {
                _cursor.reset( new DBClientCursor(_conn, _db + ".$cmd", _cmd, -1/*limit*/, 0, NULL, _options, 0));
                _cursor->initLazy();
            }
            else {
                _done = true; // we set _done first because even if there is an error we're done
                _ok = _conn->runCommand( db , cmd , _res , options );
            }
        }
        catch ( std::exception& e ) {
            error() << "Future::spawnComand (part 1) exception: " << e.what() << endl;
            _ok = false;
            _done = true;
        }
    }

    bool Future::CommandResult::join() {
        if (_done)
            return _ok;

        try {
            // TODO:  Allow retries?
            bool retry = false;
            bool finished = _cursor->initLazyFinish( retry );

            // Shouldn't need to communicate with server any more
            if ( _connHolder )
                _connHolder->done();

            uassert(14812,  str::stream() << "Error running command on server: " << _server, finished);
            massert(14813, "Command returned nothing", _cursor->more());

            _res = _cursor->nextSafe();
            _ok = _res["ok"].trueValue();

        }
        catch ( std::exception& e ) {
            error() << "Future::spawnComand (part 2) exception: " << e.what() << endl;
            _ok = false;
        }

        _done = true;
        return _ok;
    }

    shared_ptr<Future::CommandResult> Future::spawnCommand( const string& server , const string& db , const BSONObj& cmd , int options , DBClientBase * conn ) {
        shared_ptr<Future::CommandResult> res (new Future::CommandResult( server , db , cmd , options , conn  ));
        return res;
    }

}
