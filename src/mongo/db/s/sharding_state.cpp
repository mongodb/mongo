/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/db/s/sharding_state.h"

#include "mongo/client/connection_string.h"
#include "mongo/client/remote_command_targeter_factory_impl.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/lock_state.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/s/collection_metadata.h"
#include "mongo/db/s/metadata_loader.h"
#include "mongo/db/s/operation_shard_version.h"
#include "mongo/db/s/sharded_connection_info.h"
#include "mongo/s/catalog/catalog_manager.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/config.h"
#include "mongo/s/grid.h"
#include "mongo/s/sharding_initialization.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/sock.h"

namespace mongo {

using std::shared_ptr;
using std::string;
using std::vector;

namespace {

const auto getShardingState = ServiceContext::declareDecoration<ShardingState>();

// Max number of concurrent config server refresh threads
const int kMaxConfigServerRefreshThreads = 3;

enum class VersionChoice { Local, Remote, Unknown };

/**
 * Compares a remotely-loaded version (remoteVersion) to the latest local version of a collection
 * (localVersion) and returns which one is the newest.
 *
 * Because it isn't clear during epoch changes which epoch is newer, the local version before the
 * reload occurred, 'prevLocalVersion', is used to determine whether the remote epoch is definitely
 * newer, or we're not sure.
 */
VersionChoice chooseNewestVersion(ChunkVersion prevLocalVersion,
                                  ChunkVersion localVersion,
                                  ChunkVersion remoteVersion) {
    OID prevEpoch = prevLocalVersion.epoch();
    OID localEpoch = localVersion.epoch();
    OID remoteEpoch = remoteVersion.epoch();

    // Everything changed in-flight, so we need to try again
    if (prevEpoch != localEpoch && localEpoch != remoteEpoch) {
        return VersionChoice::Unknown;
    }

    // We're in the same (zero) epoch as the latest metadata, nothing to do
    if (localEpoch == remoteEpoch && !remoteEpoch.isSet()) {
        return VersionChoice::Local;
    }

    // We're in the same (non-zero) epoch as the latest metadata, so increment the version
    if (localEpoch == remoteEpoch && remoteEpoch.isSet()) {
        // Use the newer version if possible
        if (localVersion < remoteVersion) {
            return VersionChoice::Remote;
        } else {
            return VersionChoice::Local;
        }
    }

    // We're now sure we're installing a new epoch and the epoch didn't change during reload
    dassert(prevEpoch == localEpoch && localEpoch != remoteEpoch);
    return VersionChoice::Remote;
}

}  // namespace

bool isMongos() {
    return false;
}

ShardingState::ShardingState() : _configServerTickets(kMaxConfigServerRefreshThreads) {}

ShardingState::~ShardingState() = default;

ShardingState* ShardingState::get(ServiceContext* serviceContext) {
    return &getShardingState(serviceContext);
}

ShardingState* ShardingState::get(OperationContext* operationContext) {
    return ShardingState::get(operationContext->getServiceContext());
}

bool ShardingState::enabled() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _enabled;
}

ConnectionString ShardingState::getConfigServer(OperationContext* txn) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    invariant(_enabled);

    return grid.shardRegistry()->getConfigServerConnectionString();
}

string ShardingState::getShardName() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    invariant(_enabled);

    return _shardName;
}

void ShardingState::initialize(OperationContext* txn, const string& server) {
    uassert(18509,
            "Unable to obtain host name during sharding initialization.",
            !getHostName().empty());

    stdx::lock_guard<stdx::mutex> lk(_mutex);

    if (_enabled) {
        // TODO: Do we need to throw exception if the config servers have changed from what we
        // already have in place? How do we test for that?
        return;
    }

    ShardedConnectionInfo::addHook();
    ReplicaSetMonitor::setSynchronousConfigChangeHook(
        &ConfigServer::replicaSetChangeShardRegistryUpdateHook);

    ConnectionString configServerCS = uassertStatusOK(ConnectionString::parse(server));
    uassertStatusOK(initializeGlobalShardingState(txn, configServerCS, false));

    _enabled = true;
}

