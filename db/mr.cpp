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

namespace mongo {

    namespace mr {
        
        typedef pair<BSONObj,BSONObj> Data;
        //typedef list< Data > InMemory;
        typedef map< BSONObj,list<BSONObj>,BSONObjCmp > InMemory;
        typedef map< BSONObj,int,BSONObjCmp > KeyNums;

        
        class MyCmp {
        public:
            MyCmp(){}
            bool operator()( const Data &l, const Data &r ) const {
                return l.first.woCompare( r.first ) < 0;
            }
        };
    
        BSONObj reduceValues( list<BSONObj>& values , Scope * s , ScriptingFunction reduce ){
            uassert( "need values" , values.size() );
        
            BSONObj key;

            BSONObjBuilder reduceArgs;
        
            BSONObjBuilder valueBuilder;
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
        
            string tempCollectionName( string coll , bool tmp ){
                static int inc = 1;
                stringstream ss;
                ss << cc().database()->name << ".";
                if ( tmp )
                    ss << "tmp.";
                ss << "mr." << coll << "_" << time(0) << "_" << inc++;
                return ss.str();
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
        
            bool run(const char *dbname, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl ){
                Timer t;
                
                bool verboseOutput = cmdObj["verbose"].trueValue();

                string ns = cc().database()->name + '.' + cmdObj.firstElement().valuestr();
                log(1) << "mr ns: " << ns << endl;
                
                if ( ! db.exists( ns ) ){
                    errmsg = "ns doesn't exist";
                    return false;
                }
                

                auto_ptr<Scope> s = globalScriptEngine->getPooledScope( ns );
                s->localConnect( cc().database()->name.c_str() );
                
                bool istemp = ! cmdObj["keeptemp"].trueValue();
                string resultColl = tempCollectionName( cmdObj.firstElement().valuestr() , istemp );
                if ( istemp )
                    currentClient->addTempCollection( resultColl );
                string finalOutput = resultColl;
                if ( cmdObj["out"].type() == String )
                    finalOutput = cc().database()->name + "." + cmdObj["out"].valuestr();
                
                string resultCollShort = resultColl.substr( cc().database()->name.size() + 1 );
                string finalOutputShort = finalOutput.substr( cc().database()->name.size() + 1 );
                log(1) << "\t resultColl: " << resultColl << " short: " << resultCollShort << endl;
                db.dropCollection( resultColl );
            
                long long num = 0;
                long long inReduce = 0;
                BSONObjBuilder countsBuilder;
                BSONObjBuilder timingBuilder;
                try {
                    dbtemprelease temprlease;
                    
                    s->execSetup( (string)"tempcoll = db[\"" + resultCollShort + "\"];" , "tempcoll1" );
                    s->execSetup( "MR.init()" );

                    s->injectNative( "get_num" , get_num );
                    s->injectNative( "reset_num" , reset_num );
                    
                    ScriptingFunction mapFunction = s->createFunction( cmdObj["map"].ascode().c_str() );
                    ScriptingFunction reduceFunction = s->createFunction( cmdObj["reduce"].ascode().c_str() );
                    s->execSetup( (string)"$reduce = " + cmdObj["reduce"].ascode() );
                    
                    MRTL * mrtl = new MRTL( &db , resultColl , s.get() , reduceFunction );
                    _tlmr.reset( mrtl );

                    Query q;
                    BSONObj filter;
                    if ( cmdObj["query"].type() == Object ){
                        filter = cmdObj["query"].embeddedObjectUserCheck();
                        q = filter;
                    }
                    
                    if ( cmdObj["sort"].type() == Object )
                        q.sort( cmdObj["sort"].embeddedObjectUserCheck() );

                    ProgressMeter pm( db.count( ns , filter ) );
                    auto_ptr<DBClientCursor> cursor = db.query( ns , q );
                    long long mapTime = 0;
                    Timer mt;
                    while ( cursor->more() ){
                        BSONObj o = cursor->next(); 
                    
                        if ( verboseOutput ) mt.reset();
                        
                        s->setThis( &o );
                        if ( s->invoke( mapFunction , BSONObj() , 0 , true ) )
                            throw UserException( (string)"map invoke failed: " + s->getError() );
                        
                        if ( verboseOutput ) mapTime += mt.micros();
                    
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
                    countsBuilder.append( "emit" , s->getNumber( "$numEmits" ) );
                    
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
                    if ( verboseOutput ){
                        countsBuilder.append( "reduces" , s->getNumber( "$numReduces" ) );
                        countsBuilder.append( "reducesToDB" , s->getNumber( "$numReducesToDB" ) );
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
                    db.dropCollection( resultColl );
                    throw;
                }
                
                
                if ( finalOutput != resultColl ){
                    // need to do this with the full dblock, that's why its after the try/catch
                    db.dropCollection( finalOutput );
                    BSONObj info;
                    uassert( "rename failed" , db.runCommand( "admin" , BSON( "renameCollection" << resultColl << "to" << finalOutput ) , info ) );
                }

                timingBuilder.append( "total" , t.millis() );
                
                result.append( "result" , finalOutputShort );
                result.append( "timeMillis" , t.millis() );
                countsBuilder.append( "output" , (long long)(db.count( finalOutput )) );
                if ( verboseOutput ) result.append( "timing" , timingBuilder.obj() );
                result.append( "counts" , countsBuilder.obj() );
                
                return true;
            }

        private:
            DBDirectClient db;

        } mapReduceCommand;

    }

}

