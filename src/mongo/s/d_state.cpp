// @file d_state.cpp

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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/d_state.h"

#include <map>
#include <string>
#include <vector>

#include "mongo/client/connpool.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/db.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/wire_version.h"
#include "mongo/s/catalog/legacy/catalog_manager_legacy.h"
#include "mongo/s/client/shard_connection.h"
#include "mongo/s/client/sharding_connection_hook.h"
#include "mongo/s/config.h"
#include "mongo/s/grid.h"
#include "mongo/s/legacy_dist_lock_manager.h"
#include "mongo/s/metadata_loader.h"
#include "mongo/s/stale_exception.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/concurrency/ticketholder.h"
#include "mongo/util/log.h"
#include "mongo/util/stringutils.h"

namespace mongo {

    using std::endl;
    using std::string;
    using std::stringstream;
    using std::vector;


    bool isMongos() {
        return false;
    }


    // -----ShardingState START ----

    ShardingState::ShardingState()
        : _enabled(false),
          _configServerTickets( 3 /* max number of concurrent config server refresh threads */ ) {
    }

    bool ShardingState::enabled() {
        boost::lock_guard<boost::mutex> lk(_mutex);
        return _enabled;
    }

    string ShardingState::getConfigServer() {
        boost::lock_guard<boost::mutex> lk(_mutex);
        invariant(_enabled);

        return configServer.getConnectionString().toString();
    }

    void ShardingState::initialize(const string& server) {
        uassert(18509,
                "Unable to obtain host name during sharding initialization.",
                !getHostName().empty());

        shardingState._initialize(server);
    }

    // TODO: Consolidate and eliminate these various ways of setting / validating shard names
    bool ShardingState::setShardName( const string& name ) {
        return setShardNameAndHost( name, "" );
    }

    std::string ShardingState::getShardName() {
        boost::lock_guard<boost::mutex> lk(_mutex);
        return _shardName;
    }

    bool ShardingState::setShardNameAndHost( const string& name, const string& host ) {
        boost::lock_guard<boost::mutex> lk(_mutex);
        if ( _shardName.size() == 0 ) {
            // TODO SERVER-2299 remotely verify the name is sound w.r.t IPs
            _shardName = name;

            string clientAddr = cc().clientAddress(true);

            log() << "remote client " << clientAddr << " initialized this host "
                  << ( host.empty() ? string( "" ) : string( "(" ) + host + ") " )
                  << "as shard " << name;

            return true;
        }

        if ( _shardName == name )
            return true;

        string clientAddr = cc().clientAddress(true);

        warning() << "remote client " << clientAddr << " tried to initialize this host "
                  << ( host.empty() ? string( "" ) : string( "(" ) + host + ") " )
                  << "as shard " << name
                  << ", but shard name was previously initialized as " << _shardName;

        return false;
    }

    void ShardingState::gotShardName( const string& name ) {
        gotShardNameAndHost( name, "" );
    }

    void ShardingState::gotShardNameAndHost( const string& name, const string& host ) {
        if ( setShardNameAndHost( name, host ) )
            return;

        string clientAddr = cc().clientAddress(true);
        stringstream ss;

        // Same error as above, to match for reporting
        ss << "remote client " << clientAddr << " tried to initialize this host "
           << ( host.empty() ? string( "" ) : string( "(" ) + host + ") " )
           << "as shard " << name
           << ", but shard name was previously initialized as " << _shardName;

        msgasserted( 13298 , ss.str() );
    }

    void ShardingState::clearCollectionMetadata() {
        boost::lock_guard<boost::mutex> lk(_mutex);
        _collMetadata.clear();
    }

    // TODO we shouldn't need three ways for checking the version. Fix this.
    bool ShardingState::hasVersion( const string& ns ) {
        boost::lock_guard<boost::mutex> lk(_mutex);

        CollectionMetadataMap::const_iterator it = _collMetadata.find(ns);
        return it != _collMetadata.end();
    }

    bool ShardingState::hasVersion( const string& ns , ChunkVersion& version ) {
        boost::lock_guard<boost::mutex> lk(_mutex);

        CollectionMetadataMap::const_iterator it = _collMetadata.find(ns);
        if ( it == _collMetadata.end() )
            return false;

        CollectionMetadataPtr p = it->second;
        version = p->getShardVersion();
        return true;
    }

    ChunkVersion ShardingState::getVersion(const string& ns) {
        boost::lock_guard<boost::mutex> lk(_mutex);

        CollectionMetadataMap::const_iterator it = _collMetadata.find( ns );
        if ( it != _collMetadata.end() ) {
            CollectionMetadataPtr p = it->second;
            return p->getShardVersion();
        }
        else {
            return ChunkVersion( 0, 0, OID() );
        }
    }

    void ShardingState::donateChunk(OperationContext* txn,
                                    const string& ns,
                                    const BSONObj& min,
                                    const BSONObj& max,
                                    ChunkVersion version) {
        
        invariant(txn->lockState()->isCollectionLockedForMode(ns, MODE_X));
        boost::lock_guard<boost::mutex> lk( _mutex );

        CollectionMetadataMap::const_iterator it = _collMetadata.find( ns );
        verify( it != _collMetadata.end() ) ;
        CollectionMetadataPtr p = it->second;

        // empty shards should have version 0
        version =
                ( p->getNumChunks() > 1 ) ?
                        version : ChunkVersion( 0, 0, p->getCollVersion().epoch() );

        ChunkType chunk;
        chunk.setMin( min );
        chunk.setMax( max );
        string errMsg;

        CollectionMetadataPtr cloned( p->cloneMigrate( chunk, version, &errMsg ) );
        // uassert to match old behavior, TODO: report errors w/o throwing
        uassert( 16855, errMsg, NULL != cloned.get() );

        // TODO: a bit dangerous to have two different zero-version states - no-metadata and
        // no-version
        _collMetadata[ns] = cloned;
    }

