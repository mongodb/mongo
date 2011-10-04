// mr_shard.cpp

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
#include "../util/net/message.h"
#include "../db/dbmessage.h"
#include "../scripting/engine.h"

#include "mr_shard.h"

namespace mongo {

    namespace mr_shard {

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
            uassert( 14836 , str::stream() << "couldn't compile code for: " << _type , _func );

            // install in JS scope so that it can be called in JS mode
            _scope->setFunction(_type.c_str(), _code.c_str());
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

                uassert( 14837 , "value too large to reduce" , ee.size() < ( BSONObjMaxUserSize / 2 ) );

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

            s->invokeSafe( _func.func() , &args, 0, 0, false, true, true );
            ++numReduces;

            if ( s->type( "return" ) == Array ) {
                uasserted( 14838 , "reduce -> multiple not supported yet");
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

        Config::Config( const string& _dbname , const BSONObj& cmdObj ) {

            dbname = _dbname;
            ns = dbname + "." + cmdObj.firstElement().valuestr();

            verbose = cmdObj["verbose"].trueValue();
            jsMode = cmdObj["jsMode"].trueValue();

            jsMaxKeys = 500000;
            reduceTriggerRatio = 2.0;
            maxInMemSize = 5 * 1024 * 1024;

            uassert( 14841 , "outType is no longer a valid option" , cmdObj["outType"].eoo() );

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
                    uasserted( 14839 , str::stream() << "unknown out specifier [" << t << "]" );
                }

                if (o.hasElement("db")) {
                    outDB = o["db"].String();
                }
            }
            else {
                uasserted( 14840 , "'out' has to be a string or an object" );
            }

            if ( outType != INMEMORY ) { // setup names
                tempLong = str::stream() << (outDB.empty() ? dbname : outDB) << ".tmp.mr." << cmdObj.firstElement().String() << "_" << finalShort << "_" << JOB_NUMBER++;

                incLong = tempLong + "_inc";

                finalLong = str::stream() << (outDB.empty() ? dbname : outDB) << "." << finalShort;
            }

            {
                // scope and code

                if ( cmdObj["scope"].type() == Object )
                    scopeSetup = cmdObj["scope"].embeddedObjectUserCheck();

                reducer.reset( new JSReducer( cmdObj["reduce"] ) );
                if ( cmdObj["finalize"].type() && cmdObj["finalize"].trueValue() )
                    finalizer.reset( new JSFinalizer( cmdObj["finalize"] ) );

            }

            {
                // query options
                if ( cmdObj["limit"].isNumber() )
                    limit = cmdObj["limit"].numberLong();
                else
                    limit = 0;
            }
        }

        State::State( const Config& c ) : _config( c ) {
            _onDisk = _config.outType != Config::INMEMORY;
        }

        State::~State() {
            if ( _onDisk ) {
                try {
//                    _db.dropCollection( _config.tempLong );
//                    _db.dropCollection( _config.incLong );
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
//            _scope->localConnect( _config.dbname.c_str() );
            _scope->externalSetup();

            if ( ! _config.scopeSetup.isEmpty() )
                _scope->init( &_config.scopeSetup );

            _config.reducer->init( this );
            if ( _config.finalizer )
                _config.finalizer->init( this );
            _scope->setBoolean("_doFinal", _config.finalizer);
        }
    }
}

