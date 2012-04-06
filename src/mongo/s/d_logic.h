// @file d_logic.h
/*
 *    Copyright (C) 2010 10gen Inc.
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


#pragma once

#include "../pch.h"

#include "../db/jsobj.h"

#include "d_chunk_manager.h"
#include "util.h"
#include "mongo/util/concurrency/ticketholder.h"

namespace mongo {

    class Database;
    class DiskLoc;

    typedef ShardChunkVersion ConfigVersion;

    // --------------
    // --- global state ---
    // --------------

    class ShardingState {
    public:
        ShardingState();

        bool enabled() const { return _enabled; }
        const string& getConfigServer() const { return _configServer; }
        void enable( const string& server );

        void gotShardName( const string& name );
        void gotShardHost( string host );

        string getShardName() { return _shardName; }
        string getShardHost() { return _shardHost; }

        /** Reverts back to a state where this mongod is not sharded. */
        void resetShardingState(); 

        // versioning support

        bool hasVersion( const string& ns );
        bool hasVersion( const string& ns , ConfigVersion& version );
        const ConfigVersion getVersion( const string& ns ) const;

        /**
         * Uninstalls the manager for a given collection. This should be used when the collection is dropped.
         *
         * NOTE:
         *   An existing collection with no chunks on this shard will have a manager on version 0, which is different than a
         *   a dropped collection, which will not have a manager.
         *
         * TODO
         *   When sharding state is enabled, absolutely all collections should have a manager. (The non-sharded ones are
         *   a be degenerate case of one-chunk collections).
         *   For now, a dropped collection and an non-sharded one are indistinguishable (SERVER-1849)
         *
         * @param ns the collection to be dropped
         */
        void resetVersion( const string& ns );

        /**
         * Requests to access a collection at a certain version. If the collection's manager is not at that version it
         * will try to update itself to the newest version. The request is only granted if the version is the current or
         * the newest one.
         *
         * @param ns collection to be accessed
         * @param version (IN) the client belive this collection is on and (OUT) the version the manager is actually in
         * @return true if the access can be allowed at the provided version
         */
        bool trySetVersion( const string& ns , ConfigVersion& version );

        void appendInfo( BSONObjBuilder& b );

        // querying support

        bool needShardChunkManager( const string& ns ) const;
        ShardChunkManagerPtr getShardChunkManager( const string& ns );

        // chunk migrate and split support

        /**
         * Creates and installs a new chunk manager for a given collection by "forgetting" about one of its chunks.
         * The new manager uses the provided version, which has to be higher than the current manager's.
         * One exception: if the forgotten chunk is the last one in this shard for the collection, version has to be 0.
         *
         * If it runs successfully, clients need to grab the new version to access the collection.
         *
         * @param ns the collection
         * @param min max the chunk to eliminate from the current manager
         * @param version at which the new manager should be at
         */
        void donateChunk( const string& ns , const BSONObj& min , const BSONObj& max , ShardChunkVersion version );

        /**
         * Creates and installs a new chunk manager for a given collection by reclaiming a previously donated chunk.
         * The previous manager's version has to be provided.
         *
         * If it runs successfully, clients that became stale by the previous donateChunk will be able to access the
         * collection again.
         *
         * @param ns the collection
         * @param min max the chunk to reclaim and add to the current manager
         * @param version at which the new manager should be at
         */
        void undoDonateChunk( const string& ns , const BSONObj& min , const BSONObj& max , ShardChunkVersion version );

        /**
         * Creates and installs a new chunk manager for a given collection by splitting one of its chunks in two or more.
         * The version for the first split chunk should be provided. The subsequent chunks' version would be the latter with the
         * minor portion incremented.
         *
         * The effect on clients will depend on the version used. If the major portion is the same as the current shards,
         * clients shouldn't perceive the split.
         *
         * @param ns the collection
         * @param min max the chunk that should be split
         * @param splitKeys point in which to split
         * @param version at which the new manager should be at
         */
        void splitChunk( const string& ns , const BSONObj& min , const BSONObj& max , const vector<BSONObj>& splitKeys ,
                         ShardChunkVersion version );

        bool inCriticalMigrateSection();

    private:
        bool _enabled;

        string _configServer;

        string _shardName;
        string _shardHost;

        // protects state below
        mutable mongo::mutex _mutex;
        // protects accessing the config server
        // Using a ticket holder so we can have multiple redundant tries at any given time
        mutable TicketHolder _configServerTickets;

        // map from a namespace into the ensemble of chunk ranges that are stored in this mongod
        // a ShardChunkManager carries all state we need for a collection at this shard, including its version information
        typedef map<string,ShardChunkManagerPtr> ChunkManagersMap;
        ChunkManagersMap _chunks;
    };

    extern ShardingState shardingState;

    /**
     * one per connection from mongos
     * holds version state for each namesapce
     */
    class ShardedConnectionInfo {
    public:
        ShardedConnectionInfo();

        const OID& getID() const { return _id; }
        bool hasID() const { return _id.isSet(); }
        void setID( const OID& id );

        const ConfigVersion getVersion( const string& ns ) const;
        void setVersion( const string& ns , const ConfigVersion& version );

        static ShardedConnectionInfo* get( bool create );
        static void reset();
        static void addHook();

        bool inForceVersionOkMode() const {
            return _forceVersionOk;
        }

        void enterForceVersionOkMode() { _forceVersionOk = true; }
        void leaveForceVersionOkMode() { _forceVersionOk = false; }

    private:

        OID _id;
        bool _forceVersionOk; // if this is true, then chunk version #s aren't check, and all ops are allowed

        typedef map<string,ConfigVersion> NSVersionMap;
        NSVersionMap _versions;

        static boost::thread_specific_ptr<ShardedConnectionInfo> _tl;
    };

    struct ShardForceVersionOkModeBlock {
        ShardForceVersionOkModeBlock() {
            info = ShardedConnectionInfo::get( false );
            if ( info )
                info->enterForceVersionOkMode();
        }
        ~ShardForceVersionOkModeBlock() {
            if ( info )
                info->leaveForceVersionOkMode();
        }

        ShardedConnectionInfo * info;
    };

    // -----------------
    // --- core ---
    // -----------------

    unsigned long long extractVersion( BSONElement e , string& errmsg );


    /**
     * @return true if we have any shard info for the ns
     */
    bool haveLocalShardingInfo( const string& ns );

    /**
     * @return true if the current threads shard version is ok, or not in sharded version
     * Also returns an error message and the Config/ShardChunkVersions causing conflicts
     */
    bool shardVersionOk( const string& ns , string& errmsg, ConfigVersion& received, ConfigVersion& wanted );

    /**
     * @return true if we took care of the message and nothing else should be done
     */
    struct DbResponse;

    bool _handlePossibleShardedMessage( Message &m, DbResponse * dbresponse );

    /** What does this do? document please? */
    inline bool handlePossibleShardedMessage( Message &m, DbResponse * dbresponse ) {
        if( !shardingState.enabled() ) 
            return false;
        return _handlePossibleShardedMessage(m, dbresponse);
    }

    void logOpForSharding( const char * opstr , const char * ns , const BSONObj& obj , BSONObj * patt );
    void aboutToDeleteForSharding( const Database* db , const DiskLoc& dl );

}
