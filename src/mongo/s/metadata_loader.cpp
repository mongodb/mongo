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

    MetadataLoader::MetadataLoader( const ConnectionString& configLoc ) :
            _configLoc( configLoc )
    {
    }

    MetadataLoader::~MetadataLoader() {
    }

    Status MetadataLoader::makeCollectionMetadata( const string& ns,
                                                   const string& shard,
                                                   const CollectionMetadata* oldMetadata,
                                                   CollectionMetadata* metadata ) const
    {
        Status status = initCollection( ns, shard, metadata );
        if ( !status.isOK() || metadata->getKeyPattern().isEmpty() ) return status;
        return initChunks( ns, shard, oldMetadata, metadata );
    }

    Status MetadataLoader::initCollection( const string& ns,
                                           const string& shard,
                                           CollectionMetadata* metadata ) const
    {
        //
        // Bring collection entry from the config server.
        //

        BSONObj collDoc;
        {
            try {
                ScopedDbConnection conn( _configLoc.toString(), 30 );
                collDoc = conn->findOne( CollectionType::ConfigNS, QUERY(CollectionType::ns()<<ns));
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

        string errMsg;
        if ( collDoc.isEmpty() ) {

            errMsg = str::stream() << "could not load metadata, collection " << ns << " not found";
            warning() << errMsg << endl;

            return Status( ErrorCodes::NamespaceNotFound, errMsg );
        }

        CollectionType collInfo;
        if ( !collInfo.parseBSON( collDoc, &errMsg ) || !collInfo.isValid( &errMsg ) ) {

            errMsg = str::stream() << "could not parse metadata for collection " << ns
                                   << causedBy( errMsg );
            warning() << errMsg << endl;

            return Status( ErrorCodes::FailedToParse, errMsg );
        }

        if ( collInfo.isDroppedSet() && collInfo.getDropped() ) {

            errMsg = str::stream() << "could not load metadata, collection " << ns
                                   << " was dropped";
            warning() << errMsg << endl;

            return Status( ErrorCodes::NamespaceNotFound, errMsg );
        }

        if ( collInfo.isKeyPatternSet() && !collInfo.getKeyPattern().isEmpty() ) {

            // Sharded collection, need to load chunks

            metadata->_keyPattern = collInfo.getKeyPattern();
            metadata->_shardVersion = ChunkVersion( 0, 0, collInfo.getEpoch() );
            metadata->_collVersion = ChunkVersion( 0, 0, collInfo.getEpoch() );

            return Status::OK();
        }
        else if ( collInfo.isPrimarySet() && collInfo.getPrimary() == shard ) {

            // A collection with a non-default primary

            // Empty primary field not allowed if set
            dassert( collInfo.getPrimary() != "" );

            metadata->_keyPattern = BSONObj();
            metadata->_shardVersion = ChunkVersion( 1, 0, collInfo.getEpoch() );
            metadata->_collVersion = metadata->_shardVersion;

            return Status::OK();
        }
        else {

            // A collection with a primary that doesn't match this shard or is empty, the primary
            // may have changed before we loaded.

            errMsg = // br
                    str::stream() << "collection " << ns << " does not have a shard key "
                                  << "and primary "
                                  << ( collInfo.isPrimarySet() ? collInfo.getPrimary() : "" )
                                  << " does not match this shard " << shard;

            warning() << errMsg << endl;

            metadata->_collVersion = ChunkVersion( 0, 0, OID() );

            return Status( ErrorCodes::RemoteChangeDetected, errMsg );
        }
    }

    Status MetadataLoader::initChunks( const string& ns,
                                       const string& shard,
                                       const CollectionMetadata* oldMetadata,
                                       CollectionMetadata* metadata ) const
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
        else {
            // Preserve the epoch
            versionMap[shard] = metadata->_shardVersion;
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

                // Make our metadata invalid
                metadata->_collVersion = ChunkVersion( 0, 0, OID() );
                metadata->_chunksMap.clear();
                conn.done();

                return Status( ErrorCodes::HostUnreachable,
                               "problem opening chunk metadata cursor" );
            }

            //
            // The diff tracker should always find at least one chunk (the highest chunk we saw
            // last time).  If not, something has changed on the config server (potentially between
            // when we read the collection data and when we read the chunks data).
            //

            int diffsApplied = differ.calculateConfigDiff( *cursor );
            if ( diffsApplied > 0 ) {

                // Chunks found, return ok

                LOG(2) << "loaded " << diffsApplied << " chunks into new metadata for " << ns
                           << " with version " << metadata->_collVersion << endl;

                metadata->_shardVersion = versionMap[shard];
                metadata->fillRanges();
                conn.done();

                return Status::OK();
            }
            else if ( diffsApplied == 0 ) {

                // No chunks found, something changed or we're confused

                string errMsg = // br
                        str::stream() << "no chunks found when reloading " << ns
                                      << ", previous version was "
                                      << metadata->_collVersion.toString();

                warning() << errMsg << endl;

                metadata->_collVersion = ChunkVersion( 0, 0, OID() );
                metadata->_chunksMap.clear();
                conn.done();

                return Status( ErrorCodes::RemoteChangeDetected, errMsg );
            }
            else {

                // Invalid chunks found, our epoch may have changed because we dropped/recreated
                // the collection.

                string errMsg = // br
                        str::stream() << "invalid chunks found when reloading " << ns
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
