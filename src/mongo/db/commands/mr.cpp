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
#include "../../client/connpool.h"
#include "../../client/parallel.h"
#include "../matcher.h"
#include "../clientcursor.h"
#include "../replutil.h"
#include "../../s/d_chunk_manager.h"
#include "../../s/d_logic.h"
#include "../../s/grid.h"

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
            verify( _scope );
            _scope->init( &_wantedScope );

            _func = _scope->createFunction( _code.c_str() );
            uassert( 13598 , str::stream() << "couldn't compile code for: " << _type , _func );

            // install in JS scope so that it can be called in JS mode
            _scope->setFunction(_type.c_str(), _code.c_str());
        }

        void JSMapper::init( State * state ) {
            _func.init( state );
            _params = state->config().mapParams;
        }

        /**
         * Applies the map function to an object, which should internally call emit()
         */
        void JSMapper::map( const BSONObj& o ) {
            Scope * s = _func.scope();
            verify( s );
            if ( s->invoke( _func.func() , &_params, &o , 0 , true, false, true ) )
                throw UserException( 9014, str::stream() << "map invoke failed: " + s->getError() );
        }

        /**
         * Applies the finalize function to a tuple obj (key, val)
         * Returns tuple obj {_id: key, value: newval}
         */
        BSONObj JSFinalizer::finalize( const BSONObj& o ) {
            Scope * s = _func.scope();

            Scope::NoDBAccess no = s->disableDBAccess( "can't access db inside finalize" );
            s->invokeSafe( _func.func() , &o, 0 );

            // don't want to use o.objsize() to size b
            // since there are many cases where the point of finalize
            // is converting many fields to 1
            BSONObjBuilder b;
            b.append( o.firstElement() );
            s->append( b , "value" , "return" );
            return b.obj();
        }

        void JSReducer::init( State * state ) {
            _func.init( state );
        }

        /**
         * Reduces a list of tuple objects (key, value) to a single tuple {"0": key, "1": value}
         */
        BSONObj JSReducer::reduce( const BSONList& tuples ) {
            if (tuples.size() <= 1)
                return tuples[0];
            BSONObj key;
            int endSizeEstimate = 16;
            _reduce( tuples , key , endSizeEstimate );

            BSONObjBuilder b(endSizeEstimate);
            b.appendAs( key.firstElement() , "0" );
            _func.scope()->append( b , "1" , "return" );
            return b.obj();
        }

        /**
         * Reduces a list of tuple object (key, value) to a single tuple {_id: key, value: val}
         * Also applies a finalizer method if present.
         */
        BSONObj JSReducer::finalReduce( const BSONList& tuples , Finalizer * finalizer ) {

            BSONObj res;
            BSONObj key;

            if (tuples.size() == 1) {
                // 1 obj, just use it
                key = tuples[0];
                BSONObjBuilder b(key.objsize());
                BSONObjIterator it(key);
                b.appendAs( it.next() , "_id" );
                b.appendAs( it.next() , "value" );
                res = b.obj();
            }
            else {
                // need to reduce
                int endSizeEstimate = 16;
                _reduce( tuples , key , endSizeEstimate );
                BSONObjBuilder b(endSizeEstimate);
                b.appendAs( key.firstElement() , "_id" );
                _func.scope()->append( b , "value" , "return" );
                res = b.obj();
            }

            if ( finalizer ) {
                res = finalizer->finalize( res );
            }

            return res;
        }

        /**
         * actually applies a reduce, to a list of tuples (key, value).
         * After the call, tuples will hold a single tuple {"0": key, "1": value}
         */
        void JSReducer::_reduce( const BSONList& tuples , BSONObj& key , int& endSizeEstimate ) {
            uassert( 10074 ,  "need values" , tuples.size() );

            int sizeEstimate = ( tuples.size() * tuples.begin()->getField( "value" ).size() ) + 128;

            // need to build the reduce args: ( key, [values] )
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

                uassert( 13070 , "value too large to reduce" , ee.size() < ( BSONObjMaxUserSize / 2 ) );

                if ( sizeSoFar + ee.size() > BSONObjMaxUserSize ) {
                    verify( n > 1 ); // if not, inf. loop
                    break;
                }

                valueBuilder->append( ee );
                sizeSoFar += ee.size();
            }
            verify(valueBuilder);
            valueBuilder->done();
            BSONObj args = reduceArgs.obj();

            Scope * s = _func.scope();

            s->invokeSafe( _func.func() , &args, 0, 0, false, true, true );
            ++numReduces;

            if ( s->type( "return" ) == Array ) {
                uasserted( 10075 , "reduce -> multiple not supported yet");
                return;
            }

            endSizeEstimate = key.objsize() + ( args.objsize() / tuples.size() );

            if ( n == tuples.size() )
                return;

            // the input list was too large, add the rest of elmts to new tuples and reduce again
            // note: would be better to use loop instead of recursion to avoid stack overflow
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

        Config::Config( const string& _dbname , const BSONObj& cmdObj ) :
            outNonAtomic(false)
        {

            dbname = _dbname;
            ns = dbname + "." + cmdObj.firstElement().valuestr();

            verbose = cmdObj["verbose"].trueValue();
            jsMode = cmdObj["jsMode"].trueValue();
            splitInfo = 0;
            if (cmdObj.hasField("splitInfo"))
                splitInfo = cmdObj["splitInfo"].Int();

            jsMaxKeys = 500000;
            reduceTriggerRatio = 10.0;
            maxInMemSize = 500 * 1024;

            uassert( 13602 , "outType is no longer a valid option" , cmdObj["outType"].eoo() );

            if ( cmdObj["out"].type() == String ) {
                finalShort = cmdObj["out"].String();
                outType = REPLACE;
            }
            else if ( cmdObj["out"].type() == Object ) {
                BSONObj o = cmdObj["out"].embeddedObject();

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

                if (o.hasElement("db")) {
                    outDB = o["db"].String();
                }

                if (o.hasElement("nonAtomic")) {
                    outNonAtomic = o["nonAtomic"].Bool();
                    if (outNonAtomic)
                        uassert( 15895 , "nonAtomic option cannot be used with this output type", (outType == REDUCE || outType == MERGE) );
                }
            }
            else {
                uasserted( 13606 , "'out' has to be a string or an object" );
            }

            shardedFirstPass = false;
            if (cmdObj.hasField("shardedFirstPass") && cmdObj["shardedFirstPass"].trueValue()){
                massert(16054, "shardedFirstPass should only use replace outType", outType == REPLACE);
                shardedFirstPass = true;
            }

            if ( outType != INMEMORY ) { // setup names
                tempLong = str::stream() << (outDB.empty() ? dbname : outDB) << ".tmp.mr." << cmdObj.firstElement().String() << "_" << JOB_NUMBER++;

                incLong = tempLong + "_inc";

                finalLong = str::stream() << (outDB.empty() ? dbname : outDB) << "." << finalShort;
            }

            {
                // scope and code

                if ( cmdObj["scope"].type() == Object )
                    scopeSetup = cmdObj["scope"].embeddedObjectUserCheck();

                mapper.reset( new JSMapper( cmdObj["map"] ) );
                reducer.reset( new JSReducer( cmdObj["reduce"] ) );
                if ( cmdObj["finalize"].type() && cmdObj["finalize"].trueValue() )
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

        /**
         * Create temporary collection, set up indexes
         */
        void State::prepTempCollection() {
            if ( ! _onDisk )
                return;

            if (_config.incLong != _config.tempLong) {
                // create the inc collection and make sure we have index on "0" key
                _db.dropCollection( _config.incLong );
                {
                    Client::WriteContext ctx( _config.incLong );
                    string err;
                    if ( ! userCreateNS( _config.incLong.c_str() , BSON( "autoIndexId" << 0 << "temp" << true ) , err , false ) ) {
                        uasserted( 13631 , str::stream() << "userCreateNS failed for mr incLong ns: " << _config.incLong << " err: " << err );
                    }
                }

                BSONObj sortKey = BSON( "0" << 1 );
                _db.ensureIndex( _config.incLong , sortKey );
            }

            // create temp collection
            _db.dropCollection( _config.tempLong );
            {
                Client::WriteContext ctx( _config.tempLong.c_str() );
                string errmsg;
                if ( ! userCreateNS( _config.tempLong.c_str() , BSON("temp" << true) , errmsg , true ) ) {
                    uasserted( 13630 , str::stream() << "userCreateNS failed for mr tempLong ns: " << _config.tempLong << " err: " << errmsg );
                }
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

        /**
         * For inline mode, appends results to output object.
         * Makes sure (key, value) tuple is formatted as {_id: key, value: val}
         */
        void State::appendResults( BSONObjBuilder& final ) {
            if ( _onDisk ) {
                if (!_config.outDB.empty()) {
                    BSONObjBuilder loc;
                    if ( !_config.outDB.empty())
                        loc.append( "db" , _config.outDB );
                    if ( !_config.finalShort.empty() )
                        loc.append( "collection" , _config.finalShort );
                    final.append("result", loc.obj());
                }
                else {
                    if ( !_config.finalShort.empty() )
                        final.append( "result" , _config.finalShort );
                }

                if ( _config.splitInfo > 0 ) {
                    // add split points, used for shard
                    BSONObj res;
                    BSONObj idKey = BSON( "_id" << 1 );
                    if ( ! _db.runCommand( "admin" , BSON( "splitVector" << _config.finalLong << "keyPattern" << idKey << "maxChunkSizeBytes" << _config.splitInfo ) , res ) ) {
                        uasserted( 15921 ,  str::stream() << "splitVector failed: " << res );
                    }
                    if ( res.hasField( "splitKeys" ) )
                        final.append( res.getField( "splitKeys" ) );
                }
                return;
            }

            if (_jsMode) {
                ScriptingFunction getResult = _scope->createFunction("var map = _mrMap; var result = []; for (key in map) { result.push({_id: key, value: map[key]}) } return result;");
                _scope->invoke(getResult, 0, 0, 0, false);
                BSONObj obj = _scope->getObject("return");
                final.append("results", BSONArray(obj));
                return;
            }

            uassert( 13604 , "too much data for in memory map/reduce" , _size < BSONObjMaxUserSize );

            BSONArrayBuilder b( (int)(_size * 1.2) ); // _size is data size, doesn't count overhead and keys

            for ( InMemory::iterator i=_temp->begin(); i!=_temp->end(); ++i ) {
                BSONObj key = i->first;
                BSONList& all = i->second;

                verify( all.size() == 1 );

                BSONObjIterator vi( all[0] );
                vi.next();

                BSONObjBuilder temp( b.subobjStart() );
                temp.appendAs( key.firstElement() , "_id" );
                temp.appendAs( vi.next() , "value" );
                temp.done();
            }

            BSONArray res = b.arr();
            final.append( "results" , res );
        }

        /**
         * Does post processing on output collection.
         * This may involve replacing, merging or reducing.
         */
        long long State::postProcessCollection(CurOp* op, ProgressMeterHolder& pm) {
            if ( _onDisk == false || _config.outType == Config::INMEMORY )
                return numInMemKeys();

            if (_config.outNonAtomic)
                return postProcessCollectionNonAtomic(op, pm);
            Lock::GlobalWrite lock; // TODO(erh): this is how it was, but seems it doesn't need to be global
            return postProcessCollectionNonAtomic(op, pm);
        }

        long long State::postProcessCollectionNonAtomic(CurOp* op, ProgressMeterHolder& pm) {

            if ( _config.finalLong == _config.tempLong )
                return _db.count( _config.finalLong );

            if ( _config.outType == Config::REPLACE || _db.count( _config.finalLong ) == 0 ) {
                Lock::GlobalWrite lock; // TODO(erh): why global???
                // replace: just rename from temp to final collection name, dropping previous collection
                _db.dropCollection( _config.finalLong );
                BSONObj info;

                if ( ! _db.runCommand( "admin"
                                      , BSON( "renameCollection" << _config.tempLong <<
                                              "to" << _config.finalLong <<
                                              "stayTemp" << _config.shardedFirstPass )
                                      , info ) ) {
                    uasserted( 10076 ,  str::stream() << "rename failed: " << info );
                }
                         
                _db.dropCollection( _config.tempLong );
            }
            else if ( _config.outType == Config::MERGE ) {
                // merge: upsert new docs into old collection
                op->setMessage( "m/r: merge post processing" , _db.count( _config.tempLong, BSONObj() ) );
                auto_ptr<DBClientCursor> cursor = _db.query( _config.tempLong , BSONObj() );
                while ( cursor->more() ) {
                    Lock::DBWrite lock( _config.finalLong );
                    BSONObj o = cursor->next();
                    Helpers::upsert( _config.finalLong , o );
                    getDur().commitIfNeeded();
                    pm.hit();
                }
                _db.dropCollection( _config.tempLong );
                pm.finished();
            }
            else if ( _config.outType == Config::REDUCE ) {
                // reduce: apply reduce op on new result and existing one
                BSONList values;

                op->setMessage( "m/r: reduce post processing" , _db.count( _config.tempLong, BSONObj() ) );
                auto_ptr<DBClientCursor> cursor = _db.query( _config.tempLong , BSONObj() );
                while ( cursor->more() ) {
                    Lock::GlobalWrite lock; // TODO(erh) why global?
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
                        Helpers::upsert( _config.finalLong , _config.reducer->finalReduce( values , _config.finalizer.get() ) );
                    }
                    else {
                        Helpers::upsert( _config.finalLong , temp );
                    }
                    getDur().commitIfNeeded();
                    pm.hit();
                }
                _db.dropCollection( _config.tempLong );
                pm.finished();
            }

            return _db.count( _config.finalLong );
        }

        /**
         * Insert doc in collection
         */
        void State::insert( const string& ns , const BSONObj& o ) {
            verify( _onDisk );

            Client::WriteContext ctx( ns );

            theDataFileMgr.insertAndLog( ns.c_str() , o , false );
        }

        /**
         * Insert doc into the inc collection
         */
        void State::_insertToInc( BSONObj& o ) {
            verify( _onDisk );
            theDataFileMgr.insertWithObjMod( _config.incLong.c_str() , o , true );
            getDur().commitIfNeeded();
        }

        State::State( const Config& c ) : _config( c ), _size(0), _dupCount(0), _numEmits(0) {
            _temp.reset( new InMemory() );
            _onDisk = _config.outType != Config::INMEMORY;
        }

        bool State::sourceExists() {
            return _db.exists( _config.ns );
        }

        long long State::incomingDocuments() {
            return _db.count( _config.ns , _config.filter , QueryOption_SlaveOk , (unsigned) _config.limit );
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

            if (_scope) {
                // cleanup js objects
                ScriptingFunction cleanup = _scope->createFunction("delete _emitCt; delete _keyCt; delete _mrMap;");
                _scope->invoke(cleanup, 0, 0, 0, true);
            }
        }

        /**
         * Initialize the mapreduce operation, creating the inc collection
         */
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
            _scope->setBoolean("_doFinal", _config.finalizer.get() != 0);

            // by default start in JS mode, will be faster for small jobs
            _jsMode = _config.jsMode;
//            _jsMode = true;
            switchMode(_jsMode);

            // global JS map/reduce hashmap
            // we use a standard JS object which means keys are only simple types
            // we could also add a real hashmap from a library, still we need to add object comparison methods
//            _scope->setObject("_mrMap", BSONObj(), false);
            ScriptingFunction init = _scope->createFunction("_emitCt = 0; _keyCt = 0; _dupCt = 0; _redCt = 0; if (typeof(_mrMap) === 'undefined') { _mrMap = {}; }");
            _scope->invoke(init, 0, 0, 0, true);

            // js function to run reduce on all keys
//            redfunc = _scope->createFunction("for (var key in hashmap) {  print('Key is ' + key); list = hashmap[key]; ret = reduce(key, list); print('Value is ' + ret); };");
            _reduceAll = _scope->createFunction("var map = _mrMap; var list, ret; for (var key in map) { list = map[key]; if (list.length != 1) { ret = _reduce(key, list); map[key] = [ret]; ++_redCt; } } _dupCt = 0;");
            _reduceAndEmit = _scope->createFunction("var map = _mrMap; var list, ret; for (var key in map) { list = map[key]; if (list.length == 1) { ret = list[0]; } else { ret = _reduce(key, list); ++_redCt; } emit(key, ret); }; delete _mrMap;");
            _reduceAndFinalize = _scope->createFunction("var map = _mrMap; var list, ret; for (var key in map) { list = map[key]; if (list.length == 1) { if (!_doFinal) {continue;} ret = list[0]; } else { ret = _reduce(key, list); ++_redCt; }; if (_doFinal){ ret = _finalize(key, ret); } map[key] = ret; }");
            _reduceAndFinalizeAndInsert = _scope->createFunction("var map = _mrMap; var list, ret; for (var key in map) { list = map[key]; if (list.length == 1) { ret = list[0]; } else { ret = _reduce(key, list); ++_redCt; }; if (_doFinal){ ret = _finalize(key, ret); } _nativeToTemp({_id: key, value: ret}); }");

        }

        void State::switchMode(bool jsMode) {
            _jsMode = jsMode;
            if (jsMode) {
                // emit function that stays in JS
                _scope->setFunction("emit", "function(key, value) { if (typeof(key) === 'object') { _bailFromJS(key, value); return; }; ++_emitCt; var map = _mrMap; var list = map[key]; if (!list) { ++_keyCt; list = []; map[key] = list; } else { ++_dupCt; } list.push(value); }");
                _scope->injectNative("_bailFromJS", _bailFromJS, this);
            }
            else {
                // emit now populates C++ map
                _scope->injectNative( "emit" , fast_emit, this );
            }
        }

        void State::bailFromJS() {
            log(1) << "M/R: Switching from JS mode to mixed mode" << endl;

            // reduce and reemit into c++
            switchMode(false);
            _scope->invoke(_reduceAndEmit, 0, 0, 0, true);
            // need to get the real number emitted so far
            _numEmits = _scope->getNumberInt("_emitCt");
            _config.reducer->numReduces = _scope->getNumberInt("_redCt");
        }

        /**
         * Applies last reduce and finalize on a list of tuples (key, val)
         * Inserts single result {_id: key, value: val} into temp collection
         */
        void State::finalReduce( BSONList& values ) {
            if ( !_onDisk || values.size() == 0 )
                return;

            BSONObj res = _config.reducer->finalReduce( values , _config.finalizer.get() );
            insert( _config.tempLong , res );
        }

        BSONObj _nativeToTemp( const BSONObj& args, void* data ) {
            State* state = (State*) data;
            BSONObjIterator it(args);
            state->insert(state->_config.tempLong, it.next().Obj());
            return BSONObj();
        }

//        BSONObj _nativeToInc( const BSONObj& args, void* data ) {
//            State* state = (State*) data;
//            BSONObjIterator it(args);
//            const BSONObj& obj = it.next().Obj();
//            state->_insertToInc(const_cast<BSONObj&>(obj));
//            return BSONObj();
//        }

        /**
         * Applies last reduce and finalize.
         * After calling this method, the temp collection will be completed.
         * If inline, the results will be in the in memory map
         */
        void State::finalReduce( CurOp * op , ProgressMeterHolder& pm ) {

            if (_jsMode) {
                // apply the reduce within JS
                if (_onDisk) {
                    _scope->injectNative("_nativeToTemp", _nativeToTemp, this);
                    _scope->invoke(_reduceAndFinalizeAndInsert, 0, 0, 0, true);
                    return;
                }
                else {
                    _scope->invoke(_reduceAndFinalize, 0, 0, 0, true);
                    return;
                }
            }

            if ( ! _onDisk ) {
                // all data has already been reduced, just finalize
                if ( _config.finalizer ) {
                    long size = 0;
                    for ( InMemory::iterator i=_temp->begin(); i!=_temp->end(); ++i ) {
                        BSONObj key = i->first;
                        BSONList& all = i->second;

                        verify( all.size() == 1 );

                        BSONObj res = _config.finalizer->finalize( all[0] );

                        all.clear();
                        all.push_back( res );
                        size += res.objsize();
                    }
                    _size = size;
                }
                return;
            }

            // use index on "0" to pull sorted data
            verify( _temp->size() == 0 );
            BSONObj sortKey = BSON( "0" << 1 );
            {
                bool foundIndex = false;

                auto_ptr<DBClientCursor> idx = _db.getIndexes( _config.incLong );
                while ( idx.get() && idx->more() ) {
                    BSONObj x = idx->next();
                    if ( sortKey.woCompare( x["key"].embeddedObject() ) == 0 ) {
                        foundIndex = true;
                        break;
                    }
                }

                verify( foundIndex );
            }

            Client::ReadContext ctx( _config.incLong );

            BSONObj prev;
            BSONList all;

            verify( pm == op->setMessage( "m/r: (3/3) final reduce to collection" , _db.count( _config.incLong, BSONObj(), QueryOption_SlaveOk ) ) );

            shared_ptr<Cursor> temp =
            NamespaceDetailsTransient::bestGuessCursor( _config.incLong.c_str() , BSONObj() ,
                                                       sortKey );
            auto_ptr<ClientCursor> cursor( new ClientCursor( QueryOption_NoCursorTimeout , temp , _config.incLong.c_str() ) );

            // iterate over all sorted objects
            while ( cursor->ok() ) {
                BSONObj o = cursor->current().getOwned();
                cursor->advance();

                pm.hit();

                if ( o.woSortOrder( prev , sortKey ) == 0 ) {
                    // object is same as previous, add to array
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

                try {
                    // reduce a finalize array
                    finalReduce( all );
                }
                catch (...) {
                    yield.relock();
                    cursor.release();
                    throw;
                }

                all.clear();
                prev = o;
                all.push_back( o );

                if ( ! yield.stillOk() ) {
                    cursor.release();
                    break;
                }

                killCurrentOp.checkForInterrupt();
            }
            
            // we need to release here since we temp release below
            cursor.release();

            {
                dbtempreleasecond tl;
                if ( ! tl.unlocked() )
                    log( LL_WARNING ) << "map/reduce can't temp release" << endl;
                // reduce and finalize last array
                finalReduce( all );
            }

            pm.finished();
        }

        /**
         * Attempts to reduce objects in the memory map.
         * A new memory map will be created to hold the results.
         * If applicable, objects with unique key may be dumped to inc collection.
         * Input and output objects are both {"0": key, "1": val}
         */
        void State::reduceInMemory() {

            if (_jsMode) {
                // in js mode the reduce is applied when writing to collection
                return;
            }

            auto_ptr<InMemory> n( new InMemory() ); // for new data
            long nSize = 0;
            _dupCount = 0;

            for ( InMemory::iterator i=_temp->begin(); i!=_temp->end(); ++i ) {
                BSONObj key = i->first;
                BSONList& all = i->second;

                if ( all.size() == 1 ) {
                    // only 1 value for this key
                    if ( _onDisk ) {
                        // this key has low cardinality, so just write to collection
                        Client::WriteContext ctx(_config.incLong.c_str());
                        _insertToInc( *(all.begin()) );
                    }
                    else {
                        // add to new map
                        _add( n.get() , all[0] , nSize );
                    }
                }
                else if ( all.size() > 1 ) {
                    // several values, reduce and add to map
                    BSONObj res = _config.reducer->reduce( all );
                    _add( n.get() , res , nSize );
                }
            }

            // swap maps
            _temp.reset( n.release() );
            _size = nSize;
        }

        /**
         * Dumps the entire in memory map to the inc collection.
         */
        void State::dumpToInc() {
            if ( ! _onDisk )
                return;

            Lock::DBWrite kl(_config.incLong);
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

        /**
         * Adds object to in memory map
         */
        void State::emit( const BSONObj& a ) {
            _numEmits++;
            _add( _temp.get() , a , _size );
        }

        void State::_add( InMemory* im, const BSONObj& a , long& size ) {
            BSONList& all = (*im)[a];
            all.push_back( a );
            size += a.objsize() + 16;
            if (all.size() > 1)
                ++_dupCount;
        }

        /**
         * this method checks the size of in memory map and potentially flushes to disk
         */
        void State::checkSize() {
            if (_jsMode) {
                // try to reduce if it is beneficial
                int dupCt = _scope->getNumberInt("_dupCt");
                int keyCt = _scope->getNumberInt("_keyCt");

                if (keyCt > _config.jsMaxKeys) {
                    // too many keys for JS, switch to mixed
                    _bailFromJS(BSONObj(), this);
                    // then fall through to check map size
                }
                else if (dupCt > (keyCt * _config.reduceTriggerRatio)) {
                    // reduce now to lower mem usage
                    Timer t;
                    _scope->invoke(_reduceAll, 0, 0, 0, true);
                    log(1) << "  MR - did reduceAll: keys=" << keyCt << " dups=" << dupCt << " newKeys=" << _scope->getNumberInt("_keyCt") << " time=" << t.millis() << "ms" << endl;
                    return;
                }
            }

            if (_jsMode)
                return;

            if (_size > _config.maxInMemSize || _dupCount > (_temp->size() * _config.reduceTriggerRatio)) {
                // attempt to reduce in memory map, if memory is too high or we have many duplicates
                long oldSize = _size;
                Timer t;
                reduceInMemory();
                log(1) << "  MR - did reduceInMemory: size=" << oldSize << " dups=" << _dupCount << " newSize=" << _size << " time=" << t.millis() << "ms" << endl;

                // if size is still high, or values are not reducing well, dump
                if ( _onDisk && (_size > _config.maxInMemSize || _size > oldSize / 2) ) {
                    dumpToInc();
                    log(1) << "  MR - dumping to db" << endl;
                }
            }
        }

        /**
         * emit that will be called by js function
         */
        BSONObj fast_emit( const BSONObj& args, void* data ) {
            uassert( 10077 , "fast_emit takes 2 args" , args.nFields() == 2 );
            uassert( 13069 , "an emit can't be more than half max bson size" , args.objsize() < ( BSONObjMaxUserSize / 2 ) );
            
            State* state = (State*) data;
            if ( args.firstElement().type() == Undefined ) {
                BSONObjBuilder b( args.objsize() );
                b.appendNull( "" );
                BSONObjIterator i( args );
                i.next();
                b.append( i.next() );
                state->emit( b.obj() );
            }
            else {
                state->emit( args );
            }
            return BSONObj();
        }

        /**
         * function is called when we realize we cant use js mode for m/r on the 1st key
         */
        BSONObj _bailFromJS( const BSONObj& args, void* data ) {
            State* state = (State*) data;
            state->bailFromJS();

            // emit this particular key if there is one
            if (!args.isEmpty()) {
                fast_emit(args, data);
            }
            return BSONObj();
        }

        /**
         * This class represents a map/reduce command executed on a single server
         */
        class MapReduceCommand : public Command {
        public:
            MapReduceCommand() : Command("mapReduce", false, "mapreduce") {}

            /* why !replset ?
               bad things happen with --slave (i think because of this)
            */
            virtual bool slaveOk() const { return !replSet; }

            virtual bool slaveOverrideOk() const { return true; }

            virtual void help( stringstream &help ) const {
                help << "Run a map/reduce operation on the server.\n";
                help << "Note this is used for aggregation, not querying, in MongoDB.\n";
                help << "http://www.mongodb.org/display/DOCS/MapReduce";
            }

            virtual LockType locktype() const { return NONE; }

            bool run(const string& dbname , BSONObj& cmd, int, string& errmsg, BSONObjBuilder& result, bool fromRepl ) {
                Timer t;
                Client& client = cc();
                CurOp * op = client.curop();

                Config config( dbname , cmd );

                log(1) << "mr ns: " << config.ns << endl;

                uassert( 16149 , "cannot run map reduce without the js engine", globalScriptEngine );

                auto_ptr<ClientCursor> holdCursor;
                ShardChunkManagerPtr chunkManager;

                {
                    // Get chunk manager before we check our version, to make sure it doesn't increment
                    // in the meantime
                    if ( shardingState.needShardChunkManager( config.ns ) ) {
                        chunkManager = shardingState.getShardChunkManager( config.ns );
                    }

                    // Check our version immediately, to avoid migrations happening in the meantime while we do prep
                    Client::ReadContext ctx( config.ns );

                    // Get a very basic cursor, prevents deletion of migrated data while we m/r
                    shared_ptr<Cursor> temp = NamespaceDetailsTransient::getCursor( config.ns.c_str(), BSONObj(), BSONObj() );
                    uassert( 15876, str::stream() << "could not create cursor over " << config.ns << " to hold data while prepping m/r", temp.get() );
                    holdCursor = auto_ptr<ClientCursor>( new ClientCursor( QueryOption_NoCursorTimeout , temp , config.ns.c_str() ) );
                    uassert( 15877, str::stream() << "could not create m/r holding client cursor over " << config.ns, holdCursor.get() );

                }

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

                if (replSet && state.isOnDisk()) {
                    // this means that it will be doing a write operation, make sure we are on Master
                    // ideally this check should be in slaveOk(), but at that point config is not known
                    if (!isMaster(dbname.c_str())) {
                        errmsg = "not master";
                        return false;
                    }
                }

                if (state.isOnDisk() && !client.getAuthenticationInfo()->isAuthorized(dbname)) {
                    errmsg = "read-only user cannot output mapReduce to collection, use inline instead";
                    return false;
                }

                try {
                    state.init();
                    state.prepTempCollection();
                    ProgressMeterHolder pm( op->setMessage( "m/r: (1/3) emit phase" , state.incomingDocuments() ) );

                    wassert( config.limit < 0x4000000 ); // see case on next line to 32 bit unsigned
                    long long mapTime = 0;
                    {
                        // We've got a cursor preventing migrations off, now re-establish our useful cursor

                        // Need lock and context to use it
                        Lock::DBRead lock( config.ns );
                        // This context does no version check, safe b/c we checked earlier and have an
                        // open cursor
                        Client::Context ctx( config.ns, dbpath, true, false );

                        // obtain full cursor on data to apply mr to
                        shared_ptr<Cursor> temp = NamespaceDetailsTransient::getCursor( config.ns.c_str(), config.filter, config.sort );
                        uassert( 16052, str::stream() << "could not create cursor over " << config.ns << " for query : " << config.filter << " sort : " << config.sort, temp.get() );
                        auto_ptr<ClientCursor> cursor( new ClientCursor( QueryOption_NoCursorTimeout , temp , config.ns.c_str() ) );
                        uassert( 16053, str::stream() << "could not create client cursor over " << config.ns << " for query : " << config.filter << " sort : " << config.sort, cursor.get() );

                        // Cleanup our previous cursor
                        holdCursor.reset();

                        Timer mt;
                        // go through each doc
                        while ( cursor->ok() ) {
                            if ( ! cursor->currentMatches() ) {
                                cursor->advance();
                                continue;
                            }

                            // make sure we dont process duplicates in case data gets moved around during map
                            // TODO This won't actually help when data gets moved, it's to handle multikeys.
                            if ( cursor->currentIsDup() ) {
                                cursor->advance();
                                continue;
                            }
                                                        
                            BSONObj o = cursor->current();
                            cursor->advance();

                            // check to see if this is a new object we don't own yet
                            // because of a chunk migration
                            if ( chunkManager && ! chunkManager->belongsToMe( o ) )
                                continue;

                            // do map
                            if ( config.verbose ) mt.reset();
                            config.mapper->map( o );
                            if ( config.verbose ) mapTime += mt.micros();

                            num++;
                            if ( num % 1000 == 0 ) {
                                // try to yield lock regularly
                                ClientCursor::YieldLock yield (cursor.get());
                                Timer t;
                                // check if map needs to be dumped to disk
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
                    // update counters
                    countsBuilder.appendNumber( "input" , num );
                    countsBuilder.appendNumber( "emit" , state.numEmits() );
                    if ( state.numEmits() )
                        shouldHaveData = true;

                    timingBuilder.appendNumber( "mapTime" , mapTime / 1000 );
                    timingBuilder.append( "emitLoop" , t.millis() );

                    op->setMessage( "m/r: (2/3) final reduce in memory" );
                    Timer rt;
                    // do reduce in memory
                    // this will be the last reduce needed for inline mode
                    state.reduceInMemory();
                    // if not inline: dump the in memory map to inc collection, all data is on disk
                    state.dumpToInc();
                    // final reduce
                    state.finalReduce( op , pm );
                    inReduce += rt.micros();
                    countsBuilder.appendNumber( "reduce" , state.numReduces() );
                    timingBuilder.appendNumber( "reduceTime" , inReduce / 1000 );
                    timingBuilder.append( "mode" , state.jsMode() ? "js" : "mixed" );

                    long long finalCount = state.postProcessCollection(op, pm);
                    state.appendResults( result );

                    timingBuilder.appendNumber( "total" , t.millis() );
                    result.appendNumber( "timeMillis" , t.millis() );
                    countsBuilder.appendNumber( "output" , finalCount );
                    if ( config.verbose ) result.append( "timing" , timingBuilder.obj() );
                    result.append( "counts" , countsBuilder.obj() );

                    if ( finalCount == 0 && shouldHaveData ) {
                        result.append( "cmd" , cmd );
                        errmsg = "there were emits but no data!";
                        return false;
                    }
                }
                catch( SendStaleConfigException& e ){
                    log() << "mr detected stale config, should retry" << causedBy(e) << endl;
                    throw e;
                }
                // TODO:  The error handling code for queries is v. fragile,
                // *requires* rethrow AssertionExceptions - should probably fix.
                catch ( AssertionException& e ){
                    log() << "mr failed, removing collection" << causedBy(e) << endl;
                    throw e;
                }
                catch ( std::exception& e ){
                    log() << "mr failed, removing collection" << causedBy(e) << endl;
                    throw e;
                }
                catch ( ... ) {
                    log() << "mr failed for unknown reason, removing collection" << endl;
                    throw;
                }

                return true;
            }

        } mapReduceCommand;

        /**
         * This class represents a map/reduce command executed on the output server of a sharded env
         */
        class MapReduceFinishCommand : public Command {
        public:
            MapReduceFinishCommand() : Command( "mapreduce.shardedfinish" ) {}
            virtual bool slaveOk() const { return !replSet; }
            virtual bool slaveOverrideOk() const { return true; }

            virtual LockType locktype() const { return NONE; }
            bool run(const string& dbname , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
                ShardedConnectionInfo::addHook();
                // legacy name
                string shardedOutputCollection = cmdObj["shardedOutputCollection"].valuestrsafe();
                string inputNS = cmdObj["inputNS"].valuestrsafe();
                if (inputNS.empty())
                    inputNS = dbname + "." + shardedOutputCollection;

                Client& client = cc();
                CurOp * op = client.curop();

                Config config( dbname , cmdObj.firstElement().embeddedObjectUserCheck() );
                State state(config);
                state.init();

                // no need for incremental collection because records are already sorted
                config.incLong = config.tempLong;

                BSONObj shardCounts = cmdObj["shardCounts"].embeddedObjectUserCheck();
                BSONObj counts = cmdObj["counts"].embeddedObjectUserCheck();

                ProgressMeterHolder pm( op->setMessage( "m/r: merge sort and reduce" ) );
                set<ServerAndQuery> servers;
                vector< auto_ptr<DBClientCursor> > shardCursors;

                {
                    // parse per shard results
                    BSONObjIterator i( shardCounts );
                    while ( i.more() ) {
                        BSONElement e = i.next();
                        string shard = e.fieldName();
//                        BSONObj res = e.embeddedObjectUserCheck();
                        servers.insert( shard );
                    }
                }

                state.prepTempCollection();

                BSONList values;
                if (!config.outDB.empty()) {
                    BSONObjBuilder loc;
                    if ( !config.outDB.empty())
                        loc.append( "db" , config.outDB );
                    if ( !config.finalShort.empty() )
                        loc.append( "collection" , config.finalShort );
                    result.append("result", loc.obj());
                }
                else {
                    if ( !config.finalShort.empty() )
                        result.append( "result" , config.finalShort );
                }

                // fetch result from other shards 1 chunk at a time
                // it would be better to do just one big $or query, but then the sorting would not be efficient
                string shardName = shardingState.getShardName();
                DBConfigPtr confOut = grid.getDBConfig( dbname , false );
                vector<ChunkPtr> chunks;
                if ( confOut->isSharded(config.finalLong) ) {
                    ChunkManagerPtr cm = confOut->getChunkManager( config.finalLong );
                    const ChunkMap& chunkMap = cm->getChunkMap();
                    for ( ChunkMap::const_iterator it = chunkMap.begin(); it != chunkMap.end(); ++it ) {
                        ChunkPtr chunk = it->second;
                        if (chunk->getShard().getName() == shardName) chunks.push_back(chunk);
                    }
                }

                long long inputCount = 0;
                unsigned int index = 0;
                BSONObj query;
                BSONArrayBuilder chunkSizes;
                while (true) {
                    ChunkPtr chunk;
                    if (chunks.size() > 0) {
                        chunk = chunks[index];
                        BSONObjBuilder b;
                        b.appendAs(chunk->getMin().firstElement(), "$gte");
                        b.appendAs(chunk->getMax().firstElement(), "$lt");
                        query = BSON("_id" << b.obj());
//                        chunkSizes.append(min);
                    }

                    // reduce from each shard for a chunk
                    BSONObj sortKey = BSON( "_id" << 1 );
                    ParallelSortClusteredCursor cursor( servers , inputNS , Query( query ).sort( sortKey ) );
                    cursor.init();
                    int chunkSize = 0;

                    while ( cursor.more() || !values.empty() ) {
                        BSONObj t;
                        if (cursor.more()) {
                            t = cursor.next().getOwned();
                            ++inputCount;

                            if ( values.size() == 0 ) {
                                values.push_back( t );
                                continue;
                            }

                            if ( t.woSortOrder( *(values.begin()) , sortKey ) == 0 ) {
                                values.push_back( t );
                                continue;
                            }
                        }

                        BSONObj res = config.reducer->finalReduce( values , config.finalizer.get());
                        chunkSize += res.objsize();
                        if (state.isOnDisk())
                            state.insert( config.tempLong , res );
                        else
                            state.emit(res);
                        values.clear();
                        if (!t.isEmpty())
                            values.push_back( t );
                    }

                    if (chunk) {
                        chunkSizes.append(chunk->getMin());
                        chunkSizes.append(chunkSize);
                    }
                    if (++index >= chunks.size())
                        break;
                }

                result.append( "chunkSizes" , chunkSizes.arr() );

                long long outputCount = state.postProcessCollection(op, pm);
                state.appendResults( result );

                BSONObjBuilder countsB(32);
                countsB.append("input", inputCount);
                countsB.append("reduce", state.numReduces());
                countsB.append("output", outputCount);
                result.append( "counts" , countsB.obj() );

                return 1;
            }
        } mapReduceFinishCommand;

    }

}

