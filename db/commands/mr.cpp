// mr.cpp

/**
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
#include "../db.h"
#include "../instance.h"
#include "../commands.h"
#include "../../scripting/engine.h"
#include "../../client/dbclient.h"
#include "../../client/connpool.h"
#include "../../client/parallel.h"
#include "../queryoptimizer.h"
#include "../matcher.h"
#include "../clientcursor.h"

#include "mr.h"

namespace mongo {

    namespace mr {

        AtomicUInt Config::JOB_NUMBER;

        JSFunction::JSFunction( string type , const BSONElement& e ) {
            _type = type;
            _code = e._asCode();

            if ( e.type() == CodeWScope )
                _wantedScope = e.codeWScopeObject();
        }

        void JSFunction::init( State * state ) {
            _scope = state->scope();
            assert( _scope );
            _scope->init( &_wantedScope );

            _func = _scope->createFunction( _code.c_str() );
            uassert( 13598 , str::stream() << "couldn't compile code for: " << _type , _func );
        }

        void JSMapper::init( State * state ) {
            _func.init( state );
            _params = state->config().mapParams;
        }

        void JSMapper::map( const BSONObj& o ) {
            Scope * s = _func.scope();
            assert( s );
            s->setThis( &o );
            if ( s->invoke( _func.func() , _params , 0 , true ) )
                throw UserException( 9014, str::stream() << "map invoke failed: " + s->getError() );
        }

        BSONObj JSFinalizer::finalize( const BSONObj& o ) {
            Scope * s = _func.scope();

            Scope::NoDBAccess no = s->disableDBAccess( "can't access db inside finalize" );
            s->invokeSafe( _func.func() , o );

            // don't want to use o.objsize() to size b
            // since there are many cases where the point of finalize
            // is converting many fields to 1
            BSONObjBuilder b;
            b.append( o.firstElement() );
            s->append( b , "value" , "return" );
            return b.obj();
        }

        BSONObj JSReducer::reduce( const BSONList& tuples ) {
            BSONObj key;
            int endSizeEstimate = 16;

            _reduce( tuples , key , endSizeEstimate );

            BSONObjBuilder b(endSizeEstimate);
            b.appendAs( key.firstElement() , "0" );
            _func.scope()->append( b , "1" , "return" );
            return b.obj();
        }

        BSONObj JSReducer::reduce( const BSONList& tuples , Finalizer * finalizer ) {

            BSONObj key;
            int endSizeEstimate = 16;

            _reduce( tuples , key , endSizeEstimate );

            BSONObjBuilder b(endSizeEstimate);
            b.appendAs( key.firstElement() , "_id" );
            _func.scope()->append( b , "value" , "return" );
            BSONObj res = b.obj();

            if ( finalizer ) {
                res = finalizer->finalize( res );
            }

            return res;
        }

        void JSReducer::_reduce( const BSONList& tuples , BSONObj& key , int& endSizeEstimate ) {
            uassert( 10074 ,  "need values" , tuples.size() );

            int sizeEstimate = ( tuples.size() * tuples.begin()->getField( "value" ).size() ) + 128;

            BSONObjBuilder reduceArgs( sizeEstimate );
            boost::scoped_ptr<BSONArrayBuilder>  valueBuilder;

            int sizeSoFar = 0;
            unsigned n = 0;
            for ( ; n<tuples.size(); n++ ) {
                BSONObjIterator j(tuples[n]);
                BSONElement keyE = j.next();
                if ( n == 0 ) {
                    reduceArgs.append( keyE );
                    key = keyE.wrap();
                    sizeSoFar = 5 + keyE.size();
                    valueBuilder.reset(new BSONArrayBuilder( reduceArgs.subarrayStart( "tuples" ) ));
                }

                BSONElement ee = j.next();

                uassert( 13070 , "value to large to reduce" , ee.size() < ( BSONObjMaxUserSize / 2 ) );

                if ( sizeSoFar + ee.size() > BSONObjMaxUserSize ) {
                    assert( n > 1 ); // if not, inf. loop
                    break;
                }

                valueBuilder->append( ee );
                sizeSoFar += ee.size();
            }
            assert(valueBuilder);
            valueBuilder->done();
            BSONObj args = reduceArgs.obj();

            Scope * s = _func.scope();

            s->invokeSafe( _func.func() , args );

            if ( s->type( "return" ) == Array ) {
                uasserted( 10075 , "reduce -> multiple not supported yet");
                return;
            }

            endSizeEstimate = key.objsize() + ( args.objsize() / tuples.size() );

            if ( n == tuples.size() )
                return;

            // the input list was too large

            BSONList x;
            for ( ; n < tuples.size(); n++ ) {
                x.push_back( tuples[n] );
            }
            BSONObjBuilder temp( endSizeEstimate );
            temp.append( key.firstElement() );
            s->append( temp , "1" , "return" );
            x.push_back( temp.obj() );
            _reduce( x , key , endSizeEstimate );
        }

        Config::Config( const string& _dbname , const BSONObj& cmdObj ) {

            dbname = _dbname;
            ns = dbname + "." + cmdObj.firstElement().valuestr();

            verbose = cmdObj["verbose"].trueValue();

            uassert( 13602 , "outType is no longer a valid option" , cmdObj["outType"].eoo() );

            if ( cmdObj["out"].type() == String ) {
                finalShort = cmdObj["out"].String();
                outType = REPLACE;
            }
            else if ( cmdObj["out"].type() == Object ) {
                BSONObj o = cmdObj["out"].embeddedObject();
                uassert( 13607 , "'out' has to have a single field" , o.nFields() == 1 );

                BSONElement e = o.firstElement();
                string t = e.fieldName();

                if ( t == "normal" || t == "replace" ) {
                    outType = REPLACE;
                    finalShort = e.String();
                }
                else if ( t == "merge" ) {
                    outType = MERGE;
                    finalShort = e.String();
                }
                else if ( t == "reduce" ) {
                    outType = REDUCE;
                    finalShort = e.String();
                }
                else if ( t == "inline" ) {
                    outType = INMEMORY;
                }
                else {
                    uasserted( 13522 , str::stream() << "unknown out specifier [" << t << "]" );
                }
            }
            else {
                uasserted( 13606 , "'out' has to be a string or an object" );
            }

            if ( outType != INMEMORY ) { // setup names
                tempLong = str::stream() << dbname << ".tmp.mr." << cmdObj.firstElement().String() << "_" << finalShort << "_" << JOB_NUMBER++;

                incLong = tempLong + "_inc";

                finalLong = str::stream() << dbname << "." << finalShort;
            }

            {
                // scope and code

                if ( cmdObj["scope"].type() == Object )
                    scopeSetup = cmdObj["scope"].embeddedObjectUserCheck();

                mapper.reset( new JSMapper( cmdObj["map"] ) );
                reducer.reset( new JSReducer( cmdObj["reduce"] ) );
                if ( cmdObj["finalize"].type() )
                    finalizer.reset( new JSFinalizer( cmdObj["finalize"] ) );

                if ( cmdObj["mapparams"].type() == Array ) {
                    mapParams = cmdObj["mapparams"].embeddedObjectUserCheck();
                }

            }

            {
                // query options
                BSONElement q = cmdObj["query"];
                if ( q.type() == Object )
                    filter = q.embeddedObjectUserCheck();
                else
                    uassert( 13608 , "query has to be blank or an Object" , ! q.trueValue() );


                BSONElement s = cmdObj["sort"];
                if ( s.type() == Object )
                    sort = s.embeddedObjectUserCheck();
                else
                    uassert( 13609 , "sort has to be blank or an Object" , ! s.trueValue() );

                if ( cmdObj["limit"].isNumber() )
                    limit = cmdObj["limit"].numberLong();
                else
                    limit = 0;
            }
        }

        void State::prepTempCollection() {
            if ( ! _onDisk )
                return;

            _db.dropCollection( _config.tempLong );

            {
                // create
                writelock lock( _config.tempLong.c_str() );
                Client::Context ctx( _config.tempLong.c_str() );
                string errmsg;
                assert( userCreateNS( _config.tempLong.c_str() , BSONObj() , errmsg , true ) );
            }


            {
                // copy indexes
                auto_ptr<DBClientCursor> idx = _db.getIndexes( _config.finalLong );
                while ( idx->more() ) {
                    BSONObj i = idx->next();

                    BSONObjBuilder b( i.objsize() + 16 );
                    b.append( "ns" , _config.tempLong );
                    BSONObjIterator j( i );
                    while ( j.more() ) {
                        BSONElement e = j.next();
                        if ( str::equals( e.fieldName() , "_id" ) ||
                                str::equals( e.fieldName() , "ns" ) )
                            continue;

                        b.append( e );
                    }

                    BSONObj indexToInsert = b.obj();
                    insert( Namespace( _config.tempLong.c_str() ).getSisterNS( "system.indexes" ).c_str() , indexToInsert );
                }

            }

        }

        void State::appendResults( BSONObjBuilder& final ) {
            if ( _onDisk )
                return;

            uassert( 13604 , "too much data for in memory map/reduce" , _size < ( BSONObjMaxUserSize / 2 ) );

            BSONArrayBuilder b( (int)(_size * 1.2) ); // _size is data size, doesn't count overhead and keys

            for ( InMemory::iterator i=_temp->begin(); i!=_temp->end(); ++i ) {
                BSONObj key = i->first;
                BSONList& all = i->second;

                assert( all.size() == 1 );

                BSONObjIterator vi( all[0] );
                vi.next();

                BSONObjBuilder temp( b.subobjStart() );
                temp.appendAs( key.firstElement() , "_id" );
                temp.appendAs( vi.next() , "value" );
                temp.done();
            }

            BSONArray res = b.arr();
            uassert( 13605 , "too much data for in memory map/reduce" , res.objsize() < ( BSONObjMaxUserSize * 2 / 3 ) );

            final.append( "results" , res );
        }

        long long State::renameIfNeeded() {
            if ( ! _onDisk )
                return _temp->size();

            dblock lock;

            if ( _config.finalLong == _config.tempLong )
                return _db.count( _config.finalLong );

            switch ( _config.outType ) {
            case Config::REPLACE: {
                _db.dropCollection( _config.finalLong );
                BSONObj info;
                uassert( 10076 ,  "rename failed" ,
                         _db.runCommand( "admin" , BSON( "renameCollection" << _config.tempLong << "to" << _config.finalLong ) , info ) );
                _db.dropCollection( _config.tempLong );
                break;
            }
            case Config::MERGE: {
                auto_ptr<DBClientCursor> cursor = _db.query( _config.tempLong , BSONObj() );
                while ( cursor->more() ) {
                    BSONObj o = cursor->next();
                    Helpers::upsert( _config.finalLong , o );
                }
                _db.dropCollection( _config.tempLong );
                break;
            }
            case Config::REDUCE: {
                BSONList values;

                auto_ptr<DBClientCursor> cursor = _db.query( _config.tempLong , BSONObj() );
                while ( cursor->more() ) {
                    BSONObj temp = cursor->next();
                    BSONObj old;

                    bool found;
                    {
                        Client::Context tx( _config.finalLong );
                        found = Helpers::findOne( _config.finalLong.c_str() , temp["_id"].wrap() , old , true );
                    }

                    if ( found ) {
                        // need to reduce
                        values.clear();
                        values.push_back( temp );
                        values.push_back( old );
                        Helpers::upsert( _config.finalLong , _config.reducer->reduce( values , _config.finalizer.get() ) );
                    }
                    else {
                        Helpers::upsert( _config.finalLong , temp );
                    }
                }
                _db.dropCollection( _config.tempLong );
                break;
            }
            case Config::INMEMORY: {
                return _temp->size();
            }
            }

            return _db.count( _config.finalLong );
        }

        void State::insert( const string& ns , BSONObj& o ) {
            assert( _onDisk );

            writelock l( ns );
            Client::Context ctx( ns );

            theDataFileMgr.insertAndLog( ns.c_str() , o , false );
        }

        void State::_insertToInc( BSONObj& o ) {
            assert( _onDisk );
            theDataFileMgr.insertWithObjMod( _config.incLong.c_str() , o , true );
        }

        State::State( const Config& c ) : _config( c ), _size(0), _numEmits(0) {
            _temp.reset( new InMemory() );
            _onDisk = _config.outType != Config::INMEMORY;
        }

        bool State::sourceExists() {
            return _db.exists( _config.ns );
        }

        long long State::incomingDocuments() {
            return _db.count( _config.ns , _config.filter , 0 , (unsigned) _config.limit );
        }

        State::~State() {
            if ( _onDisk ) {
                try {
                    _db.dropCollection( _config.tempLong );
                    _db.dropCollection( _config.incLong );
                }
                catch ( std::exception& e ) {
                    error() << "couldn't cleanup after map reduce: " << e.what() << endl;
                }
            }
        }

        void State::init() {
            // setup js
            _scope.reset(globalScriptEngine->getPooledScope( _config.dbname ).release() );
            _scope->localConnect( _config.dbname.c_str() );

            if ( ! _config.scopeSetup.isEmpty() )
                _scope->init( &_config.scopeSetup );

            _config.mapper->init( this );
            _config.reducer->init( this );
            if ( _config.finalizer )
                _config.finalizer->init( this );

            _scope->injectNative( "emit" , fast_emit );

            if ( _onDisk ) {
                // clear temp collections
                _db.dropCollection( _config.tempLong );
                _db.dropCollection( _config.incLong );

                writelock l( _config.incLong );
                Client::Context ctx( _config.incLong );
                string err;
                assert( userCreateNS( _config.incLong.c_str() , BSON( "autoIndexId" << 0 ) , err , false ) );
            }

        }

        void State::finalReduce( BSONList& values ) {
            if ( values.size() == 0 )
                return;

            BSONObj key = values.begin()->firstElement().wrap( "_id" );
            BSONObj res = _config.reducer->reduce( values , _config.finalizer.get() );

            insert( _config.tempLong , res );
        }

        void State::finalReduce( CurOp * op , ProgressMeterHolder& pm ) {
            if ( ! _onDisk ) {
                if ( _config.finalizer ) {
                    long size = 0;
                    for ( InMemory::iterator i=_temp->begin(); i!=_temp->end(); ++i ) {
                        BSONObj key = i->first;
                        BSONList& all = i->second;

                        assert( all.size() == 1 );

                        BSONObj res = _config.finalizer->finalize( all[0] );

                        all.clear();
                        all.push_back( res );
                        size += res.objsize();
                    }
                    _size = size;
                }
                return;
            }

            assert( _temp->size() == 0 );

            // TODO: this is a bit sketchy
            //       since it could block
            BSONObj sortKey = BSON( "0" << 1 );
            _db.ensureIndex( _config.incLong , sortKey );

            readlock rl( _config.incLong.c_str() );
            Client::Context ctx( _config.incLong );

            BSONObj prev;
            BSONList all;

            assert( pm == op->setMessage( "m/r: (3/3) final reduce to collection" , _db.count( _config.incLong ) ) );

            shared_ptr<Cursor> temp = bestGuessCursor( _config.incLong.c_str() , BSONObj() , sortKey );
            auto_ptr<ClientCursor> cursor( new ClientCursor( QueryOption_NoCursorTimeout , temp , _config.incLong.c_str() ) );

            while ( cursor->ok() ) {
                BSONObj o = cursor->current().getOwned();
                cursor->advance();

                pm.hit();

                if ( o.woSortOrder( prev , sortKey ) == 0 ) {
                    all.push_back( o );
                    if ( pm->hits() % 1000 == 0 ) {
                        if ( ! cursor->yield() ) {
                            cursor.release();
                            break;
                        }
                        killCurrentOp.checkForInterrupt();
                    }
                    continue;
                }

                ClientCursor::YieldLock yield (cursor.get());
                finalReduce( all );

                all.clear();
                prev = o;
                all.push_back( o );

                if ( ! yield.stillOk() ) {
                    cursor.release();
                    break;
                }

                killCurrentOp.checkForInterrupt();
            }

            {
                dbtempreleasecond tl;
                if ( ! tl.unlocked() )
                    log( LL_WARNING ) << "map/reduce can't temp release" << endl;
                finalReduce( all );
            }

            pm.finished();
        }

        void State::reduceInMemory() {

            InMemory * n = new InMemory(); // for new data
            long nSize = 0;

            for ( InMemory::iterator i=_temp->begin(); i!=_temp->end(); ++i ) {
                BSONObj key = i->first;
                BSONList& all = i->second;

                if ( all.size() == 1 ) {
                    if ( _onDisk ) {
                        // this key has low cardinality, so just write to db
                        writelock l(_config.incLong);
                        Client::Context ctx(_config.incLong.c_str());
                        _insertToInc( *(all.begin()) );
                    }
                    else {
                        _add( n , all[0] , nSize );
                    }
                }
                else if ( all.size() > 1 ) {
                    BSONObj res = _config.reducer->reduce( all );
                    _add( n , res , nSize );
                }
            }

            _temp.reset( n );
            _size = nSize;
        }

        void State::dumpToInc() {
            if ( ! _onDisk )
                return;

            writelock l(_config.incLong);
            Client::Context ctx(_config.incLong);

            for ( InMemory::iterator i=_temp->begin(); i!=_temp->end(); i++ ) {
                BSONList& all = i->second;
                if ( all.size() < 1 )
                    continue;

                for ( BSONList::iterator j=all.begin(); j!=all.end(); j++ )
                    _insertToInc( *j );
            }
            _temp->clear();
            _size = 0;

        }

        void State::emit( const BSONObj& a ) {
            _numEmits++;
            _add( _temp.get() , a , _size );
        }

        void State::_add( InMemory* im, const BSONObj& a , long& size ) {
            BSONList& all = (*im)[a];
            all.push_back( a );
            size += a.objsize() + 16;
        }

        void State::checkSize() {
            if ( ! _onDisk )
                return;

            if ( _size < 1024 * 5 )
                return;

            long before = _size;
            reduceInMemory();
            log(1) << "  mr: did reduceInMemory  " << before << " -->> " << _size << endl;

            if ( _size < 1024 * 15 )
                return;

            dumpToInc();
            log(1) << "  mr: dumping to db" << endl;
        }

        boost::thread_specific_ptr<State*> _tl;

        BSONObj fast_emit( const BSONObj& args ) {
            uassert( 10077 , "fast_emit takes 2 args" , args.nFields() == 2 );
            uassert( 13069 , "an emit can't be more than half max bson size" , args.objsize() < ( BSONObjMaxUserSize / 2 ) );
            (*_tl)->emit( args );
            return BSONObj();
        }

        class MapReduceCommand : public Command {
        public:
            MapReduceCommand() : Command("mapReduce", false, "mapreduce") {}
            virtual bool slaveOk() const { return true; }

            virtual void help( stringstream &help ) const {
                help << "Run a map/reduce operation on the server.\n";
                help << "Note this is used for aggregation, not querying, in MongoDB.\n";
                help << "http://www.mongodb.org/display/DOCS/MapReduce";
            }
            virtual LockType locktype() const { return NONE; }
            bool run(const string& dbname , BSONObj& cmd, string& errmsg, BSONObjBuilder& result, bool fromRepl ) {
                Timer t;
                Client::GodScope cg;
                Client& client = cc();
                CurOp * op = client.curop();

                Config config( dbname , cmd );

                log(1) << "mr ns: " << config.ns << endl;

                bool shouldHaveData = false;

                long long num = 0;
                long long inReduce = 0;

                BSONObjBuilder countsBuilder;
                BSONObjBuilder timingBuilder;
                State state( config );

                if ( ! state.sourceExists() ) {
                    errmsg = "ns doesn't exist";
                    return false;
                }

                try {
                    state.init();

                    {
                        State** s = new State*[1];
                        s[0] = &state;
                        _tl.reset( s );
                    }

                    wassert( config.limit < 0x4000000 ); // see case on next line to 32 bit unsigned
                    ProgressMeterHolder pm( op->setMessage( "m/r: (1/3) emit phase" , state.incomingDocuments() ) );
                    long long mapTime = 0;
                    {
                        readlock lock( config.ns );
                        Client::Context ctx( config.ns );

                        shared_ptr<Cursor> temp = bestGuessCursor( config.ns.c_str(), config.filter, config.sort );
                        auto_ptr<ClientCursor> cursor( new ClientCursor( QueryOption_NoCursorTimeout , temp , config.ns.c_str() ) );

                        Timer mt;
                        while ( cursor->ok() ) {

                            if ( cursor->currentIsDup() ) {
                                cursor->advance();
                                continue;
                            }

                            if ( ! cursor->currentMatches() ) {
                                cursor->advance();
                                continue;
                            }

                            BSONObj o = cursor->current();
                            cursor->advance();

                            if ( config.verbose ) mt.reset();
                            config.mapper->map( o );
                            if ( config.verbose ) mapTime += mt.micros();

                            num++;
                            if ( num % 100 == 0 ) {
                                ClientCursor::YieldLock yield (cursor.get());
                                Timer t;
                                state.checkSize();
                                inReduce += t.micros();

                                if ( ! yield.stillOk() ) {
                                    cursor.release();
                                    break;
                                }

                                killCurrentOp.checkForInterrupt();
                            }
                            pm.hit();

                            if ( config.limit && num >= config.limit )
                                break;
                        }
                    }
                    pm.finished();

                    killCurrentOp.checkForInterrupt();

                    countsBuilder.appendNumber( "input" , num );
                    countsBuilder.appendNumber( "emit" , state.numEmits() );
                    if ( state.numEmits() )
                        shouldHaveData = true;

                    timingBuilder.append( "mapTime" , mapTime / 1000 );
                    timingBuilder.append( "emitLoop" , t.millis() );

                    // final reduce
                    op->setMessage( "m/r: (2/3) final reduce in memory" );
                    state.reduceInMemory();
                    state.dumpToInc();

                    state.prepTempCollection();
                    state.finalReduce( op , pm );

                    _tl.reset();
                }
                catch ( ... ) {
                    log() << "mr failed, removing collection" << endl;
                    throw;
                }

                long long finalCount = state.renameIfNeeded();
                state.appendResults( result );

                timingBuilder.append( "total" , t.millis() );

                result.append( "result" , config.finalShort );
                result.append( "timeMillis" , t.millis() );
                countsBuilder.appendNumber( "output" , finalCount );
                if ( config.verbose ) result.append( "timing" , timingBuilder.obj() );
                result.append( "counts" , countsBuilder.obj() );

                if ( finalCount == 0 && shouldHaveData ) {
                    result.append( "cmd" , cmd );
                    errmsg = "there were emits but no data!";
                    return false;
                }

                return true;
            }

        } mapReduceCommand;

        class MapReduceFinishCommand : public Command {
        public:
            MapReduceFinishCommand() : Command( "mapreduce.shardedfinish" ) {}
            virtual bool slaveOk() const { return true; }

            virtual LockType locktype() const { return NONE; }
            bool run(const string& dbname , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {
                string shardedOutputCollection = cmdObj["shardedOutputCollection"].valuestrsafe();

                Config config( dbname , cmdObj.firstElement().embeddedObjectUserCheck() );
                config.incLong = config.tempLong;

                set<ServerAndQuery> servers;

                BSONObjBuilder shardCounts;
                map<string,long long> counts;

                BSONObj shards = cmdObj["shards"].embeddedObjectUserCheck();
                vector< auto_ptr<DBClientCursor> > shardCursors;

                {
                    // parse per shard results
                    BSONObjIterator i( shards );
                    while ( i.more() ) {
                        BSONElement e = i.next();
                        string shard = e.fieldName();

                        BSONObj res = e.embeddedObjectUserCheck();

                        uassert( 10078 ,  "something bad happened" , shardedOutputCollection == res["result"].valuestrsafe() );
                        servers.insert( shard );
                        shardCounts.appendAs( res["counts"] , shard );

                        BSONObjIterator j( res["counts"].embeddedObjectUserCheck() );
                        while ( j.more() ) {
                            BSONElement temp = j.next();
                            counts[temp.fieldName()] += temp.numberLong();
                        }

                    }

                }

                State state(config);
                state.prepTempCollection();

                {
                    // reduce from each stream

                    BSONObj sortKey = BSON( "_id" << 1 );

                    ParallelSortClusteredCursor cursor( servers , dbname + "." + shardedOutputCollection ,
                                                        Query().sort( sortKey ) );
                    cursor.init();
                    state.init();

                    BSONList values;

                    result.append( "result" , config.finalShort );

                    while ( cursor.more() ) {
                        BSONObj t = cursor.next().getOwned();

                        if ( values.size() == 0 ) {
                            values.push_back( t );
                            continue;
                        }

                        if ( t.woSortOrder( *(values.begin()) , sortKey ) == 0 ) {
                            values.push_back( t );
                            continue;
                        }


                        state.emit( config.reducer->reduce( values , config.finalizer.get() ) );
                        values.clear();
                        values.push_back( t );
                    }

                    if ( values.size() )
                        state.emit( config.reducer->reduce( values , config.finalizer.get() ) );
                }


                state.dumpToInc();

                state.renameIfNeeded();
                state.appendResults( result );

                for ( set<ServerAndQuery>::iterator i=servers.begin(); i!=servers.end(); i++ ) {
                    ScopedDbConnection conn( i->_server );
                    conn->dropCollection( dbname + "." + shardedOutputCollection );
                    conn.done();
                }

                result.append( "shardCounts" , shardCounts.obj() );

                {
                    BSONObjBuilder c;
                    for ( map<string,long long>::iterator i=counts.begin(); i!=counts.end(); i++ ) {
                        c.append( i->first , i->second );
                    }
                    result.append( "counts" , c.obj() );
                }

                return 1;
            }
        } mapReduceFinishCommand;

    }

}

