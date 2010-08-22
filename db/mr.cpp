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
#include "db.h"
#include "instance.h"
#include "commands.h"
#include "../scripting/engine.h"
#include "../client/dbclient.h"
#include "../client/connpool.h"
#include "../client/parallel.h"
#include "queryoptimizer.h"
#include "matcher.h"
#include "clientcursor.h"

namespace mongo {

    namespace mr {

        typedef vector<BSONObj> BSONList;

        class MyCmp {
        public:
            MyCmp(){}
            bool operator()( const BSONObj &l, const BSONObj &r ) const {
                return l.firstElement().woCompare( r.firstElement() ) < 0;
            }
        };

        typedef pair<BSONObj,BSONObj> Data;
        //typedef list< Data > InMemory;
        typedef map< BSONObj,BSONList,MyCmp > InMemory;

        BSONObj reduceValues( BSONList& values , Scope * s , ScriptingFunction reduce , bool final , ScriptingFunction finalize ){
            uassert( 10074 ,  "need values" , values.size() );
            
            int sizeEstimate = ( values.size() * values.begin()->getField( "value" ).size() ) + 128;
            BSONObj key;

            BSONObjBuilder reduceArgs( sizeEstimate );
            boost::scoped_ptr<BSONArrayBuilder>  valueBuilder;
            
            int sizeSoFar = 0;
            unsigned n = 0;
            for ( ; n<values.size(); n++ ){
                BSONObjIterator j(values[n]);
                BSONElement keyE = j.next();
                if ( n == 0 ){
                    reduceArgs.append( keyE );
                    key = keyE.wrap();
                    sizeSoFar = 5 + keyE.size();
                    valueBuilder.reset(new BSONArrayBuilder( reduceArgs.subarrayStart( "values" ) ));
                }
                
                BSONElement ee = j.next();
                
                uassert( 13070 , "value to large to reduce" , ee.size() < ( 2 * 1024 * 1024 ) );

                if ( sizeSoFar + ee.size() > ( 4 * 1024 * 1024 ) ){
                    assert( n > 1 ); // if not, inf. loop
                    break;
                }
                
                valueBuilder->append( ee );
                sizeSoFar += ee.size();
            }
            assert(valueBuilder);
            valueBuilder->done();
            BSONObj args = reduceArgs.obj();

            s->invokeSafe( reduce , args );
            if ( s->type( "return" ) == Array ){
                uassert( 10075 , "reduce -> multiple not supported yet",0);                
                return BSONObj();
            }

            int endSizeEstimate = key.objsize() + ( args.objsize() / values.size() );

            if ( n < values.size() ){
                BSONList x;
                for ( ; n < values.size(); n++ ){
                    x.push_back( values[n] );
                }
                BSONObjBuilder temp( endSizeEstimate );
                temp.append( key.firstElement() );
                s->append( temp , "1" , "return" );
                x.push_back( temp.obj() );
                return reduceValues( x , s , reduce , final , finalize );
            }
            


            if ( finalize ){
                BSONObjBuilder b(endSizeEstimate);
                b.appendAs( key.firstElement() , "_id" );
                s->append( b , "value" , "return" );
                s->invokeSafe( finalize , b.obj() );
            }
            
            BSONObjBuilder b(endSizeEstimate);
            b.appendAs( key.firstElement() , final ? "_id" : "0" );
            s->append( b , final ? "value" : "1" , "return" );
            return b.obj();
        }
        