    void ShardingState::undoDonateChunk(OperationContext* txn,
                                        const string& ns,
                                        CollectionMetadataPtr prevMetadata) {

        invariant(txn->lockState()->isCollectionLockedForMode(ns, MODE_X));
        boost::lock_guard<boost::mutex> lk( _mutex );
        
        log() << "ShardingState::undoDonateChunk acquired _mutex" << endl;

        CollectionMetadataMap::iterator it = _collMetadata.find( ns );
        verify( it != _collMetadata.end() );
        it->second = prevMetadata;
    }

    bool ShardingState::notePending(OperationContext* txn,
                                     const string& ns,
                                     const BSONObj& min,
                                     const BSONObj& max,
                                     const OID& epoch,
                                     string* errMsg ) {
        
        invariant(txn->lockState()->isCollectionLockedForMode(ns, MODE_X));
        boost::lock_guard<boost::mutex> lk( _mutex );

        CollectionMetadataMap::const_iterator it = _collMetadata.find( ns );
        if ( it == _collMetadata.end() ) {

            *errMsg = str::stream() << "could not note chunk " << "[" << min << "," << max << ")"
                                    << " as pending because the local metadata for " << ns
                                    << " has changed";

            return false;
        }

        CollectionMetadataPtr metadata = it->second;

        // This can currently happen because drops aren't synchronized with in-migrations
        // The idea for checking this here is that in the future we shouldn't have this problem
        if ( metadata->getCollVersion().epoch() != epoch ) {

            *errMsg = str::stream() << "could not note chunk " << "[" << min << "," << max << ")"
                                    << " as pending because the epoch for " << ns
                                    << " has changed from "
                                    << epoch << " to " << metadata->getCollVersion().epoch();

            return false;
        }

        ChunkType chunk;
        chunk.setMin( min );
        chunk.setMax( max );

        CollectionMetadataPtr cloned( metadata->clonePlusPending( chunk, errMsg ) );
        if ( !cloned ) return false;

        _collMetadata[ns] = cloned;
        return true;
    }

    bool ShardingState::forgetPending(OperationContext* txn,
                                       const string& ns,
                                       const BSONObj& min,
                                       const BSONObj& max,
                                       const OID& epoch,
                                       string* errMsg ) {
        
        invariant(txn->lockState()->isCollectionLockedForMode(ns, MODE_X));
        boost::lock_guard<boost::mutex> lk( _mutex );

        CollectionMetadataMap::const_iterator it = _collMetadata.find( ns );
        if ( it == _collMetadata.end() ) {

            *errMsg = str::stream() << "no need to forget pending chunk "
                                    << "[" << min << "," << max << ")"
                                    << " because the local metadata for " << ns << " has changed";

            return false;
        }

        CollectionMetadataPtr metadata = it->second;

        // This can currently happen because drops aren't synchronized with in-migrations
        // The idea for checking this here is that in the future we shouldn't have this problem
        if ( metadata->getCollVersion().epoch() != epoch ) {

            *errMsg = str::stream() << "no need to forget pending chunk "
                                    << "[" << min << "," << max << ")"
                                    << " because the epoch for " << ns << " has changed from "
                                    << epoch << " to " << metadata->getCollVersion().epoch();

            return false;
        }

        ChunkType chunk;
        chunk.setMin( min );
        chunk.setMax( max );

        CollectionMetadataPtr cloned( metadata->cloneMinusPending( chunk, errMsg ) );
        if ( !cloned ) return false;

        _collMetadata[ns] = cloned;
        return true;
    }

    void ShardingState::splitChunk(OperationContext* txn,
                                    const string& ns,
                                    const BSONObj& min,
                                    const BSONObj& max,
                                    const vector<BSONObj>& splitKeys,
                                    ChunkVersion version ) {
        
        invariant(txn->lockState()->isCollectionLockedForMode(ns, MODE_X));
        boost::lock_guard<boost::mutex> lk( _mutex );

        CollectionMetadataMap::const_iterator it = _collMetadata.find( ns );
        verify( it != _collMetadata.end() ) ;

        ChunkType chunk;
        chunk.setMin( min );
        chunk.setMax( max );
        string errMsg;

        CollectionMetadataPtr cloned( it->second->cloneSplit( chunk, splitKeys, version, &errMsg ) );
        // uassert to match old behavior, TODO: report errors w/o throwing
        uassert( 16857, errMsg, NULL != cloned.get() );

        _collMetadata[ns] = cloned;
    }

    void ShardingState::mergeChunks(OperationContext* txn,
                                     const string& ns,
                                     const BSONObj& minKey,
                                     const BSONObj& maxKey,
                                     ChunkVersion mergedVersion ) {

        invariant(txn->lockState()->isCollectionLockedForMode(ns, MODE_X));
        boost::lock_guard<boost::mutex> lk( _mutex );

        CollectionMetadataMap::const_iterator it = _collMetadata.find( ns );
        verify( it != _collMetadata.end() );

        string errMsg;

        CollectionMetadataPtr cloned( it->second->cloneMerge( minKey,
                                                              maxKey,
                                                              mergedVersion,
                                                              &errMsg ) );
        // uassert to match old behavior, TODO: report errors w/o throwing
        uassert( 17004, errMsg, NULL != cloned.get() );

        _collMetadata[ns] = cloned;
    }

