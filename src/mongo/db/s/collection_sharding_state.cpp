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

#include "mongo/db/s/collection_sharding_state.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/sharded_connection_info.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/network_interface_thread_pool.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/log.h"
#include "mongo/util/string_map.h"

namespace mongo {
namespace {

// How long to wait before starting cleanup of an emigrated chunk range
MONGO_EXPORT_SERVER_PARAMETER(orphanCleanupDelaySecs, int, 900);  // 900s = 15m

/**
 * Lazy-instantiated task executor shared by the collection range deleters. Must outlive the
 * CollectionShardingStateMap below.
 */
class RangeDeleterExecutorHolder {
    MONGO_DISALLOW_COPYING(RangeDeleterExecutorHolder);

public:
    RangeDeleterExecutorHolder() = default;

    ~RangeDeleterExecutorHolder() {
        if (_taskExecutor) {
            _taskExecutor->shutdown();
            _taskExecutor->join();
        }
    }

    executor::TaskExecutor* getOrCreateExecutor() {
        stdx::lock_guard<stdx::mutex> lg(_mutex);

        if (!_taskExecutor) {
            const std::string kExecName("CollectionRangeDeleter-TaskExecutor");

            auto net = executor::makeNetworkInterface(kExecName);
            auto pool = stdx::make_unique<executor::NetworkInterfaceThreadPool>(net.get());
            auto taskExecutor = stdx::make_unique<executor::ThreadPoolTaskExecutor>(std::move(pool),
                                                                                    std::move(net));
            taskExecutor->startup();

            _taskExecutor = std::move(taskExecutor);
        }

        return _taskExecutor.get();
    }

private:
    stdx::mutex _mutex;
    std::unique_ptr<executor::TaskExecutor> _taskExecutor{nullptr};
};

const auto getRangeDeleterExecutorHolder =
    ServiceContext::declareDecoration<RangeDeleterExecutorHolder>();

/**
 * This map matches 1:1 with the set of collections in the storage catalog. It is not safe to
 * look-up values from this map without holding some form of collection lock. It is only safe to
 * add/remove values when holding X lock on the respective namespace.
 */
class CollectionShardingStateMap {
    MONGO_DISALLOW_COPYING(CollectionShardingStateMap);

public:
    CollectionShardingStateMap() = default;

    static const ServiceContext::Decoration<CollectionShardingStateMap> get;

    CollectionShardingState& getOrCreate(const std::string& ns) {
        stdx::lock_guard<stdx::mutex> lg(_mutex);

        auto it = _collections.find(ns);
        if (it == _collections.end()) {
            auto inserted = _collections.try_emplace(
                ns,
                std::make_shared<CollectionShardingState>(get.owner(this), NamespaceString(ns)));
            invariant(inserted.second);
            it = std::move(inserted.first);
        }

        return *it->second;
    }

    void resetAll() {
        stdx::lock_guard<stdx::mutex> lg(_mutex);
        for (auto it = _collections.begin(); it != _collections.end(); ++it) {
            // This is a hack to get around CollectionShardingState::refreshMetadata() requiring
            // the X lock: markNotShardedAtStepdown() doesn't have a lock check. Temporary
            // measure until SERVER-31595 removes the X lock requirement.
            it->second->markNotShardedAtStepdown();
        }
    }

    void report(OperationContext* opCtx, BSONObjBuilder* builder) {
        BSONObjBuilder versionB(builder->subobjStart("versions"));

        {
            stdx::lock_guard<stdx::mutex> lg(_mutex);

            for (auto& coll : _collections) {
                ScopedCollectionMetadata metadata = coll.second->getMetadata(opCtx);
                if (metadata) {
                    versionB.appendTimestamp(coll.first, metadata->getShardVersion().toLong());
                } else {
                    versionB.appendTimestamp(coll.first, ChunkVersion::UNSHARDED().toLong());
                }
            }
        }

        versionB.done();
    }

private:
    mutable stdx::mutex _mutex;