        class MRSetup {
        public:
            MRSetup( const string& _dbname , const BSONObj& cmdObj , bool markAsTemp = true ){
                static int jobNumber = 1;
                
                dbname = _dbname;
                ns = dbname + "." + cmdObj.firstElement().valuestr();

                verbose = cmdObj["verbose"].trueValue();
                keeptemp = cmdObj["keeptemp"].trueValue();
                
                { // setup names
                    stringstream ss;
                    if ( ! keeptemp )
                        ss << "tmp.";
                    ss << "mr." << cmdObj.firstElement().fieldName() << "_" << time(0) << "_" << jobNumber++;    
                    tempShort = ss.str();
                    tempLong = dbname + "." + tempShort;
                    incLong = tempLong + "_inc";

                    if ( ! keeptemp && markAsTemp )
                        cc().addTempCollection( tempLong );

                    replicate = keeptemp;

                    if ( cmdObj["out"].type() == String ){
                        finalShort = cmdObj["out"].valuestr();
                        replicate = true;
                    }
                    else
                        finalShort = tempShort;
                    
                    finalLong = dbname + "." + finalShort;
                    
                }
             
                { // code
                    mapCode = cmdObj["map"]._asCode();
                    reduceCode = cmdObj["reduce"]._asCode();
                    if ( cmdObj["finalize"].type() ){
                        finalizeCode = cmdObj["finalize"]._asCode();
                    }
                    checkCodeWScope( "map" , cmdObj );
                    checkCodeWScope( "reduce" , cmdObj );
                    checkCodeWScope( "finalize" , cmdObj );
                    
                    if ( cmdObj["mapparams"].type() == Array ){
                        mapparams = cmdObj["mapparams"].embeddedObjectUserCheck();
                    }

                    if ( cmdObj["scope"].type() == Object ){
                        scopeSetup = cmdObj["scope"].embeddedObjectUserCheck();
                    }
                    
                }
                
                { // query options
                    if ( cmdObj["query"].type() == Object ){
                        filter = cmdObj["query"].embeddedObjectUserCheck();
                    }
                    
                    if ( cmdObj["sort"].type() == Object ){
                        sort = cmdObj["sort"].embeddedObjectUserCheck();
                    }

                    if ( cmdObj["limit"].isNumber() )
                        limit = cmdObj["limit"].numberLong();
                    else 
                        limit = 0;
                }
            }
            
            void checkCodeWScope( const char * field , const BSONObj& o ){
                BSONElement e = o[field];
                if ( e.type() != CodeWScope )
                    return;
                BSONObj x = e.codeWScopeObject();
                uassert( 13035 , (string)"can't use CodeWScope with map/reduce function: " + field , x.isEmpty() );
            }

            /**
               @return number objects in collection
             */
            long long renameIfNeeded( DBDirectClient& db ){
                if ( finalLong != tempLong ){
                    db.dropCollection( finalLong );
                    if ( db.count( tempLong ) ){
                        BSONObj info;
                        uassert( 10076 ,  "rename failed" , db.runCommand( "admin" , BSON( "renameCollection" << tempLong << "to" << finalLong ) , info ) );
                    }
                }
                return db.count( finalLong );
            }
                
            string dbname;
            string ns;
            
            // options
            bool verbose;            
            bool keeptemp;
            bool replicate;

            // query options
            
            BSONObj filter;
            BSONObj sort;
            long long limit;

            // functions
            
            string mapCode;
            string reduceCode;
            string finalizeCode;
            
            BSONObj mapparams;
            BSONObj scopeSetup;
            
            // output tables
            string incLong;
            
            string tempShort;
            string tempLong;
            
            string finalShort;
            string finalLong;
            
        }; // end MRsetup

        class MRState {
        public:
            MRState( MRSetup& s ) : setup(s){
                scope = globalScriptEngine->getPooledScope( setup.dbname );
                scope->localConnect( setup.dbname.c_str() );
                
                map = scope->createFunction( setup.mapCode.c_str() );
                if ( ! map )
                    throw UserException( 9012, (string)"map compile failed: " + scope->getError() );

                reduce = scope->createFunction( setup.reduceCode.c_str() );
                if ( ! reduce )
                    throw UserException( 9013, (string)"reduce compile failed: " + scope->getError() );

                if ( setup.finalizeCode.size() )
                    finalize  = scope->createFunction( setup.finalizeCode.c_str() );
                else
                    finalize = 0;
                
                if ( ! setup.scopeSetup.isEmpty() )
                    scope->init( &setup.scopeSetup );

                db.dropCollection( setup.tempLong );
                db.dropCollection( setup.incLong );
                
                writelock l( setup.incLong );
                Client::Context ctx( setup.incLong );
                string err;
                assert( userCreateNS( setup.incLong.c_str() , BSON( "autoIndexId" << 0 ) , err , false ) );

            }

            void finalReduce( BSONList& values ){
                if ( values.size() == 0 )
                    return;

                BSONObj key = values.begin()->firstElement().wrap( "_id" );
                BSONObj res = reduceValues( values , scope.get() , reduce , 1 , finalize );
                
                writelock l( setup.tempLong );
                Client::Context ctx( setup.incLong );
                if ( setup.replicate )
                    theDataFileMgr.insertAndLog( setup.tempLong.c_str() , res , false );
                else
                    theDataFileMgr.insertWithObjMod( setup.tempLong.c_str() , res , false );
            }

            
            MRSetup& setup;
            auto_ptr<Scope> scope;
            DBDirectClient db;