    void ShardingState::resetMetadata( const string& ns ) {
        boost::lock_guard<boost::mutex> lk( _mutex );

        warning() << "resetting metadata for " << ns << ", this should only be used in testing"
                  << endl;

        _collMetadata.erase( ns );
    }

    Status ShardingState::refreshMetadataIfNeeded( OperationContext* txn,
                                                   const string& ns,
                                                   const ChunkVersion& reqShardVersion,
                                                   ChunkVersion* latestShardVersion )
    {
        // The _configServerTickets serializes this process such that only a small number of threads
        // can try to refresh at the same time.

        LOG( 2 ) << "metadata refresh requested for " << ns << " at shard version "
                 << reqShardVersion << endl;

        //
        // Queuing of refresh requests starts here when remote reload is needed. This may take time.
        // TODO: Explicitly expose the queuing discipline.
        //

        _configServerTickets.waitForTicket();
        TicketHolderReleaser needTicketFrom( &_configServerTickets );

        //
        // Fast path - check if the requested version is at a higher version than the current
        // metadata version or a different epoch before verifying against config server.
        //

        CollectionMetadataPtr storedMetadata;
        {
            boost::lock_guard<boost::mutex> lk( _mutex );
            CollectionMetadataMap::iterator it = _collMetadata.find( ns );
            if ( it != _collMetadata.end() ) storedMetadata = it->second;
        }
        ChunkVersion storedShardVersion;
        if ( storedMetadata ) storedShardVersion = storedMetadata->getShardVersion();
        *latestShardVersion = storedShardVersion;

        if ( storedShardVersion >= reqShardVersion &&
             storedShardVersion.epoch() == reqShardVersion.epoch() ) {

            // Don't need to remotely reload if we're in the same epoch with a >= version
            return Status::OK();
        }

        //
        // Slow path - remotely reload
        //
        // Cases:
        // A) Initial config load and/or secondary take-over.
        // B) Migration TO this shard finished, notified by mongos.
        // C) Dropping a collection, notified (currently) by mongos.
        // D) Stale client wants to reload metadata with a different *epoch*, so we aren't sure.

        if ( storedShardVersion.epoch() != reqShardVersion.epoch() ) {
            // Need to remotely reload if our epochs aren't the same, to verify
            LOG( 1 ) << "metadata change requested for " << ns << ", from shard version "
                     << storedShardVersion << " to " << reqShardVersion
                     << ", need to verify with config server" << endl;
        }
        else {
            // Need to remotely reload since our epochs aren't the same but our version is greater
            LOG( 1 ) << "metadata version update requested for " << ns
                     << ", from shard version " << storedShardVersion << " to " << reqShardVersion
                     << ", need to verify with config server" << endl;
        }

        return doRefreshMetadata(txn, ns, reqShardVersion, true, latestShardVersion);
    }

    Status ShardingState::refreshMetadataNow(OperationContext* txn,
                                             const string& ns,
                                             ChunkVersion* latestShardVersion) {
        return doRefreshMetadata(txn, ns, ChunkVersion(0, 0, OID()), false, latestShardVersion);
    }

    void ShardingState::_initialize(const string& server) {
        // Ensure only one caller at a time initializes
        boost::lock_guard<boost::mutex> lk(_mutex);

        if (_enabled) {
            // TODO: Do we need to throw exception if the config servers have changed from what we
            // already have in place? How do we test for that?
            return;
        }

        ShardedConnectionInfo::addHook();

        std::string errmsg;
        ConnectionString configServerCS = ConnectionString::parse(server, errmsg);
        uassert(28633,
                str::stream() << "Invalid config server connection string: " << errmsg,
                configServerCS.isValid());

        configServer.init(configServerCS);

        auto catalogManager = stdx::make_unique<CatalogManagerLegacy>();
        uassertStatusOK(catalogManager->init(configServerCS));
        grid.setCatalogManager(std::move(catalogManager));

        _enabled = true;
    }