    using CollectionsMap = StringMap<std::shared_ptr<CollectionShardingState>>;
    CollectionsMap _collections;
};

const ServiceContext::Decoration<CollectionShardingStateMap> CollectionShardingStateMap::get =
    ServiceContext::declareDecoration<CollectionShardingStateMap>();

}  // namespace

CollectionShardingState::CollectionShardingState(ServiceContext* sc, NamespaceString nss)
    : _nss(std::move(nss)),
      _metadataManager(std::make_shared<MetadataManager>(
          sc, _nss, getRangeDeleterExecutorHolder(sc).getOrCreateExecutor())) {}

CollectionShardingState* CollectionShardingState::get(OperationContext* opCtx,
                                                      const NamespaceString& nss) {
    return CollectionShardingState::get(opCtx, nss.ns());
}

CollectionShardingState* CollectionShardingState::get(OperationContext* opCtx,
                                                      const std::string& ns) {
    // Collection lock must be held to have a reference to the collection's sharding state
    dassert(opCtx->lockState()->isCollectionLockedForMode(ns, MODE_IS));

    auto& collectionsMap = CollectionShardingStateMap::get(opCtx->getServiceContext());
    return &collectionsMap.getOrCreate(ns);
}

void CollectionShardingState::resetAll(OperationContext* opCtx) {
    auto& collectionsMap = CollectionShardingStateMap::get(opCtx->getServiceContext());
    collectionsMap.resetAll();
}

void CollectionShardingState::report(OperationContext* opCtx, BSONObjBuilder* builder) {
    auto& collectionsMap = CollectionShardingStateMap::get(opCtx->getServiceContext());
    collectionsMap.report(opCtx, builder);
}

ScopedCollectionMetadata CollectionShardingState::getMetadata(OperationContext* opCtx) {
    auto atClusterTime = repl::ReadConcernArgs::get(opCtx).getArgsAtClusterTime();
    return _metadataManager->getActiveMetadata(_metadataManager, atClusterTime);
}

void CollectionShardingState::refreshMetadata(OperationContext* opCtx,
                                              std::unique_ptr<CollectionMetadata> newMetadata) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(_nss.ns(), MODE_X));

    _metadataManager->refreshActiveMetadata(std::move(newMetadata));
}

void CollectionShardingState::markNotShardedAtStepdown() {
    _metadataManager->refreshActiveMetadata(nullptr);
}

auto CollectionShardingState::beginReceive(ChunkRange const& range) -> CleanupNotification {
    return _metadataManager->beginReceive(range);
}

void CollectionShardingState::forgetReceive(const ChunkRange& range) {
    _metadataManager->forgetReceive(range);
}

auto CollectionShardingState::cleanUpRange(ChunkRange const& range, CleanWhen when)
    -> CleanupNotification {
    Date_t time = (when == kNow) ? Date_t{} : Date_t::now() +
            stdx::chrono::seconds{orphanCleanupDelaySecs.load()};
    return _metadataManager->cleanUpRange(range, time);
}

std::vector<ScopedCollectionMetadata> CollectionShardingState::overlappingMetadata(
    ChunkRange const& range) const {
    return _metadataManager->overlappingMetadata(_metadataManager, range);
}

void CollectionShardingState::enterCriticalSectionCatchUpPhase(OperationContext* opCtx) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(_nss.ns(), MODE_X));
    _critSec.enterCriticalSectionCatchUpPhase();
}

void CollectionShardingState::enterCriticalSectionCommitPhase(OperationContext* opCtx) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(_nss.ns(), MODE_X));
    _critSec.enterCriticalSectionCommitPhase();
}

void CollectionShardingState::exitCriticalSection(OperationContext* opCtx) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(_nss.ns(), MODE_X));
    _critSec.exitCriticalSection();
}

