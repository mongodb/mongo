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

#include "stdafx.h"
#include "db.h"
#include "instance.h"
#include "commands.h"
#include "../scripting/engine.h"
#include "../client/dbclient.h"
#include "../client/connpool.h"
#include "../client/parallel.h"

namespace mongo {

    namespace mr {

        class MyCmp {
        public:
            MyCmp(){}
            bool operator()( const BSONObj &l, const BSONObj &r ) const {
                return l.firstElement().woCompare( r.firstElement() ) < 0;
            }
        };

        typedef pair<BSONObj,BSONObj> Data;
        //typedef list< Data > InMemory;
        typedef map< BSONObj,list<BSONObj>,MyCmp > InMemory;

        BSONObj reduceValues( list<BSONObj>& values , Scope * s , ScriptingFunction reduce , bool final , ScriptingFunction finalize ){
            uassert( "need values" , values.size() );
            
            int sizeEstimate = ( values.size() * values.begin()->getField( "value" ).size() ) + 128;
            BSONObj key;

            BSONObjBuilder reduceArgs( sizeEstimate );
        
            BSONObjBuilder valueBuilder( sizeEstimate );
            int n = 0;
            for ( list<BSONObj>::iterator i=values.begin(); i!=values.end(); i++){
                BSONObj o = *i;
                BSONObjIterator j(o);
                BSONElement keyE = j.next();
                if ( n == 0 ){
                    reduceArgs.append( keyE );
                    BSONObjBuilder temp;
                    temp.append( keyE );
                    key = temp.obj();
                }
                valueBuilder.appendAs( j.next() , BSONObjBuilder::numStr( n++ ).c_str() );
            }
        
            reduceArgs.appendArray( "values" , valueBuilder.obj() );
            BSONObj args = reduceArgs.obj();
            
            s->invokeSafe( reduce , args );
            if ( s->type( "return" ) == Array ){
                uassert("reduce -> multiple not supported yet",0);                
                return BSONObj();
            }
            
            if ( finalize ){
                BSONObjBuilder b;
                b.appendAs( key.firstElement() , "_id" );
                s->append( b , "value" , "return" );
                s->invokeSafe( finalize , b.obj() );
            }
            
            BSONObjBuilder b;
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

                    if ( cmdObj["out"].type() == String )
                        finalShort = cmdObj["out"].valuestr();
                    else
                        finalShort = tempShort;
                    
                    finalLong = dbname + "." + finalShort;
                    
                }
             
                { // code
                    mapCode = cmdObj["map"].ascode();
                    reduceCode = cmdObj["reduce"].ascode();
                    if ( cmdObj["finalize"].type() ){
                        finalizeCode = cmdObj["finalize"].ascode();
                    }
                    

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
                        q = filter;
                    }
                    
                    if ( cmdObj["sort"].type() == Object )
                        q.sort( cmdObj["sort"].embeddedObjectUserCheck() );

