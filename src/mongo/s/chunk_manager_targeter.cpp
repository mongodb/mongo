/**
 *    Copyright (C) 2013 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/s/chunk_manager_targeter.h"

#include "mongo/s/config.h"
#include "mongo/s/grid.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    using mongoutils::str::stream;

    /**
     * Helper to get the DBConfigPtr object in an exception-safe way.
     */
    static bool getDBConfigSafe( const StringData& db, DBConfigPtr& config, string* errMsg ) {
        try {
            config = grid.getDBConfig( db, true );
            if ( !config ) *errMsg = stream() << "could not load or create database " << db;
        }
        catch ( const DBException& ex ) {
            *errMsg = ex.toString();
        }

        return config;
    }

    ChunkManagerTargeter::ChunkManagerTargeter() :
            _needsTargetingRefresh( false ), _stats( new TargeterStats ) {
    }

    Status ChunkManagerTargeter::init( const NamespaceString& nss ) {

        _nss = nss;

        //
        // Get the latest metadata information from the cache
        //

        DBConfigPtr config;

        string errMsg;
        if ( !getDBConfigSafe( _nss.db(), config, &errMsg ) ) {
            return Status( ErrorCodes::DatabaseNotFound, errMsg );
        }

        // Get either the chunk manager or primary shard
        config->getChunkManagerOrPrimary( _nss.ns(), _manager, _primary );

        return Status::OK();
    }

    const NamespaceString& ChunkManagerTargeter::getNS() const {
        return _nss;
    }

    Status ChunkManagerTargeter::targetDoc( const BSONObj& doc, ShardEndpoint** endpoint ) const {

        if ( !_primary && !_manager ) return Status( ErrorCodes::NamespaceNotFound, "" );

        if ( _manager ) {
            if ( !_manager->hasShardKey( doc ) ) {
                return Status( ErrorCodes::ShardKeyNotFound,
                               stream() << "document " << doc
                                        << " does not contain shard key for pattern "
                                        << _manager->getShardKey().key() );
            }
            ChunkPtr chunk = _manager->findChunkForDoc( doc );
            *endpoint = new ShardEndpoint( chunk->getShard().getName(),
                                           _manager->getVersion( chunk->getShard() ),
                                           chunk->getShard().getAddress() );

            _stats->chunkSizeDelta[chunk->getMin()] += doc.objsize();
        }
        else {
            *endpoint = new ShardEndpoint( _primary->getName(),
                                           ChunkVersion::UNSHARDED(),
                                           _primary->getAddress() );
        }

        return Status::OK();
    }

    Status ChunkManagerTargeter::targetQuery( const BSONObj& query,
                                              vector<ShardEndpoint*>* endpoints ) const {

        if ( !_primary && !_manager ) return Status( ErrorCodes::NamespaceNotFound, "" );

        set<Shard> shards;
        if ( _manager ) {
            _manager->getShardsForQuery( shards, query );
        }
        else {
            shards.insert( *_primary );
        }

        for ( set<Shard>::iterator it = shards.begin(); it != shards.end(); ++it ) {
            endpoints->push_back( new ShardEndpoint( it->getName(),
                                                     _manager ?
                                                         _manager->getVersion( *it ) :
                                                         ChunkVersion::UNSHARDED(),
                                                     it->getAddress() ) );
        }

        return Status::OK();
    }

    void ChunkManagerTargeter::noteStaleResponse( const ShardEndpoint& endpoint,
                                                  const BSONObj& staleInfo ) {
        dassert( !_needsTargetingRefresh );

        ChunkVersion remoteShardVersion = ChunkVersion::fromBSON( staleInfo, "vWanted" );

        // We assume here that we can't have more than one stale config per-shard
        dassert( _remoteShardVersions.find( endpoint.shardName ) == _remoteShardVersions.end() );
        _remoteShardVersions.insert( make_pair( endpoint.shardName, remoteShardVersion ) );
    }

    void ChunkManagerTargeter::noteCouldNotTarget() {
        dassert( _remoteShardVersions.empty() );
        _needsTargetingRefresh = true;
    }

    const TargeterStats* ChunkManagerTargeter::getStats() const {
        return _stats.get();
    }

    namespace {

        //
        // Utilities to compare shard versions
        //

        enum CompareResult {
            CompareResult_Unknown, CompareResult_GTE, CompareResult_LT
        };

        /**
         * Returns the relationship of two shard versions.  Shard versions of a collection that has
         * not been dropped and recreated and where there is at least one chunk on a shard are
         * comparable, otherwise the result is ambiguous.
         */
        CompareResult compareShardVersions( const ChunkVersion& shardVersionA,
                                            const ChunkVersion& shardVersionB ) {

            // Collection may have been dropped
            if ( !shardVersionA.hasCompatibleEpoch( shardVersionB ) ) return CompareResult_Unknown;

            // Zero shard versions are only comparable to themselves
            if ( !shardVersionA.isSet() || !shardVersionB.isSet() ) {
                // If both are zero...
                if ( !shardVersionA.isSet() && !shardVersionB.isSet() ) return CompareResult_GTE;
                // Otherwise...
                return CompareResult_Unknown;
            }

            if ( shardVersionA < shardVersionB ) return CompareResult_LT;
            else return CompareResult_GTE;
        }

        // NOTE: CAN THROW, since Shard() throws
        ChunkVersion getShardVersion( const string& shardName,
                                      const ChunkManagerPtr& manager,
                                      const ShardPtr& primary ) {

            dassert( !( manager && primary ) );
            dassert( manager || primary );

            if ( primary ) return ChunkVersion::UNSHARDED();

            return manager->getVersion( Shard( shardName ) );
        }

        /**
         * Returns the relationship between two maps of shard versions.  As above, these maps are
         * often comparable when the collection has not been dropped and there is at least one
         * chunk on the shards.
         *
         * If any versions in the maps are not comparable, the result is _Unknown.
         *
         * If any versions in the first map (cached) are _LT the versions in the second map
         * (remote), the first (cached) versions are _LT the second (remote) versions.
         *
         * Note that the signature here is weird since our cached map of chunk versions is
         * stored in a ChunkManager or is implicit in the primary shard of the collection.
         */
        CompareResult //
        compareAllShardVersions( const ChunkManagerPtr& cachedShardVersions,
                                 const ShardPtr& cachedPrimary,
                                 const map<string, ChunkVersion>& remoteShardVersions ) {

            CompareResult finalResult = CompareResult_GTE;

            for ( map<string, ChunkVersion>::const_iterator it = remoteShardVersions.begin();
                it != remoteShardVersions.end(); ++it ) {

                //
                // Get the remote and cached version for the next shard
                //

                const string& shardName = it->first;
                const ChunkVersion& remoteShardVersion = it->second;
                ChunkVersion cachedShardVersion;

                try {
                    // Throws b/c shard constructor throws
                    cachedShardVersion = getShardVersion( shardName,
                                                          cachedShardVersions,
                                                          cachedPrimary );
                }
                catch ( const DBException& ex ) {

                    warning() << "could not lookup shard " << shardName
                              << " in local cache, shard metadata may have changed"
                              << " or be unavailable" << causedBy( ex ) << endl;

                    return CompareResult_Unknown;
                }

                //
                // Compare the remote and cached versions
                //

                CompareResult result = compareShardVersions( cachedShardVersion,
                                                             remoteShardVersion );

                if ( result == CompareResult_Unknown ) return result;
                if ( result == CompareResult_LT ) finalResult = CompareResult_LT;

                // Note that we keep going after _LT b/c there could be more _Unknowns.
            }

            return finalResult;
        }

        /**
         * Whether or not the manager/primary pair is different from the other manager/primary pair
         */
        bool wasMetadataRefreshed( const ChunkManagerPtr& managerA,
                                   const ShardPtr& primaryA,
                                   const ChunkManagerPtr& managerB,
                                   const ShardPtr& primaryB ) {

            if ( ( managerA && !managerB ) || ( !managerA && managerB ) || ( primaryA && !primaryB )
                 || ( !primaryA && primaryB ) ) return true;

            if ( managerA ) {
                return managerA->getSequenceNumber() != managerB->getSequenceNumber();
            }

            dassert( NULL != primaryA.get() );
            return primaryA->getName() != primaryB->getName();
        }
    }

    Status ChunkManagerTargeter::refreshIfNeeded() {

        //
        // Did we have any stale config or targeting errors at all?
        //

        if ( !_needsTargetingRefresh && _remoteShardVersions.empty() ) return Status::OK();

        //
        // Get the latest metadata information from the cache if there were issues
        //

        ChunkManagerPtr lastManager = _manager;
        ShardPtr lastPrimary = _primary;

        DBConfigPtr config;

        string errMsg;
        if ( !getDBConfigSafe( _nss.db(), config, &errMsg ) ) {
            return Status( ErrorCodes::DatabaseNotFound, errMsg );
        }

        // Get either the chunk manager or primary shard
        config->getChunkManagerOrPrimary( _nss.ns(), _manager, _primary );

        // We now have the latest metadata from the cache.

        //
        // See if and how we need to do a remote refresh.
        // Either we couldn't target at all, or we have stale versions, but not both.
        //

        dassert( !( _needsTargetingRefresh && !_remoteShardVersions.empty() ) );

        if ( _needsTargetingRefresh ) {

            // Reset the field
            _needsTargetingRefresh = false;

            // If we couldn't target, we might need to refresh if we haven't remotely refreshed the
            // metadata since we last got it from the cache.

            bool alreadyRefreshed = wasMetadataRefreshed( lastManager,
                                                          lastPrimary,
                                                          _manager,
                                                          _primary );

            // If didn't already refresh the targeting information, refresh it
            if ( !alreadyRefreshed ) {
                // To match previous behavior, we just need an incremental refresh here
                return refreshNow( RefreshType_RefreshChunkManager );
            }

            return Status::OK();
        }
        else if ( !_remoteShardVersions.empty() ) {

            // If we got stale shard versions from remote shards, we may need to refresh
            // NOTE: Not sure yet if this can happen simultaneously with targeting issues

            CompareResult result = compareAllShardVersions( _manager,
                                                            _primary,
                                                            _remoteShardVersions );
            // Reset the versions
            _remoteShardVersions.clear();

            if ( result == CompareResult_Unknown ) {
                // Our current shard versions aren't all comparable to the old versions, maybe drop
                return refreshNow( RefreshType_ReloadDatabase );
            }
            else if ( result == CompareResult_LT ) {
                // Our current shard versions are less than the remote versions, but no drop
                return refreshNow( RefreshType_RefreshChunkManager );
            }

            return Status::OK();
        }

        // unreachable
        dassert( false );
        return Status::OK();
    }

    Status ChunkManagerTargeter::refreshNow( RefreshType refreshType ) {

        DBConfigPtr config;

        string errMsg;
        if ( !getDBConfigSafe( _nss.db(), config, &errMsg ) ) {
            return Status( ErrorCodes::DatabaseNotFound, errMsg );
        }

        // TODO: Improve synchronization and make more explicit
        if ( refreshType == RefreshType_RefreshChunkManager ) {
            try {
                // Forces a remote check of the collection info, synchronization between threads
                // happens internally.
                config->getChunkManagerIfExists( _nss.ns(), true );
            }
            catch ( const DBException& ex ) {
                return Status( ErrorCodes::UnknownError, ex.toString() );
            }
            config->getChunkManagerOrPrimary( _nss.ns(), _manager, _primary );
        }
        else if ( refreshType == RefreshType_ReloadDatabase ) {
            try {
                // Dumps the db info, reloads it all, synchronization between threads happens
                // internally.
                config->reload();
                config->getChunkManagerIfExists( _nss.ns(), true, true );
            }
            catch ( const DBException& ex ) {
                return Status( ErrorCodes::UnknownError, ex.toString() );
            }
            config->getChunkManagerOrPrimary( _nss.ns(), _manager, _primary );
        }

        return Status::OK();
    }

} // namespace mongo