    Status ShardingState::doRefreshMetadata( OperationContext* txn,
                                             const string& ns,
                                             const ChunkVersion& reqShardVersion,
                                             bool useRequestedVersion,
                                             ChunkVersion* latestShardVersion )
    {
        // The idea here is that we're going to reload the metadata from the config server, but
        // we need to do so outside any locks.  When we get our result back, if the current metadata
        // has changed, we may not be able to install the new metadata.

        //
        // Get the initial metadata
        // No DBLock is needed since the metadata is expected to change during reload.
        //

        CollectionMetadataPtr beforeMetadata;

        {
            boost::lock_guard<boost::mutex> lk( _mutex );

            // We can't reload if sharding is not enabled - i.e. without a config server location
            if (!_enabled) {
                string errMsg = str::stream() << "cannot refresh metadata for " << ns
                                              << " before sharding has been enabled";

                warning() << errMsg;
                return Status(ErrorCodes::NotYetInitialized, errMsg);
            }

            // We also can't reload if a shard name has not yet been set.
            if (_shardName.empty()) {

                string errMsg = str::stream() << "cannot refresh metadata for " << ns
                                              << " before shard name has been set";

                warning() << errMsg;
                return Status(ErrorCodes::NotYetInitialized, errMsg);
            }

            CollectionMetadataMap::iterator it = _collMetadata.find(ns);
            if (it != _collMetadata.end()) {
                beforeMetadata = it->second;
            }
        }

        ChunkVersion beforeShardVersion;
        ChunkVersion beforeCollVersion;
        if ( beforeMetadata ) {
            beforeShardVersion = beforeMetadata->getShardVersion();
            beforeCollVersion = beforeMetadata->getCollVersion();
        }

        *latestShardVersion = beforeShardVersion;

        //
        // Determine whether we need to diff or fully reload
        //

        bool fullReload = false;
        if ( !beforeMetadata ) {
            // We don't have any metadata to reload from
            fullReload = true;
        }
        else if ( useRequestedVersion && reqShardVersion.epoch() != beforeShardVersion.epoch() ) {
            // It's not useful to use the metadata as a base because we think the epoch will differ
            fullReload = true;
        }

        //
        // Load the metadata from the remote server, start construction
        //

        LOG( 0 ) << "remotely refreshing metadata for " << ns
                 << ( useRequestedVersion ?
                      string( " with requested shard version " ) + reqShardVersion.toString() : "" )
                 << ( fullReload ?
                      ", current shard version is " : " based on current shard version " )
                 << beforeShardVersion
                 << ", current metadata version is " << beforeCollVersion << endl;

        string errMsg;

        MetadataLoader mdLoader(configServer.getConnectionString());
        CollectionMetadata* remoteMetadataRaw = new CollectionMetadata();
        CollectionMetadataPtr remoteMetadata( remoteMetadataRaw );

        Timer refreshTimer;
        Status status =
                mdLoader.makeCollectionMetadata(grid.catalogManager(),
                                                ns,
                                                getShardName(),
                                                fullReload ? NULL : beforeMetadata.get(),
                                                remoteMetadataRaw);
        long long refreshMillis = refreshTimer.millis();

        if ( status.code() == ErrorCodes::NamespaceNotFound ) {
            remoteMetadata.reset();
            remoteMetadataRaw = NULL;
        }
        else if ( !status.isOK() ) {

            warning() << "could not remotely refresh metadata for " << ns
                      << causedBy( status.reason() ) << endl;

            return status;
        }

        ChunkVersion remoteShardVersion;
        ChunkVersion remoteCollVersion;
        if ( remoteMetadata ) {
            remoteShardVersion = remoteMetadata->getShardVersion();
            remoteCollVersion = remoteMetadata->getCollVersion();
        }

        //
        // Get ready to install loaded metadata if needed
        //

        CollectionMetadataPtr afterMetadata;
        ChunkVersion afterShardVersion;
        ChunkVersion afterCollVersion;
        ChunkVersion::VersionChoice choice;

        // If we choose to install the new metadata, this describes the kind of install
        enum InstallType {
            InstallType_New, InstallType_Update, InstallType_Replace, InstallType_Drop,
            InstallType_None
        } installType = InstallType_None; // compiler complains otherwise

        {
            // Exclusive collection lock needed since we're now potentially changing the metadata,
            // and don't want reads/writes to be ongoing.
            ScopedTransaction transaction(txn, MODE_IX);
            Lock::DBLock dbLock(txn->lockState(), nsToDatabaseSubstring(ns), MODE_IX);
            Lock::CollectionLock collLock(txn->lockState(), ns, MODE_X);

            //
            // Get the metadata now that the load has completed
            //

            boost::lock_guard<boost::mutex> lk( _mutex );

            // Don't reload if our config server has changed or sharding is no longer enabled
            if (!_enabled) {
                string errMsg = str::stream() << "could not refresh metadata for " << ns
                                              << ", sharding is no longer enabled";

                warning() << errMsg;
                return Status(ErrorCodes::NotYetInitialized, errMsg);
            }

            CollectionMetadataMap::iterator it = _collMetadata.find( ns );
            if ( it != _collMetadata.end() ) afterMetadata = it->second;

            if ( afterMetadata ) {
                afterShardVersion = afterMetadata->getShardVersion();
                afterCollVersion = afterMetadata->getCollVersion();
            }

            *latestShardVersion = afterShardVersion;

            //
            // Resolve newer pending chunks with the remote metadata, finish construction
            //

            status = mdLoader.promotePendingChunks( afterMetadata.get(), remoteMetadataRaw );

            if ( !status.isOK() ) {

                warning() << "remote metadata for " << ns
                          << " is inconsistent with current pending chunks"
                          << causedBy( status.reason() ) << endl;

                return status;
            }

            //
            // Compare the 'before', 'after', and 'remote' versions/epochs and choose newest
            // Zero-epochs (sentinel value for "dropped" collections), are tested by
            // !epoch.isSet().
            //

            choice = ChunkVersion::chooseNewestVersion( beforeCollVersion,
                                                        afterCollVersion,
                                                        remoteCollVersion );

            if ( choice == ChunkVersion::VersionChoice_Remote ) {
                dassert(!remoteCollVersion.epoch().isSet() ||
                        remoteShardVersion >= beforeShardVersion);

                if ( !afterCollVersion.epoch().isSet() ) {

                    // First metadata load
                    installType = InstallType_New;
                    dassert( it == _collMetadata.end() );
                    _collMetadata.insert( make_pair( ns, remoteMetadata ) );
                }
                else if ( remoteCollVersion.epoch().isSet() &&
                          remoteCollVersion.epoch() == afterCollVersion.epoch() ) {

                    // Update to existing metadata
                    installType = InstallType_Update;

                    // Invariant: If CollMetadata was not found, version should be have been 0.
                    dassert( it != _collMetadata.end() );
                    it->second = remoteMetadata;
                }
                else if ( remoteCollVersion.epoch().isSet() ) {

                    // New epoch detected, replacing metadata
                    installType = InstallType_Replace;

                    // Invariant: If CollMetadata was not found, version should be have been 0.
                    dassert( it != _collMetadata.end() );
                    it->second = remoteMetadata;
                }
                else {
                    dassert( !remoteCollVersion.epoch().isSet() );

                    // Drop detected
                    installType = InstallType_Drop;
                    _collMetadata.erase( it );
                }

                *latestShardVersion = remoteShardVersion;
            }
        }
        // End _mutex
        // End DBWrite

        //
        // Do messaging based on what happened above
        //
        string localShardVersionMsg =
                beforeShardVersion.epoch() == afterShardVersion.epoch() ?
                        afterShardVersion.toString() :
                        beforeShardVersion.toString() + " / " + afterShardVersion.toString();

        if ( choice == ChunkVersion::VersionChoice_Unknown ) {

            string errMsg = str::stream()
                << "need to retry loading metadata for " << ns
                << ", collection may have been dropped or recreated during load"
                << " (loaded shard version : " << remoteShardVersion.toString()
                << ", stored shard versions : " << localShardVersionMsg
                << ", took " << refreshMillis << "ms)";

            warning() << errMsg;
            return Status( ErrorCodes::RemoteChangeDetected, errMsg );
        }

        if ( choice == ChunkVersion::VersionChoice_Local ) {

            LOG( 0 ) << "metadata of collection " << ns << " already up to date (shard version : "
                     << afterShardVersion.toString() << ", took " << refreshMillis << "ms)"
                     << endl;
            return Status::OK();
        }

        dassert( choice == ChunkVersion::VersionChoice_Remote );

        switch( installType ) {
        case InstallType_New:
            LOG( 0 ) << "collection " << ns << " was previously unsharded"
                     << ", new metadata loaded with shard version " << remoteShardVersion
                     << endl;
            break;
        case InstallType_Update:
            LOG( 0 ) << "updating metadata for " << ns << " from shard version "
                     << localShardVersionMsg << " to shard version " << remoteShardVersion
                     << endl;
            break;
        case InstallType_Replace:
            LOG( 0 ) << "replacing metadata for " << ns << " at shard version "
                     << localShardVersionMsg << " with a new epoch (shard version "
                     << remoteShardVersion << ")" << endl;
            break;
        case InstallType_Drop:
            LOG( 0 ) << "dropping metadata for " << ns << " at shard version "
                     << localShardVersionMsg << ", took " << refreshMillis << "ms" << endl;
            break;
        default:
            verify( false );
            break;
        }

        if ( installType != InstallType_Drop ) {
            LOG( 0 ) << "collection version was loaded at version " << remoteCollVersion
                     << ", took " << refreshMillis << "ms" << endl;
        }

        return Status::OK();
    }

