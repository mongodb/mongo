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

        return config.get();
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

    Status ChunkManagerTargeter::targetInsert( const BSONObj& doc,
                                               ShardEndpoint** endpoint ) const {

        if ( !_primary && !_manager )  {
            return Status( ErrorCodes::NamespaceNotFound,
                           str::stream() << "could not target insert in collection "
                                         << getNS().ns()
                                         << "; no metadata found" );
        }

        if ( _primary ) {
            *endpoint = new ShardEndpoint( _primary->getName(),
                                           ChunkVersion::UNSHARDED() );
        }
        else {

            //
            // Sharded collections have the following requirements for targeting:
            //
            // Inserts must contain the exact shard key.
            //

            if ( !_manager->hasShardKey( doc ) ) {
                return Status( ErrorCodes::ShardKeyNotFound,
                               stream() << "document " << doc
                                        << " does not contain shard key for pattern "
                                        << _manager->getShardKey().key() );
            }

            ChunkPtr chunk = _manager->findChunkForDoc( doc );
            *endpoint = new ShardEndpoint( chunk->getShard().getName(),
                                           _manager->getVersion( chunk->getShard() ) );

            // Track autosplit stats for sharded collections
            _stats->chunkSizeDelta[chunk->getMin()] += doc.objsize();
        }

        return Status::OK();
    }

    namespace {

        // TODO: Expose these for unit testing via dbtests

        enum UpdateType {
            UpdateType_Replacement, UpdateType_OpStyle, UpdateType_Unknown
        };

        /**
         * There are two styles of update expressions:
         * coll.update({ x : 1 }, { y : 2 }) // Replacement style
         * coll.update({ x : 1 }, { $set : { y : 2 } }) // OpStyle
         */
        UpdateType getUpdateExprType( const BSONObj& updateExpr ) {

            UpdateType updateType = UpdateType_Unknown;

            // Empty update is replacement-style, by default
            if ( updateExpr.isEmpty() ) return UpdateType_Replacement;

            BSONObjIterator it( updateExpr );
            while ( it.more() ) {
                BSONElement next = it.next();

                if ( next.fieldName()[0] == '$' ) {
                    if ( updateType == UpdateType_Unknown ) {
                        updateType = UpdateType_OpStyle;
                    }
                    else if ( updateType == UpdateType_Replacement ) {
                        return UpdateType_Unknown;
                    }
                }
                else {
                    if ( updateType == UpdateType_Unknown ) {
                        updateType = UpdateType_Replacement;
                    }
                    else if ( updateType == UpdateType_OpStyle ) {
                        return UpdateType_Unknown;
                    }
                }
            }

            return updateType;
        }

        /**
         * This returns "does the query have an _id field" and "is the _id field
         * querying for a direct value like _id : 3 and not _id : { $gt : 3 }"
         *
         * Ex: { _id : 1 } => true
         *     { foo : <anything>, _id : 1 } => true
         *     { _id : { $lt : 30 } } => false
         *     { foo : <anything> } => false
         */
        bool isExactIdQuery( const BSONObj& query ) {
            return query.hasField( "_id" ) && getGtLtOp( query["_id"] ) == BSONObj::Equality;
        }
    }

    Status ChunkManagerTargeter::targetUpdate( const BatchedUpdateDocument& updateDoc,
                                               vector<ShardEndpoint*>* endpoints ) const {

        //
        // Update targeting may use either the query or the update.  This is to support save-style
        // updates, of the form:
        //
        // coll.update({ _id : xxx }, { _id : xxx, shardKey : 1, foo : bar }, { upsert : true })
        //
        // Because drivers do not know the shard key, they can't pull the shard key automatically
        // into the query doc, and to correctly support upsert we must target a single shard.
        //
        // The rule is simple - If the update is replacement style (no '$set'), we target using the
        // update.  If the update is replacement style, we target using the query.
        //

        BSONObj query = updateDoc.getQuery();
        BSONObj updateExpr = updateDoc.getUpdateExpr();

        UpdateType updateType = getUpdateExprType( updateDoc.getUpdateExpr() );

        if ( updateType == UpdateType_Unknown ) {
            return Status( ErrorCodes::UnsupportedFormat,
                           stream() << "update document " << updateExpr
                                    << " has mixed $operator and non-$operator style fields" );
        }

        BSONObj targetedDoc = updateType == UpdateType_OpStyle ? query : updateExpr;

        bool exactShardKeyQuery = false;

        if ( _manager ) {

            //
            // Sharded collections have the following futher requirements for targeting:
            //
            // Upserts must be targeted exactly by shard key.
            // Non-multi updates must be targeted exactly by shard key *or* exact _id.
            //

            exactShardKeyQuery = _manager->hasTargetableShardKey(targetedDoc);

            if ( updateDoc.getUpsert() && !exactShardKeyQuery ) {
                return Status( ErrorCodes::ShardKeyNotFound,
                               stream() << "upsert " << updateDoc.toBSON()
                                        << " does not contain shard key for pattern "
                                        << _manager->getShardKey().key() );
            }

            bool exactIdQuery = isExactIdQuery( updateDoc.getQuery() );

            if ( !updateDoc.getMulti() && !exactShardKeyQuery && !exactIdQuery ) {
                return Status( ErrorCodes::ShardKeyNotFound,
                               stream() << "update " << updateDoc.toBSON()
                                        << " does not contain _id or shard key for pattern "
                                        << _manager->getShardKey().key() );
            }

            // Track autosplit stats for sharded collections
            // Note: this is only best effort accounting and is not accurate.
            if ( exactShardKeyQuery ) {
                ChunkPtr chunk = _manager->findChunkForDoc(targetedDoc);
                _stats->chunkSizeDelta[chunk->getMin()] +=
                    ( query.objsize() + updateExpr.objsize() );
            }
        }

        Status result = Status::OK();
        if (exactShardKeyQuery) {
            // We can't rely on our query targeting to be exact
            ShardEndpoint* endpoint = NULL;
            result = targetShardKey(targetedDoc, &endpoint);
            endpoints->push_back(endpoint);

            invariant(result.isOK());
            invariant(NULL != endpoint);
        }
        else {
            result = targetQuery(targetedDoc, endpoints);
        }

        return result;
    }

    Status ChunkManagerTargeter::targetDelete( const BatchedDeleteDocument& deleteDoc,
                                               vector<ShardEndpoint*>* endpoints ) const {

        bool exactShardKeyQuery = false;

        if ( _manager ) {

            //
            // Sharded collections have the following further requirements for targeting:
            //
            // Limit-1 deletes must be targeted exactly by shard key *or* exact _id
            //

            exactShardKeyQuery = _manager->hasTargetableShardKey(deleteDoc.getQuery());
            bool exactIdQuery = isExactIdQuery( deleteDoc.getQuery() );

            if ( deleteDoc.getLimit() == 1 && !exactShardKeyQuery && !exactIdQuery ) {
                return Status( ErrorCodes::ShardKeyNotFound,
                               stream() << "delete " << deleteDoc.toBSON()
                                        << " does not contain _id or shard key for pattern "
                                        << _manager->getShardKey().key() );
            }
        }

        Status result = Status::OK();
        if (exactShardKeyQuery) {
            // We can't rely on our query targeting to be exact
            ShardEndpoint* endpoint = NULL;
            result = targetShardKey(deleteDoc.getQuery(), &endpoint);
            endpoints->push_back(endpoint);

            invariant(result.isOK());
            invariant(NULL != endpoint);
        }
        else {
            result = targetQuery(deleteDoc.getQuery(), endpoints);
        }

        return result;
    }

    Status ChunkManagerTargeter::targetQuery( const BSONObj& query,
                                              vector<ShardEndpoint*>* endpoints ) const {

        if ( !_primary && !_manager ) {
            return Status( ErrorCodes::NamespaceNotFound,
                           str::stream() << "could not target query in "
                                         << getNS().ns()
                                         << "; no metadata found" );
        }

        set<Shard> shards;
        if ( _manager ) {
            try {
                _manager->getShardsForQuery( shards, query );
            } catch ( const DBException& ex ) {
                return ex.toStatus();
            }
        }
        else {
            shards.insert( *_primary );
        }

        for ( set<Shard>::iterator it = shards.begin(); it != shards.end(); ++it ) {
            endpoints->push_back( new ShardEndpoint( it->getName(),
                                                     _manager ?
                                                         _manager->getVersion( *it ) :
                                                         ChunkVersion::UNSHARDED() ) );
        }

        return Status::OK();
    }

    Status ChunkManagerTargeter::targetShardKey(const BSONObj& doc,
                                                ShardEndpoint** endpoint) const {

        invariant(NULL != _manager);
        dassert(_manager->hasShardKey(doc));

        ChunkPtr chunk = _manager->findChunkForDoc(doc);

        Shard shard = chunk->getShard();
        *endpoint = new ShardEndpoint(shard.getName(),
                                      _manager->getVersion(StringData(shard.getName())));

        return Status::OK();
    }

    Status ChunkManagerTargeter::targetCollection( vector<ShardEndpoint*>* endpoints ) const {

        if ( !_primary && !_manager ) {
            return Status( ErrorCodes::NamespaceNotFound,
                           str::stream() << "could not target full range of "
                                         << getNS().ns()
                                         << "; metadata not found" );
        }

        set<Shard> shards;
        if ( _manager ) {
            _manager->getAllShards( shards );
        }
        else {
            shards.insert( *_primary );
        }

        for ( set<Shard>::iterator it = shards.begin(); it != shards.end(); ++it ) {
            endpoints->push_back( new ShardEndpoint( it->getName(),
                                                     _manager ?
                                                         _manager->getVersion( *it ) :
                                                         ChunkVersion::UNSHARDED() ) );
        }

        return Status::OK();
    }

    Status ChunkManagerTargeter::targetAllShards( vector<ShardEndpoint*>* endpoints ) const {

        if ( !_primary && !_manager ) {
            return Status( ErrorCodes::NamespaceNotFound,
                           str::stream() << "could not target every shard with versions for "
                                         << getNS().ns()
                                         << "; metadata not found" );
        }

        vector<Shard> shards;
        Shard::getAllShards( shards );

        for ( vector<Shard>::iterator it = shards.begin(); it != shards.end(); ++it ) {
            endpoints->push_back( new ShardEndpoint( it->getName(),
                                                     _manager ?
                                                         _manager->getVersion( *it ) :
                                                         ChunkVersion::UNSHARDED() ) );
        }

        return Status::OK();
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
            if ( !shardVersionA.hasEqualEpoch( shardVersionB ) ) return CompareResult_Unknown;

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

        ChunkVersion getShardVersion( const StringData& shardName,
                                      const ChunkManagerPtr& manager,
                                      const ShardPtr& primary ) {

            dassert( !( manager && primary ) );
            dassert( manager || primary );

            if ( primary ) return ChunkVersion::UNSHARDED();

            return manager->getVersion( shardName );
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
        bool isMetadataDifferent( const ChunkManagerPtr& managerA,
                                  const ShardPtr& primaryA,
                                  const ChunkManagerPtr& managerB,
                                  const ShardPtr& primaryB ) {

            if ( ( managerA && !managerB ) || ( !managerA && managerB ) || ( primaryA && !primaryB )
                 || ( !primaryA && primaryB ) ) return true;

            if ( managerA ) {
                return !managerA->getVersion().isStrictlyEqualTo( managerB->getVersion() );
            }

            dassert( NULL != primaryA.get() );
            return primaryA->getName() != primaryB->getName();
        }

        /**
         * Whether or not the manager/primary pair was changed or refreshed from a previous version
         * of the metadata.
         */
        bool wasMetadataRefreshed( const ChunkManagerPtr& managerA,
                                   const ShardPtr& primaryA,
                                   const ChunkManagerPtr& managerB,
                                   const ShardPtr& primaryB ) {

            if ( isMetadataDifferent( managerA, primaryA, managerB, primaryB ) )
                return true;

            if ( managerA ) {
                dassert( managerB.get() ); // otherwise metadata would be different
                return managerA->getSequenceNumber() != managerB->getSequenceNumber();
            }

            return false;
        }
    }

    void ChunkManagerTargeter::noteStaleResponse( const ShardEndpoint& endpoint,
                                                  const BSONObj& staleInfo ) {
        dassert( !_needsTargetingRefresh );

        ChunkVersion remoteShardVersion;
        if ( staleInfo["vWanted"].eoo() ) {
            // If we don't have a vWanted sent, assume the version is higher than our current
            // version.
            remoteShardVersion = getShardVersion( endpoint.shardName, _manager, _primary );
            remoteShardVersion.incMajor();
        }
        else {
            remoteShardVersion = ChunkVersion::fromBSON( staleInfo, "vWanted" );
        }

        ShardVersionMap::iterator it = _remoteShardVersions.find( endpoint.shardName );
        if ( it == _remoteShardVersions.end() ) {
            _remoteShardVersions.insert( make_pair( endpoint.shardName, remoteShardVersion ) );
        }
        else {
            ChunkVersion& previouslyNotedVersion = it->second;
            if ( previouslyNotedVersion.hasEqualEpoch( remoteShardVersion )) {
                if ( previouslyNotedVersion.isOlderThan( remoteShardVersion )) {
                    remoteShardVersion.cloneTo( &previouslyNotedVersion );
                }
            }
            else {
                // Epoch changed midway while applying the batch so set the version to
                // something unique and non-existent to force a reload when
                // refreshIsNeeded is called.
                ChunkVersion::IGNORED().cloneTo( &previouslyNotedVersion );
            }
        }
    }

    void ChunkManagerTargeter::noteCouldNotTarget() {
        dassert( _remoteShardVersions.empty() );
        _needsTargetingRefresh = true;
    }

    const TargeterStats* ChunkManagerTargeter::getStats() const {
        return _stats.get();
    }

    Status ChunkManagerTargeter::refreshIfNeeded( bool *wasChanged ) {

        bool dummy;
        if ( !wasChanged )
            wasChanged = &dummy;

        *wasChanged = false;

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

            *wasChanged = isMetadataDifferent( lastManager, lastPrimary, _manager, _primary );
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

            *wasChanged = isMetadataDifferent( lastManager, lastPrimary, _manager, _primary );
            return Status::OK();
        }

        // unreachable
        dassert( false );
        return Status::OK();
    }

    // To match legacy reload behavior, we have to backoff on config reload per-thread
    // TODO: Centralize this behavior better by refactoring config reload in mongos
    static const int maxWaitMillis = 500;
    static boost::thread_specific_ptr<Backoff> perThreadBackoff;

    static void refreshBackoff() {
        if ( !perThreadBackoff.get() )
            perThreadBackoff.reset( new Backoff( maxWaitMillis, maxWaitMillis * 2 ) );
        perThreadBackoff.get()->nextSleepMillis();
    }

    Status ChunkManagerTargeter::refreshNow( RefreshType refreshType ) {

        DBConfigPtr config;

        string errMsg;
        if ( !getDBConfigSafe( _nss.db(), config, &errMsg ) ) {
            return Status( ErrorCodes::DatabaseNotFound, errMsg );
        }

        // Try not to spam the configs
        refreshBackoff();

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

