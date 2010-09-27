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
        static string shard;
        
        static string database;
        static string collection;
        static string chunk;

        static string mongos;
        static string settings;
    };

    /**
     * Field names used in the 'shards' collection.
     */
    struct ShardFields {
        static BSONField<bool> draining;      // is it draining chunks?
        static BSONField<long long> maxSize;  // max allowed disk space usage
    };
        
    class ConfigServer;

    class DBConfig;
    typedef boost::shared_ptr<DBConfig> DBConfigPtr;

    extern DBConfigPtr configServerPtr;
    extern ConfigServer& configServer;

    class ChunkManager;
    typedef shared_ptr<ChunkManager> ChunkManagerPtr;
    
    /**
     * top level configuration for a database
     */
    class DBConfig  {

        struct CollectionInfo {
            CollectionInfo(){
                _dirty = false;
                _dropped = false;
            }
            
            CollectionInfo( DBConfig * db , const BSONObj& in );
            
            bool isSharded() const {
                return _cm.get();
            }
            
            ChunkManagerPtr getCM() const {
                return _cm;
            }

            void shard( DBConfig * db , const string& ns , const ShardKeyPattern& key , bool unique );
            void unshard();

            bool isDirty() const { return _dirty; }
            bool wasDropped() const { return _dropped; }
            
            void save( const string& ns , DBClientBase* conn );
            

        private:
            ChunkManagerPtr _cm;
            bool _dirty;
            bool _dropped;
        };
        
        typedef map<string,CollectionInfo> Collections;
        
    public:

        DBConfig( string name ) 
            : _name( name ) , 
              _primary("config","") , 
              _shardingEnabled(false), 
              _lock("DBConfig"){
            assert( name.size() );
        }
        virtual ~DBConfig(){}
        
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
         * @return whether or not the 'ns' collection is partitioned
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
        
        void setPrimary( string s );

        bool load();
        bool reload();
        
        bool dropDatabase( string& errmsg );

        // model stuff

        // lockless loading
        void serialize(BSONObjBuilder& to);

        /**
         * if i need save in new format
         */
        bool unserialize(const BSONObj& from);

        void getAllShards(set<Shard>& shards) const;

    protected:

        /** 
            lockless
        */
        bool _isSharded( const string& ns );

        bool _dropShardedCollections( int& num, set<Shard>& allServers , string& errmsg );

        bool _load();
        bool _reload();
        void _save();

        
        /**
           @return true if there was sharding info to remove
         */
        bool removeSharding( const string& ns );

        string _name; // e.g. "alleyinsider"
        Shard _primary; // e.g. localhost , mongo.foo.com:9999
        bool _shardingEnabled;
        
        //map<string,CollectionInfo> _sharded; // { "alleyinsider.blog.posts" : { ts : 1 }  , ... ] - all ns that are sharded
        //map<string,ChunkManagerPtr> _shards; // this will only have entries for things that have been looked at

        Collections _collections;

        mongo::mutex _lock; // TODO: change to r/w lock ??

        friend class ChunkManager;
    };

    class ConfigServer : public DBConfig {
    public:

        ConfigServer();
        ~ConfigServer();
        
        bool ok( bool checkConsistency = false );
        
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
        

        /**
         * check to see if all config servers have the same state
         * will try tries time to make sure not catching in a bad state
         */
        bool checkConfigServersConsistent( string& errmsg , int tries = 4 ) const;

    private:
        string getHost( string name , bool withPort );
        vector<string> _config;
    };

} // namespace mongo
