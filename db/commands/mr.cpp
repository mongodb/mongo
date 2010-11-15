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

        AtomicUInt MRSetup::JOB_NUMBER;

        BSONObj reduceValues( BSONList& values , MRReduceState * state , bool final ){
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
                
                uassert( 13070 , "value to large to reduce" , ee.size() < ( BSONObjMaxUserSize / 2 ) );

                if ( sizeSoFar + ee.size() > BSONObjMaxUserSize ){
                    assert( n > 1 ); // if not, inf. loop
                    break;
                }
                
                valueBuilder->append( ee );
                sizeSoFar += ee.size();
            }
            assert(valueBuilder);
            valueBuilder->done();
            BSONObj args = reduceArgs.obj();

            state->scope->invokeSafe( state->reduce , args );
            if ( state->scope->type( "return" ) == Array ){
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
                state->scope->append( temp , "1" , "return" );
                x.push_back( temp.obj() );
                return reduceValues( x , state , final );
            }
            


            if ( state->finalize ){
                Scope::NoDBAccess no = state->scope->disableDBAccess( "can't access db inside finalize" );
                BSONObjBuilder b(endSizeEstimate);
                b.appendAs( key.firstElement() , "_id" );
                state->scope->append( b , "value" , "return" );
                state->scope->invokeSafe( state->finalize , b.obj() );
            }
            
            BSONObjBuilder b(endSizeEstimate);
            b.appendAs( key.firstElement() , final ? "_id" : "0" );
            state->scope->append( b , final ? "value" : "1" , "return" );
            return b.obj();
        }
        
        MRSetup::MRSetup( const string& _dbname , const BSONObj& cmdObj , bool markAsTemp ){
            
            dbname = _dbname;
            ns = dbname + "." + cmdObj.firstElement().valuestr();
            
            verbose = cmdObj["verbose"].trueValue();
            keeptemp = cmdObj["keeptemp"].trueValue();
            
            { // setup names
                stringstream ss;
                if ( ! keeptemp )
                    ss << "tmp.";
                ss << "mr." << cmdObj.firstElement().String() << "_" << time(0) << "_" << JOB_NUMBER++;    
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
                
            if ( cmdObj["outType"].type() == String ){
                uassert( 13521 , "need 'out' if using 'outType'" , cmdObj["out"].type() == String );
                string t = cmdObj["outType"].String();
                if ( t == "normal" )
                    outType = NORMAL;
                else if ( t == "merge" )
                    outType = MERGE;
                else if ( t == "reduce" )
                    outType = REDUCE;
                else 
                    uasserted( 13522 , str::stream() << "unknown outType [" << t << "]" );
            }
            else {
                outType = NORMAL;
            }

            { // scope and code
                // NOTE: function scopes are merged with m/r scope, not nested like they should be
                BSONObjBuilder scopeBuilder;

                if ( cmdObj["scope"].type() == Object ){
                    scopeBuilder.appendElements( cmdObj["scope"].embeddedObjectUserCheck() );
                }

                mapCode = scopeAndCode( scopeBuilder, cmdObj["map"] );
                reduceCode = scopeAndCode( scopeBuilder, cmdObj["reduce"] );
                if ( cmdObj["finalize"].type() ){
                    finalizeCode = scopeAndCode( scopeBuilder, cmdObj["finalize"] );
                }
                    
                scopeSetup = scopeBuilder.obj();

                if ( cmdObj["mapparams"].type() == Array ){
                    mapparams = cmdObj["mapparams"].embeddedObjectUserCheck();
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

        string MRSetup::scopeAndCode (BSONObjBuilder& scopeBuilder, const BSONElement& field) {
            if ( field.type() == CodeWScope )
                scopeBuilder.appendElements( field.codeWScopeObject() );
            return field._asCode();
        }

        long long MRSetup::renameIfNeeded( DBDirectClient& db , MRReduceState * state ){
            assertInWriteLock();
            if ( finalLong != tempLong ){
                    
                if ( outType == NORMAL ){
                    db.dropCollection( finalLong );
                    if ( db.count( tempLong ) ){
                        BSONObj info;
                        uassert( 10076 ,  "rename failed" , 
                                 db.runCommand( "admin" , BSON( "renameCollection" << tempLong << "to" << finalLong ) , info ) );
                    }
                }
                else if ( outType == MERGE ){
                    auto_ptr<DBClientCursor> cursor = db.query( tempLong , BSONObj() );
                    while ( cursor->more() ){
                        BSONObj o = cursor->next();
                        Helpers::upsert( finalLong , o );
                    }
                    db.dropCollection( tempLong );
                }
                else if ( outType == REDUCE ){
                    BSONList values;
                    
                    auto_ptr<DBClientCursor> cursor = db.query( tempLong , BSONObj() );
                    while ( cursor->more() ){
                        BSONObj temp = cursor->next();
                        BSONObj old;

                        bool found;
                        {
                            Client::Context tx( finalLong );
                            found = Helpers::findOne( finalLong.c_str() , temp["_id"].wrap() , old , true );
                        }
                        
                        if ( found ){
                            // need to reduce
                            values.clear();
                            values.push_back( temp );
                            values.push_back( old );
                            Helpers::upsert( finalLong , reduceValues( values , state , true ) );
                        }
                        else {
                            Helpers::upsert( finalLong , temp );
                        }
                    }
                    db.dropCollection( tempLong );
                }
                else {
                    assert(0);
                }
            }
            return db.count( finalLong );
        }
        
        void MRSetup::insert( const string& ns , BSONObj& o ){
            writelock l( ns );
            Client::Context ctx( ns );
                
            if ( replicate )
                theDataFileMgr.insertAndLog( ns.c_str() , o , false );
            else
                theDataFileMgr.insertWithObjMod( ns.c_str() , o , false );

        }


        MRState::MRState( MRSetup& s ) 
            : setup(s){
        }

        void MRState::init(){
            scope.reset(globalScriptEngine->getPooledScope( setup.dbname ).release() );
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
        
        void MRState::finalReduce( BSONList& values ){
            if ( values.size() == 0 )
                return;
            
            BSONObj key = values.begin()->firstElement().wrap( "_id" );
            BSONObj res = reduceValues( values , this , true );
            
            setup.insert( setup.tempLong , res );
        }

        MRTL::MRTL( MRState& state ) 
            : _state( state )
            , _temp(new InMemory())
        {
            _size = 0;
            numEmits = 0;
        }
            
        void MRTL::reduceInMemory(){
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
                    BSONObj res = reduceValues( all , &_state , false );
                    insert( res );
                }
            }
        }
        
        void MRTL::dump(){
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
            
        void MRTL::insert( const BSONObj& a ){
            BSONList& all = (*_temp)[a];
            all.push_back( a );
            _size += a.objsize() + 16;
        }

        void MRTL::checkSize(){
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

        void MRTL::write( BSONObj& o ){
            theDataFileMgr.insertWithObjMod( _state.setup.incLong.c_str() , o , true );
        }

        boost::thread_specific_ptr<MRTL> _tlmr;

        BSONObj fast_emit( const BSONObj& args ){
            uassert( 10077 , "fast_emit takes 2 args" , args.nFields() == 2 );
            uassert( 13069 , "an emit can't be more than 2mb" , args.objsize() < ( BSONObjMaxUserSize / 2 ) );
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
                MRState state( mr );
                
                try {
                    state.init();

                    state.scope->injectNative( "emit" , fast_emit );
                    
                    MRTL * mrtl = new MRTL( state );
                    _tlmr.reset( mrtl );

                    ProgressMeterHolder pm( op->setMessage( "m/r: (1/3) emit phase" , db.count( mr.ns , mr.filter , 0 , mr.limit ) ) );
                    long long mapTime = 0;
                    {
                        readlock lock( mr.ns );
                        Client::Context ctx( mr.ns );
                        
                        shared_ptr<Cursor> temp = bestGuessCursor( mr.ns.c_str(), mr.filter, mr.sort );
                        auto_ptr<ClientCursor> cursor( new ClientCursor( QueryOption_NoCursorTimeout , temp , mr.ns.c_str() ) );

                        Timer mt;
                        while ( cursor->ok() ){

                            if ( cursor->currentIsDup() ){
                                cursor->advance();
                                continue;
                            }
                            
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
                    
                    { // create temp output collection
                        writelock lock( mr.tempLong.c_str() );
                        Client::Context ctx( mr.tempLong.c_str() );
                        assert( userCreateNS( mr.tempLong.c_str() , BSONObj() , errmsg , mr.replicate ) );
                    }
                    
                    { // copy indexes 
                        assert( db.count( mr.tempLong ) == 0 );
                        auto_ptr<DBClientCursor> idx = db.getIndexes( mr.finalLong );
                        while ( idx->more() ){
                            BSONObj i = idx->next();

                            BSONObjBuilder b( i.objsize() + 16 );
                            b.append( "ns" , mr.tempLong );
                            BSONObjIterator j( i );
                            while ( j.more() ){
                                BSONElement e = j.next();
                                if ( str::equals( e.fieldName() , "_id" ) || 
                                     str::equals( e.fieldName() , "ns" ) )
                                    continue;
                                
                                b.append( e );
                            }
                            
                            BSONObj indexToInsert = b.obj();
                            mr.insert( Namespace( mr.tempLong.c_str() ).getSisterNS( "system.indexes" ).c_str() , indexToInsert );
                        }
                        
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
                
                    finalCount = mr.renameIfNeeded( db , &state );
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
                
                MRReduceState state;

                DBDirectClient db;
                    
                { // reduce from each stream
                    
                    BSONObj sortKey = BSON( "_id" << 1 );
                    
                    ParallelSortClusteredCursor cursor( servers , dbname + "." + shardedOutputCollection ,
                                                        Query().sort( sortKey ) );
                    cursor.init();
                    
                    state.scope.reset( globalScriptEngine->getPooledScope( dbname ).release() );
                    state.scope->localConnect( dbname.c_str() );
                    state.reduce = state.scope->createFunction( mr.reduceCode.c_str() );
                    if ( mr.finalizeCode.size() )
                        state.finalize = state.scope->createFunction( mr.finalizeCode.c_str() );
                    
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
                        
                        
                        db.insert( mr.tempLong , reduceValues( values , &state , true ) );
                        values.clear();
                        values.push_back( t );
                    }
                    
                    if ( values.size() )
                        db.insert( mr.tempLong , reduceValues( values , &state , true ) );
                }
                

                long long finalCount = mr.renameIfNeeded( db , &state );
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

