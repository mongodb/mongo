/**
 *    Copyright (C) 2012 10gen Inc.
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

#include "mongo/s/metadata_loader.h"

#include "mongo/client/connpool.h"
#include "mongo/client/dbclientcursor.h"
#include "mongo/client/dbclientmockcursor.h"
#include "mongo/s/chunk_diff.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/collection_metadata.h"
#include "mongo/s/type_chunk.h"
#include "mongo/s/type_collection.h"

namespace mongo {

    /**
     * This is an adapter so we can use config diffs - mongos and mongod do them slightly
     * differently.
     *
     * The mongod adapter here tracks only a single shard, and stores ranges by (min, max).
     */
    class SCMConfigDiffTracker : public ConfigDiffTracker<BSONObj, string> {
    public:
        SCMConfigDiffTracker( const string& currShard ) :
                _currShard( currShard )
        {
        }

        virtual bool isTracked( const BSONObj& chunkDoc ) const {
            return chunkDoc["shard"].type() == String && chunkDoc["shard"].String() == _currShard;
        }

        virtual BSONObj maxFrom( const BSONObj& val ) const {
            return val;
        }

        virtual pair<BSONObj, BSONObj> rangeFor( const BSONObj& chunkDoc,
                                                 const BSONObj& min,
                                                 const BSONObj& max ) const
        {
            return make_pair( min, max );
        }

        virtual string shardFor( const string& name ) const {
            return name;
        }

        virtual string nameFrom( const string& shard ) const {
            return shard;
        }

        string _currShard;
    };

    //
    // MetadataLoader implementation
    //

    MetadataLoader::MetadataLoader( ConnectionString configLoc ) :
            _configLoc( configLoc )
    {
    }

    MetadataLoader::~MetadataLoader() {
    }

    Status MetadataLoader::makeCollectionMetadata( const string& ns,
                                                   const string& shard,
                                                   const CollectionMetadata* oldMetadata,
                                                   CollectionMetadata* metadata )
    {
        Status status = initCollection( ns, shard, metadata );
        if ( !status.isOK() || metadata->getKeyPattern().isEmpty() ) return status;
        return initChunks( ns, shard, oldMetadata, metadata );
    }

    Status MetadataLoader::initCollection( const string& ns,
                                           const string& shard,
                                           CollectionMetadata* metadata )
    {
        //
        // Bring collection entry from the config server.
        //

        BSONObj collObj;
        {
            try {
                ScopedDbConnection conn( _configLoc.toString(), 30 );
                collObj = conn->findOne( CollectionType::ConfigNS, QUERY(CollectionType::ns()<<ns));
                conn.done();
            }
            catch ( const DBException& e ) {
                string errMsg = str::stream() << "could not query collection metadata"
                                              << causedBy( e );

                // We deliberately do not return conn to the pool, since it was involved
                // with the error here.

                return Status( ErrorCodes::HostUnreachable, errMsg );
            }
        }

        CollectionType collDoc;
        string errMsg;
        if ( !collDoc.parseBSON( collObj, &errMsg ) || !collDoc.isValid( &errMsg ) ) {
            return Status( ErrorCodes::FailedToParse, errMsg );
        }

        //
        // Load or generate default chunks for collection config.
        //

        if ( collDoc.isKeyPatternSet() && !collDoc.getKeyPattern().isEmpty() ) {

            metadata->_keyPattern = collDoc.getKeyPattern();
            metadata->_shardVersion = ChunkVersion( 0, 0, collDoc.getEpoch() );
            metadata->_collVersion = ChunkVersion( 0, 0, collDoc.getEpoch() );

            return Status::OK();
        }
        else if ( collDoc.isPrimarySet() && collDoc.getPrimary() == shard ) {

            if ( shard == "" ) {
                warning() << "shard not verified, assuming collection " << ns
                          << " is unsharded on this shard" << endl;
            }

            metadata->_keyPattern = BSONObj();
            metadata->_shardVersion = ChunkVersion( 1, 0, collDoc.getEpoch() );
            metadata->_collVersion = metadata->_shardVersion;

            return Status::OK();
        }
        else {
            errMsg = str::stream() << "collection " << ns << " does not have a shard key "
                    << "and primary " << ( collDoc.isPrimarySet() ? collDoc.getPrimary() : "" )
                    << " does not match this shard " << shard;
            return Status( ErrorCodes::RemoteChangeDetected, errMsg );
        }
    }

    Status MetadataLoader::initChunks( const string& ns,
                                       const string& shard,
                                       const CollectionMetadata* oldMetadata,
                                       CollectionMetadata* metadata )
    {
        map<string, ChunkVersion> versionMap;
        OID epoch = metadata->getCollVersion().epoch();

        // Check to see if we should use the old version or not.
        if ( oldMetadata ) {

            ChunkVersion oldVersion = oldMetadata->getShardVersion();

            if ( oldVersion.isSet() && oldVersion.hasCompatibleEpoch( epoch ) ) {

                // Our epoch for coll version and shard version should be the same.
                verify( oldMetadata->getCollVersion().hasCompatibleEpoch( epoch ) );

                versionMap[shard] = oldMetadata->_shardVersion;
                metadata->_collVersion = oldMetadata->_collVersion;

                // TODO: This could be made more efficient if copying not required, but
                // not as frequently reloaded as in mongos.
                metadata->_chunksMap = oldMetadata->_chunksMap;

                LOG(2) << "loading new chunks for collection " << ns
                              << " using old metadata w/ version " << oldMetadata->getShardVersion()
                              << " and " << metadata->_chunksMap.size() << " chunks" << endl;
            }
        }

        // Exposes the new metadata's range map and version to the "differ," who
        // would ultimately be responsible of filling them up.
        SCMConfigDiffTracker differ( shard );
        differ.attach( ns, metadata->_chunksMap, metadata->_collVersion, versionMap );

        try {

            ScopedDbConnection conn( _configLoc.toString(), 30 );

            auto_ptr<DBClientCursor> cursor = conn->query( ChunkType::ConfigNS,
                                                           differ.configDiffQuery() );

            if ( !cursor.get() ) {
                metadata->_collVersion = ChunkVersion();
                metadata->_chunksMap.clear();
                conn.done();
                return Status( ErrorCodes::HostUnreachable,
                               "problem opening chunk metadata cursor" );
            }

            // Diff tracker should *always* find at least one chunk if this shard owns a chunk.
            int diffsApplied = differ.calculateConfigDiff( *cursor );
            if ( diffsApplied > 0 ) {

                LOG(2) << "loaded " << diffsApplied << " chunks into new metadata for " << ns
                           << " with version " << metadata->_collVersion << endl;

                metadata->_shardVersion = versionMap[shard];
                metadata->fillRanges();
                conn.done();
                return Status::OK();
            }
            else if ( diffsApplied == 0 ) {

                warning() << "no chunks found when reloading " << ns << ", previous version was "
                          << metadata->_collVersion.toString() << endl;

                metadata->_collVersion = ChunkVersion( 0, 0, OID() );
                metadata->_chunksMap.clear();
                conn.done();
                return Status::OK();
            }
            else {

                // TODO: make this impossible by making sure we don't migrate / split on this
                // shard during the reload.  No chunks were found for the ns.

                string errMsg = str::stream() << "invalid chunks found when reloading " << ns
                                              << ", previous version was "
                                              << metadata->_collVersion.toString()
                                              << ", this should be rare";

                warning() << errMsg << endl;

                metadata->_collVersion = ChunkVersion( 0, 0, OID() );
                metadata->_chunksMap.clear();
                conn.done();
                return Status( ErrorCodes::RemoteChangeDetected, errMsg );
            }
        }
        catch ( const DBException& e ) {
            string errMsg = str::stream() << "problem querying chunks metadata" << causedBy( e );

            // We deliberately do not return connPtr to the pool, since it was involved
            // with the error here.

            return Status( ErrorCodes::HostUnreachable, errMsg );
        }
    }

} // namespace mongo
