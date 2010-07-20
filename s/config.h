// config.h

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

/* This file is things related to the "grid configuration":
   - what machines make up the db component of our cloud
   - where various ranges of things live
*/

#pragma once

#include "../db/namespace.h"
#include "../client/dbclient.h"
#include "../client/model.h"
#include "shardkey.h"
#include "shard.h"

namespace mongo {

    struct ShardNS {
        static string database;
        static string shard;
        static string chunk;
        static string mongos;
        static string settings;
    };

    /**
     * Field names used in the 'shards' collection.
     */
    struct ShardFields {
        static BSONField<bool> draining;
        static BSONField<long long> maxSize;
        static BSONField<long long> currSize;
    };
        
    class Grid;
    class ConfigServer;

    class DBConfig;
    typedef boost::shared_ptr<DBConfig> DBConfigPtr;

    extern DBConfigPtr configServerPtr;
    extern ConfigServer& configServer;
    extern Grid grid;

    class ChunkManager;
    typedef shared_ptr<ChunkManager> ChunkManagerPtr;
    
    class CollectionInfo {
    public:
        CollectionInfo( ShardKeyPattern _key = BSONObj() , bool _unique = false ) : 
            key( _key ) , unique( _unique ){}

        ShardKeyPattern key;
        bool unique;
    };
    
    /**
     * top level grid configuration for an entire database
     * TODO: use shared_ptr for ChunkManager
     */
    class DBConfig : public Model {
    public:

        DBConfig( string name = "" ) : _name( name ) , _primary("config","") , 
            _shardingEnabled(false), _lock("DBConfig") { }
        
        string getName(){ return _name; };

        /**
         * @return if anything in this db is partitioned or not
         */
        bool isShardingEnabled(){
            return _shardingEnabled;
        }
        
        void enableSharding();
        ChunkManagerPtr shardCollection( const string& ns , ShardKeyPattern fieldsAndOrder , bool unique );
        
        /**
         * @return whether or not this partition is partitioned
         */
        bool isSharded( const string& ns );
        
        ChunkManagerPtr getChunkManager( const string& ns , bool reload = false );
        
        /**
         * @return the correct for shard for the ns
         * if the namespace is sharded, will return NULL
         */
        const Shard& getShard( const string& ns );
        
        const Shard& getPrimary() const {
            uassert( 8041 , (string)"no primary shard configured for db: " + _name , _primary.ok() );
            return _primary;
        }
        
        void setPrimary( string s ){
            _primary.reset( s );
        }

        bool reload();

        bool dropDatabase( string& errmsg );

        virtual string modelServer();
        
        // model stuff

        virtual const char * getNS(){ return "config.databases"; }
        virtual void serialize(BSONObjBuilder& to);
        virtual void unserialize(const BSONObj& from);
        
    protected:

        /** 
            lockless
        */
        bool _isSharded( const string& ns );

        bool _dropShardedCollections( int& num, set<Shard>& allServers , string& errmsg );
        
        bool doload();
        
        /**
           @return true if there was sharding info to remove
         */
        bool removeSharding( const string& ns );

        string _name; // e.g. "alleyinsider"
        Shard _primary; // e.g. localhost , mongo.foo.com:9999
        bool _shardingEnabled;
        
        map<string,CollectionInfo> _sharded; // { "alleyinsider.blog.posts" : { ts : 1 }  , ... ] - all ns that are sharded
        map<string,ChunkManagerPtr> _shards; // this will only have entries for things that have been looked at

        mongo::mutex _lock; // TODO: change to r/w lock ??

        friend class Grid;
        friend class ChunkManager;
    };

    /**
     * stores meta-information about the grid
     * TODO: used shard_ptr for DBConfig pointers
     */
    class Grid {
    public:
        Grid() : _lock("Grid") { }

        /**
           gets the config the db.
           will return an empty DBConfig if not in db already
         */
        DBConfigPtr getDBConfig( string ns , bool create=true);
        
        /**
         * removes db entry.
         * on next getDBConfig call will fetch from db
         */
        void removeDB( string db );
        
        bool knowAboutShard( string name ) const;
        
        unsigned long long getNextOpTime() const;
    private:
        map<string, DBConfigPtr > _databases;
        mongo::mutex _lock; // TODO: change to r/w lock ??
    };

    class ConfigServer : public DBConfig {
    public:

        ConfigServer();
        ~ConfigServer();
        
        bool ok(){
            return _primary.ok();
        }
        
        virtual string modelServer(){
            uassert( 10190 ,  "ConfigServer not setup" , _primary.ok() );
            return _primary.getConnString();
        }
        
        /**
           call at startup, this will initiate connection to the grid db 
        */
        bool init( vector<string> configHosts );
        
        bool init( string s );

        bool allUp();
        bool allUp( string& errmsg );
        
        int dbConfigVersion();
        int dbConfigVersion( DBClientBase& conn );
        
        void reloadSettings();

        /**
         * @return 0 = ok, otherwise error #
         */
        int checkConfigVersion( bool upgrade );
        
        /**
         * log a change to config.changes 
         * @param what e.g. "split" , "migrate"
         * @param msg any more info
         */
        void logChange( const string& what , const string& ns , const BSONObj& detail = BSONObj() );

        ConnectionString getConnectionString() const {
            return ConnectionString( _primary.getConnString() , ConnectionString::SYNC );
        }

        static int VERSION;
        
    private:
        string getHost( string name , bool withPort );
    };

} // namespace mongo
