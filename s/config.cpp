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

#include "pch.h"
#include "../util/message.h"
#include "../util/stringutils.h"
#include "../util/unittest.h"
#include "../client/connpool.h"
#include "../client/model.h"
#include "../db/pdfile.h"
#include "../db/cmdline.h"

#include "server.h"
#include "config.h"
#include "chunk.h"

namespace mongo {

    int ConfigServer::VERSION = 3;
    Shard Shard::EMPTY;
    
    string ShardNS::database = "config.databases";
    string ShardNS::chunk = "config.chunks";
    string ShardNS::shard = "config.shards";
    string ShardNS::mongos = "config.mongos";
    string ShardNS::settings = "config.settings";

    BSONField<bool>      ShardFields::draining("draining");
    BSONField<long long> ShardFields::maxSize ("maxSize");
    BSONField<long long> ShardFields::currSize("currSize");

    OID serverID;

    /* --- DBConfig --- */

    bool DBConfig::isSharded( const string& ns ){
        if ( ! _shardingEnabled )
            return false;
        scoped_lock lk( _lock );
        return _isSharded( ns );
    }

    bool DBConfig::_isSharded( const string& ns ){
        if ( ! _shardingEnabled )
            return false;
        return _sharded.find( ns ) != _sharded.end();
    }


    const Shard& DBConfig::getShard( const string& ns ){
        if ( isSharded( ns ) )
            return Shard::EMPTY;
        
        uassert( 10178 ,  "no primary!" , _primary.ok() );
        return _primary;
    }
    
    void DBConfig::enableSharding(){
        _shardingEnabled = true; 
    }
    
    ChunkManagerPtr DBConfig::shardCollection( const string& ns , ShardKeyPattern fieldsAndOrder , bool unique ){
        if ( ! _shardingEnabled )
            throw UserException( 8042 , "db doesn't have sharding enabled" );
        
        scoped_lock lk( _lock );

        ChunkManagerPtr info = _shards[ns];
        if ( info )
            return info;
        
        if ( _isSharded( ns ) )
            throw UserException( 8043 , "already sharded" );

        log() << "enable sharding on: " << ns << " with shard key: " << fieldsAndOrder << endl;
        _sharded[ns] = CollectionInfo( fieldsAndOrder , unique );

        info.reset( new ChunkManager( this , ns , fieldsAndOrder , unique ) );
        info->maybeChunkCollection();
        _shards[ns] = info;
        return info;

    }

    bool DBConfig::removeSharding( const string& ns ){
        if ( ! _shardingEnabled ){
            return false;
        }
        
        scoped_lock lk( _lock );

        ChunkManagerPtr info = _shards[ns];
        map<string,CollectionInfo>::iterator i = _sharded.find( ns );

        if ( info == 0 && i == _sharded.end() ){
            return false;
        }
        uassert( 10179 ,  "_sharded but no info" , info );
        uassert( 10180 ,  "info but no sharded" , i != _sharded.end() );
        
        _sharded.erase( i );
        _shards.erase( ns );
        return true;
    }

    ChunkManagerPtr DBConfig::getChunkManager( const string& ns , bool shouldReload ){
        scoped_lock lk( _lock );
        
        ChunkManagerPtr m = _shards[ns];
        if ( m && ! shouldReload )
            return m;
        
        if ( shouldReload && ! _isSharded( ns ) )
            _reload();
        
        massert( 10181 ,  (string)"not sharded:" + ns , _isSharded( ns ) );
        
        if ( m && shouldReload ){
            log() << "reloading shard info for: " << ns << endl;
            _reload();
        }
        
        // this means it was sharded and now isn't....
        // i'm going to return null here
        // though i'm not 100% sure its a good idea
        if ( ! _isSharded(ns) )
            return ChunkManagerPtr();
        
        m.reset( new ChunkManager( this , ns , _sharded[ ns ].key , _sharded[ns].unique ) );
        _shards[ns] = m;
        return m;
    }
    
