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
            }
            else {
                BSONObjBuilder b;
                b.append( key["key"] );
                s->append( b , "value" , "return" );
                db.insert( resultColl , b.obj() );
            }
            
            
        }

        void finalReduce( const string& resultColl , list<BSONObj>& values , Scope * s , ScriptingFunction reduce ){
            if ( values.size() == 0 )
                return;
            db.remove( resultColl , values.begin()->extractFields( BSON( "key" << 1 ) ) );
            doReduce( resultColl , values , s , reduce );
        }
        
        bool run(const char *dbname, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl ){
            Timer t;

            string ns = database->name + '.' + cmdObj.firstElement().valuestr();
            log(1) << "mr ns: " << ns << endl;
            
            auto_ptr<Scope> s = globalScriptEngine->getPooledScope( ns );
            s->localConnect( database->name.c_str() );
            
            string resultColl = tempCollectionName( cmdObj.firstElement().valuestr() );
            string resultCollShort = resultColl.substr( database->name.size() + 1 );
            log(1) << "\t resultColl: " << resultColl << " short: " << resultCollShort << endl;
            db.dropCollection( resultColl );
            db.ensureIndex( resultColl , BSON( "key" << 1 ) );
            
            int num = 0;
            
            try {
                s->execSetup( (string)"tempcoll = db[\"" + resultCollShort + "\"];" , "tempcoll1" );
                
                s->execSetup( "emit = function( k , v ){"
                              "  $lastKey = k;"
                              "  tempcoll.insert( { key : k , value : v } );"
                              "}" , "emit1" );
                
                ScriptingFunction mapFunction = s->createFunction( cmdObj["map"].ascode().c_str() );
                ScriptingFunction reduceFunction = s->createFunction( cmdObj["reduce"].ascode().c_str() );
                s->execSetup( "$reduce = " + cmdObj["reduce"].ascode() , "mp reduce setup" );
                
                BSONObj q;
                
                auto_ptr<DBClientCursor> cursor = db.query( ns , q );
                while ( cursor->more() ){
                    BSONObj o = cursor->next(); 
                    s->setThis( &o );
                    
                    if ( s->invoke( mapFunction , BSONObj() , 0 , true ) )
                        throw UserException( (string)"map invoke failed: " + s->getError() );
                    
                    num++;
                    if ( num % 100 == 0 ){
                        //assert( 0 );
                    }
                }


                result.append( "timeMillis.emit" , t.millis() );

                // final reduce
                
                BSONObj prev;
                list<BSONObj> all;
                BSONObj sortKey = BSON( "key" << 1 );
                
                cursor = db.query( resultColl, Query().sort( BSON( "key" << 1 ) ) );
                while ( cursor->more() ){
                    BSONObj o = cursor->next();

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
            
            result.append( "result" , resultCollShort );
            result.append( "numObjects" , num );
            result.append( "timeMillis" , t.millis() );
            
            return false;
        }

    private:
        DBDirectClient db;

    } mapReduceCommand;

}