    void ShardingState::appendInfo(BSONObjBuilder& builder) {
        boost::lock_guard<boost::mutex> lk(_mutex);

        builder.appendBool("enabled", _enabled);
        if (!_enabled) {
            return;
        }

        builder.append("configServer", configServer.getConnectionString().toString());
        builder.append("shardName", _shardName);

        BSONObjBuilder versionB(builder.subobjStart("versions"));
        for (CollectionMetadataMap::const_iterator it = _collMetadata.begin();
             it != _collMetadata.end();
             ++it) {

            CollectionMetadataPtr metadata = it->second;
            versionB.appendTimestamp(it->first, metadata->getShardVersion().toLong());
        }

        versionB.done();
    }

    bool ShardingState::needCollectionMetadata( const string& ns ) const {
        if ( ! _enabled )
            return false;

        if ( ! ShardedConnectionInfo::get( false ) )
            return false;

        return true;
    }

    CollectionMetadataPtr ShardingState::getCollectionMetadata( const string& ns ) {
        boost::lock_guard<boost::mutex> lk( _mutex );

        CollectionMetadataMap::const_iterator it = _collMetadata.find( ns );
        if ( it == _collMetadata.end() ) {
            return CollectionMetadataPtr();
        }
        else {
            return it->second;
        }
    }

    ShardingState shardingState;

    // -----ShardingState END ----

    // -----ShardedConnectionInfo START ----

    boost::thread_specific_ptr<ShardedConnectionInfo> ShardedConnectionInfo::_tl;

    ShardedConnectionInfo::ShardedConnectionInfo() {
        _forceVersionOk = false;
    }

    ShardedConnectionInfo* ShardedConnectionInfo::get( bool create ) {
        ShardedConnectionInfo* info = _tl.get();
        if ( ! info && create ) {
            LOG(1) << "entering shard mode for connection" << endl;
            info = new ShardedConnectionInfo();
            _tl.reset( info );
        }
        return info;
    }

    void ShardedConnectionInfo::reset() {
        _tl.reset();
    }

    const ChunkVersion ShardedConnectionInfo::getVersion( const string& ns ) const {
        NSVersionMap::const_iterator it = _versions.find( ns );
        if ( it != _versions.end() ) {
            return it->second;
        }
        else {
            return ChunkVersion( 0, 0, OID() );
        }
    }

    void ShardedConnectionInfo::setVersion( const string& ns , const ChunkVersion& version ) {
        _versions[ns] = version;
    }

