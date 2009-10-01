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

#include "../stdafx.h"
#include "db.h"
#include "instance.h"
#include "commands.h"
#include "../scripting/engine.h"

namespace mongo {

    namespace mr {

        typedef pair<BSONObj,BSONObj> Data;
        typedef list< Data > InMemory;
        
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
                    reduceArgs.append( o["key"] );
                    BSONObjBuilder temp;
                    temp.append( o["key"] );
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
            b.append( key["key"] );
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
                list<BSONObj> all;
                
                _temp->sort( MyCmp() );

                InMemory * old = _temp;
                InMemory * n = new InMemory();
                _temp = n;
                _size = 0;
                
                for ( InMemory::iterator i=old->begin(); i!=old->end(); i++ ){
                    BSONObj key = i->first;
                    BSONObj value = i->second;
                    
                    if ( key.woCompare( prevKey ) == 0 ){
                        all.push_back( value );
                        continue;
                    }
                    
                    if ( all.size() == 1 ){
                        insert( prevKey , *(all.begin()) );
                        all.clear();
                    }
                    else if ( all.size() > 1 ){
                        BSONObj res = reduceValues( all , _scope , _reduce );
                        insert( prevKey , res );
                        all.clear();
                    }
                    prevKey = key.getOwned();
                    all.push_back( value );
                }

                if ( all.size() == 1 ){
                    insert( prevKey , *(all.begin()) );
                }
                else if ( all.size() > 1 ){
                    BSONObj res = reduceValues( all , _scope , _reduce );
                    insert( prevKey , res );
                }
                
                delete( old );
            }
        
            void dump(){
                for ( InMemory::iterator i=_temp->begin(); i!=_temp->end(); i++ ){
                    BSONObj key = i->first;
                    BSONObj value = i->second;
                    BSONObjBuilder b;
                    b.appendElements( value );
                
                    BSONObj o = b.obj();
                    _db->insert( _coll , o );
                }
                _temp->clear();
                _size = 0;
            }
            
            void insert( const BSONObj& key , const BSONObj& value ){
                _temp->push_back( pair<BSONObj,BSONObj>( key , value ) );
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
            
        private:
            DBDirectClient * _db;
            string _coll;
            Scope * _scope;
            ScriptingFunction _reduce;
        
            InMemory * _temp;
            
            long _size;
        };

        boost::thread_specific_ptr<MRTL> _tlmr;

        BSONObj fast_emit( const BSONObj& args ){
            BSONObj key,value;

            BSONObjIterator i( args );
        
            {
                assert( i.more() );
                BSONObjBuilder b;
                b.appendAs( i.next() , "key" );
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
    
        class MapReduceCommand : public Command {
        public:
            MapReduceCommand() : Command("mapreduce"){}
            virtual bool slaveOk() { return true; }
        
            virtual void help( stringstream &help ) const {
                help << "see http://www.mongodb.org/display/DOCS/MapReduce";
            }
        
            string tempCollectionName( string coll ){
                static int inc = 1;
                stringstream ss;
                ss << database->name << ".mr." << coll << "." << time(0) << "." << inc++;
                return ss.str();
            }

            void doReduce( const string& resultColl , list<BSONObj>& values , Scope * s , ScriptingFunction reduce ){
                if ( values.size() == 0 )
                    return;

                BSONObj res = reduceValues( values , s , reduce );
                db.insert( resultColl , res );
            }

            void finalReduce( const string& resultColl , list<BSONObj>& values , Scope * s , ScriptingFunction reduce ){
                if ( values.size() == 0 )
                    return;
                
                BSONObj key = values.begin()->extractFields( BSON( "key" << 1 ) );
                
                if ( values.size() == 1 ){
                    assert( db.count( resultColl , key  ) == 1 );
                    return;
                }

                db.remove( resultColl , key );
                doReduce( resultColl , values , s , reduce );
            }
        
            bool run(const char *dbname, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl ){
                Timer t;

                string ns = database->name + '.' + cmdObj.firstElement().valuestr();
                log(1) << "mr ns: " << ns << endl;
                
                if ( ! db.exists( ns ) ){
                    errmsg = "ns doesn't exist";
                    return false;
                }
                

                auto_ptr<Scope> s = globalScriptEngine->getPooledScope( ns );
                s->localConnect( database->name.c_str() );
                
                string resultColl = tempCollectionName( cmdObj.firstElement().valuestr() );
                string finalOutput = resultColl;
                if ( cmdObj["out"].type() == String )
                    finalOutput = database->name + "." + cmdObj["out"].valuestr();
                
                string resultCollShort = resultColl.substr( database->name.size() + 1 );
                string finalOutputShort = finalOutput.substr( database->name.size() + 1 );
                log(1) << "\t resultColl: " << resultColl << " short: " << resultCollShort << endl;
                db.dropCollection( resultColl );
                db.ensureIndex( resultColl , BSON( "key" << 1 ) );
            
                int num = 0;
            
                try {
                    dbtemprelease temprlease;

                    s->execSetup( (string)"tempcoll = db[\"" + resultCollShort + "\"];" , "tempcoll1" );
                    if ( s->type( "emit" ) == 6 ){
                        s->injectNative( "emit" , fast_emit );
                    }

                    ScriptingFunction mapFunction = s->createFunction( cmdObj["map"].ascode().c_str() );
                    ScriptingFunction reduceFunction = s->createFunction( cmdObj["reduce"].ascode().c_str() );
                
                    MRTL * mrtl = new MRTL( &db , resultColl , s.get() , reduceFunction );
                    _tlmr.reset( mrtl );

                    BSONObj q;
                    if ( cmdObj["query"].type() == Object )
                        q = cmdObj["query"].embeddedObjectUserCheck();

                    auto_ptr<DBClientCursor> cursor = db.query( ns , q );
                    while ( cursor->more() ){
                        BSONObj o = cursor->next(); 
                        s->setThis( &o );
                    
                        if ( s->invoke( mapFunction , BSONObj() , 0 , true ) )
                            throw UserException( (string)"map invoke failed: " + s->getError() );
                    
                        num++;
                        if ( num % 100 == 0 ){
                            mrtl->checkSize();
                        }
                    }

                    
                    result.append( "timeMillis.emit" , t.millis() );

                    // final reduce
                
                    mrtl->reduceInMemory();
                    mrtl->dump();

                    BSONObj prev;
                    list<BSONObj> all;
                    BSONObj sortKey = BSON( "key" << 1 );
                
                    cursor = db.query( resultColl, Query().sort( BSON( "key" << 1 ) ) );
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

                result.append( "result" , finalOutputShort );
                result.append( "numObjects" , num );
                result.append( "timeMillis" , t.millis() );
            
                return true;
            }

        private:
            DBDirectClient db;

        } mapReduceCommand;

    }

}

