// config.cpp

/**
*    Copyright (C) 2008 10gen Inc.
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
#include "../util/message.h"
#include "../util/unittest.h"
#include "../client/connpool.h"
#include "../client/model.h"
#include "../db/pdfile.h"
#include "../db/cmdline.h"

#include "server.h"
#include "config.h"
#include "chunk.h"

namespace mongo {

    int ConfigServer::VERSION = 2;

    /* --- DBConfig --- */

    string DBConfig::modelServer() {
        return configServer.modelServer();
    }
    
    bool DBConfig::isSharded( const string& ns ){
        if ( ! _shardingEnabled )
            return false;
        return _sharded.find( ns ) != _sharded.end();
    }

    string DBConfig::getShard( const string& ns ){
        if ( isSharded( ns ) )
            return "";
        
        uassert( "no primary!" , _primary.size() );
        return _primary;
    }
    
    void DBConfig::enableSharding(){
        _shardingEnabled = true; 
    }
    
    ChunkManager* DBConfig::shardCollection( const string& ns , ShardKeyPattern fieldsAndOrder , bool unique ){
        if ( ! _shardingEnabled )
            throw UserException( "db doesn't have sharding enabled" );
        
        ChunkManager * info = _shards[ns];
        if ( info )
            return info;
        
        if ( isSharded( ns ) )
            throw UserException( "already sharded" );

        _sharded[ns] = CollectionInfo( fieldsAndOrder , unique );

        info = new ChunkManager( this , ns , fieldsAndOrder , unique );
        _shards[ns] = info;
        return info;

    }

    bool DBConfig::removeSharding( const string& ns ){
        if ( ! _shardingEnabled ){
            cout << "AAAA" << endl;
            return false;
        }
        
        ChunkManager * info = _shards[ns];
        map<string,CollectionInfo>::iterator i = _sharded.find( ns );

        if ( info == 0 && i == _sharded.end() ){
            cout << "BBBB" << endl;
            return false;
        }
        uassert( "_sharded but no info" , info );
        uassert( "info but no sharded" , i != _sharded.end() );
        
        _sharded.erase( i );
        _shards.erase( ns ); // TODO: clean this up, maybe switch to shared_ptr
        return true;
    }

    ChunkManager* DBConfig::getChunkManager( const string& ns , bool reload ){
        ChunkManager* m = _shards[ns];
        if ( m && ! reload )
            return m;

        uassert( (string)"not sharded:" + ns , isSharded( ns ) );
        if ( m && reload )
            log() << "reloading shard info for: " << ns << endl;
        m = new ChunkManager( this , ns , _sharded[ ns ].key , _sharded[ns].unique );
        _shards[ns] = m;
        return m;
    }

    void DBConfig::serialize(BSONObjBuilder& to){
        to.append("name", _name);
        to.appendBool("partitioned", _shardingEnabled );
        to.append("primary", _primary );
        
        if ( _sharded.size() > 0 ){
            BSONObjBuilder a;
            for ( map<string,CollectionInfo>::reverse_iterator i=_sharded.rbegin(); i != _sharded.rend(); i++){
                BSONObjBuilder temp;
                temp.append( "key" , i->second.key.key() );
                temp.appendBool( "unique" , i->second.unique );
                a.append( i->first.c_str() , temp.obj() );
            }
            to.append( "sharded" , a.obj() );
        }
    }
    
    void DBConfig::unserialize(const BSONObj& from){
        _name = from.getStringField("name");
        _shardingEnabled = from.getBoolField("partitioned");
        _primary = from.getStringField("primary");
        
        _sharded.clear();
        BSONObj sharded = from.getObjectField( "sharded" );
        if ( ! sharded.isEmpty() ){
            BSONObjIterator i(sharded);
            while ( i.more() ){
                BSONElement e = i.next();
                uassert( "sharded things have to be objects" , e.type() == Object );
                BSONObj c = e.embeddedObject();
                uassert( "key has to be an object" , c["key"].type() == Object );
                _sharded[e.fieldName()] = CollectionInfo( c["key"].embeddedObject() , 
                                                          c["unique"].trueValue() );
            }
        }
    }
    
    void DBConfig::save( bool check ){
        Model::save( check );
        for ( map<string,ChunkManager*>::iterator i=_shards.begin(); i != _shards.end(); i++)
            i->second->save();
    }

    bool DBConfig::reload(){
        // TODO: i don't think is 100% correct
        return doload();
    }
    
    bool DBConfig::doload(){
        BSONObjBuilder b;
        b.append("name", _name.c_str());
        BSONObj q = b.done();
        return load(q);
    }

    bool DBConfig::dropDatabase( string& errmsg ){
        /**
         * 1) make sure everything is up
         * 2) update config server
         * 3) drop and reset sharded collections
         * 4) drop and reset primary
         * 5) drop everywhere to clean up loose ends
         */

        log(1) << "DBConfig::dropDatabase: " << _name << endl;
        
        // 1
        if ( ! configServer.allUp( errmsg ) ){
            log(1) << "\t DBConfig::dropDatabase not all up" << endl;
            return 0;
        }
        
        // 2
        grid.removeDB( _name );
        remove( true );
        if ( ! configServer.allUp( errmsg ) ){
            log() << "error removing from config server even after checking!" << endl;
            return 0;
        }
        log(1) << "\t removed entry from config server for: " << _name << endl;
        
        set<string> allServers;

        // 3
        while ( true ){
            int num;
            if ( ! _dropShardedCollections( num , allServers , errmsg ) )
                return 0;
            log() << "   DBConfig::dropDatabase: " << _name << " dropped sharded collections: " << num << endl;
            if ( num == 0 )
                break;
        }
        
        // 4
        {
            ScopedDbConnection conn( _primary );
            BSONObj res;
            if ( ! conn->dropDatabase( _name , &res ) ){
                errmsg = res.toString();
                return 0;
            }
            conn.done();
        }
        
        // 5
        for ( set<string>::iterator i=allServers.begin(); i!=allServers.end(); i++ ){
            string s = *i;
            ScopedDbConnection conn( s );
            BSONObj res;
            if ( ! conn->dropDatabase( _name , &res ) ){
                errmsg = res.toString();
                return 0;
            }
            conn.done();            
        }
        
        log(1) << "\t dropped primary db for: " << _name << endl;

        return true;
    }

    bool DBConfig::_dropShardedCollections( int& num, set<string>& allServers , string& errmsg ){
        num = 0;
        set<string> seen;
        while ( true ){
            map<string,ChunkManager*>::iterator i = _shards.begin();

            if ( i == _shards.end() )
                break;

            if ( seen.count( i->first ) ){
                errmsg = "seen a collection twice!";
                return false;
            }

            seen.insert( i->first );
            log(1) << "\t dropping sharded collection: " << i->first << endl;

            i->second->getAllServers( allServers );
            i->second->drop();
            
            num++;
            uassert( "_dropShardedCollections too many collections - bailing" , num < 100000 );
            log(2) << "\t\t dropped " << num << " so far" << endl;
        }
        return true;
    }
    
    /* --- Grid --- */
    
    string Grid::pickShardForNewDB(){
        ScopedDbConnection conn( configServer.getPrimary() );
        
        // TODO: this is temporary
        
        vector<string> all;
        auto_ptr<DBClientCursor> c = conn->query( "config.shards" , Query() );
        while ( c->more() ){
            BSONObj s = c->next();
            all.push_back( s["host"].valuestrsafe() );
            // look at s["maxSize"] if exists
        }
        conn.done();
        
        if ( all.size() == 0 )
            return "";
        
        return all[ rand() % all.size() ];
    }

    bool Grid::knowAboutShard( string name ) const{
        ScopedDbConnection conn( configServer.getPrimary() );
        BSONObj shard = conn->findOne( "config.shards" , BSON( "host" << name ) );
        conn.done();
        return ! shard.isEmpty();
    }

    DBConfig* Grid::getDBConfig( string database , bool create ){
        {
            string::size_type i = database.find( "." );
            if ( i != string::npos )
                database = database.substr( 0 , i );
        }
        
        if ( database == "config" )
            return &configServer;

        boostlock l( _lock );

        DBConfig*& cc = _databases[database];
        if ( cc == 0 ){
            cc = new DBConfig( database );
            if ( ! cc->doload() ){
                if ( create ){
                    // note here that cc->primary == 0.
                    log() << "couldn't find database [" << database << "] in config db" << endl;
                    
                    if ( database == "admin" )
                        cc->_primary = configServer.getPrimary();
                    else
                        cc->_primary = pickShardForNewDB();
                    
                    if ( cc->_primary.size() ){
                        cc->save();
                        log() << "\t put [" << database << "] on: " << cc->_primary << endl;
                    }
                    else {
                        log() << "\t can't find a shard to put new db on" << endl;
                        uassert( "can't find a shard to put new db on" , 0 );
                    }
                }
                else {
                    cc = 0;
                }
            }
            
        }
        
        return cc;
    }

    void Grid::removeDB( string database ){
        uassert( "removeDB expects db name" , database.find( '.' ) == string::npos );
        boostlock l( _lock );
        _databases.erase( database );
        
    }

    unsigned long long Grid::getNextOpTime() const {
        ScopedDbConnection conn( configServer.getPrimary() );
        
        BSONObj result;
        massert( "getoptime failed" , conn->simpleCommand( "admin" , &result , "getoptime" ) );
        conn.done();

        return result["optime"].date();
    }

    /* --- ConfigServer ---- */

    ConfigServer::ConfigServer() {
        _shardingEnabled = false;
        _primary = "";
        _name = "grid";
    }
    
    ConfigServer::~ConfigServer() {
    }

    bool ConfigServer::init( vector<string> configHosts ){
        uassert( "need configdbs" , configHosts.size() );

        string hn = getHostName();
        if ( hn.empty() ) {
            sleepsecs(5);
            dbexit( EXIT_BADOPTIONS );
        }
        ourHostname = hn;
        
        set<string> hosts;
        for ( size_t i=0; i<configHosts.size(); i++ ){
            string host = configHosts[i];
            hosts.insert( getHost( host , false ) );
            configHosts[i] = getHost( host , true );
        }

        for ( set<string>::iterator i=hosts.begin(); i!=hosts.end(); i++ ){
            string host = *i;
            bool ok = false;
            for ( int x=0; x<10; x++ ){
                if ( ! hostbyname( host.c_str() ).empty() ){
                    ok = true;
                    break;
                }
                log() << "can't resolve DNS for [" << host << "]  sleeping and trying " << (10-x) << " more times" << endl;
                sleepsecs( 10 );
            }
            if ( ! ok )
                return false;
        }
        
        uassert( "can only hand 1 config db right now" , configHosts.size() == 1 );
        _primary = configHosts[0];
        
        return true;
    }

    bool ConfigServer::allUp(){
        string errmsg;
        return allUp( errmsg );
    }
    
    bool ConfigServer::allUp( string& errmsg ){
        try {
            ScopedDbConnection conn( _primary );
            conn->getLastError();
            conn.done();
            return true;
        }
        catch ( DBException& ){
            log() << "ConfigServer::allUp : " << _primary << " seems down!" << endl;
            errmsg = _primary + " seems down";
            return false;
        }
        
    }
    
    int ConfigServer::dbConfigVersion(){
        ScopedDbConnection conn( _primary );
        int version = dbConfigVersion( conn.conn() );
        conn.done();
        return version;
    }
    
    int ConfigServer::dbConfigVersion( DBClientBase& conn ){
        auto_ptr<DBClientCursor> c = conn.query( "config.version" , BSONObj() );
        int version = 0;
        if ( c->more() ){
            BSONObj o = c->next();
            version = o["version"].numberInt();
            uassert( "should only have 1 thing in config.version" , ! c->more() );
        }
        else {
            if ( conn.count( "config.shard" ) || conn.count( "config.databases" ) ){
                version = 1;
            }
        }
        
        return version;
    }
    
    int ConfigServer::checkConfigVersion(){
        int cur = dbConfigVersion();
        if ( cur == VERSION )
            return 0;
        
        if ( cur == 0 ){
            ScopedDbConnection conn( _primary );
            conn->insert( "config.version" , BSON( "version" << VERSION ) );
            pool.flush();
            assert( VERSION == dbConfigVersion( conn.conn() ) );
            conn.done();
            return 0;
        }

        log() << "don't know how to upgrade " << cur << " to " << VERSION << endl;
        return -8;
    }

    string ConfigServer::getHost( string name , bool withPort ){
        if ( name.find( ":" ) ){
            if ( withPort )
                return name;
            return name.substr( 0 , name.find( ":" ) );
        }

        if ( withPort ){
            stringstream ss;
            ss << name << ":" << CmdLine::ConfigServerPort;
            return ss.str();
        }
        
        return name;
    }

    ConfigServer configServer;    
    Grid grid;


    class DBConfigUnitTest : public UnitTest {
    public:
        void testInOut( DBConfig& c , BSONObj o ){
            c.unserialize( o );
            BSONObjBuilder b;
            c.serialize( b );

            BSONObj out = b.obj();
            
            if ( o.toString() == out.toString() )
                return;
            
            log() << "DBConfig serialization broken\n" 
                  << "in  : " << o.toString()  << "\n"
                  << "out : " << out.toString() 
                  << endl;
            assert(0);
        }

        void a(){
            BSONObjBuilder b;
            b << "name" << "abc";
            b.appendBool( "partitioned" , true );
            b << "primary" << "myserver";
            
            DBConfig c;
            testInOut( c , b.obj() );
        }

        void b(){
            BSONObjBuilder b;
            b << "name" << "abc";
            b.appendBool( "partitioned" , true );
            b << "primary" << "myserver";
            
            BSONObjBuilder a;
            a << "abc.foo" << fromjson( "{ 'key' : { 'a' : 1 } , 'unique' : false }" );
            a << "abc.bar" << fromjson( "{ 'key' : { 'kb' : -1 } , 'unique' : true }" );
            
            b.appendArray( "sharded" , a.obj() );

            DBConfig c;
            testInOut( c , b.obj() );
            assert( c.isSharded( "abc.foo" ) );
            assert( ! c.isSharded( "abc.food" ) );
        }
        
        void run(){
            a();
            b();
        }
        
    } dbConfigUnitTest;
} 