void ShardingState::setShardName(const string& name) {
    const string clientAddr = cc().clientAddress(true);

    stdx::lock_guard<stdx::mutex> lk(_mutex);

    if (_shardName.empty()) {
        // TODO SERVER-2299 remotely verify the name is sound w.r.t IPs
        _shardName = name;

        log() << "remote client " << clientAddr << " initialized this host as shard " << name;
        return;
    }

    if (_shardName != name) {
        const string message = str::stream()
            << "remote client " << clientAddr << " tried to initialize this host as shard " << name
            << ", but shard name was previously initialized as " << _shardName;

        warning() << message;
        uassertStatusOK({ErrorCodes::AlreadyInitialized, message});
    }
}

void ShardingState::clearCollectionMetadata() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _collMetadata.clear();
}

// TODO we shouldn't need three ways for checking the version. Fix this.
bool ShardingState::hasVersion(const string& ns) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    CollectionMetadataMap::const_iterator it = _collMetadata.find(ns);
    return it != _collMetadata.end();
}

ChunkVersion ShardingState::getVersion(const string& ns) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    CollectionMetadataMap::const_iterator it = _collMetadata.find(ns);
    if (it != _collMetadata.end()) {
        shared_ptr<CollectionMetadata> p = it->second;
        return p->getShardVersion();
    } else {
        return ChunkVersion(0, 0, OID());
    }
}

void ShardingState::donateChunk(OperationContext* txn,
                                const string& ns,
                                const BSONObj& min,
                                const BSONObj& max,
                                ChunkVersion version) {
    invariant(txn->lockState()->isCollectionLockedForMode(ns, MODE_X));
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    CollectionMetadataMap::const_iterator it = _collMetadata.find(ns);
    verify(it != _collMetadata.end());
    shared_ptr<CollectionMetadata> p = it->second;

    // empty shards should have version 0
    version = (p->getNumChunks() > 1) ? version : ChunkVersion(0, 0, p->getCollVersion().epoch());

    ChunkType chunk;
    chunk.setMin(min);
    chunk.setMax(max);
    string errMsg;

    shared_ptr<CollectionMetadata> cloned(p->cloneMigrate(chunk, version, &errMsg));
    // uassert to match old behavior, TODO: report errors w/o throwing
    uassert(16855, errMsg, NULL != cloned.get());

    // TODO: a bit dangerous to have two different zero-version states - no-metadata and
    // no-version
    _collMetadata[ns] = cloned;
}

void ShardingState::undoDonateChunk(OperationContext* txn,
                                    const string& ns,
                                    shared_ptr<CollectionMetadata> prevMetadata) {
    invariant(txn->lockState()->isCollectionLockedForMode(ns, MODE_X));
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    log() << "ShardingState::undoDonateChunk acquired _mutex";

    CollectionMetadataMap::iterator it = _collMetadata.find(ns);
    verify(it != _collMetadata.end());
    it->second = prevMetadata;
}

bool ShardingState::notePending(OperationContext* txn,
                                const string& ns,
                                const BSONObj& min,
                                const BSONObj& max,
                                const OID& epoch,
                                string* errMsg) {
    invariant(txn->lockState()->isCollectionLockedForMode(ns, MODE_X));
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    CollectionMetadataMap::const_iterator it = _collMetadata.find(ns);
    if (it == _collMetadata.end()) {
        *errMsg = str::stream() << "could not note chunk "
                                << "[" << min << "," << max << ")"
                                << " as pending because the local metadata for " << ns
                                << " has changed";

        return false;
    }

    shared_ptr<CollectionMetadata> metadata = it->second;

    // This can currently happen because drops aren't synchronized with in-migrations
    // The idea for checking this here is that in the future we shouldn't have this problem
    if (metadata->getCollVersion().epoch() != epoch) {
        *errMsg = str::stream() << "could not note chunk "
                                << "[" << min << "," << max << ")"
                                << " as pending because the epoch for " << ns
                                << " has changed from " << epoch << " to "
                                << metadata->getCollVersion().epoch();

        return false;
    }

    ChunkType chunk;
    chunk.setMin(min);
    chunk.setMax(max);

    shared_ptr<CollectionMetadata> cloned(metadata->clonePlusPending(chunk, errMsg));
    if (!cloned)
        return false;

    _collMetadata[ns] = cloned;
    return true;
}