    void DBConfig::serialize(BSONObjBuilder& to){
        to.append("_id", _name);
        to.appendBool("partitioned", _shardingEnabled );
        to.append("primary", _primary.getName() );
        
        if ( _sharded.size() > 0 ){
            BSONObjBuilder a;
            for ( map<string,CollectionInfo>::reverse_iterator i=_sharded.rbegin(); i != _sharded.rend(); i++){
                BSONObjBuilder temp;
                temp.append( "key" , i->second.key.key() );
                temp.appendBool( "unique" , i->second.unique );
                a.append( i->first , temp.obj() );
            }
            to.append( "sharded" , a.obj() );
        }
    }
    
    void DBConfig::unserialize(const BSONObj& from){
        log(1) << "DBConfig unserialize: " << _name << " " << from << endl;
        assert( _name == from["_id"].String() );

        _shardingEnabled = from.getBoolField("partitioned");
        _primary.reset( from.getStringField("primary") );
        
        _sharded.clear();
        BSONObj sharded = from.getObjectField( "sharded" );
        if ( ! sharded.isEmpty() ){
            BSONObjIterator i(sharded);
            while ( i.more() ){
                BSONElement e = i.next();
                uassert( 10182 ,  "sharded things have to be objects" , e.type() == Object );
                BSONObj c = e.embeddedObject();
                uassert( 10183 ,  "key has to be an object" , c["key"].type() == Object );
                _sharded[e.fieldName()] = CollectionInfo( c["key"].embeddedObject() , 
                                                          c["unique"].trueValue() );
            }
        }
    }

    bool DBConfig::load(){
        scoped_lock lk( _lock );
        return _load();
    }

    bool DBConfig::_load(){
        ScopedDbConnection conn( configServer.modelServer() );
        
        BSONObj o = conn->findOne( ShardNS::database , BSON( "_id" << _name ) );
        conn.done();

        if ( o.isEmpty() )
            return false;
        unserialize( o );
        return true;
    }
    
    void DBConfig::save(){
        scoped_lock lk( _lock );
        _save();
    }
    
    void DBConfig::_save(){
        ScopedDbConnection conn( configServer.modelServer() );
        
        BSONObj n;
        {
            BSONObjBuilder b;
            serialize(b);
            n = b.obj();
        }
        
        conn->update( ShardNS::database , BSON( "_id" << _name ) , n , true );
        string err = conn->getLastError();
        conn.done();
        
        uassert( 13396 , (string)"DBConfig save failed: " + err , err.size() == 0 );
    }

    
    bool DBConfig::reload(){
        scoped_lock lk( _lock );
        return _reload();
    }
    
    bool DBConfig::_reload(){
        // TODO: i don't think is 100% correct
        return _load();
    }
    
    bool DBConfig::dropDatabase( string& errmsg ){
        /**
         * 1) make sure everything is up
         * 2) update config server
         * 3) drop and reset sharded collections
         * 4) drop and reset primary
         * 5) drop everywhere to clean up loose ends
         */

        log() << "DBConfig::dropDatabase: " << _name << endl;
        configServer.logChange( "dropDatabase.start" , _name , BSONObj() );
        
        // 1
        if ( ! configServer.allUp( errmsg ) ){
            log(1) << "\t DBConfig::dropDatabase not all up" << endl;
            return 0;
        }
        
        // 2
        grid.removeDB( _name );
        {
            ScopedDbConnection conn( configServer.modelServer() );
            conn->remove( ShardNS::database , BSON( "_id" << _name ) );
            conn.done();
        }

        if ( ! configServer.allUp( errmsg ) ){
            log() << "error removing from config server even after checking!" << endl;
            return 0;
        }
        log(1) << "\t removed entry from config server for: " << _name << endl;
        
        set<Shard> allServers;

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
        for ( set<Shard>::iterator i=allServers.begin(); i!=allServers.end(); i++ ){
            ScopedDbConnection conn( *i );
            BSONObj res;
            if ( ! conn->dropDatabase( _name , &res ) ){
                errmsg = res.toString();
                return 0;
            }
            conn.done();            
        }
        
        log(1) << "\t dropped primary db for: " << _name << endl;

        configServer.logChange( "dropDatabase" , _name , BSONObj() );
        return true;
    }

