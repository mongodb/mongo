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
    
    bool DBConfig::loadByName(const char *nm){
        BSONObjBuilder b;
        b.append("name", nm);
        BSONObj q = b.done();
        return load(q);
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

        DBConfig*& cc = _databases[database];
        if ( cc == 0 ){
            cc = new DBConfig( database );
            if ( ! cc->loadByName(database.c_str()) ){
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
                        log() << "\t can't find a server" << endl;
                        cc = 0;
                    }
                }
                else {
                    cc = 0;
                }
            }
            
        }
        
        return cc;
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

    bool ConfigServer::init( vector<string> configHosts , bool infer ){
        string hn = getHostName();
        if ( hn.empty() ) {
            sleepsecs(5);
            dbexit( EXIT_BADOPTIONS );
        }
        ourHostname = hn;

        char buf[256];
        strcpy(buf, hn.c_str());

        if ( configHosts.empty() ) {
            char *p = strchr(buf, '-');
            if ( p )
                p = strchr(p+1, '-');
            if ( !p ) {
                log() << "can't parse server's hostname, expect <city>-<locname>-n<nodenum>, got: " << buf << endl;
                sleepsecs(5);
                dbexit( EXIT_BADOPTIONS );
            }
            p[1] = 0;
        }

        string left, right; // with :port#
        string hostLeft, hostRight;

        if ( configHosts.empty() ) {
            if ( ! infer ) {
                out() << "--configdb or --infer required\n";
                dbexit( EXIT_BADOPTIONS );
            }
            stringstream sl, sr;
            sl << buf << "grid-l";
            sr << buf << "grid-r";
            hostLeft = sl.str();
            hostRight = sr.str();
            sl << ":" << Port;
            sr << ":" << Port;
            left = sl.str();
            right = sr.str();
        }
        else {
            hostLeft = getHost( configHosts[0] , false );
            left = getHost( configHosts[0] , true );

            if ( configHosts.size() > 1 ) {
                hostRight = getHost( configHosts[1] , false );
                right = getHost( configHosts[1] , true );
            }
        }
        

        if ( !isdigit(left[0]) )
            /* this loop is not really necessary, we we print out if we can't connect
               but it gives much prettier error msg this way if the config is totally
               wrong so worthwhile.
               */
            while ( 1 ) {
                if ( hostbyname(hostLeft.c_str()).empty() ) {
                    log() << "can't resolve DNS for " << hostLeft << ", sleeping and then trying again" << endl;
                    sleepsecs(15);
                    continue;
                }
                if ( !hostRight.empty() && hostbyname(hostRight.c_str()).empty() ) {
                    log() << "can't resolve DNS for " << hostRight << ", sleeping and then trying again" << endl;
                    sleepsecs(15);
                    continue;
                }
                break;
            }
        
        Nullstream& l = log();
        l << "connecting to griddb ";
        
        if ( !hostRight.empty() ) {
            // connect in paired mode
            l << "L:" << left << " R:" << right << "...";
            l.flush();
            _primary = left + "," + right;
        }
        else {
            l << left << "...";
            l.flush();
            _primary = left;
        }
        
        return true;
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
            ss << name << ":" << Port;
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