bool ShardingState::forgetPending(OperationContext* txn,
                                  const string& ns,
                                  const BSONObj& min,
                                  const BSONObj& max,
                                  const OID& epoch,
                                  string* errMsg) {
    invariant(txn->lockState()->isCollectionLockedForMode(ns, MODE_X));
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    CollectionMetadataMap::const_iterator it = _collMetadata.find(ns);
    if (it == _collMetadata.end()) {
        *errMsg = str::stream() << "no need to forget pending chunk "
                                << "[" << min << "," << max << ")"
                                << " because the local metadata for " << ns << " has changed";

        return false;
    }

    shared_ptr<CollectionMetadata> metadata = it->second;

    // This can currently happen because drops aren't synchronized with in-migrations
    // The idea for checking this here is that in the future we shouldn't have this problem
    if (metadata->getCollVersion().epoch() != epoch) {
        *errMsg = str::stream() << "no need to forget pending chunk "
                                << "[" << min << "," << max << ")"
                                << " because the epoch for " << ns << " has changed from " << epoch
                                << " to " << metadata->getCollVersion().epoch();

        return false;
    }

    ChunkType chunk;
    chunk.setMin(min);
    chunk.setMax(max);

    shared_ptr<CollectionMetadata> cloned(metadata->cloneMinusPending(chunk, errMsg));
    if (!cloned)
        return false;

    _collMetadata[ns] = cloned;
    return true;
}

void ShardingState::splitChunk(OperationContext* txn,
                               const string& ns,
                               const BSONObj& min,
                               const BSONObj& max,
                               const vector<BSONObj>& splitKeys,
                               ChunkVersion version) {
    invariant(txn->lockState()->isCollectionLockedForMode(ns, MODE_X));
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    CollectionMetadataMap::const_iterator it = _collMetadata.find(ns);
    verify(it != _collMetadata.end());

    ChunkType chunk;
    chunk.setMin(min);
    chunk.setMax(max);
    string errMsg;

    shared_ptr<CollectionMetadata> cloned(
        it->second->cloneSplit(chunk, splitKeys, version, &errMsg));
    // uassert to match old behavior, TODO: report errors w/o throwing
    uassert(16857, errMsg, NULL != cloned.get());

    _collMetadata[ns] = cloned;
}

void ShardingState::mergeChunks(OperationContext* txn,
                                const string& ns,
                                const BSONObj& minKey,
                                const BSONObj& maxKey,
                                ChunkVersion mergedVersion) {
    invariant(txn->lockState()->isCollectionLockedForMode(ns, MODE_X));
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    CollectionMetadataMap::const_iterator it = _collMetadata.find(ns);
    verify(it != _collMetadata.end());

    string errMsg;

    shared_ptr<CollectionMetadata> cloned(
        it->second->cloneMerge(minKey, maxKey, mergedVersion, &errMsg));
    // uassert to match old behavior, TODO: report errors w/o throwing
    uassert(17004, errMsg, NULL != cloned.get());

    _collMetadata[ns] = cloned;
}

bool ShardingState::inCriticalMigrateSection() {
    return _migrationSourceManager.getInCriticalSection();
}

bool ShardingState::waitTillNotInCriticalSection(int maxSecondsToWait) {
    return _migrationSourceManager.waitTillNotInCriticalSection(maxSecondsToWait);
}

void ShardingState::resetMetadata(const string& ns) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    warning() << "resetting metadata for " << ns << ", this should only be used in testing";

    _collMetadata.erase(ns);
}

