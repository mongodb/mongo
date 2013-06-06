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

    CollectionMetadata* // br
    MetadataLoader::makeCollectionMetadata( const string& ns,
                                            const string& shard,
                                            const CollectionMetadata* oldMetadata,
                                            string* errMsg )
    {
        // The error message string is optional.
        string dummy;
        if ( errMsg == NULL ) {
            errMsg = &dummy;
        }

        auto_ptr<CollectionMetadata> metadata( new CollectionMetadata );
        if ( initCollection( ns, shard, oldMetadata, metadata.get(), errMsg ) ) {
            if ( metadata->getNumChunks() > 0 ) {
                dassert(metadata->isValid());
            }

            return metadata.release();
        }

        return NULL;
    }

    CollectionMetadata* MetadataLoader::makeEmptyCollectionMetadata() {
        CollectionMetadata* metadata = new CollectionMetadata;
        metadata->_collVersion = ChunkVersion( 1, 0, OID() );
        metadata->_shardVersion = ChunkVersion( 1, 0, OID() );
        dassert(metadata->isValid());
        return metadata;
    }

    bool MetadataLoader::initCollection( const string& ns,
                                         const string& shard,
                                         const CollectionMetadata* oldMetadata,
                                         CollectionMetadata* metadata,
                                         string* errMsg )
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
                *errMsg = str::stream() << "caught exception accessing the config servers "
                                        << causedBy( e );

                // We deliberately do not return conn to the pool, since it was involved
                // with the error here.

                return false;
            }
        }

        CollectionType collDoc;
        if ( !collDoc.parseBSON( collObj, errMsg ) || !collDoc.isValid( errMsg ) ) {
            return false;
        }

        //
        // Load or generate default chunks for collection config.
        //

        if ( !collDoc.getKeyPattern().isEmpty() ) {

            metadata->_keyPattern = collDoc.getKeyPattern();

            if ( !initChunks( collDoc, ns, shard, oldMetadata, metadata, errMsg ) ) {
                return false;
            }
        }
        else if ( collDoc.getPrimary() == shard ) {

            if ( shard == "" ) {
                warning() << "shard not verified, assuming collection " << ns
                          << " is unsharded on this shard" << endl;
            }

            metadata->_keyPattern = BSONObj();
            metadata->_shardVersion = ChunkVersion( 1, 0, collDoc.getEpoch() );
            metadata->_collVersion = metadata->_shardVersion;
        }
        else {
            *errMsg = str::stream() << "collection " << ns << " does not have a shard key "
                                    << "and primary " << collDoc.getPrimary()
                                    << " does not match this shard " << shard;
            return false;
        }

        return true;
    }

    bool MetadataLoader::initChunks( const CollectionType& collDoc,
                                     const string& ns,
                                     const string& shard,
                                     const CollectionMetadata* oldMetadata,
                                     CollectionMetadata* metadata,
                                     string* errMsg )
    {

        map<string, ChunkVersion> versionMap;
        metadata->_collVersion = ChunkVersion( 0, 0, collDoc.getEpoch() );

        // Check to see if we should use the old version or not.
        if ( oldMetadata ) {

            ChunkVersion oldVersion = oldMetadata->getShardVersion();

            if ( oldVersion.isSet() && oldVersion.hasCompatibleEpoch( collDoc.getEpoch() ) ) {

                // Our epoch for coll version and shard version should be the same.
                verify(oldMetadata->getCollVersion().hasCompatibleEpoch(collDoc.getEpoch()));

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
                // 'errMsg' was filled by the getChunkCursor() call.
                metadata->_collVersion = ChunkVersion();
                metadata->_chunksMap.clear();
                conn.done();
                return false;
            }

            // Diff tracker should *always* find at least one chunk if this shard owns a chunk.
            int diffsApplied = differ.calculateConfigDiff( *cursor );
            if ( diffsApplied > 0 ) {

                LOG(2) << "loaded " << diffsApplied << " chunks into new metadata for " << ns
                           << " with version " << metadata->_collVersion << endl;

                metadata->_shardVersion = versionMap[shard];
                metadata->fillRanges();
                conn.done();
                return true;
            }
            else if ( diffsApplied == 0 ) {

                warning() << "no chunks found when reloading " << ns << ", previous version was "
                          << metadata->_collVersion.toString() << endl;

                metadata->_collVersion = ChunkVersion();
                metadata->_chunksMap.clear();
                conn.done();
                return true;
            }
            else {

                // TODO: make this impossible by making sure we don't migrate / split on this
                // shard during the reload.  No chunks were found for the ns.

                *errMsg = str::stream() << "invalid chunks found when reloading " << ns
                                        << ", previous version was "
                                        << metadata->_collVersion.toString()
                                        << ", this should be rare";

                warning() << errMsg << endl;

                metadata->_collVersion = ChunkVersion();
                metadata->_chunksMap.clear();
                conn.done();
                return false;
            }
        }
        catch ( const DBException& e ) {
            *errMsg = str::stream() << "caught exception accessing the config servers"
                                    << causedBy( e );

            // We deliberately do not return connPtr to the pool, since it was involved
            // with the error here.

            return false;
        }
    }

} // namespace mongo
