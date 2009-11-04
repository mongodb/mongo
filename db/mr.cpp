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
        
        typedef pair<BSONObj,BSONObj> Data;
        //typedef list< Data > InMemory;
        typedef map< BSONObj,list<BSONObj>,BSONObjCmp > InMemory;
        typedef map< BSONObj,int,BSONObjCmp > KeyNums;

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
                    
                }
                
                { // query options
                    if ( cmdObj["query"].type() == Object ){
                        filter = cmdObj["query"].embeddedObjectUserCheck();
                        q = filter;
                    }
                    
                    if ( cmdObj["sort"].type() == Object )
                        q.sort( cmdObj["sort"].embeddedObjectUserCheck() );
                }
            }
            
            /**
               @return number objects in collection
             */
            long long renameIfNeeded( DBDirectClient& db ){
                if ( finalLong != tempLong ){
                    dblock l;
                    db.dropCollection( finalLong );
                    BSONObj info;
                    uassert( "rename failed" , db.runCommand( "admin" , BSON( "renameCollection" << tempLong << "to" << finalLong ) , info ) );
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

            // functions
            
            string mapCode;
            string reduceCode;
            string finalizeCode;

            // output tables
            string tempShort;
            string tempLong;
            
            string finalShort;
            string finalLong;

        };
        
        class MyCmp {
        public:
            MyCmp(){}
            bool operator()( const Data &l, const Data &r ) const {
                return l.first.woCompare( r.first ) < 0;
            }
        };
    
        BSONObj reduceValues( list<BSONObj>& values , Scope * s , ScriptingFunction reduce ){
            uassert( "need values" , values.size() );
            
            int sizeEstimate = ( values.size() * values.begin()->getField( "value" ).size() ) + 128;
            BSONObj key;

            BSONObjBuilder reduceArgs( sizeEstimate );
        
            BSONObjBuilder valueBuilder( sizeEstimate );
            int n = 0;
            for ( list<BSONObj>::iterator i=values.begin(); i!=values.end(); i++){
                BSONObj o = *i;
                if ( n == 0 ){
                    reduceArgs.append( o["_id"] );
                    BSONObjBuilder temp;
                    temp.append( o["_id"] );
                    key = temp.obj();
                }
                valueBuilder.appendAs( o["value"] , BSONObjBuilder::numStr( n++ ).c_str() );
            }
        
            reduceArgs.appendArray( "values" , valueBuilder.obj() );
            BSONObj args = reduceArgs.obj();
            
            s->invokeSafe( reduce , args );
            if ( s->type( "return" ) == Array ){
                uassert("reduce -> multiple not supported yet",0);                
                return BSONObj();
            }
            BSONObjBuilder b;
            b.append( key["_id"] );
            s->append( b , "value" , "return" );
            return b.obj();
        }
        

        class MRTL {
        public:
            MRTL( DBDirectClient * db , string coll , Scope * s , ScriptingFunction reduce ) : 
                _db( db ) , _coll( coll ) , _scope( s ) , _reduce( reduce ) , _size(0){
                _temp = new InMemory();
            }
            ~MRTL(){
                delete _temp;
            }
            
            void reduceInMemory(){
                BSONObj prevKey;
                
                InMemory * old = _temp;
                InMemory * n = new InMemory();
                _temp = n;
                _size = 0;
                
                for ( InMemory::iterator i=old->begin(); i!=old->end(); i++ ){
                    BSONObj key = i->first;
                    list<BSONObj>& all = i->second;
                    
                    if ( all.size() == 1 ){
                        insert( key , *(all.begin()) );
                    }
                    else if ( all.size() > 1 ){
                        BSONObj res = reduceValues( all , _scope , _reduce );
                        insert( key , res );
                    }
                }
                
                delete( old );
            }
        
            void dump(){
                for ( InMemory::iterator i=_temp->begin(); i!=_temp->end(); i++ ){
                    BSONObj key = i->first;
                    list<BSONObj>& all = i->second;
                    if ( all.size() < 1 )
                        continue;
                    assert( all.size() == 1 );
                    
                    BSONObj value = *(all.begin());

                    BSONObjBuilder b;
                    b.appendElements( value );
                
                    BSONObj o = b.obj();
                    _db->insert( _coll , o );
                }
                _temp->clear();
                _size = 0;
            }
            
            void insert( const BSONObj& key , const BSONObj& value ){
                list<BSONObj>& all = (*_temp)[key];
                all.push_back( value );
                _size += key.objsize() + value.objsize() + 32;
            }
            
            void checkSize(){
                if ( _size < 1024 * 10 )
                    return;

                long before = _size;
                reduceInMemory();
                log(1) << "  mr: did reduceInMemory  " << before << " -->> " << _size << endl;

                if ( _size < 1024 * 15 )
                    return;
                
                dump();
                log(1) << "  mr: dumping to db" << endl;
            }
            
            int getNum( const BSONObj& key ){
                KeyNums::iterator i = _nums.find( key );
                if ( i != _nums.end() )
                    return i->second;
                int n = _nums.size() + 1;
                _nums[key] = n;
                return n;
            }

            void resetNum(){
                _nums.clear();
            }

        private:
            DBDirectClient * _db;
            string _coll;
            Scope * _scope;
            ScriptingFunction _reduce;
        
            InMemory * _temp;
            
            long _size;

            map<BSONObj,int,BSONObjCmp> _nums;
        };

        boost::thread_specific_ptr<MRTL> _tlmr;

        BSONObj fast_emit( const BSONObj& args ){
            BSONObj key,value;

            BSONObjIterator i( args );
        
            {
                assert( i.more() );
                BSONObjBuilder b;
                b.appendAs( i.next() , "_id" );
                key = b.obj();
            }

            {
                assert( i.more() );
                BSONObjBuilder b;
                b.append( key.firstElement() );
                b.appendAs( i.next() , "value" );
                value = b.obj();
            }
            assert( ! i.more() );
            _tlmr->insert( key , value );
            return BSON( "x" << 1 );
        }

        BSONObj get_num( const BSONObj& args ){
            return BSON( "" << _tlmr->getNum( args ) );
        }
        
        BSONObj reset_num( const BSONObj& args ){
            _tlmr->resetNum();
            return BSONObj();
        }

        class MapReduceCommand : public Command {
        public:
            MapReduceCommand() : Command("mapreduce"){}
            virtual bool slaveOk() { return true; }
        
            virtual void help( stringstream &help ) const {
                help << "see http://www.mongodb.org/display/DOCS/MapReduce";
            }
        
            void doReduce( const string& resultColl , list<BSONObj>& values , Scope * s , ScriptingFunction reduce ){
                if ( values.size() == 0 )
                    return;

                BSONObj res = reduceValues( values , s , reduce );
                db.update( resultColl , res.extractFields( BSON( "_id" << 1 ) ) , res , true );
            }

            void finalReduce( const string& resultColl , list<BSONObj>& values , Scope * s , ScriptingFunction reduce ){
                if ( values.size() == 0 )
                    return;
                
                BSONObj key = values.begin()->extractFields( BSON( "_id" << 1 ) );
                
                if ( values.size() == 1 ){
                    assert( db.count( resultColl , key  ) == 1 );
                    return;
                }

                db.remove( resultColl , key );
                doReduce( resultColl , values , s , reduce );
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

                auto_ptr<Scope> s = globalScriptEngine->getPooledScope( mr.dbname );
                s->localConnect( mr.dbname.c_str() );
                
                db.dropCollection( mr.tempLong );
            
                long long num = 0;
                long long inReduce = 0;
                long long numEmits = 0;
                BSONObjBuilder countsBuilder;
                BSONObjBuilder timingBuilder;
                try {
                    dbtemprelease temprlease;
                    
                    s->execSetup( (string)"tempcoll = db[\"" + mr.tempShort + "\"]; db.getMongo().setSlaveOk();" , "tempcoll1" );
                    s->execSetup( "MR.init()" );

                    s->injectNative( "get_num" , get_num );
                    s->injectNative( "reset_num" , reset_num );
                    
                    ScriptingFunction mapFunction = s->createFunction( mr.mapCode.c_str() );
                    ScriptingFunction reduceFunction = s->createFunction( mr.reduceCode.c_str() );
                    s->execSetup( (string)"$reduce = " + mr.reduceCode );
                    
                    MRTL * mrtl = new MRTL( &db , mr.tempLong , s.get() , reduceFunction );
                    _tlmr.reset( mrtl );

                    ProgressMeter pm( db.count( mr.ns , mr.filter ) );
                    auto_ptr<DBClientCursor> cursor = db.query( mr.ns , mr.q );
                    long long mapTime = 0;
                    Timer mt;
                    while ( cursor->more() ){
                        BSONObj o = cursor->next(); 
                    
                        if ( mr.verbose ) mt.reset();
                        
                        s->setThis( &o );
                        if ( s->invoke( mapFunction , BSONObj() , 0 , true ) )
                            throw UserException( (string)"map invoke failed: " + s->getError() );
                        
                        if ( mr.verbose ) mapTime += mt.micros();
                    
                        num++;
                        if ( num % 100 == 0 ){
                            Timer t;
                            s->exec( "MR.check();" , "reduce-i" , false , true , true );
                            inReduce += t.micros();
                            //mrtl->checkSize();
                        }
                        pm.hit();
                    }
                    
                    
                    countsBuilder.append( "input" , num );
                    numEmits = (long long)(s->getNumber( "$numEmits" ));
                    countsBuilder.append( "emit" , numEmits );
                    
                    timingBuilder.append( "mapTime" , mapTime / 1000 );
                    timingBuilder.append( "emitLoop" , t.millis() );
                    
                    // final reduce
                    
                    mrtl->reduceInMemory();
                    mrtl->dump();
                                     
                    {
                        Timer t;
                        s->exec( "MR.doReduce(true)" , "reduce" , false , true , true );
                        inReduce += t.micros();
                        timingBuilder.append( "reduce" , inReduce / 1000 );
                    }
                    if ( mr.verbose ){
                        countsBuilder.append( "reduces" , s->getNumber( "$numReduces" ) );
                        countsBuilder.append( "reducesToDB" , s->getNumber( "$numReducesToDB" ) );
                    }
                    
                    if ( mr.finalizeCode.size() ){
                        s->execSetup( (string)"$finalize = " + mr.finalizeCode );
                        s->execSetup( "MR.finalize()" );
                    }

                    s->execSetup( "MR.cleanup()" );
                    _tlmr.reset( 0 );
                    /*
                    BSONObj prev;
                    list<BSONObj> all;
                    BSONObj sortKey = BSON( "_id" << 1 );
                
                    cursor = db.query( resultColl, Query().sort( sortKey ) );
                    while ( cursor->more() ){
                        BSONObj o = cursor->next().getOwned();

                        if ( o.woSortOrder( prev , sortKey ) == 0 ){
                            all.push_back( o );
                            continue;
                        }
                    
                        finalReduce( resultColl , all , s.get() , reduceFunction );

                        all.clear();
                        prev = o;
                        all.push_back( o );
                    }
                    
                    finalReduce( resultColl , all , s.get() , reduceFunction );
                    */
                }
                catch ( ... ){
                    log() << "mr failed, removing collection" << endl;
                    db.dropCollection( mr.tempLong );
                    throw;
                }
                
                
                long long finalCount = mr.renameIfNeeded( db );

                if ( finalCount == 0 && numEmits > 0 ){
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
                    
                    db.insert( mr.tempLong , reduceValues( values , s.get() , reduceFunction ) );
                    values.clear();
                    values.push_back( t );
                }
                
                if ( values.size() )
                    db.insert( mr.tempLong , reduceValues( values , s.get() , reduceFunction ) );
                
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