Status ShardingState::refreshMetadataIfNeeded(OperationContext* txn,
                                              const string& ns,
                                              const ChunkVersion& reqShardVersion,
                                              ChunkVersion* latestShardVersion) {
    // The _configServerTickets serializes this process such that only a small number of threads
    // can try to refresh at the same time.

    LOG(2) << "metadata refresh requested for " << ns << " at shard version " << reqShardVersion;

    //
    // Queuing of refresh requests starts here when remote reload is needed. This may take time.
    // TODO: Explicitly expose the queuing discipline.
    //

    _configServerTickets.waitForTicket();
    TicketHolderReleaser needTicketFrom(&_configServerTickets);

    //
    // Fast path - check if the requested version is at a higher version than the current
    // metadata version or a different epoch before verifying against config server.
    //

    shared_ptr<CollectionMetadata> storedMetadata;
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        CollectionMetadataMap::iterator it = _collMetadata.find(ns);
        if (it != _collMetadata.end())
            storedMetadata = it->second;
    }

    ChunkVersion storedShardVersion;
    if (storedMetadata)
        storedShardVersion = storedMetadata->getShardVersion();
    *latestShardVersion = storedShardVersion;

    if (storedShardVersion >= reqShardVersion &&
        storedShardVersion.epoch() == reqShardVersion.epoch()) {
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

    if (storedShardVersion.epoch() != reqShardVersion.epoch()) {
        // Need to remotely reload if our epochs aren't the same, to verify
        LOG(1) << "metadata change requested for " << ns << ", from shard version "
               << storedShardVersion << " to " << reqShardVersion
               << ", need to verify with config server";
    } else {
        // Need to remotely reload since our epochs aren't the same but our version is greater
        LOG(1) << "metadata version update requested for " << ns << ", from shard version "
               << storedShardVersion << " to " << reqShardVersion
               << ", need to verify with config server";
    }

    return _refreshMetadata(txn, ns, reqShardVersion, true, latestShardVersion);
}

Status ShardingState::refreshMetadataNow(OperationContext* txn,
                                         const string& ns,
                                         ChunkVersion* latestShardVersion) {
    return _refreshMetadata(txn, ns, ChunkVersion(0, 0, OID()), false, latestShardVersion);
}