    bool DBConfig::_dropShardedCollections( int& num, set<Shard>& allServers , string& errmsg ){
        num = 0;
        set<string> seen;
        while ( true ){
            map<string,ChunkManagerPtr>::iterator i = _shards.begin();
            
            if ( i == _shards.end() )
                break;

            if ( seen.count( i->first ) ){
                errmsg = "seen a collection twice!";
                return false;
            }

            seen.insert( i->first );
            log(1) << "\t dropping sharded collection: " << i->first << endl;

            i->second->getAllShards( allServers );
            i->second->drop( i->second );
            
            num++;
            uassert( 10184 ,  "_dropShardedCollections too many collections - bailing" , num < 100000 );
            log(2) << "\t\t dropped " << num << " so far" << endl;
        }
        
        return true;
    }
    
    /* --- Grid --- */
    
    bool Grid::knowAboutShard( string name ) const{
        ShardConnection conn( configServer.getPrimary() , "" );
        BSONObj shard = conn->findOne( "config.shards" , BSON( "host" << name ) );
        conn.done();
        return ! shard.isEmpty();
    }

    DBConfigPtr Grid::getDBConfig( string database , bool create ){
        {
            string::size_type i = database.find( "." );
            if ( i != string::npos )
                database = database.substr( 0 , i );
        }
        
        if ( database == "config" )
            return configServerPtr;

        scoped_lock l( _lock );

        DBConfigPtr& cc = _databases[database];
        if ( !cc ){
            cc.reset(new DBConfig( database ));
            if ( ! cc->load() ){
                if ( create ){
                    // note here that cc->primary == 0.
                    log() << "couldn't find database [" << database << "] in config db" << endl;
                    
                    if ( database == "admin" )
                        cc->_primary = configServer.getPrimary();
                    else
                        cc->_primary = Shard::pick();
                    
                    if ( cc->_primary.ok() ){
                        cc->save();
                        log() << "\t put [" << database << "] on: " << cc->_primary.toString() << endl;
                    }
                    else {
                        cc.reset();
                        log() << "\t can't find a shard to put new db on" << endl;
                        uassert( 10185 ,  "can't find a shard to put new db on" , 0 );
                    }
                }
                else {
                    cc.reset();
                }
            }
            
        }
        
        return cc;
    }

    void Grid::removeDB( string database ){
        uassert( 10186 ,  "removeDB expects db name" , database.find( '.' ) == string::npos );
        scoped_lock l( _lock );
        _databases.erase( database );
        
    }

    unsigned long long Grid::getNextOpTime() const {
        ScopedDbConnection conn( configServer.getPrimary() );
        
        BSONObj result;
        massert( 10421 ,  "getoptime failed" , conn->simpleCommand( "admin" , &result , "getoptime" ) );
        conn.done();

        return result["optime"]._numberLong();
    }

    /* --- ConfigServer ---- */

    ConfigServer::ConfigServer() : DBConfig( "config" ){
        _shardingEnabled = false;
    }
    
    ConfigServer::~ConfigServer() {
    }

    bool ConfigServer::init( string s ){
        vector<string> configdbs;
        splitStringDelim( s, &configdbs, ',' );
        return init( configdbs );
    }

    bool ConfigServer::init( vector<string> configHosts ){
        uassert( 10187 ,  "need configdbs" , configHosts.size() );

        string hn = getHostName();
        if ( hn.empty() ) {
            sleepsecs(5);
            dbexit( EXIT_BADOPTIONS );
        }
        
        set<string> hosts;
        for ( size_t i=0; i<configHosts.size(); i++ ){
            string host = configHosts[i];
            hosts.insert( getHost( host , false ) );
            configHosts[i] = getHost( host , true );
        }
        
        for ( set<string>::iterator i=hosts.begin(); i!=hosts.end(); i++ ){
            string host = *i;
            bool ok = false;
            for ( int x=10; x>0; x-- ){
                if ( ! hostbyname( host.c_str() ).empty() ){
                    ok = true;
                    break;
                }
                log() << "can't resolve DNS for [" << host << "]  sleeping and trying " << x << " more times" << endl;
                sleepsecs( 10 );
            }
            if ( ! ok )
                return false;
        }
        
        string fullString;
        joinStringDelim( configHosts, &fullString, ',' );
        _primary.setAddress( fullString , true );
        log(1) << " config string : " << fullString << endl;
        
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
            log() << "ConfigServer::allUp : " << _primary.toString() << " seems down!" << endl;
            errmsg = _primary.toString() + " seems down";
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
            uassert( 10189 ,  "should only have 1 thing in config.version" , ! c->more() );
        }
        else {
            if ( conn.count( ShardNS::shard ) || conn.count( ShardNS::database ) ){
                version = 1;
            }
        }
        
