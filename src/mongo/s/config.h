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
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects
*    for all of the code used other than as permitted herein. If you modify
*    file(s) with this exception, you may extend this exception to your
*    version of the file(s), but you are not obligated to do so. If you do not
*    wish to do so, delete this exception statement from your version. If you
*    delete this exception statement from all source files in the program,
*    then also delete it in the license file.
*/

/* This file is things related to the "grid configuration":
   - what machines make up the db component of our cloud
   - where various ranges of things live
*/

#pragma once

#include <boost/shared_ptr.hpp>

#include "mongo/client/dbclient_rs.h"
#include "mongo/s/shard.h"
#include "mongo/s/shard_key_pattern.h"

namespace mongo {

    class ChunkManager;
    class ConfigServer;
    class DBConfig;

    typedef boost::shared_ptr<DBConfig> DBConfigPtr;

    extern DBConfigPtr configServerPtr;
    extern ConfigServer& configServer;

    /**
     * top level configuration for a database
     */
    class DBConfig  {
    public:
        DBConfig(std::string name);
        virtual ~DBConfig();

        const std::string& name() const { return _name; };

        /**
         * @return if anything in this db is partitioned or not
         */
        bool isShardingEnabled() {
            return _shardingEnabled;
        }

        void enableSharding( bool save = true );

        /* Makes all the configuration changes necessary to shard a new collection.
         * Optionally, chunks will be created based on a set of specified initial split points, and
         * distributed in a round-robin fashion onto a set of initial shards.  If no initial shards
         * are specified, only the primary will be used.
         *
         * WARNING: It's not safe to place initial chunks onto non-primary shards using this method.
         * The initShards parameter allows legacy behavior expected by map-reduce.
         */
        boost::shared_ptr<ChunkManager> shardCollection(const std::string& ns,
                                                        const ShardKeyPattern& fieldsAndOrder,
                                                        bool unique,
                                                        std::vector<BSONObj>* initPoints,
                                                        std::vector<Shard>* initShards = NULL);

        /**
           @return true if there was sharding info to remove
         */
        bool removeSharding( const std::string& ns );

        /**
         * @return whether or not the 'ns' collection is partitioned
         */
        bool isSharded( const std::string& ns );

        // Atomically returns *either* the chunk manager *or* the primary shard for the collection,
        // neither if the collection doesn't exist.
        void getChunkManagerOrPrimary(const std::string& ns, boost::shared_ptr<ChunkManager>& manager, ShardPtr& primary);

        boost::shared_ptr<ChunkManager> getChunkManager(const std::string& ns, bool reload = false, bool forceReload = false);
        boost::shared_ptr<ChunkManager> getChunkManagerIfExists(const std::string& ns, bool reload = false, bool forceReload = false);

        const Shard& getShard( const std::string& ns );
        /**
         * @return the correct for shard for the ns
         * if the namespace is sharded, will return NULL
         */
        ShardPtr getShardIfExists( const std::string& ns );

        const Shard& getPrimary() const {
            uassert( 8041 , (std::string)"no primary shard configured for db: " + _name , _primary.ok() );
            return _primary;
        }

        void setPrimary( const std::string& s );

        bool load();
        bool reload();

        bool dropDatabase( std::string& errmsg );

        // model stuff

        // lockless loading
        void getAllShards(std::set<Shard>& shards) const;

        void getAllShardedCollections(std::set<std::string>& namespaces) const;

    protected:
        struct CollectionInfo {
            CollectionInfo() {
                _dirty = false;
                _dropped = false;
            }

            CollectionInfo(const BSONObj& in);
            ~CollectionInfo();

            bool isSharded() const {
                return _cm.get();
            }

            boost::shared_ptr<ChunkManager> getCM() const {
                return _cm;
            }

            void resetCM(ChunkManager * cm);

            void shard(ChunkManager* cm);
            void unshard();

            bool isDirty() const { return _dirty; }
            bool wasDropped() const { return _dropped; }

            void save(const std::string& ns);

            bool unique() const { return _unqiue; }
            BSONObj key() const { return _key; }


        private:
            BSONObj _key;
            bool _unqiue;
            boost::shared_ptr<ChunkManager> _cm;
            bool _dirty;
            bool _dropped;
        };

        typedef std::map<std::string, CollectionInfo> CollectionInfoMap;


        /**
            lockless
        */
        bool _isSharded( const std::string& ns );

        bool _dropShardedCollections( int& num, std::set<Shard>& allServers , std::string& errmsg );

        bool _load();
        bool _reload();
        void _save( bool db = true, bool coll = true );


        const std::string _name; // e.g. "alleyinsider"

        Shard _primary; // e.g. localhost , mongo.foo.com:9999
        bool _shardingEnabled;

        CollectionInfoMap _collections;

        mutable mongo::mutex _lock; // TODO: change to r/w lock ??
        mutable mongo::mutex _hitConfigServerLock;
    };


    class ConfigServer : public DBConfig {
    public:
        ConfigServer();
        ~ConfigServer();

        bool ok( bool checkConsistency = false );

        std::string modelServer() {
            uassert( 10190 ,  "ConfigServer not setup" , _primary.ok() );
            return _primary.getConnString();
        }

        /**
           call at startup, this will initiate connection to the grid db
        */
        bool init( std::vector<std::string> configHosts );

        bool init( const std::string& s );

        /**
         * Check hosts are unique. Returns true if all configHosts
         * hostname:port entries are unique. Otherwise return false
         * and fill errmsg with message containing the offending server.
         */
        bool checkHostsAreUnique( const std::vector<std::string>& configHosts, std::string* errmsg );

        /**
         * Checks if all config servers are up.
         *
         * If localCheckOnly is true, only check if the socket is still open with no errors.
         * Otherwise, also send a getLastError command with recv timeout.
         *
         * TODO: fix this - SERVER-15811
         */
        bool allUp(bool localCheckOnly);
        bool allUp(bool localCheckOnly, std::string& errmsg);

        int dbConfigVersion();
        int dbConfigVersion( DBClientBase& conn );

        void reloadSettings();

        ConnectionString getConnectionString() const {
            return ConnectionString( _primary.getConnString() , ConnectionString::SYNC );
        }

        void replicaSetChange(const std::string& setName, const std::string& newConnectionString);

        static int VERSION;


        /**
         * check to see if all config servers have the same state
         * will try tries time to make sure not catching in a bad state
         */
        bool checkConfigServersConsistent( std::string& errmsg , int tries = 4 ) const;

    private:
        std::string getHost( const std::string& name , bool withPort );
        std::vector<std::string> _config;
    };

} // namespace mongo
