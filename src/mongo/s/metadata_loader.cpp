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

#include "mongo/s/metadata_loader.h"

#include "mongo/client/connpool.h"
#include "mongo/client/dbclientcursor.h"
#include "mongo/client/dbclientmockcursor.h"
#include "mongo/s/chunk_diff.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/collection_metadata.h"
#include "mongo/s/range_arithmetic.h"
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

        // Preserve the epoch
        versionMap[shard] = metadata->_shardVersion;
        OID epoch = metadata->getCollVersion().epoch();
        bool fullReload = true;

        // Check to see if we should use the old version or not.
        if ( oldMetadata ) {

            // If our epochs are compatible, it's useful to use the old metadata for diffs
            if ( oldMetadata->getCollVersion().hasCompatibleEpoch( epoch ) ) {

                fullReload = false;
                dassert( oldMetadata->isValid() );

                versionMap[shard] = oldMetadata->_shardVersion;
                metadata->_collVersion = oldMetadata->_collVersion;

                // TODO: This could be made more efficient if copying not required, but
                // not as frequently reloaded as in mongos.
                metadata->_chunksMap = oldMetadata->_chunksMap;

                LOG( 2 ) << "loading new chunks for collection " << ns
                         << " using old metadata w/ version " << oldMetadata->getShardVersion()
                         << " and " << metadata->_chunksMap.size() << " chunks" << endl;
            }
            else {
                warning() << "reloading collection metadata for " << ns << " with new epoch "
                          << epoch.toString() << ", the current epoch is "
                          << oldMetadata->getCollVersion().epoch().toString() << endl;
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

                dassert( metadata->isValid() );
                return Status::OK();
            }
            else if ( diffsApplied == 0 ) {

                // No chunks found, the collection is dropping or we're confused
                // If this is a full reload, assume it is a drop for backwards compatibility
                // TODO: drop the config.collections entry *before* the chunks and eliminate this
                // ambiguity

                string errMsg =
                    str::stream() << "no chunks found when reloading " << ns
                                  << ", previous version was "
                                  << metadata->_collVersion.toString()
                                  << ( fullReload ? ", this is a drop" : "" );

                warning() << errMsg << endl;

                metadata->_collVersion = ChunkVersion( 0, 0, OID() );
                metadata->_chunksMap.clear();
                conn.done();

                return fullReload ? Status( ErrorCodes::NamespaceNotFound, errMsg ) :
                                    Status( ErrorCodes::RemoteChangeDetected, errMsg );
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

    Status MetadataLoader::promotePendingChunks( const CollectionMetadata* afterMetadata,
                                                 CollectionMetadata* remoteMetadata ) const {

        // Ensure pending chunks are applicable
        bool notApplicable =
            ( NULL == afterMetadata || NULL == remoteMetadata ) ||
            ( afterMetadata->getShardVersion() > remoteMetadata->getShardVersion() ) ||
            ( afterMetadata->getShardVersion().epoch() != 
                  remoteMetadata->getShardVersion().epoch() );
        if ( notApplicable ) return Status::OK();

        // The chunks from remoteMetadata are the latest version, and the pending chunks
        // from afterMetadata are the latest version.  If no trickery is afoot, pending chunks
        // should match exactly zero or one loaded chunk.

        remoteMetadata->_pendingMap = afterMetadata->_pendingMap;

        // Resolve our pending chunks against the chunks we've loaded
        for ( RangeMap::iterator it = remoteMetadata->_pendingMap.begin();
                it != remoteMetadata->_pendingMap.end(); ) {

            if ( !rangeMapOverlaps( remoteMetadata->_chunksMap, it->first, it->second ) ) {
                ++it;
                continue;
            }

            // Our pending range overlaps at least one chunk

            if ( rangeMapContains( remoteMetadata->_chunksMap, it->first, it->second ) ) {

                // Chunk was promoted from pending, successful migration
                LOG( 2 ) << "verified chunk " << rangeToString( it->first, it->second )
                         << " was migrated earlier to this shard" << endl;

                remoteMetadata->_pendingMap.erase( it++ );
            }
            else {

                // Something strange happened, maybe manual editing of config?
                RangeVector overlap;
                getRangeMapOverlap( remoteMetadata->_chunksMap,
                                    it->first,
                                    it->second,
                                    &overlap );

                string errMsg = str::stream()
                    << "the remote metadata changed unexpectedly, pending range "
                    << rangeToString( it->first, it->second )
                    << " does not exactly overlap loaded chunks "
                    << overlapToString( overlap );

                return Status( ErrorCodes::RemoteChangeDetected, errMsg );
            }
        }

        return Status::OK();
    }


} // namespace mongo