                    if ( cmdObj["limit"].isNumber() )
                        limit = cmdObj["limit"].numberLong();
                    else 
                        limit = 0;
                }
            }
            
            /**
               @return number objects in collection
             */
            long long renameIfNeeded( DBDirectClient& db ){
                if ( finalLong != tempLong ){
                    db.dropCollection( finalLong );
                    if ( db.count( tempLong ) ){
                        BSONObj info;
                        uassert( "rename failed" , db.runCommand( "admin" , BSON( "renameCollection" << tempLong << "to" << finalLong ) , info ) );
                    }
                }
                return db.count( finalLong );
            }
                
            string dbname;
            string ns;
            
            // options
            bool verbose;            
            bool keeptemp;

            // query options
            
            BSONObj filter;
            Query q;
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
                    throw UserException( (string)"map compile failed: " + scope->getError() );

                reduce = scope->createFunction( setup.reduceCode.c_str() );
                if ( ! reduce )
                    throw UserException( (string)"reduce compile failed: " + scope->getError() );

                if ( setup.finalizeCode.size() )
                    finalize  = scope->createFunction( setup.finalizeCode.c_str() );
                else
                    finalize = 0;
                
                if ( ! setup.scopeSetup.isEmpty() )
                    scope->init( &setup.scopeSetup );

                db.dropCollection( setup.tempLong );
                db.dropCollection( setup.incLong );
                
                writelock l( setup.incLong );
                string err;
                assert( userCreateNS( setup.incLong.c_str() , BSON( "autoIndexId" << 0 ) , err , false ) );

            }

            void finalReduce( list<BSONObj>& values ){
                if ( values.size() == 0 )
                    return;

                BSONObj key = values.begin()->firstElement().wrap( "_id" );
                BSONObj res = reduceValues( values , scope.get() , reduce , 1 , finalize );
                
                writelock l( setup.tempLong );
                theDataFileMgr.insertAndLog( setup.tempLong.c_str() , res , false );
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
            MRTL( MRState& state ) : _state( state ){
                _temp = new InMemory();
                _size = 0;
                numEmits = 0;
            }
            ~MRTL(){
                delete _temp;
            }
            
            
            void reduceInMemory(){
                
                InMemory * old = _temp;
                InMemory * n = new InMemory();
                _temp = n;
                _size = 0;
                
                for ( InMemory::iterator i=old->begin(); i!=old->end(); i++ ){
                    BSONObj key = i->first;
                    list<BSONObj>& all = i->second;
                    
                    if ( all.size() == 1 ){
                        // this key has low cardinality, so just write to db
                        writelock l(_state.setup.incLong);
                        write( *(all.begin()) );
                    }
                    else if ( all.size() > 1 ){
                        BSONObj res = reduceValues( all , _state.scope.get() , _state.reduce , false , 0 );
                        insert( res );
                    }
                }
                
                delete( old );

            }

            void dump(){
                writelock l(_state.setup.incLong);
                    
                for ( InMemory::iterator i=_temp->begin(); i!=_temp->end(); i++ ){
                    list<BSONObj>& all = i->second;
                    if ( all.size() < 1 )
                        continue;
                    
                    for ( list<BSONObj>::iterator j=all.begin(); j!=all.end(); j++ )
                        write( *j );
                }
                _temp->clear();
                _size = 0;

            }
            
            void insert( const BSONObj& a ){
                list<BSONObj>& all = (*_temp)[a];
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
                theDataFileMgr.insert( _state.setup.incLong.c_str() , o , true );
            }
            
            MRState& _state;
        
            InMemory * _temp;
            long _size;
            
        public:
            long long numEmits;
        };

        boost::thread_specific_ptr<MRTL> _tlmr;

        BSONObj fast_emit( const BSONObj& args ){
            uassert( "fast_emit takes 2 args" , args.nFields() == 2 );
            _tlmr->insert( args );
            _tlmr->numEmits++;
            return BSONObj();
        }

        class MapReduceCommand : public Command {
        public:
            MapReduceCommand() : Command("mapreduce"){}
            virtual bool slaveOk() { return true; }
        
            virtual void help( stringstream &help ) const {
                help << "see http://www.mongodb.org/display/DOCS/MapReduce";
            }
        
            bool run(const char *dbname, BSONObj& cmd, string& errmsg, BSONObjBuilder& result, bool fromRepl ){
                Timer t;
                Client::GodScope cg;
                MRSetup mr( cc().database()->name , cmd );

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
                    dbtemprelease temprlease;
                    
                    MRState state( mr );
                    state.scope->injectNative( "emit" , fast_emit );
                    
                    MRTL * mrtl = new MRTL( state );
                    _tlmr.reset( mrtl );

                    ProgressMeter pm( db.count( mr.ns , mr.filter ) );
                    auto_ptr<DBClientCursor> cursor = db.query( mr.ns , mr.q );
                    long long mapTime = 0;
                    Timer mt;
                    while ( cursor->more() ){
                        BSONObj o = cursor->next(); 
                    
                        if ( mr.verbose ) mt.reset();
                        
                        state.scope->setThis( &o );
                        if ( state.scope->invoke( state.map , state.setup.mapparams , 0 , true ) )
                            throw UserException( (string)"map invoke failed: " + state.scope->getError() );
                        
                        if ( mr.verbose ) mapTime += mt.micros();
                    
                        num++;
                        if ( num % 100 == 0 ){
                            Timer t;
                            mrtl->checkSize();
                            inReduce += t.micros();
                        }
                        pm.hit();

                        if ( mr.limit && num >= mr.limit )
                            break;
                    }
                    
                    
                    countsBuilder.append( "input" , num );
                    countsBuilder.append( "emit" , mrtl->numEmits );
                    if ( mrtl->numEmits )
                        shouldHaveData = true;
                    
                    timingBuilder.append( "mapTime" , mapTime / 1000 );
                    timingBuilder.append( "emitLoop" , t.millis() );
                    
                    // final reduce
                    
                    mrtl->reduceInMemory();
                    mrtl->dump();
                    
                    BSONObj sortKey = BSON( "0" << 1 );
                    db.ensureIndex( mr.incLong , sortKey );
                    
                    BSONObj prev;
                    list<BSONObj> all;
                    
                    ProgressMeter fpm( db.count( mr.incLong ) );
                    cursor = db.query( mr.incLong, Query().sort( sortKey ) );
                    while ( cursor->more() ){
                        BSONObj o = cursor->next().getOwned();
                        
                        if ( o.woSortOrder( prev , sortKey ) == 0 ){
                            all.push_back( o );
                            continue;
                        }
                        
                        state.finalReduce( all );
                        
                        all.clear();
                        prev = o;
                        all.push_back( o );
                        fpm.hit();
                    }
                    
                    state.finalReduce( all );

                    _tlmr.reset( 0 );
                }
                catch ( ... ){
                    log() << "mr failed, removing collection" << endl;
                    db.dropCollection( mr.tempLong );
                    db.dropCollection( mr.incLong );
                    throw;
                }
                
                db.dropCollection( mr.incLong );
                
                long long finalCount = mr.renameIfNeeded( db );

                if ( finalCount == 0 && shouldHaveData ){
                    errmsg = "there were emits but no data!";
                    return false;
                }

                timingBuilder.append( "total" , t.millis() );
                
                result.append( "result" , mr.finalShort );
                result.append( "timeMillis" , t.millis() );
                countsBuilder.append( "output" , finalCount );
                if ( mr.verbose ) result.append( "timing" , timingBuilder.obj() );
                result.append( "counts" , countsBuilder.obj() );
                
                return true;
            }

        private:
            DBDirectClient db;

        } mapReduceCommand;
        
        class MapReduceFinishCommand : public Command {
        public:
            MapReduceFinishCommand() : Command( "mapreduce.shardedfinish" ){}
            virtual bool slaveOk() { return true; }

            bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
                dbtemprelease temprlease; // we don't touch the db directly
                                    
                string dbname = cc().database()->name;
                string shardedOutputCollection = cmdObj["shardedOutputCollection"].valuestrsafe();

                MRSetup mr( dbname , cmdObj.firstElement().embeddedObjectUserCheck() , false );
                
                set<ServerAndQuery> servers;
                
                BSONObjBuilder shardCounts;
                map<string,long long> counts;
                
                BSONObj shards = cmdObj["shards"].embeddedObjectUserCheck();
                vector< auto_ptr<DBClientCursor> > shardCursors;
                BSONObjIterator i( shards );
                while ( i.more() ){
                    BSONElement e = i.next();
                    string shard = e.fieldName();

                    BSONObj res = e.embeddedObjectUserCheck();
                    
                    uassert( "something bad happened" , shardedOutputCollection == res["result"].valuestrsafe() );
                    servers.insert( shard );
                    shardCounts.appendAs( res["counts"] , shard.c_str() );

                    BSONObjIterator j( res["counts"].embeddedObjectUserCheck() );
                    while ( j.more() ){
                        BSONElement temp = j.next();
                        counts[temp.fieldName()] += temp.numberLong();
                    }

                }

                BSONObj sortKey = BSON( "_id" << 1 );

                ParallelSortClusteredCursor cursor( servers , dbname + "." + shardedOutputCollection ,
                                                    Query().sort( sortKey ) );
                
                
                auto_ptr<Scope> s = globalScriptEngine->getPooledScope( ns );
                ScriptingFunction reduceFunction = s->createFunction( mr.reduceCode.c_str() );
                ScriptingFunction finalizeFunction = 0;
                if ( mr.finalizeCode.size() )
                    finalizeFunction = s->createFunction( mr.finalizeCode.c_str() );

                list<BSONObj> values;

                result.append( "result" , mr.finalShort );

                DBDirectClient db;
                
                while ( cursor.more() ){
                    BSONObj t = cursor.next();
                                        
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
                
                long long finalCount = mr.renameIfNeeded( db );
                log(0) << " mapreducefinishcommand " << mr.finalLong << " " << finalCount << endl;

                for ( set<ServerAndQuery>::iterator i=servers.begin(); i!=servers.end(); i++ ){
                    ScopedDbConnection conn( i->_server );
                    conn->dropCollection( dbname + "." + shardedOutputCollection );
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