Status ShardingState::_refreshMetadata(OperationContext* txn,
                                       const string& ns,
                                       const ChunkVersion& reqShardVersion,
                                       bool useRequestedVersion,
                                       ChunkVersion* latestShardVersion) {
    // The idea here is that we're going to reload the metadata from the config server, but
    // we need to do so outside any locks.  When we get our result back, if the current metadata
    // has changed, we may not be able to install the new metadata.

    //
    // Get the initial metadata
    // No DBLock is needed since the metadata is expected to change during reload.
    //

    shared_ptr<CollectionMetadata> beforeMetadata;

    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);

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
    if (beforeMetadata) {
        beforeShardVersion = beforeMetadata->getShardVersion();
        beforeCollVersion = beforeMetadata->getCollVersion();
    }

    *latestShardVersion = beforeShardVersion;

    //
    // Determine whether we need to diff or fully reload
    //

    bool fullReload = false;
    if (!beforeMetadata) {
        // We don't have any metadata to reload from
        fullReload = true;
    } else if (useRequestedVersion && reqShardVersion.epoch() != beforeShardVersion.epoch()) {
        // It's not useful to use the metadata as a base because we think the epoch will differ
        fullReload = true;
    }

    //
    // Load the metadata from the remote server, start construction
    //

    LOG(0) << "remotely refreshing metadata for " << ns
           << (useRequestedVersion
                   ? string(" with requested shard version ") + reqShardVersion.toString()
                   : "")
           << (fullReload ? ", current shard version is " : " based on current shard version ")
           << beforeShardVersion << ", current metadata version is " << beforeCollVersion;

    string errMsg;

    MetadataLoader mdLoader;
    shared_ptr<CollectionMetadata> remoteMetadata(std::make_shared<CollectionMetadata>());

    Timer refreshTimer;
    long long refreshMillis;

    {
        Status status = mdLoader.makeCollectionMetadata(txn,
                                                        grid.catalogManager(txn),
                                                        ns,
                                                        getShardName(),
                                                        fullReload ? NULL : beforeMetadata.get(),
                                                        remoteMetadata.get());
        refreshMillis = refreshTimer.millis();

        if (status.code() == ErrorCodes::NamespaceNotFound) {
            remoteMetadata.reset();
        } else if (!status.isOK()) {
            warning() << "could not remotely refresh metadata for " << ns
                      << causedBy(status.reason());

            return status;
        }
    }

    ChunkVersion remoteShardVersion;
    ChunkVersion remoteCollVersion;
    if (remoteMetadata) {
        remoteShardVersion = remoteMetadata->getShardVersion();
        remoteCollVersion = remoteMetadata->getCollVersion();
    }

    //
    // Get ready to install loaded metadata if needed
    //

    shared_ptr<CollectionMetadata> afterMetadata;
    ChunkVersion afterShardVersion;
    ChunkVersion afterCollVersion;
    VersionChoice choice;

    // If we choose to install the new metadata, this describes the kind of install
    enum InstallType {
        InstallType_New,
        InstallType_Update,
        InstallType_Replace,
        InstallType_Drop,
        InstallType_None
    } installType = InstallType_None;  // compiler complains otherwise

    {
        // Exclusive collection lock needed since we're now potentially changing the metadata,
        // and don't want reads/writes to be ongoing.
        ScopedTransaction transaction(txn, MODE_IX);
        Lock::DBLock dbLock(txn->lockState(), nsToDatabaseSubstring(ns), MODE_IX);
        Lock::CollectionLock collLock(txn->lockState(), ns, MODE_X);

        //
        // Get the metadata now that the load has completed
        //

        stdx::lock_guard<stdx::mutex> lk(_mutex);

        // Don't reload if our config server has changed or sharding is no longer enabled
        if (!_enabled) {
            string errMsg = str::stream() << "could not refresh metadata for " << ns
                                          << ", sharding is no longer enabled";

            warning() << errMsg;
            return Status(ErrorCodes::NotYetInitialized, errMsg);
        }

        CollectionMetadataMap::iterator it = _collMetadata.find(ns);
        if (it != _collMetadata.end())
            afterMetadata = it->second;

        if (afterMetadata) {
            afterShardVersion = afterMetadata->getShardVersion();
            afterCollVersion = afterMetadata->getCollVersion();
        }

        *latestShardVersion = afterShardVersion;

        //
        // Resolve newer pending chunks with the remote metadata, finish construction
        //

        Status status = mdLoader.promotePendingChunks(afterMetadata.get(), remoteMetadata.get());

        if (!status.isOK()) {
            warning() << "remote metadata for " << ns
                      << " is inconsistent with current pending chunks"
                      << causedBy(status.reason());

            return status;
        }

        //
        // Compare the 'before', 'after', and 'remote' versions/epochs and choose newest
        // Zero-epochs (sentinel value for "dropped" collections), are tested by
        // !epoch.isSet().
        //

        choice = chooseNewestVersion(beforeCollVersion, afterCollVersion, remoteCollVersion);

        if (choice == VersionChoice::Remote) {
            dassert(!remoteCollVersion.epoch().isSet() || remoteShardVersion >= beforeShardVersion);

            if (!afterCollVersion.epoch().isSet()) {
                // First metadata load
                installType = InstallType_New;
                dassert(it == _collMetadata.end());
                _collMetadata.insert(make_pair(ns, remoteMetadata));
            } else if (remoteCollVersion.epoch().isSet() &&
                       remoteCollVersion.epoch() == afterCollVersion.epoch()) {
                // Update to existing metadata
                installType = InstallType_Update;

                // Invariant: If CollMetadata was not found, version should be have been 0.
                dassert(it != _collMetadata.end());
                it->second = remoteMetadata;
            } else if (remoteCollVersion.epoch().isSet()) {
                // New epoch detected, replacing metadata
                installType = InstallType_Replace;

                // Invariant: If CollMetadata was not found, version should be have been 0.
                dassert(it != _collMetadata.end());
                it->second = remoteMetadata;
            } else {
                dassert(!remoteCollVersion.epoch().isSet());

                // Drop detected
                installType = InstallType_Drop;
                _collMetadata.erase(it);
            }

            *latestShardVersion = remoteShardVersion;
        }
    }
    // End _mutex
    // End DBWrite

    //
    // Do messaging based on what happened above
    //
    string localShardVersionMsg = beforeShardVersion.epoch() == afterShardVersion.epoch()
        ? afterShardVersion.toString()
        : beforeShardVersion.toString() + " / " + afterShardVersion.toString();

    if (choice == VersionChoice::Unknown) {
        string errMsg = str::stream()
            << "need to retry loading metadata for " << ns
            << ", collection may have been dropped or recreated during load"
            << " (loaded shard version : " << remoteShardVersion.toString()
            << ", stored shard versions : " << localShardVersionMsg << ", took " << refreshMillis
            << "ms)";

        warning() << errMsg;
        return Status(ErrorCodes::RemoteChangeDetected, errMsg);
    }

    if (choice == VersionChoice::Local) {
        LOG(0) << "metadata of collection " << ns
               << " already up to date (shard version : " << afterShardVersion.toString()
               << ", took " << refreshMillis << "ms)";
        return Status::OK();
    }

    dassert(choice == VersionChoice::Remote);

    switch (installType) {
        case InstallType_New:
            LOG(0) << "collection " << ns << " was previously unsharded"
                   << ", new metadata loaded with shard version " << remoteShardVersion;
            break;
        case InstallType_Update:
            LOG(0) << "updating metadata for " << ns << " from shard version "
                   << localShardVersionMsg << " to shard version " << remoteShardVersion;
            break;
        case InstallType_Replace:
            LOG(0) << "replacing metadata for " << ns << " at shard version "
                   << localShardVersionMsg << " with a new epoch (shard version "
                   << remoteShardVersion << ")";
            break;
        case InstallType_Drop:
            LOG(0) << "dropping metadata for " << ns << " at shard version " << localShardVersionMsg
                   << ", took " << refreshMillis << "ms";
            break;
        default:
            verify(false);
            break;
    }

    if (installType != InstallType_Drop) {
        LOG(0) << "collection version was loaded at version " << remoteCollVersion << ", took "
               << refreshMillis << "ms";
    }

    return Status::OK();
}