            ScriptingFunction map;
            ScriptingFunction reduce;
            ScriptingFunction finalize;
            
        };
        
        class MRTL {
        public:
            MRTL( MRState& state ) 
                : _state( state )
                , _temp(new InMemory())
            {
                _size = 0;
                numEmits = 0;
            }
            
            void reduceInMemory(){
                boost::shared_ptr<InMemory> old = _temp;
                _temp.reset(new InMemory());
                _size = 0;
                
                for ( InMemory::iterator i=old->begin(); i!=old->end(); i++ ){
                    BSONObj key = i->first;
                    BSONList& all = i->second;
                    
                    if ( all.size() == 1 ){
                        // this key has low cardinality, so just write to db
                        writelock l(_state.setup.incLong);
                        Client::Context ctx(_state.setup.incLong.c_str());
                        write( *(all.begin()) );
                    }
                    else if ( all.size() > 1 ){
                        BSONObj res = reduceValues( all , _state.scope.get() , _state.reduce , false , 0 );
                        insert( res );
                    }
                }
            }

            void dump(){
                writelock l(_state.setup.incLong);
                Client::Context ctx(_state.setup.incLong);
                    
                for ( InMemory::iterator i=_temp->begin(); i!=_temp->end(); i++ ){
                    BSONList& all = i->second;
                    if ( all.size() < 1 )
                        continue;
                    
                    for ( BSONList::iterator j=all.begin(); j!=all.end(); j++ )
                        write( *j );
                }
                _temp->clear();
                _size = 0;

            }
            
            void insert( const BSONObj& a ){
                BSONList& all = (*_temp)[a];
                all.push_back( a );
                _size += a.objsize() + 16;
            }

            void checkSize(){
                if ( _size < 1024 * 5 )
                    return;

                long before = _size;
                reduceInMemory();
                log(1) << "  mr: did reduceInMemory  " << before << " -->> " << _size << endl;

                if ( _size < 1024 * 15 )
                    return;
                
                dump();
                log(1) << "  mr: dumping to db" << endl;
            }

        private:
            void write( BSONObj& o ){
                theDataFileMgr.insertWithObjMod( _state.setup.incLong.c_str() , o , true );
            }
            
            MRState& _state;
        
            boost::shared_ptr<InMemory> _temp;
            long _size;
            
        public:
            long long numEmits;
        };

        boost::thread_specific_ptr<MRTL> _tlmr;

        BSONObj fast_emit( const BSONObj& args ){
            uassert( 10077 , "fast_emit takes 2 args" , args.nFields() == 2 );
            uassert( 13069 , "an emit can't be more than 2mb" , args.objsize() < ( 2 * 1024 * 1024 ) );
            _tlmr->insert( args );
            _tlmr->numEmits++;
            return BSONObj();
        }

        class MapReduceCommand : public Command {
        public:
            MapReduceCommand() : Command("mapReduce", false, "mapreduce"){}
            virtual bool slaveOk() const { return true; }
        