void CollectionShardingState::checkShardVersionOrThrow(OperationContext* opCtx) {
    auto& oss = OperationShardingState::get(opCtx);

    const auto receivedShardVersion = [&] {
        // If there is a version attached to the OperationContext, use it as the received version,
        // otherwise get the received version from the ShardedConnectionInfo
        if (oss.hasShardVersion()) {
            return oss.getShardVersion(_nss);
        } else if (auto const info = ShardedConnectionInfo::get(opCtx->getClient(), false)) {
            auto connectionShardVersion = info->getVersion(_nss.ns());

            // For backwards compatibility with map/reduce, which can access up to 2 sharded
            // collections in a single call, the lack of version for a namespace on the collection
            // must be treated as UNSHARDED
            return connectionShardVersion.value_or(ChunkVersion::UNSHARDED());
        } else {
            // There is no shard version information on either 'opCtx' or 'client'. This means that
            // the operation represented by 'opCtx' is unversioned, and the shard version is always
            // OK for unversioned operations.
            return ChunkVersion::IGNORED();
        }
    }();

    if (ChunkVersion::isIgnoredVersion(receivedShardVersion)) {
        return;
    }

    // An operation with read concern 'available' should never have shardVersion set.
    invariant(repl::ReadConcernArgs::get(opCtx).getLevel() !=
              repl::ReadConcernLevel::kAvailableReadConcern);

    // Set this for error messaging purposes before potentially returning false.
    auto metadata = getMetadata(opCtx);
    const auto wantedShardVersion =
        metadata ? metadata->getShardVersion() : ChunkVersion::UNSHARDED();

    auto criticalSectionSignal = _critSec.getSignal(opCtx->lockState()->isWriteLocked()
                                                        ? ShardingMigrationCriticalSection::kWrite
                                                        : ShardingMigrationCriticalSection::kRead);
    if (criticalSectionSignal) {
        // Set migration critical section on operation sharding state: operation will wait for the
        // migration to finish before returning failure and retrying.
        oss.setMigrationCriticalSectionSignal(criticalSectionSignal);

        uasserted(StaleConfigInfo(_nss, receivedShardVersion, wantedShardVersion),
                  str::stream() << "migration commit in progress for " << _nss.ns());
    }

    if (receivedShardVersion.isWriteCompatibleWith(wantedShardVersion)) {
        return;
    }

    //
    // Figure out exactly why not compatible, send appropriate error message
    // The versions themselves are returned in the error, so not needed in messages here
    //

    StaleConfigInfo sci(_nss, receivedShardVersion, wantedShardVersion);

    uassert(std::move(sci),
            str::stream() << "epoch mismatch detected for " << _nss.ns() << ", "
                          << "the collection may have been dropped and recreated",
            wantedShardVersion.epoch() == receivedShardVersion.epoch());

    if (!wantedShardVersion.isSet() && receivedShardVersion.isSet()) {
        uasserted(std::move(sci),
                  str::stream() << "this shard no longer contains chunks for " << _nss.ns() << ", "
                                << "the collection may have been dropped");
    }

    if (wantedShardVersion.isSet() && !receivedShardVersion.isSet()) {
        uasserted(std::move(sci),
                  str::stream() << "this shard contains chunks for " << _nss.ns() << ", "
                                << "but the client expects unsharded collection");
    }

    if (wantedShardVersion.majorVersion() != receivedShardVersion.majorVersion()) {
        // Could be > or < - wanted is > if this is the source of a migration, wanted < if this is
        // the target of a migration
        uasserted(std::move(sci), str::stream() << "version mismatch detected for " << _nss.ns());
    }

    // Those are all the reasons the versions can mismatch
    MONGO_UNREACHABLE;
}

// Call with collection unlocked.  Note that the CollectionShardingState object involved might not
// exist anymore at the time of the call, or indeed anytime outside the AutoGetCollection block, so
// anything that might alias something in it must be copied first.

Status CollectionShardingState::waitForClean(OperationContext* opCtx,
                                             const NamespaceString& nss,
                                             OID const& epoch,
                                             ChunkRange orphanRange) {
    while (true) {
        boost::optional<CleanupNotification> stillScheduled;

        {
            AutoGetCollection autoColl(opCtx, nss, MODE_IX);
            auto css = CollectionShardingState::get(opCtx, nss);

            {
                // First, see if collection was dropped, but do it in a separate scope in order to
                // not hold reference on it, which would make it appear in use
                auto metadata =
                    css->_metadataManager->getActiveMetadata(css->_metadataManager, boost::none);
                if (!metadata || metadata->getCollVersion().epoch() != epoch) {
                    return {ErrorCodes::StaleShardVersion, "Collection being migrated was dropped"};
                }
            }

            stillScheduled = css->trackOrphanedDataCleanup(orphanRange);
            if (!stillScheduled) {
                log() << "Finished deleting " << nss.ns() << " range "
                      << redact(orphanRange.toString());
                return Status::OK();
            }
        }

        log() << "Waiting for deletion of " << nss.ns() << " range " << orphanRange;

        Status result = stillScheduled->waitStatus(opCtx);
        if (!result.isOK()) {
            return result.withContext(str::stream() << "Failed to delete orphaned " << nss.ns()
                                                    << " range "
                                                    << orphanRange.toString());
        }
    }

    MONGO_UNREACHABLE;
}

auto CollectionShardingState::trackOrphanedDataCleanup(ChunkRange const& range)
    -> boost::optional<CleanupNotification> {
    return _metadataManager->trackOrphanedDataCleanup(range);
}

boost::optional<ChunkRange> CollectionShardingState::getNextOrphanRange(BSONObj const& from) {
    return _metadataManager->getNextOrphanRange(from);
}

}  // namespace mongo