        return version;
    }
    
    void ConfigServer::reloadSettings(){
        set<string> got;
        
        ScopedDbConnection conn( _primary );
        auto_ptr<DBClientCursor> c = conn->query( ShardNS::settings , BSONObj() );
        while ( c->more() ){
            BSONObj o = c->next();
            string name = o["_id"].valuestrsafe();
            got.insert( name );
            if ( name == "chunksize" ){
                log(1) << "MaxChunkSize: " << o["value"] << endl;
                Chunk::MaxChunkSize = o["value"].numberInt() * 1024 * 1024;
            }
            else if ( name == "balancer" ){
                // ones we ignore here
            }
            else {
                log() << "warning: unknown setting [" << name << "]" << endl;
            }
        }

        if ( ! got.count( "chunksize" ) ){
            conn->insert( ShardNS::settings , BSON( "_id" << "chunksize"  <<
                                                    "value" << (Chunk::MaxChunkSize / ( 1024 * 1024 ) ) ) );
        }
        
        
        // indexes
        conn->ensureIndex( ShardNS::chunk , BSON( "ns" << 1 << "min" << 1 ) , true );
        conn->ensureIndex( ShardNS::chunk , BSON( "ns" << 1 << "shard" << 1 << "min" << 1 ) , true );
        conn->ensureIndex( ShardNS::chunk , BSON( "ns" << 1 << "lastmod" << 1 ) , true );
        conn->ensureIndex( ShardNS::shard , BSON( "host" << 1 ) , true );

        conn.done();
    }

    string ConfigServer::getHost( string name , bool withPort ){
        if ( name.find( ":" ) != string::npos ){
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

    void ConfigServer::logChange( const string& what , const string& ns , const BSONObj& detail ){
        assert( _primary.ok() );

        static bool createdCapped = false;
        static AtomicUInt num;
        
        ScopedDbConnection conn( _primary );
        
        if ( ! createdCapped ){
            try {
                conn->createCollection( "config.changelog" , 1024 * 1024 * 10 , true );
            }
            catch ( UserException& e ){
                log(1) << "couldn't create changelog (like race condition): " << e << endl;
                // don't care
            }
            createdCapped = true;
        }
     
        stringstream id;
        id << getHostNameCached() << "-" << terseCurrentTime() << "-" << num++;

        BSONObj msg = BSON( "_id" << id.str() << "server" << getHostNameCached() << "time" << DATENOW <<
                            "what" << what << "ns" << ns << "details" << detail );
        log() << "config change: " << msg << endl;
        conn->insert( "config.changelog" , msg );
        
        conn.done();
    }

    DBConfigPtr configServerPtr (new ConfigServer());    
    ConfigServer& configServer = dynamic_cast<ConfigServer&>(*configServerPtr);    
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
                  << "in  : " << o.toString()  << '\n'
                  << "out : " << out.toString() 
                  << endl;
            assert(0);
        }

        void a(){
            BSONObjBuilder b;
            b << "_id" << "abc";
            b.appendBool( "partitioned" , true );
            b << "primary" << "myserver";
            
            DBConfig c( "abc" );
            testInOut( c , b.obj() );
        }

        void b(){
            BSONObjBuilder b;
            b << "_id" << "abc";
            b.appendBool( "partitioned" , true );
            b << "primary" << "myserver";
            
            BSONObjBuilder a;
            a << "abc.foo" << fromjson( "{ 'key' : { 'a' : 1 } , 'unique' : false }" );
            a << "abc.bar" << fromjson( "{ 'key' : { 'kb' : -1 } , 'unique' : true }" );
            
            b.append( "sharded" , a.obj() );

            DBConfig c("abc");
            testInOut( c , b.obj() );
            assert( c.isSharded( "abc.foo" ) );
            assert( ! c.isSharded( "abc.food" ) );
        }
        
        void run(){
            {
                Shard s( "config" , "localhost" );
                s.setAddress( "localhost" , true );
            }

            {
                Shard s( "myserver" , "localhost" );
                s.setAddress( "localhost" , true );
            }
            
            a();
            b();
        }
        
    } dbConfigUnitTest;
} 