    void ShardedConnectionInfo::addHook() {
        static mongo::mutex lock;
        static bool done = false;

        boost::lock_guard<boost::mutex> lk(lock);
        if (!done) {
            log() << "first cluster operation detected, adding sharding hook to enable versioning "
                    "and authentication to remote servers" << endl;
            pool.addHook(new ShardingConnectionHook(false));
            shardConnectionPool.addHook(new ShardingConnectionHook(true));
            done = true;
        }
    }

    class MongodShardCommand : public Command {
    public:
        MongodShardCommand( const char * n ) : Command( n ) {
        }
        virtual bool slaveOk() const {
            return false;
        }
        virtual bool adminOnly() const {
            return true;
        }
    };


    bool haveLocalShardingInfo( const string& ns ) {
        if ( ! shardingState.enabled() )
            return false;

        if ( ! shardingState.hasVersion( ns ) )
            return false;

        return ShardedConnectionInfo::get(false) != NULL;
    }

    class UnsetShardingCommand : public MongodShardCommand {
    public:
        UnsetShardingCommand() : MongodShardCommand("unsetSharding") {}

        virtual void help( stringstream& help ) const {
            help << "internal";
        }

        virtual bool isWriteCommandForConfigServer() const { return false; }

        virtual bool slaveOk() const { return true; }

        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::internal);
            out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
        }

        bool run(OperationContext* txn,
                 const string&,
                 BSONObj& cmdObj,
                 int,
                 string& errmsg,
                 BSONObjBuilder& result) {
            ShardedConnectionInfo::reset();
            return true;
        }

    } unsetShardingCommand;

    class SetShardVersion : public MongodShardCommand {
    public:
        SetShardVersion() : MongodShardCommand("setShardVersion") {}

        virtual void help( stringstream& help ) const {
            help << "internal";
        }

        virtual bool slaveOk() const { return true; }
        virtual bool isWriteCommandForConfigServer() const { return false; }
        
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::internal);
            out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
        }

        bool checkConfigOrInit(OperationContext* txn,
                               const string& configdb,
                               bool authoritative,
                               string& errmsg,
                               BSONObjBuilder& result,
                               bool locked = false ) const {
            if ( configdb.size() == 0 ) {
                errmsg = "no configdb";
                return false;
            }
            
            if ( shardingState.enabled() ) {
                if ( configdb == shardingState.getConfigServer() ) 
                    return true;
                
                result.append( "configdb" , BSON( "stored" << shardingState.getConfigServer() << 
                                                  "given" << configdb ) );
                                                  
                errmsg = str::stream() << "mongos specified a different config database string : "
                                       << "stored : " << shardingState.getConfigServer()
                                       << " vs given : " << configdb;
                return false;
            }
            
            if ( ! authoritative ) {
                result.appendBool( "need_authoritative" , true );
                errmsg = "first setShardVersion";
                return false;
            }
            
            if ( locked ) {
                ShardingState::initialize(configdb);
                return true;
            }

            ScopedTransaction transaction(txn, MODE_X);
            Lock::GlobalWrite lk(txn->lockState());
            return checkConfigOrInit(txn, configdb, authoritative, errmsg, result, true);
        }

        bool run(OperationContext* txn,
                 const string&,
                 BSONObj& cmdObj,
                 int,
                 string& errmsg,
                 BSONObjBuilder& result) {

            // Compatibility error for < v3.0 mongoses still active in the cluster
            // TODO: Remove post-3.0
            if (!cmdObj["serverID"].eoo()) {

                // This mongos is too old to talk to us
                string errMsg = stream() << "v3.0 mongod is incompatible with v2.6 mongos, "
                                         << "a v2.6 mongos may be running in the v3.0 cluster at "
                                         << txn->getClient()->clientAddress(false);
                error() << errMsg;
                return appendCommandStatus(result, Status(ErrorCodes::ProtocolError, errMsg));
            }

            // Steps
            // 1. check basic config
            // 2. extract params from command
            // 3. fast check
            // 4. slow check (LOCKS)
            
            // step 1

            lastError.disableForCommand();
            ShardedConnectionInfo* info = ShardedConnectionInfo::get( true );

            bool authoritative = cmdObj.getBoolField( "authoritative" );
            
            // check config server is ok or enable sharding
            if (!checkConfigOrInit(
                        txn, cmdObj["configdb"].valuestrsafe(), authoritative, errmsg, result)) {
                return false;
            }

            // check shard name is correct
            if ( cmdObj["shard"].type() == String ) {
                // The shard host is also sent when using setShardVersion, report this host if there
                // is an error.
                shardingState.gotShardNameAndHost( cmdObj["shard"].String(),
                                                   cmdObj["shardHost"].str() );
            }
            
            // Handle initial shard connection
            if( cmdObj["version"].eoo() && cmdObj["init"].trueValue() ){

                result.append( "initialized", true );

                // Send back wire version to let mongos know what protocol we can speak
                result.append( "minWireVersion", minWireVersion );
                result.append( "maxWireVersion", maxWireVersion );

                return true;
            }

            string ns = cmdObj["setShardVersion"].valuestrsafe();
            if ( ns.size() == 0 ) {
                errmsg = "need to specify namespace";
                return false;
            }


            // we can run on a slave up to here
            if (!repl::getGlobalReplicationCoordinator()->canAcceptWritesForDatabase(
                        NamespaceString(ns).db())) {
                result.append( "errmsg" , "not master" );
                result.append( "note" , "from post init in setShardVersion" );
                return false;
            }

            // step 2
            if( ! ChunkVersion::canParseBSON( cmdObj, "version" ) ){
                errmsg = "need to specify version";
                return false;
            }

            const ChunkVersion version = ChunkVersion::fromBSON( cmdObj, "version" );
            
            // step 3

            const ChunkVersion oldVersion = info->getVersion(ns);
            const ChunkVersion globalVersion = shardingState.getVersion(ns);

            oldVersion.addToBSON( result, "oldVersion" );

            if ( version.isWriteCompatibleWith( globalVersion )) {
                // mongos and mongod agree!
                if ( !oldVersion.isWriteCompatibleWith( version )) {
                    if ( oldVersion < globalVersion &&
                            oldVersion.hasEqualEpoch( globalVersion )) {
                        info->setVersion( ns, version );
                    }
                    else if ( authoritative ) {
                        // this means there was a drop and our version is reset
                        info->setVersion( ns, version );
                    }
                    else {
                        result.append( "ns", ns );
                        result.appendBool( "need_authoritative", true );
                        errmsg = "verifying drop on '" + ns + "'";
                        return false;
                    }
                }

                return true;
            }

            // step 4
            // Cases below all either return OR fall-through to remote metadata reload.
            const bool isDropRequested = !version.isSet() && globalVersion.isSet();

            if (isDropRequested) {
                if ( ! authoritative ) {
                    result.appendBool( "need_authoritative" , true );
                    result.append( "ns" , ns );
                    globalVersion.addToBSON( result, "globalVersion" );
                    errmsg = "dropping needs to be authoritative";
                    return false;
                }

                // Fall through to metadata reload below
            }
            else {
                // Not Dropping

                // TODO: Refactor all of this
                if ( version < oldVersion && version.hasEqualEpoch( oldVersion ) ) {
                    errmsg = str::stream() << "this connection already had a newer version "
                                           << "of collection '" << ns << "'";
                    result.append( "ns" , ns );
                    version.addToBSON( result, "newVersion" );
                    globalVersion.addToBSON( result, "globalVersion" );
                    return false;
                }

                // TODO: Refactor all of this
                if ( version < globalVersion && version.hasEqualEpoch( globalVersion ) ) {
                    while ( shardingState.inCriticalMigrateSection() ) {
                        log() << "waiting till out of critical section" << endl;
                        shardingState.waitTillNotInCriticalSection( 10 );
                    }
                    errmsg = str::stream() << "shard global version for collection is higher "
                                           << "than trying to set to '" << ns << "'";
                    result.append( "ns" , ns );
                    version.addToBSON( result, "version" );
                    globalVersion.addToBSON( result, "globalVersion" );
                    result.appendBool( "reloadConfig" , true );
                    return false;
                }

                if ( ! globalVersion.isSet() && ! authoritative ) {
                    // Needed b/c when the last chunk is moved off a shard,
                    // the version gets reset to zero, which should require a reload.
                    while ( shardingState.inCriticalMigrateSection() ) {
                        log() << "waiting till out of critical section" << endl;
                        shardingState.waitTillNotInCriticalSection( 10 );
                    }

                    // need authoritative for first look
                    result.append( "ns" , ns );
                    result.appendBool( "need_authoritative" , true );
                    errmsg = "first time for collection '" + ns + "'";
                    return false;
                }

                // Fall through to metadata reload below
            }

            ChunkVersion currVersion;
            Status status = shardingState.refreshMetadataIfNeeded(txn, ns, version, &currVersion);

            if (!status.isOK()) {

                // The reload itself was interrupted or confused here

                errmsg = str::stream() << "could not refresh metadata for " << ns
                                       << " with requested shard version " << version.toString()
                                       << ", stored shard version is " << currVersion.toString()
                                       << causedBy( status.reason() );

                warning() << errmsg << endl;

                result.append( "ns" , ns );
                version.addToBSON( result, "version" );
                currVersion.addToBSON( result, "globalVersion" );
                result.appendBool( "reloadConfig", true );

                return false;
            }
            else if ( !version.isWriteCompatibleWith( currVersion ) ) {

                // We reloaded a version that doesn't match the version mongos was trying to
                // set.

                errmsg = str::stream() << "requested shard version differs from"
                                       << " config shard version for " << ns
                                       << ", requested version is " << version.toString()
                                       << " but found version " << currVersion.toString();

                OCCASIONALLY warning() << errmsg << endl;

                // WARNING: the exact fields below are important for compatibility with mongos
                // version reload.

                result.append( "ns" , ns );
                currVersion.addToBSON( result, "globalVersion" );

                // If this was a reset of a collection or the last chunk moved out, inform mongos to
                // do a full reload.
                if (currVersion.epoch() != version.epoch() || !currVersion.isSet() ) {
                    result.appendBool( "reloadConfig", true );
                    // Zero-version also needed to trigger full mongos reload, sadly
                    // TODO: Make this saner, and less impactful (full reload on last chunk is bad)
                    ChunkVersion( 0, 0, OID() ).addToBSON( result, "version" );
                    // For debugging
                    version.addToBSON( result, "origVersion" );
                }
                else {
                    version.addToBSON( result, "version" );
                }

                return false;
            }

            info->setVersion( ns , version );
            return true;
        }

    } setShardVersionCmd;

    class GetShardVersion : public MongodShardCommand {
    public:
        GetShardVersion() : MongodShardCommand("getShardVersion") {}

        virtual void help( stringstream& help ) const {
            help << " example: { getShardVersion : 'alleyinsider.foo'  } ";
        }

        virtual bool isWriteCommandForConfigServer() const { return false; }

        virtual Status checkAuthForCommand(ClientBasic* client,
                                           const std::string& dbname,
                                           const BSONObj& cmdObj) {
            if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                    ResourcePattern::forExactNamespace(NamespaceString(parseNs(dbname, cmdObj))),
                    ActionType::getShardVersion)) {
                return Status(ErrorCodes::Unauthorized, "Unauthorized");
            }
            return Status::OK();
        }
        virtual std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const {
            return parseNsFullyQualified(dbname, cmdObj);
        }

        bool run(OperationContext* txn,
                 const string&,
                 BSONObj& cmdObj,
                 int,
                 string& errmsg,
                 BSONObjBuilder& result) {
            const string ns = cmdObj["getShardVersion"].valuestrsafe();
            if (ns.size() == 0) {
                errmsg = "need to specify full namespace";
                return false;
            }

            if (shardingState.enabled()) {
                result.append("configServer", shardingState.getConfigServer());
            }
            else {
                result.append("configServer", "");
            }

            result.appendTimestamp("global", shardingState.getVersion(ns).toLong());

            ShardedConnectionInfo* const info = ShardedConnectionInfo::get(false);
            result.appendBool("inShardedMode", info != NULL);
            if (info) {
                result.appendTimestamp("mine", info->getVersion(ns).toLong());
            }
            else {
                result.appendTimestamp("mine", 0);
            }

            if (cmdObj["fullMetadata"].trueValue()) {
                CollectionMetadataPtr metadata = shardingState.getCollectionMetadata(ns);
                if (metadata) {
                    result.append("metadata", metadata->toBSON());
                }
                else {
                    result.append("metadata", BSONObj());
                }
            }

            return true;
        }

    } getShardVersion;

    class ShardingStateCmd : public MongodShardCommand {
    public:
        ShardingStateCmd() : MongodShardCommand( "shardingState" ) {}

        virtual bool isWriteCommandForConfigServer() const { return true; }

        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::shardingState);
            out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
        }

        bool run(OperationContext* txn,
                 const string& dbname,
                 BSONObj& cmdObj,
                 int,
                 string& errmsg,
                 BSONObjBuilder& result) {
            ScopedTransaction transaction(txn, MODE_IX);
            Lock::DBLock dbXLock(txn->lockState(), dbname, MODE_X);
            OldClientContext ctx(txn, dbname);

            shardingState.appendInfo( result );
            return true;
        }

    } shardingStateCmd;

    /**
     * @ return true if not in sharded mode
                     or if version for this client is ok
     */
    static bool shardVersionOk(const string& ns,
                               string& errmsg,
                               ChunkVersion& received,
                               ChunkVersion& wanted) {

        if ( ! shardingState.enabled() )
            return true;

        if (!repl::getGlobalReplicationCoordinator()->canAcceptWritesForDatabase(
                NamespaceString(ns).db()))  {
            // right now connections to secondaries aren't versioned at all
            return true;
        }

        ShardedConnectionInfo* info = ShardedConnectionInfo::get( false );

        if ( ! info ) {
            // this means the client has nothing sharded
            // so this allows direct connections to do whatever they want
            // which i think is the correct behavior
            return true;
        }

        if ( info->inForceVersionOkMode() ) {
            return true;
        }

        // TODO : all collections at some point, be sharded or not, will have a version
        //  (and a CollectionMetadata)
        received = info->getVersion( ns );

        if (ChunkVersion::isIgnoredVersion(received)) {
            return true;
        }

        wanted = shardingState.getVersion( ns );

        if( received.isWriteCompatibleWith( wanted ) ) return true;

        //
        // Figure out exactly why not compatible, send appropriate error message
        // The versions themselves are returned in the error, so not needed in messages here
        //

        // Check epoch first, to send more meaningful message, since other parameters probably
        // won't match either
        if( ! wanted.hasEqualEpoch( received ) ){
            errmsg = str::stream() << "version epoch mismatch detected for " << ns << ", "
                                   << "the collection may have been dropped and recreated";
            return false;
        }

        if( ! wanted.isSet() && received.isSet() ){
            errmsg = str::stream() << "this shard no longer contains chunks for " << ns << ", "
                                   << "the collection may have been dropped";
            return false;
        }

        if( wanted.isSet() && ! received.isSet() ){
            errmsg = str::stream() << "this shard contains versioned chunks for " << ns << ", "
                                   << "but no version set in request";
            return false;
        }

        if( wanted.majorVersion() != received.majorVersion() ){

            //
            // Could be > or < - wanted is > if this is the source of a migration,
            // wanted < if this is the target of a migration
            //

            errmsg = str::stream() << "version mismatch detected for " << ns << ", "
                                   << "stored major version " << wanted.majorVersion()
                                   << " does not match received " << received.majorVersion();
            return false;
        }

        // Those are all the reasons the versions can mismatch
        verify( false );

        return false;

    }

    void ensureShardVersionOKOrThrow(const std::string& ns) {
        string errmsg;
        ChunkVersion received;
        ChunkVersion wanted;
        if (!shardVersionOk(ns, errmsg, received, wanted)) {
            StringBuilder sb;
            sb << "[" << ns << "] shard version not ok: " << errmsg;
            throw SendStaleConfigException(ns, sb.str(), received, wanted);
        }
    }

    void usingAShardConnection( const string& addr ) {
    }

    void saveGLEStats(const BSONObj& result, const std::string& conn) {
        // Declared in cluster_last_error_info.h.
        //
        // This can be called in mongod, which is unfortunate.  To fix this,
        // we can redesign how connection pooling works on mongod for sharded operations.
    }

}