            virtual void help( stringstream &help ) const {
                help << "Run a map/reduce operation on the server.\n";
                help << "Note this is used for aggregation, not querying, in MongoDB.\n";
                help << "http://www.mongodb.org/display/DOCS/MapReduce";
            }
            virtual LockType locktype() const { return NONE; } 
            bool run(const string& dbname , BSONObj& cmd, string& errmsg, BSONObjBuilder& result, bool fromRepl ){
                Timer t;
                Client::GodScope cg;
                Client& client = cc();
                CurOp * op = client.curop();

                MRSetup mr( dbname , cmd );

                log(1) << "mr ns: " << mr.ns << endl;
                
                if ( ! db.exists( mr.ns ) ){
                    errmsg = "ns doesn't exist";
                    return false;
                }
                
                bool shouldHaveData = false;
                
                long long num = 0;
                long long inReduce = 0;
                
                BSONObjBuilder countsBuilder;
                BSONObjBuilder timingBuilder;
                try {
                    
                    MRState state( mr );
                    state.scope->injectNative( "emit" , fast_emit );
                    
                    MRTL * mrtl = new MRTL( state );
                    _tlmr.reset( mrtl );

                    ProgressMeterHolder pm( op->setMessage( "m/r: (1/3) emit phase" , db.count( mr.ns , mr.filter ) ) );
                    long long mapTime = 0;
                    {
                        readlock lock( mr.ns );
                        Client::Context ctx( mr.ns );
                        
                        shared_ptr<Cursor> temp = bestGuessCursor( mr.ns.c_str(), mr.filter, mr.sort );
                        auto_ptr<ClientCursor> cursor( new ClientCursor( QueryOption_NoCursorTimeout , temp , mr.ns.c_str() ) );

                        Timer mt;
                        while ( cursor->ok() ){
                            
                            if ( ! cursor->currentMatches() ){
                                cursor->advance();
                                continue;
                            }
                            
                            BSONObj o = cursor->current(); 
                            cursor->advance();
                            
                            if ( mr.verbose ) mt.reset();
                            
                            state.scope->setThis( &o );
                            if ( state.scope->invoke( state.map , state.setup.mapparams , 0 , true ) )
                                throw UserException( 9014, (string)"map invoke failed: " + state.scope->getError() );
                            
                            if ( mr.verbose ) mapTime += mt.micros();
                            
                            num++;
                            if ( num % 100 == 0 ){
                                ClientCursor::YieldLock yield (cursor.get());
                                Timer t;
                                mrtl->checkSize();
                                inReduce += t.micros();
                                
                                if ( ! yield.stillOk() ){
                                    cursor.release();
                                    break;
                                }

                                killCurrentOp.checkForInterrupt();
                            }
                            pm.hit();
                            
                            if ( mr.limit && num >= mr.limit )
                                break;
                        }
                    }
                    pm.finished();
                    
                    killCurrentOp.checkForInterrupt();

                    countsBuilder.appendNumber( "input" , num );
                    countsBuilder.appendNumber( "emit" , mrtl->numEmits );
                    if ( mrtl->numEmits )
                        shouldHaveData = true;
                    
                    timingBuilder.append( "mapTime" , mapTime / 1000 );
                    timingBuilder.append( "emitLoop" , t.millis() );
                    
                    // final reduce
                    op->setMessage( "m/r: (2/3) final reduce in memory" );
                    mrtl->reduceInMemory();
                    mrtl->dump();
                    
                    BSONObj sortKey = BSON( "0" << 1 );
                    db.ensureIndex( mr.incLong , sortKey );
                    
                    {
                        writelock lock( mr.tempLong.c_str() );
                        Client::Context ctx( mr.tempLong.c_str() );
                        assert( userCreateNS( mr.tempLong.c_str() , BSONObj() , errmsg , mr.replicate ) );
                    }


                    {
                        readlock rl(mr.incLong.c_str());
                        Client::Context ctx( mr.incLong );
                        
                        BSONObj prev;
                        BSONList all;
                        
                        assert( pm == op->setMessage( "m/r: (3/3) final reduce to collection" , db.count( mr.incLong ) ) );

                        shared_ptr<Cursor> temp = bestGuessCursor( mr.incLong.c_str() , BSONObj() , sortKey );
                        auto_ptr<ClientCursor> cursor( new ClientCursor( QueryOption_NoCursorTimeout , temp , mr.incLong.c_str() ) );
                        
                        while ( cursor->ok() ){
                            BSONObj o = cursor->current().getOwned();
                            cursor->advance();
                            
                            pm.hit();
                            
                            if ( o.woSortOrder( prev , sortKey ) == 0 ){
                                all.push_back( o );
                                if ( pm->hits() % 1000 == 0 ){
                                    if ( ! cursor->yield() ){
                                        cursor.release();
                                        break;
                                    } 
                                    killCurrentOp.checkForInterrupt();
                                }
                                continue;
                            }
                        
                            ClientCursor::YieldLock yield (cursor.get());
                            state.finalReduce( all );
                            
                            all.clear();
                            prev = o;
                            all.push_back( o );

                            if ( ! yield.stillOk() ){
                                cursor.release();
                                break;
                            }
                            
                            killCurrentOp.checkForInterrupt();
                        }

                        {
                            dbtempreleasecond tl;
                            if ( ! tl.unlocked() )
                                log( LL_WARNING ) << "map/reduce can't temp release" << endl;
                            state.finalReduce( all );
                        }

                        pm.finished();
                    }

                    _tlmr.reset( 0 );
                }
                catch ( ... ){
                    log() << "mr failed, removing collection" << endl;
                    db.dropCollection( mr.tempLong );
                    db.dropCollection( mr.incLong );
                    throw;
                }
                
                long long finalCount = 0;
                {
                    dblock lock;
                    db.dropCollection( mr.incLong );
                
                    finalCount = mr.renameIfNeeded( db );
                }

                timingBuilder.append( "total" , t.millis() );
                
                result.append( "result" , mr.finalShort );
                result.append( "timeMillis" , t.millis() );
                countsBuilder.appendNumber( "output" , finalCount );
                if ( mr.verbose ) result.append( "timing" , timingBuilder.obj() );
                result.append( "counts" , countsBuilder.obj() );

                if ( finalCount == 0 && shouldHaveData ){
                    result.append( "cmd" , cmd );
                    errmsg = "there were emits but no data!";
                    return false;
                }

                return true;
            }

