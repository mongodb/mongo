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
#include "../db/dbmessage.h"
#include "../s/util.h"
#include "../s/shard.h"
#include "../s/chunk.h"
#include "../s/config.h"
#include "../s/grid.h"
#include "mongo/client/dbclientcursor.h"

namespace mongo {

    LabeledLevel pc( "pcursor", 2 );

    // --------  ClusteredCursor -----------

    ClusteredCursor::ClusteredCursor( const QuerySpec& q ) {
        _ns = q.ns();
        _query = q.filter().copy();
        _hint = q.hint();
        _sort = q.sort();
        _options = q.options();
        _fields = q.fields().copy();
        _batchSize = q.ntoreturn();
        if ( _batchSize == 1 )
            _batchSize = 2;
        
        _done = false;
        _didInit = false;
    }

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
        verify( cursor );
        
        if ( cursor->hasResultFlag( ResultFlag_ShardConfigStale ) ) {
            BSONObj error;
            cursor->peekError( &error );
            throw RecvStaleConfigException( "ClusteredCursor::_checkCursor", error );
        }
        
        if ( cursor->hasResultFlag( ResultFlag_ErrSet ) ) {
            BSONObj o = cursor->next();
            throw UserException( o["code"].numberInt() , o["$err"].String() );
        }
    }

    auto_ptr<DBClientCursor> ClusteredCursor::query( const string& server , int num , BSONObj extra , int skipLeft , bool lazy ) {
        uassert( 10017 ,  "cursor already done" , ! _done );
        verify( _didInit );

        BSONObj q = _query;
        if ( ! extra.isEmpty() ) {
            q = concatQuery( q , extra );
        }

        try {
            ShardConnection conn( server , _ns );
            
            if ( conn.setVersion() ) {
                conn.done();
                // Deprecated, so we don't care about versions here
                throw RecvStaleConfigException( _ns , "ClusteredCursor::query" , true, ShardChunkVersion( 0 ), ShardChunkVersion( 0 ) );
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
            verify( ! conn.ok() );
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
        
        Query qu( q );
        qu.explain();
        if ( ! _hint.isEmpty() )
            qu.hint( _hint );
        if ( ! _sort.isEmpty() )
            qu.sort( _sort );

        BSONObj o;

        ShardConnection conn( server , _ns );
        auto_ptr<DBClientCursor> cursor = conn->query( _ns , qu , abs( _batchSize ) * -1 , 0 , _fields.isEmpty() ? 0 : &_fields );
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

    void ParallelSortClusteredCursor::explain(BSONObjBuilder& b) {
        // Note: by default we filter out allPlans and oldPlan in the shell's
        // explain() function. If you add any recursive structures, make sure to
        // edit the JS to make sure everything gets filtered.

        // Return single shard output if we're versioned but not sharded, or
        // if we specified only a single shard
        // TODO:  We should really make this simpler - all queries via mongos
        // *always* get the same explain format
        if( ( isVersioned() && ! isSharded() ) || _qShards.size() == 1 ){
            map<string,list<BSONObj> > out;
            _explain( out );
            verify( out.size() == 1 );
            list<BSONObj>& l = out.begin()->second;
            verify( l.size() == 1 );
            b.appendElements( *(l.begin()) );
            return;
        }

        b.append( "clusteredType" , type() );

        string cursorType;
        BSONObj indexBounds;
        BSONObj oldPlan;
        
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

                    if ( temp["cursor"].type() == String ) { 
                        if ( cursorType.size() == 0 )
                            cursorType = temp["cursor"].String();
                        else if ( cursorType != temp["cursor"].String() )
                            cursorType = "multiple";
                    }

                    if ( temp["indexBounds"].type() == Object )
                        indexBounds = temp["indexBounds"].Obj();

                    if ( temp["oldPlan"].type() == Object )
                        oldPlan = temp["oldPlan"].Obj();

                }
                y.done();
            }
            x.done();
        }

        b.append( "cursor" , cursorType );
        for ( map<string,long long>::iterator i=counters.begin(); i!=counters.end(); ++i )
            b.appendNumber( i->first , i->second );

        b.appendNumber( "millisShardTotal" , millis );
        b.append( "millisShardAvg" , 
                  numExplains ? static_cast<int>( static_cast<double>(millis) / numExplains )
                  : 0 );
        b.append( "numQueries" , (int)numExplains );
        b.append( "numShards" , (int)out.size() );

        if ( out.size() == 1 ) {
            b.append( "indexBounds" , indexBounds );
            if ( ! oldPlan.isEmpty() ) {
                // this is to stay in compliance with mongod behavior
                // we should make this cleaner, i.e. {} == nothing
                b.append( "oldPlan" , oldPlan );
            }
        }
        else {
            // TODO: this is lame...
        }

    }

    // --------  FilteringClientCursor -----------
    FilteringClientCursor::FilteringClientCursor( const BSONObj filter )
        : _matcher( filter ) , _pcmData( NULL ), _done( true ) {
    }

    FilteringClientCursor::FilteringClientCursor( auto_ptr<DBClientCursor> cursor , const BSONObj filter )
        : _matcher( filter ) , _cursor( cursor ) , _pcmData( NULL ), _done( cursor.get() == 0 ) {
    }

    FilteringClientCursor::FilteringClientCursor( DBClientCursor* cursor , const BSONObj filter )
        : _matcher( filter ) , _cursor( cursor ) , _pcmData( NULL ), _done( cursor == 0 ) {
    }


    FilteringClientCursor::~FilteringClientCursor() {
        // Don't use _pcmData
        _pcmData = NULL;
    }

    void FilteringClientCursor::reset( auto_ptr<DBClientCursor> cursor ) {
        _cursor = cursor;
        _next = BSONObj();
        _done = _cursor.get() == 0;
        _pcmData = NULL;
    }

    void FilteringClientCursor::reset( DBClientCursor* cursor, ParallelConnectionMetadata* pcmData ) {
        _cursor.reset( cursor );
        _pcmData = pcmData;
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
        verify( ! _next.isEmpty() );
        verify( ! _done );

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
        verify( _next.isEmpty() );
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

    ParallelSortClusteredCursor::ParallelSortClusteredCursor( const QuerySpec& qSpec, const CommandInfo& cInfo )
        : ClusteredCursor( qSpec ),
          _qSpec( qSpec ), _cInfo( cInfo ), _totalTries( 0 )
    {
        _finishCons();
    }

    ParallelSortClusteredCursor::ParallelSortClusteredCursor( const set<Shard>& qShards, const QuerySpec& qSpec )
        : ClusteredCursor( qSpec ),
          _qSpec( qSpec ), _totalTries( 0 )
    {
        for( set<Shard>::const_iterator i = qShards.begin(), end = qShards.end(); i != end; ++i )
            _qShards.insert( *i );

        _finishCons();
    }

    void ParallelSortClusteredCursor::_finishCons() {
        _numServers = _servers.size();
        _cursors = 0;

        if( ! _qSpec.isEmpty() ){

            _needToSkip = _qSpec.ntoskip();
            _cursors = 0;
            _sortKey = _qSpec.sort();
            _fields = _qSpec.fields();

            if( ! isVersioned() ) verify( _cInfo.isEmpty() );
        }

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

        if( ! _qSpec.isEmpty() ){
            _qSpec.setFields( _fields );
        }
    }

    void ParallelConnectionMetadata::cleanup( bool full ){

        if( full || errored ) retryNext = false;

        if( ! retryNext && pcState ){

            if( errored && pcState->conn ){
                // Don't return this conn to the pool if it's bad
                pcState->conn->kill();
                pcState->conn.reset();
            }
            else if( initialized ){

                verify( pcState->cursor );
                verify( pcState->conn );

                if( ! finished && pcState->conn->ok() ){
                    try{
                        // Complete the call if only halfway done
                        bool retry = false;
                        pcState->cursor->initLazyFinish( retry );
                    }
                    catch( std::exception& ){
                        warning() << "exception closing cursor" << endl;
                    }
                    catch( ... ){
                        warning() << "unknown exception closing cursor" << endl;
                    }
                }
            }

            // Double-check conn is closed
            if( pcState->conn ){
                pcState->conn->done();
            }

            pcState.reset();
        }
        else verify( finished || ! initialized );

        initialized = false;
        finished = false;
        completed = false;
        errored = false;
    }



    BSONObj ParallelConnectionState::toBSON() const {

        BSONObj cursorPeek = BSON( "no cursor" << "" );
        if( cursor ){
            vector<BSONObj> v;
            cursor->peek( v, 1 );
            if( v.size() == 0 ) cursorPeek = BSON( "no data" << "" );
            else cursorPeek = BSON( "" << v[0] );
        }

        BSONObj stateObj =
                BSON( "conn" << ( conn ? ( conn->ok() ? conn->conn().toString() : "(done)" ) : "" ) <<
                      "vinfo" << ( manager ? ( str::stream() << manager->getns() << " @ " << manager->getVersion().toString() ) :
                                               primary->toString() ) );

        // Append cursor data if exists
        BSONObjBuilder stateB;
        stateB.appendElements( stateObj );
        if( ! cursor ) stateB.append( "cursor", "(none)" );
        else {
            vector<BSONObj> v;
            cursor->peek( v, 1 );
            if( v.size() == 0 ) stateB.append( "cursor", "(empty)" );
            else stateB.append( "cursor", v[0] );
        }

        stateB.append( "count", count );
        stateB.append( "done", done );

        return stateB.obj().getOwned();
    }

    BSONObj ParallelConnectionMetadata::toBSON() const {
        return BSON( "state" << ( pcState ? pcState->toBSON() : BSONObj() ) <<
                     "retryNext" << retryNext <<
                     "init" << initialized <<
                     "finish" << finished <<
                     "errored" << errored );
    }

    BSONObj ParallelSortClusteredCursor::toBSON() const {

        BSONObjBuilder b;

        b.append( "tries", _totalTries );

        {
            BSONObjBuilder bb;
            for( map< Shard, PCMData >::const_iterator i = _cursorMap.begin(), end = _cursorMap.end(); i != end; ++i ){
                bb.append( i->first.toString(), i->second.toBSON() );
            }
            b.append( "cursors", bb.obj().getOwned() );
        }

        {
            BSONObjBuilder bb;
            for( map< string, int >::const_iterator i = _staleNSMap.begin(), end = _staleNSMap.end(); i != end; ++i ){
                bb.append( i->first, i->second );
            }
            b.append( "staleTries", bb.obj().getOwned() );
        }

        return b.obj().getOwned();
    }

    string ParallelSortClusteredCursor::toString() const {
        return str::stream() << "PCursor : " << toBSON();
    }

    void ParallelSortClusteredCursor::fullInit(){
        startInit();
        finishInit();
    }

    void ParallelSortClusteredCursor::_markStaleNS( const NamespaceString& staleNS, const StaleConfigException& e, bool& forceReload, bool& fullReload ){
        if( _staleNSMap.find( staleNS ) == _staleNSMap.end() ){
            forceReload = false;
            fullReload = false;
            _staleNSMap[ staleNS ] = 1;
        }
        else{
            int tries = ++_staleNSMap[ staleNS ];

            if( tries >= 5 ) throw SendStaleConfigException( staleNS, str::stream() << "too many retries of stale version info",
                                                             e.getVersionReceived(), e.getVersionWanted() );

            forceReload = tries > 1;
            fullReload = tries > 2;
        }
    }

    void ParallelSortClusteredCursor::_handleStaleNS( const NamespaceString& staleNS, bool forceReload, bool fullReload ){

        DBConfigPtr config = grid.getDBConfig( staleNS.db );

        // Reload db if needed, make sure it works
        if( config && fullReload && ! config->reload() ){
            // We didn't find the db after the reload, the db may have been dropped,
            // reset this ptr
            config.reset();
        }

        if( ! config ){
            warning() << "cannot reload database info for stale namespace " << staleNS << endl;
        }
        else {
            // Reload chunk manager, potentially forcing the namespace
            config->getChunkManagerIfExists( staleNS, true, forceReload );
        }

    }

    void ParallelSortClusteredCursor::startInit() {

        bool returnPartial = ( _qSpec.options() & QueryOption_PartialResults );
        bool specialVersion = _cInfo.versionedNS.size() > 0;
        bool specialFilter = ! _cInfo.cmdFilter.isEmpty();
        NamespaceString ns = specialVersion ? _cInfo.versionedNS : _qSpec.ns();

        ChunkManagerPtr manager;
        ShardPtr primary;

        string prefix;
        if( _totalTries > 0 ) prefix = str::stream() << "retrying (" << _totalTries << " tries)";
        else prefix = "creating";
        log( pc ) << prefix << " pcursor over " << _qSpec << " and " << _cInfo << endl;

        set<Shard> todoStorage;
        set<Shard>& todo = todoStorage;
        string vinfo;

        if( isVersioned() ){

            DBConfigPtr config = grid.getDBConfig( ns.db ); // Gets or loads the config
            uassert( 15989, "database not found for parallel cursor request", config );

            // Try to get either the chunk manager or the primary shard
            config->getChunkManagerOrPrimary( ns, manager, primary );

            if( manager ) vinfo = ( str::stream() << "[" << manager->getns() << " @ " << manager->getVersion().toString() << "]" );
            else vinfo = (str::stream() << "[unsharded @ " << primary->toString() << "]" );

            if( manager ) manager->getShardsForQuery( todo, specialFilter ? _cInfo.cmdFilter : _qSpec.filter() );
            else if( primary ) todo.insert( *primary );

            // Close all cursors on extra shards first, as these will be invalid
            for( map< Shard, PCMData >::iterator i = _cursorMap.begin(), end = _cursorMap.end(); i != end; ++i ){

                log( pc ) << "closing cursor on shard " << i->first << " as the connection is no longer required by " << vinfo << endl;

                // Force total cleanup of these connections
                if( todo.find( i->first ) == todo.end() ) i->second.cleanup();
            }
        }
        else{

            // Don't use version to get shards here
            todo = _qShards;
            vinfo = str::stream() << "[" << _qShards.size() << " shards specified]";

        }

        verify( todo.size() );

        log( pc ) << "initializing over " << todo.size() << " shards required by " << vinfo << endl;

        // Don't retry indefinitely for whatever reason
        _totalTries++;
        uassert( 15986, "too many retries in total", _totalTries < 10 );

        for( set<Shard>::iterator i = todo.begin(), end = todo.end(); i != end; ++i ){

            const Shard& shard = *i;
            PCMData& mdata = _cursorMap[ shard ];

            log( pc ) << "initializing on shard " << shard << ", current connection state is " << mdata.toBSON() << endl;

            // This may be the first time connecting to this shard, if so we can get an error here
            try {

                if( mdata.initialized ){

                    verify( mdata.pcState );

                    PCStatePtr state = mdata.pcState;

                    bool compatiblePrimary = true;
                    bool compatibleManager = true;

                    // Only check for compatibility if we aren't forcing the shard choices
                    if( isVersioned() ){

                        if( primary && ! state->primary )
                            warning() << "Collection becoming unsharded detected" << endl;
                        if( manager && ! state->manager )
                            warning() << "Collection becoming sharded detected" << endl;
                        if( primary && state->primary && primary != state->primary )
                            warning() << "Weird shift of primary detected" << endl;

                        compatiblePrimary = primary && state->primary && primary == state->primary;
                        compatibleManager = manager && state->manager && manager->compatibleWith( state->manager, shard );

                    }

                    if( compatiblePrimary || compatibleManager ){
                        // If we're compatible, don't need to retry unless forced
                        if( ! mdata.retryNext ) continue;
                        // Do partial cleanup
                        mdata.cleanup( false );
                    }
                    else {
                        // Force total cleanup of connection if no longer compatible
                        mdata.cleanup();
                    }
                }
                else {
                    // Cleanup connection if we're not yet initialized
                    mdata.cleanup( false );
                }

                mdata.pcState.reset( new PCState() );
                PCStatePtr state = mdata.pcState;

                // Setup manager / primary
                if( manager ) state->manager = manager;
                else if( primary ) state->primary = primary;

                verify( ! primary || shard == *primary || ! isVersioned() );

                // Setup conn
                if( ! state->conn ) state->conn.reset( new ShardConnection( shard, ns, manager ) );

                if( state->conn->setVersion() ){
                    // It's actually okay if we set the version here, since either the manager will be verified as
                    // compatible, or if the manager doesn't exist, we don't care about version consistency
                    log( pc ) << "needed to set remote version on connection to value compatible with " << vinfo << endl;
                }

                // Setup cursor
                if( ! state->cursor ){

                    // Do a sharded query if this is not a primary shard *and* this is a versioned query,
                    // or if the number of shards to query is > 1
                    if( ( isVersioned() && ! primary ) || _qShards.size() > 1 ){

                        state->cursor.reset( new DBClientCursor( state->conn->get(), _qSpec.ns(), _qSpec.query(),
                                                                 isCommand() ? 1 : 0, // nToReturn (0 if query indicates multi)
                                                                 0, // nToSkip
                                                                 // Does this need to be a ptr?
                                                                 _qSpec.fields().isEmpty() ? 0 : _qSpec.fieldsData(), // fieldsToReturn
                                                                 _qSpec.options(), // options
                                                                 // NtoReturn is weird.
                                                                 // If zero, it means use default size, so we do that for all cursors
                                                                 // If positive, it's the batch size (we don't want this cursor limiting results), that's
                                                                 // done at a higher level
                                                                 // If negative, it's the batch size, but we don't create a cursor - so we don't want
                                                                 // to create a child cursor either.
                                                                 // Either way, if non-zero, we want to pull back the batch size + the skip amount as
                                                                 // quickly as possible.  Potentially, for a cursor on a single shard or if we keep better track of
                                                                 // chunks, we can actually add the skip value into the cursor and/or make some assumptions about the
                                                                 // return value size ( (batch size + skip amount) / num_servers ).
                                                                 _qSpec.ntoreturn() == 0 ? 0 :
                                                                     ( _qSpec.ntoreturn() > 0 ? _qSpec.ntoreturn() + _qSpec.ntoskip() :
                                                                                                _qSpec.ntoreturn() - _qSpec.ntoskip() ) ) ); // batchSize
                    }
                    else{

                        // Non-sharded
                        state->cursor.reset( new DBClientCursor( state->conn->get(), _qSpec.ns(), _qSpec.query(),
                                                                 _qSpec.ntoreturn(), // nToReturn
                                                                 _qSpec.ntoskip(), // nToSkip
                                                                 // Does this need to be a ptr?
                                                                 _qSpec.fields().isEmpty() ? 0 : _qSpec.fieldsData(), // fieldsToReturn
                                                                 _qSpec.options(), // options
                                                                 0 ) ); // batchSize
                    }
                }

                bool lazyInit = state->conn->get()->lazySupported();
                if( lazyInit ){

                    // Need to keep track if this is a second or third try for replica sets
                    state->cursor->initLazy( mdata.retryNext );
                    mdata.retryNext = false;
                    mdata.initialized = true;
                }
                else{

                    // Without full initialization, throw an exception
                    uassert( 15987, str::stream() << "could not fully initialize cursor on shard " << shard.toString() << ", current connection state is " << mdata.toBSON().toString(), state->cursor->init() );
                    mdata.retryNext = false;
                    mdata.initialized = true;
                    mdata.finished = true;
                }


                log( pc ) << "initialized " << ( isCommand() ? "command " : "query " ) << ( lazyInit ? "(lazily) " : "(full) " ) << "on shard " << shard << ", current connection state is " << mdata.toBSON() << endl;

            }
            catch( SendStaleConfigException& e ){

                // Our version isn't compatible with the current version anymore on at least one shard, need to retry immediately
                NamespaceString staleNS = e.getns();

                // Probably need to retry fully
                bool forceReload, fullReload;
                _markStaleNS( staleNS, e, forceReload, fullReload );

                int logLevel = fullReload ? 0 : 1;
                log( pc + logLevel ) << "stale config of ns " << staleNS << " during initialization, will retry with forced : " << forceReload << ", full : " << fullReload << causedBy( e ) << endl;

                // This is somewhat strange
                if( staleNS != ns )
                    warning() << "versioned ns " << ns << " doesn't match stale config namespace " << staleNS << endl;

                _handleStaleNS( staleNS, forceReload, fullReload );

                // Restart with new chunk manager
                startInit();
                return;
            }
            catch( SocketException& e ){
                warning() << "socket exception when initializing on " << shard << ", current connection state is " << mdata.toBSON() << causedBy( e ) << endl;
                mdata.errored = true;
                if( returnPartial ){
                    mdata.cleanup();
                    continue;
                }
                throw;
            }
            catch( DBException& e ){
                warning() << "db exception when initializing on " << shard << ", current connection state is " << mdata.toBSON() << causedBy( e ) << endl;
                mdata.errored = true;
                if( returnPartial && e.getCode() == 15925 /* From above! */ ){
                    mdata.cleanup();
                    continue;
                }
                throw;
            }
            catch( std::exception& e){
                warning() << "exception when initializing on " << shard << ", current connection state is " << mdata.toBSON() << causedBy( e ) << endl;
                mdata.errored = true;
                throw;
            }
            catch( ... ){
                warning() << "unknown exception when initializing on " << shard << ", current connection state is " << mdata.toBSON() << endl;
                mdata.errored = true;
                throw;
            }
        }

        // Sanity check final init'ed connections
        for( map< Shard, PCMData >::iterator i = _cursorMap.begin(), end = _cursorMap.end(); i != end; ++i ){

            const Shard& shard = i->first;
            PCMData& mdata = i->second;

            if( ! mdata.pcState ) continue;

            // Make sure all state is in shards
            verify( todo.find( shard ) != todo.end() );
            verify( mdata.initialized = true );
            if( ! mdata.completed ) verify( mdata.pcState->conn->ok() );
            verify( mdata.pcState->cursor );
            if( isVersioned() ) verify( mdata.pcState->primary || mdata.pcState->manager );
            else verify( ! mdata.pcState->primary || ! mdata.pcState->manager );
            verify( ! mdata.retryNext );

            if( mdata.completed ) verify( mdata.finished );
            if( mdata.finished ) verify( mdata.initialized );
            if( ! returnPartial ) verify( mdata.initialized );
        }

    }

    void ParallelSortClusteredCursor::finishInit(){

        bool returnPartial = ( _qSpec.options() & QueryOption_PartialResults );
        bool specialVersion = _cInfo.versionedNS.size() > 0;
        string ns = specialVersion ? _cInfo.versionedNS : _qSpec.ns();

        bool retry = false;
        map< string, StaleConfigException > staleNSExceptions;

        log( pc ) << "finishing over " << _cursorMap.size() << " shards" << endl;

        for( map< Shard, PCMData >::iterator i = _cursorMap.begin(), end = _cursorMap.end(); i != end; ++i ){

            const Shard& shard = i->first;
            PCMData& mdata = i->second;

            log( pc ) << "finishing on shard " << shard << ", current connection state is " << mdata.toBSON() << endl;

            // Ignore empty conns for now
            if( ! mdata.pcState ) continue;

            PCStatePtr state = mdata.pcState;

            try {

                // Sanity checks
                if( ! mdata.completed ) verify( state->conn && state->conn->ok() );
                verify( state->cursor );
                if( isVersioned() ){
                    verify( state->manager || state->primary );
                    verify( ! state->manager || ! state->primary );
                }
                else verify( ! state->manager && ! state->primary );


                // If we weren't init'ing lazily, ignore this
                if( ! mdata.finished ){

                    mdata.finished = true;

                    // Mark the cursor as non-retry by default
                    mdata.retryNext = false;

                    if( ! state->cursor->initLazyFinish( mdata.retryNext ) ){
                        if( ! mdata.retryNext ){
                            uassert( 15988, "error querying server", false );
                        }
                        else{
                            retry = true;
                            continue;
                        }
                    }

                    mdata.completed = false;
                }

                if( ! mdata.completed ){

                    mdata.completed = true;

                    // Make sure we didn't get an error we should rethrow
                    // TODO : Rename/refactor this to something better
                    _checkCursor( state->cursor.get() );

                    // Finalize state
                    state->cursor->attach( state->conn.get() ); // Closes connection for us

                    log( pc ) << "finished on shard " << shard << ", current connection state is " << mdata.toBSON() << endl;
                }
            }
            catch( RecvStaleConfigException& e ){
                retry = true;

                // Will retry all at once
                staleNSExceptions[ e.getns() ] = e;

                // Fully clear this cursor, as it needs to be re-established
                mdata.cleanup();
                continue;
            }
            catch( SocketException& e ){
                warning() << "socket exception when finishing on " << shard << ", current connection state is " << mdata.toBSON() << causedBy( e ) << endl;
                mdata.errored = true;
                if( returnPartial ){
                    mdata.cleanup();
                    continue;
                }
                throw;
            }
            catch( DBException& e ){
                warning() << "db exception when finishing on " << shard << ", current connection state is " << mdata.toBSON() << causedBy( e ) << endl;
                mdata.errored = true;
                throw;
            }
            catch( std::exception& e){
                warning() << "exception when finishing on " << shard << ", current connection state is " << mdata.toBSON() << causedBy( e ) << endl;
                mdata.errored = true;
                throw;
            }
            catch( ... ){
                warning() << "unknown exception when finishing on " << shard << ", current connection state is " << mdata.toBSON() << endl;
                mdata.errored = true;
                throw;
            }

        }

        // Retry logic for single refresh of namespaces / retry init'ing connections
        if( retry ){

            // Refresh stale namespaces
            if( staleNSExceptions.size() ){
                for( map<string,StaleConfigException>::iterator i = staleNSExceptions.begin(), end = staleNSExceptions.end(); i != end; ++i ){

                    const string& staleNS = i->first;
                    const StaleConfigException& exception = i->second;

                    bool forceReload, fullReload;
                    _markStaleNS( staleNS, exception, forceReload, fullReload );

                    int logLevel = fullReload ? 0 : 1;
                    log( pc + logLevel ) << "stale config of ns " << staleNS << " on finishing query, will retry with forced : " << forceReload << ", full : " << fullReload << causedBy( exception ) << endl;

                    // This is somewhat strange
                    if( staleNS != ns )
                        warning() << "versioned ns " << ns << " doesn't match stale config namespace " << staleNS << endl;

                    _handleStaleNS( staleNS, forceReload, fullReload );
                }
            }

            // Re-establish connections we need to
            startInit();
            finishInit();
            return;
        }

        // Sanity check and clean final connections
        map< Shard, PCMData >::iterator i = _cursorMap.begin();
        while( i != _cursorMap.end() ){

            // const Shard& shard = i->first;
            PCMData& mdata = i->second;

            // Erase empty stuff
            if( ! mdata.pcState ){
                log() << "PCursor erasing empty state " << mdata.toBSON() << endl;
                _cursorMap.erase( i++ );
                continue;
            }
            else ++i;

            // Make sure all state is in shards
            verify( mdata.initialized = true );
            verify( mdata.finished = true );
            verify( mdata.completed = true );
            verify( ! mdata.pcState->conn->ok() );
            verify( mdata.pcState->cursor );
            if( isVersioned() ) verify( mdata.pcState->primary || mdata.pcState->manager );
            else verify( ! mdata.pcState->primary && ! mdata.pcState->manager );
        }

        // TODO : More cleanup of metadata?

        // LEGACY STUFF NOW

        _cursors = new FilteringClientCursor[ _cursorMap.size() ];

        // Put the cursors in the legacy format
        int index = 0;
        for( map< Shard, PCMData >::iterator i = _cursorMap.begin(), end = _cursorMap.end(); i != end; ++i ){

            PCMData& mdata = i->second;

            _cursors[ index ].reset( mdata.pcState->cursor.get(), &mdata );
            _servers.insert( ServerAndQuery( i->first.getConnString(), BSONObj() ) );

            index++;
        }

        _numServers = _cursorMap.size();

    }

    bool ParallelSortClusteredCursor::isSharded() {
        // LEGACY is always unsharded
        if( _qSpec.isEmpty() ) return false;

        if( ! isVersioned() ) return false;

        if( _cursorMap.size() > 1 ) return true;
        if( _cursorMap.begin()->second.pcState->manager ) return true;
        return false;
    }

    ShardPtr ParallelSortClusteredCursor::getPrimary() {
        if( isSharded() || ! isVersioned() ) return ShardPtr();
        return _cursorMap.begin()->second.pcState->primary;
    }

    void ParallelSortClusteredCursor::getQueryShards( set<Shard>& shards ) {
        for( map< Shard, PCMData >::iterator i = _cursorMap.begin(), end = _cursorMap.end(); i != end; ++i ){
            shards.insert( i->first );
        }
    }

    ChunkManagerPtr ParallelSortClusteredCursor::getChunkManager( const Shard& shard ) {
        if( ! isSharded() ) return ChunkManagerPtr();

        map<Shard,PCMData>::iterator i = _cursorMap.find( shard );

        if( i == _cursorMap.end() ) return ChunkManagerPtr();
        else return i->second.pcState->manager;
    }

    DBClientCursorPtr ParallelSortClusteredCursor::getShardCursor( const Shard& shard ) {
        map<Shard,PCMData>::iterator i = _cursorMap.find( shard );

        if( i == _cursorMap.end() ) return DBClientCursorPtr();
        else return i->second.pcState->cursor;
    }

    void ParallelSortClusteredCursor::_init() {
        if( ! _qSpec.isEmpty() ) fullInit();
        else _oldInit();
    }


    // DEPRECATED


    // TODO:  Merge with futures API?  We do a lot of error checking here that would be useful elsewhere.
    void ParallelSortClusteredCursor::_oldInit() {

        // log() << "Starting parallel search..." << endl;

        // make sure we're not already initialized
        verify( ! _cursors );
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
                    // Version is zero b/c this is deprecated codepath
                    staleConfigExs.push_back( (string)"stale config detected for " + RecvStaleConfigException( _ns , "ParallelCursor::_init" , ShardChunkVersion( 0 ), ShardChunkVersion( 0 ), true ).what() + errLoc );
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

                verify( conns[i] );
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
                catch ( StaleConfigException& e ){
                    // Our stored configuration data is actually stale, we need to reload it
                    // when we throw our exception
                    allConfigStale = true;

                    staleConfigExs.push_back( (string)"stale config detected when receiving response for " + e.what() + errLoc );
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
            verify( retries < 5 );
        }
        while( retryQueries.size() > 0 /* something to retry */ &&
               ( socketExs.size() == 0 || returnPartial ) /* no conn issues */ &&
               staleConfigExs.size() == 0 /* no config issues */ &&
               otherExs.size() == 0 /* no other issues */);

        // Assert that our conns are all closed!
        for( vector< shared_ptr<ShardConnection> >::iterator i = conns.begin(); i < conns.end(); ++i ){
            verify( ! (*i) || ! (*i)->ok() );
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

            if( throwException && staleConfigExs.size() > 0 ){
                // Version is zero b/c this is deprecated codepath
                throw RecvStaleConfigException( _ns , errMsg.str() , ! allConfigStale, ShardChunkVersion( 0 ), ShardChunkVersion( 0 ) );
            }
            else if( throwException )
                throw DBException( errMsg.str(), 14827 );
            else
                warning() << errMsg.str() << endl;
        }

        if( retries > 0 )
            log() <<  "successfully finished parallel query after " << retries << " retries" << endl;

    }

    ParallelSortClusteredCursor::~ParallelSortClusteredCursor() {
        // Clear out our metadata before removing legacy cursor data
        _cursorMap.clear();
        for( int i = 0; i < _numServers; i++ ) _cursors[i].release();

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
            if ( ! _cursors[i].more() ){
                if( _cursors[i].rawMData() )
                    _cursors[i].rawMData()->pcState->done = true;
                continue;
            }

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

        if( _cursors[bestFrom].rawMData() )
            _cursors[bestFrom].rawMData()->pcState->count++;

        return best;
    }

    void ParallelSortClusteredCursor::_explain( map< string,list<BSONObj> >& out ) {

        set<Shard> shards;
        getQueryShards( shards );

        for( set<Shard>::iterator i = shards.begin(), end = shards.end(); i != end; ++i ){
            // TODO: Make this the shard name, not address
            list<BSONObj>& l = out[ i->getAddress().toString() ];
            l.push_back( getShardCursor( *i )->peekFirst().getOwned() );
        }

    }

    // -----------------
    // ---- Future -----
    // -----------------

    Future::CommandResult::CommandResult( const string& server , const string& db , const BSONObj& cmd , int options , DBClientBase * conn )
        :_server(server) ,_db(db) , _options(options), _cmd(cmd) ,_conn(conn) ,_done(false)
    {
        init();
    }

    void Future::CommandResult::init(){
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
                _ok = _conn->runCommand( _db , _cmd , _res , _options );
            }
        }
        catch ( std::exception& e ) {
            error() << "Future::spawnComand (part 1) exception: " << e.what() << endl;
            _ok = false;
            _done = true;
        }
    }

    bool Future::CommandResult::join( int maxRetries ) {
        if (_done)
            return _ok;


        _ok = false;
        for( int i = 1; i <= maxRetries; i++ ){

            try {
                bool retry = false;
                bool finished = _cursor->initLazyFinish( retry );

                // Shouldn't need to communicate with server any more
                if ( _connHolder )
                    _connHolder->done();

                uassert(14812,  str::stream() << "Error running command on server: " << _server, finished);
                massert(14813, "Command returned nothing", _cursor->more());

                _res = _cursor->nextSafe();
                _ok = _res["ok"].trueValue();

                break;
            }
            catch ( RecvStaleConfigException& e ){

                verify( versionManager.isVersionableCB( _conn ) );

                if( i >= maxRetries ){
                    error() << "Future::spawnComand (part 2) stale config exception" << causedBy( e ) << endl;
                    throw e;
                }

                if( i >= maxRetries / 2 ){
                    if( ! versionManager.forceRemoteCheckShardVersionCB( e.getns() ) ){
                        error() << "Future::spawnComand (part 2) no config detected" << causedBy( e ) << endl;
                        throw e;
                    }
                }

                versionManager.checkShardVersionCB( _conn, e.getns(), false, 1 );

                LOG( i > 1 ? 0 : 1 ) << "retrying lazy command" << causedBy( e ) << endl;

                verify( _conn->lazySupported() );
                _done = false;
                init();
                continue;
            }
            catch ( std::exception& e ) {
                error() << "Future::spawnComand (part 2) exception: " << causedBy( e ) << endl;
                break;
            }

        }

        _done = true;
        return _ok;
    }

    shared_ptr<Future::CommandResult> Future::spawnCommand( const string& server , const string& db , const BSONObj& cmd , int options , DBClientBase * conn ) {
        shared_ptr<Future::CommandResult> res (new Future::CommandResult( server , db , cmd , options , conn  ));
        return res;
    }

}