void ShardingState::appendInfo(OperationContext* txn, BSONObjBuilder& builder) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    builder.appendBool("enabled", _enabled);
    if (!_enabled) {
        return;
    }

    builder.append("configServer",
                   grid.shardRegistry()->getConfigServerConnectionString().toString());
    builder.append("shardName", _shardName);

    BSONObjBuilder versionB(builder.subobjStart("versions"));
    for (CollectionMetadataMap::const_iterator it = _collMetadata.begin();
         it != _collMetadata.end();
         ++it) {
        shared_ptr<CollectionMetadata> metadata = it->second;
        versionB.appendTimestamp(it->first, metadata->getShardVersion().toLong());
    }

    versionB.done();
}

bool ShardingState::needCollectionMetadata(OperationContext* txn, const string& ns) const {
    if (!_enabled)
        return false;

    Client* client = txn->getClient();

    // Shard version information received from mongos may either by attached to the Client or
    // directly to the OperationContext.
    return ShardedConnectionInfo::get(client, false) ||
        OperationShardVersion::get(txn).hasShardVersion();
}

shared_ptr<CollectionMetadata> ShardingState::getCollectionMetadata(const string& ns) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    CollectionMetadataMap::const_iterator it = _collMetadata.find(ns);
    if (it == _collMetadata.end()) {
        return shared_ptr<CollectionMetadata>();
    } else {
        return it->second;
    }
}

}  // namespace mongo