        private:
            DBDirectClient db;

        } mapReduceCommand;
        
        class MapReduceFinishCommand : public Command {
        public:
            MapReduceFinishCommand() : Command( "mapreduce.shardedfinish" ){}
            virtual bool slaveOk() const { return true; }
            
            virtual LockType locktype() const { return NONE; } 
            bool run(const string& dbname , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
                string shardedOutputCollection = cmdObj["shardedOutputCollection"].valuestrsafe();

                MRSetup mr( dbname , cmdObj.firstElement().embeddedObjectUserCheck() , false );
                
                set<ServerAndQuery> servers;
                
                BSONObjBuilder shardCounts;
                map<string,long long> counts;
                
                BSONObj shards = cmdObj["shards"].embeddedObjectUserCheck();
                vector< auto_ptr<DBClientCursor> > shardCursors;

                { // parse per shard results 
                    BSONObjIterator i( shards );
                    while ( i.more() ){
                        BSONElement e = i.next();
                        string shard = e.fieldName();
                        
                        BSONObj res = e.embeddedObjectUserCheck();
                        
                        uassert( 10078 ,  "something bad happened" , shardedOutputCollection == res["result"].valuestrsafe() );
                        servers.insert( shard );
                        shardCounts.appendAs( res["counts"] , shard );
                        
                        BSONObjIterator j( res["counts"].embeddedObjectUserCheck() );
                        while ( j.more() ){
                            BSONElement temp = j.next();
                            counts[temp.fieldName()] += temp.numberLong();
                        }
                        
                    }
                    
                }
                
                DBDirectClient db;
                    
                { // reduce from each stream
                    
                    BSONObj sortKey = BSON( "_id" << 1 );
                    
                    ParallelSortClusteredCursor cursor( servers , dbname + "." + shardedOutputCollection ,
                                                        Query().sort( sortKey ) );
                    cursor.init();
                    
                    auto_ptr<Scope> s = globalScriptEngine->getPooledScope( dbname );
                    s->localConnect( dbname.c_str() );
                    ScriptingFunction reduceFunction = s->createFunction( mr.reduceCode.c_str() );
                    ScriptingFunction finalizeFunction = 0;
                    if ( mr.finalizeCode.size() )
                        finalizeFunction = s->createFunction( mr.finalizeCode.c_str() );
                    
                    BSONList values;
                    
                    result.append( "result" , mr.finalShort );
                    
                    while ( cursor.more() ){
                        BSONObj t = cursor.next().getOwned();
                        
                        if ( values.size() == 0 ){
                            values.push_back( t );
                            continue;
                        }
                        
                        if ( t.woSortOrder( *(values.begin()) , sortKey ) == 0 ){
                            values.push_back( t );
                            continue;
                        }
                        
                        
                        db.insert( mr.tempLong , reduceValues( values , s.get() , reduceFunction , 1 , finalizeFunction ) );
                        values.clear();
                        values.push_back( t );
                    }
                    
                    if ( values.size() )
                        db.insert( mr.tempLong , reduceValues( values , s.get() , reduceFunction , 1 , finalizeFunction ) );
                }
                
                long long finalCount = mr.renameIfNeeded( db );
                log(0) << " mapreducefinishcommand " << mr.finalLong << " " << finalCount << endl;

                for ( set<ServerAndQuery>::iterator i=servers.begin(); i!=servers.end(); i++ ){
                    ScopedDbConnection conn( i->_server );
                    conn->dropCollection( dbname + "." + shardedOutputCollection );
                    conn.done();
                }
                
                result.append( "shardCounts" , shardCounts.obj() );
                
                {
                    BSONObjBuilder c;
                    for ( map<string,long long>::iterator i=counts.begin(); i!=counts.end(); i++ ){
                        c.append( i->first , i->second );
                    }
                    result.append( "counts" , c.obj() );
                }

                return 1;
            }
        } mapReduceFinishCommand;

    }

}

