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

#include "mongo/pch.h"

#include "mongo/db/jsobj.h"
#include "mongo/s/collection_metadata.h"
#include "mongo/s/chunk_version.h"
#include "mongo/util/concurrency/ticketholder.h"
#include "mongo/util/net/message.h"

namespace mongo {

    class Database;
    class DiskLoc;

    // --------------
    // --- global state ---
    // --------------

    class ShardingState {
    public:
        ShardingState();

        bool enabled() const { return _enabled; }
        const string& getConfigServer() const { return _configServer; }
        void enable( const string& server );

        // Initialize sharding state and begin authenticating outgoing connections and handling
        // shard versions.  If this is not run before sharded operations occur auth will not work
        // and versions will not be tracked.
        static void initialize(const string& server);

        void gotShardName( const string& name );
        void gotShardHost( string host );

        string getShardName() { return _shardName; }
        string getShardHost() { return _shardHost; }

        /** Reverts back to a state where this mongod is not sharded. */
        void resetShardingState(); 

        // versioning support

        bool hasVersion( const string& ns );
        bool hasVersion( const string& ns , ChunkVersion& version );
        const ChunkVersion getVersion( const string& ns ) const;

        /**
         * Uninstalls the metadata for a given collection. This should be used when the collection
         * is dropped.
         *
         * NOTE:
         *   An existing collection with no chunks on this shard will have metadata on version 0,
         *   which is different than a dropped collection, which will not have metadata.
         *
         * TODO:
         *   All collections should have metadata. (The non-sharded ones are a degenerate case of
         *   one-chunk collections).
         *
         * @param ns the collection to be dropped
         */
        void resetVersion( const string& ns );

        /**
         * Requests to access a collection at a certain version. If the collection's metadata is not
         * at that version it will try to update itself to the newest version. The request is only
         * granted if the version is the current or the newest one.
         *
         * @param ns collection to be accessed
         * @param version (IN) the client believe this collection is on and (OUT) the version the
         *  metadata is actually in
         * @param forceRefresh force contacting the config server to check version
         * @return true if the access can be allowed at the provided version
         */
        bool trySetVersion( const string& ns , ChunkVersion& version, bool forceRefresh );

        void appendInfo( BSONObjBuilder& b );

        // querying support

        bool needCollectionMetadata( const string& ns ) const;
        CollectionMetadataPtr getCollectionMetadata( const string& ns );

        // chunk migrate and split support

        /**
         * Creates and installs a new chunk metadata for a given collection by "forgetting" about
         * one of its chunks.  The new metadata uses the provided version, which has to be higher
         * than the current metadata's shard version.
         *
         * One exception: if the forgotten chunk is the last one in this shard for the collection,
         * version has to be 0.
         *
         * If it runs successfully, clients need to grab the new version to access the collection.
         *
         * @param ns the collection
         * @param min max the chunk to eliminate from the current metadata
         * @param version at which the new metadata should be at
         */
        void donateChunk( const string& ns , const BSONObj& min , const BSONObj& max , ChunkVersion version );

        /**
         * Creates and installs new chunk metadata for a given collection by reclaiming a previously
         * donated chunk.  The previous metadata's shard version has to be provided.
         *
         * If it runs successfully, clients that became stale by the previous donateChunk will be
         * able to access the collection again.
         *
         * @param ns the collection
         * @param min max the chunk to reclaim and add to the current metadata
         * @param version at which the new metadata should be at
         */
        void undoDonateChunk( const string& ns , const BSONObj& min , const BSONObj& max , ChunkVersion version );

        /**
         * Remembers a chunk range between 'min' and 'max' as a range which will have data migrated
         * into it.  This data can then be protected against cleanup of orphaned data.
         *
         * Overlapping pending ranges will be removed, so it is only safe to use this when you know
         * your metadata view is definitive, such as at the start of a migration.
         *
         * @return false with errMsg if the range is owned by this shard
         */
        bool notePending( const string& ns,
                          const BSONObj& min,
                          const BSONObj& max,
                          const OID& epoch,
                          string* errMsg );

        /**
         * Stops tracking a chunk range between 'min' and 'max' that previously was having data
         * migrated into it.  This data is no longer protected against cleanup of orphaned data.
         *
         * To avoid removing pending ranges of other operations, ensure that this is only used when
         * a migration is still active.
         * TODO: Because migrations may currently be active when a collection drops, an epoch is
         * necessary to ensure the pending metadata change is still applicable.
         *
         * @return false with errMsg if the range is owned by the shard or the epoch of the metadata
         * has changed
         */
        bool forgetPending( const string& ns,
                            const BSONObj& min,
                            const BSONObj& max,
                            const OID& epoch,
                            string* errMsg );

        /**
         * Creates and installs a new chunk metadata for a given collection by splitting one of its
         * chunks in two or more. The version for the first split chunk should be provided. The
         * subsequent chunks' version would be the latter with the minor portion incremented.
         *
         * The effect on clients will depend on the version used. If the major portion is the same
         * as the current shards, clients shouldn't perceive the split.
         *
         * @param ns the collection
         * @param min max the chunk that should be split
         * @param splitKeys point in which to split
         * @param version at which the new metadata should be at
         */
        void splitChunk( const string& ns , const BSONObj& min , const BSONObj& max , const vector<BSONObj>& splitKeys ,
                         ChunkVersion version );

        bool inCriticalMigrateSection();

        /**
         * @return true if we are NOT in the critical section
         */
        bool waitTillNotInCriticalSection( int maxSecondsToWait );

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

        // Map from a namespace into the metadata we need for each collection on this shard
        typedef map<string,CollectionMetadataPtr> CollectionMetadataMap;
        CollectionMetadataMap _collMetadata;
    };

    extern ShardingState shardingState;

    /**
     * one per connection from mongos
     * holds version state for each namespace
     */
    class ShardedConnectionInfo {
    public:
        ShardedConnectionInfo();

        const OID& getID() const { return _id; }
        bool hasID() const { return _id.isSet(); }
        void setID( const OID& id );

        const ChunkVersion getVersion( const string& ns ) const;
        void setVersion( const string& ns , const ChunkVersion& version );

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

        typedef map<string,ChunkVersion> NSVersionMap;
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
     * Also returns an error message and the Config/ChunkVersions causing conflicts
     */
    bool shardVersionOk( const string& ns,
                         string& errmsg,
                         ChunkVersion& received,
                         ChunkVersion& wanted );

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

    /**
     * If a migration for the chunk in 'ns' where 'obj' lives is occurring, save this log entry
     * if it's relevant. The entries saved here are later transferred to the receiving side of
     * the migration. A relevant entry is an insertion, a deletion, or an update.
     */
    void logOpForSharding( const char * opstr,
                           const char * ns,
                           const BSONObj& obj,
                           BSONObj * patt,
                           const BSONObj* fullObj,
                           bool forMigrateCleanup );

    void aboutToDeleteForSharding( const StringData& ns, const Database* db , const DiskLoc& dl );

}
