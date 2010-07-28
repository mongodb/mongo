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

    string ShardNS::shard = "config.shards";    
    string ShardNS::database = "config.databases";
    string ShardNS::collection = "config.collections";
    string ShardNS::chunk = "config.chunks";

    string ShardNS::mongos = "config.mongos";
    string ShardNS::settings = "config.settings";

    BSONField<bool>      ShardFields::draining("draining");
    BSONField<long long> ShardFields::maxSize ("maxSize");
    BSONField<long long> ShardFields::currSize("currSize");

    OID serverID;

    /* --- DBConfig --- */

    DBConfig::CollectionInfo::CollectionInfo( DBConfig * db , const BSONObj& in ){
        _dirty = false;
        _dropped = in["dropped"].trueValue();
        if ( in["key"].isABSONObj() )
            shard( db , in["_id"].String() , in["key"].Obj() , in["unique"].trueValue() );
    }


    void DBConfig::CollectionInfo::shard( DBConfig * db , const string& ns , const ShardKeyPattern& key , bool unique ){
        _cm.reset( new ChunkManager( db, ns , key , unique ) );
        _dirty = true;
    }

    void DBConfig::CollectionInfo::unshard(){
        _cm.reset();
        _dropped = true;
        _dirty = true;
    }
    
    void DBConfig::CollectionInfo::save( const string& ns , DBClientBase* conn ){
        BSONObj key = BSON( "_id" << ns );
        
        BSONObjBuilder val;
        val.append( "_id" , ns );
        val.appendDate( "lastmod" , time(0) );
        val.appendBool( "dropped" , _dropped );
        if ( _cm )
            _cm->getInfo( val );
        
        conn->update( ShardNS::collection , key , val.obj() , true );
        _dirty = false;
    }


    bool DBConfig::isSharded( const string& ns ){
        if ( ! _shardingEnabled )
            return false;
        scoped_lock lk( _lock );
        return _isSharded( ns );
    }

    bool DBConfig::_isSharded( const string& ns ){
        if ( ! _shardingEnabled )
            return false;
        Collections::iterator i = _collections.find( ns );
        if ( i == _collections.end() )
            return false;
        return i->second.isSharded();
    }


    const Shard& DBConfig::getShard( const string& ns ){
        if ( isSharded( ns ) )
            return Shard::EMPTY;
        
        uassert( 10178 ,  "no primary!" , _primary.ok() );
        return _primary;
    }
    
    void DBConfig::enableSharding(){
        if ( _shardingEnabled )
            return;
        scoped_lock lk( _lock );
        _shardingEnabled = true; 
        _save();
    }
    
    ChunkManagerPtr DBConfig::shardCollection( const string& ns , ShardKeyPattern fieldsAndOrder , bool unique ){
        uassert( 8042 , "db doesn't have sharding enabled" , _shardingEnabled );
        
        scoped_lock lk( _lock );

        CollectionInfo& ci = _collections[ns];
        uassert( 8043 , "already sharded" , ! ci.isSharded() );

        log() << "enable sharding on: " << ns << " with shard key: " << fieldsAndOrder << endl;

        ci.shard( this , ns , fieldsAndOrder , unique );
        ci.getCM()->maybeChunkCollection();

        _save();
        return ci.getCM();
    }

    bool DBConfig::removeSharding( const string& ns ){
        if ( ! _shardingEnabled ){
            return false;
        }
        
        scoped_lock lk( _lock );
        
        Collections::iterator i = _collections.find( ns );

        if ( i == _collections.end() )
            return false;
        
        CollectionInfo& ci = _collections[ns];
        if ( ! ci.isSharded() )
            return false;
        
        ci.unshard();
        _save();
        return true;
    }
    
    ChunkManagerPtr DBConfig::getChunkManager( const string& ns , bool shouldReload ){
        scoped_lock lk( _lock );

        if ( shouldReload )
            _reload();

        CollectionInfo& ci = _collections[ns];
        massert( 10181 ,  (string)"not sharded:" + ns , ci.isSharded() || ci.wasDropped() );
        return ci.getCM();
    }

    void DBConfig::setPrimary( string s ){
        scoped_lock lk( _lock );
        _primary.reset( s );
        _save();
    }
    
    void DBConfig::serialize(BSONObjBuilder& to){
        to.append("_id", _name);
        to.appendBool("partitioned", _shardingEnabled );
        to.append("primary", _primary.getName() );
    }
    
    bool DBConfig::unserialize(const BSONObj& from){
        log(1) << "DBConfig unserialize: " << _name << " " << from << endl;
        assert( _name == from["_id"].String() );

        _shardingEnabled = from.getBoolField("partitioned");
        _primary.reset( from.getStringField("primary") );

        // this is a temporary migration thing
        BSONObj sharded = from.getObjectField( "sharded" );
        if ( sharded.isEmpty() )
             return false;
        
        BSONObjIterator i(sharded);
        while ( i.more() ){
            BSONElement e = i.next();
            uassert( 10182 ,  "sharded things have to be objects" , e.type() == Object );
            
            BSONObj c = e.embeddedObject();
            uassert( 10183 ,  "key has to be an object" , c["key"].type() == Object );
            
            _collections[e.fieldName()].shard( this , e.fieldName() , c["key"].Obj() , c["unique"].trueValue() );
        }
        return true;
    }

    bool DBConfig::load(){
        scoped_lock lk( _lock );
        return _load();
    }

    bool DBConfig::_load(){
        ScopedDbConnection conn( configServer.modelServer() );
        
        BSONObj o = conn->findOne( ShardNS::database , BSON( "_id" << _name ) );


        if ( o.isEmpty() ){
            conn.done();
            return false;
        }
        
        if ( unserialize( o ) )
            _save();
        
        BSONObjBuilder b;
        b.appendRegex( "_id" , (string)"^" + _name + "." );
        

        auto_ptr<DBClientCursor> cursor = conn->query( ShardNS::collection ,b.obj() );
        while ( cursor->more() ){
            BSONObj o = cursor->next();
            _collections[o["_id"].String()] = CollectionInfo( this , o );
        }
        
        conn.done();        

        return true;
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
        uassert( 13396 , (string)"DBConfig save failed: " + err , err.size() == 0 );
        
        for ( Collections::iterator i=_collections.begin(); i!=_collections.end(); ++i ){
            if ( ! i->second.isDirty() )
                continue;
            i->second.save( i->first , conn.get() );
        }

        conn.done();
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
            Collections::iterator i = _collections.begin();
            for ( ; i != _collections.end(); ++i ){
                if ( i->second.isSharded() )
                    break;
            }
            
            if ( i == _collections.end() )
                break;

            if ( seen.count( i->first ) ){
                errmsg = "seen a collection twice!";
                return false;
            }

            seen.insert( i->first );
            log(1) << "\t dropping sharded collection: " << i->first << endl;

            i->second.getCM()->getAllShards( allServers );
            i->second.getCM()->drop( i->second.getCM() );
            
            num++;
            uassert( 10184 ,  "_dropShardedCollections too many collections - bailing" , num < 100000 );
            log(2) << "\t\t dropped " << num << " so far" << endl;
        }
        
        return true;
    }
    
    void DBConfig::getAllShards(set<Shard>& shards) const{
        shards.insert(getPrimary());
        for (Collections::const_iterator it(_collections.begin()), end(_collections.end()); it != end; ++it){
            if (it->second.isSharded()){
                it->second.getCM()->getAllShards(shards);
            } // TODO: handle collections on non-primary shard
        }
    }

    /* --- Grid --- */
    
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
                    
                    { // lets check case
                        ScopedDbConnection conn( configServer.modelServer() );
                        BSONObjBuilder b;
                        b.appendRegex( "_id" , (string)"^" + database + "$" , "i" );
                        BSONObj d = conn->findOne( ShardNS::database , b.obj() );
                        conn.done();

                        if ( ! d.isEmpty() ){
                            cc.reset();
                            stringstream ss;
                            ss <<  "can't have 2 databases that just differ on case " 
                               << " have: " << d["_id"].String()
                               << " want to add: " << database;

                            uasserted( DatabaseDifferCaseCode ,ss.str() );
                        }
                    }

                    if ( database == "admin" )
                        cc->_primary = configServer.getPrimary();
                    else
                        cc->_primary = Shard::pick();
                    
                    if ( cc->_primary.ok() ){
                        cc->_save();
                        log() << "\t put [" << database << "] on: " << cc->_primary.toString() << endl;
                    }
                    else {
                        cc.reset();
                        log() << "\t can't find a shard to put new db on" << endl;
                        uasserted( 10185 ,  "can't find a shard to put new db on" );
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

    bool Grid::allowLocalHost() const {
        return _allowLocalShard;
    }

    void Grid::setAllowLocalHost( bool allow ){
        _allowLocalShard = allow;
    }

    bool Grid::addShard( string* name , const string& host , long long maxSize , string* errMsg ){
        // errMsg is required but name is optional
        DEV assert( errMsg );
        string nameInternal;
        if ( ! name ) {
            name = &nameInternal;
        }

        // check whether host exists and is operative
        try {
            ScopedDbConnection newShardConn( host );
            newShardConn->getLastError();
            newShardConn.done();
        }
        catch ( DBException& e ){
            ostringstream ss;
            ss << "couldn't connect to new shard ";
            ss << e.what();
            *errMsg = ss.str();
            return false;
        }
                
        // if a name for a shard wasn't provided, pick one.
        if ( name->empty() && ! _getNewShardName( name ) ){
            *errMsg = "error generating new shard name";
            return false;
        }
            
        // build the ConfigDB shard document.
        BSONObjBuilder b;
        b.append( "_id" , *name );
        b.append( "host" , host );
        if ( maxSize > 0 ){
            b.append( ShardFields::maxSize.name() , maxSize );
        }
        BSONObj shardDoc = b.obj();

        {
            ScopedDbConnection conn( configServer.getPrimary() );
                
            // check whether this host:port is already a known shard
            BSONObj old = conn->findOne( ShardNS::shard , BSON( "host" << host ) );
            if ( ! old.isEmpty() ){
                *errMsg = "host already used";
                conn.done();
                return false;
            }

            log() << "going to add shard: " << shardDoc << endl;

            conn->insert( ShardNS::shard , shardDoc );
            *errMsg = conn->getLastError();
            if ( ! errMsg->empty() ){
                log() << "error adding shard: " << shardDoc << " err: " << *errMsg << endl;
                return false;
            }

            conn.done();
        }

        Shard::reloadShardInfo();
        return true;
    }
        
    bool Grid::knowAboutShard( const string& name ) const{
        ShardConnection conn( configServer.getPrimary() , "" );
        BSONObj shard = conn->findOne( ShardNS::shard , BSON( "host" << name ) );
        conn.done();
        return ! shard.isEmpty();
    }

    bool Grid::_getNewShardName( string* name ) const{
        DEV assert( name );

        bool ok = false;
        int count = 0; 

        ShardConnection conn( configServer.getPrimary() , "" );
        BSONObj o = conn->findOne( ShardNS::shard , Query( fromjson ( "{_id: /^shard/}" ) ).sort(  BSON( "_id" << -1 ) ) ); 
        if ( ! o.isEmpty() ) {
            string last = o["_id"].String();
            istringstream is( last.substr( 5 ) );
            is >> count;
            count++;
        }                                                                                                               
        if (count < 9999) {
            stringstream ss;
            ss << "shard" << setfill('0') << setw(4) << count;
            *name = ss.str();
            ok = true;
        }
        conn.done();

        return ok;
    }

    bool Grid::shouldBalance() const {
        ShardConnection conn( configServer.getPrimary() , "" );

        // look for the stop balancer marker
        BSONObj stopMarker = conn->findOne( ShardNS::settings, BSON( "_id" << "balancer" << "stopped" << true ) );
        conn.done();
        return stopMarker.isEmpty();
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

} 
