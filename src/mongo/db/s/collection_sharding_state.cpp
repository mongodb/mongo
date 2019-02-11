/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/collection_sharding_state.h"

#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/sharded_connection_info.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/log.h"
#include "mongo/util/string_map.h"

namespace mongo {
namespace {

class CollectionShardingStateMap {
    MONGO_DISALLOW_COPYING(CollectionShardingStateMap);

public:
    static const ServiceContext::Decoration<boost::optional<CollectionShardingStateMap>> get;

    CollectionShardingStateMap(std::unique_ptr<CollectionShardingStateFactory> factory)
        : _factory(std::move(factory)) {}

    CollectionShardingState& getOrCreate(const NamespaceString& nss) {
        stdx::lock_guard<stdx::mutex> lg(_mutex);

        auto it = _collections.find(nss.ns());
        if (it == _collections.end()) {
            auto inserted = _collections.try_emplace(nss.ns(), _factory->make(nss));
            invariant(inserted.second);
            it = std::move(inserted.first);
        }

        return *it->second;
    }

    void report(OperationContext* opCtx, BSONObjBuilder* builder) {
        BSONObjBuilder versionB(builder->subobjStart("versions"));

        {
            stdx::lock_guard<stdx::mutex> lg(_mutex);

            for (auto& coll : _collections) {
                const auto optMetadata = coll.second->getCurrentMetadataIfKnown();
                if (optMetadata) {
                    const auto& metadata = *optMetadata;
                    versionB.appendTimestamp(coll.first, metadata->getShardVersion().toLong());
                }
            }
        }

        versionB.done();
    }

private:
    using CollectionsMap = StringMap<std::shared_ptr<CollectionShardingState>>;

    std::unique_ptr<CollectionShardingStateFactory> _factory;

    stdx::mutex _mutex;
    CollectionsMap _collections;
};

const ServiceContext::Decoration<boost::optional<CollectionShardingStateMap>>
    CollectionShardingStateMap::get =
        ServiceContext::declareDecoration<boost::optional<CollectionShardingStateMap>>();

class UnshardedCollection : public ScopedCollectionMetadata::Impl {
public:
    UnshardedCollection() = default;

    const CollectionMetadata& get() override {
        return _metadata;
    }

private:
    CollectionMetadata _metadata;
};

const auto kUnshardedCollection = std::make_shared<UnshardedCollection>();

ChunkVersion getOperationReceivedVersion(OperationContext* opCtx, const NamespaceString& nss) {
    auto& oss = OperationShardingState::get(opCtx);

    // If there is a version attached to the OperationContext, use it as the received version,
    // otherwise get the received version from the ShardedConnectionInfo
    if (oss.hasShardVersion()) {
        return oss.getShardVersion(nss);
    } else if (auto const info = ShardedConnectionInfo::get(opCtx->getClient(), false)) {
        auto connectionShardVersion = info->getVersion(nss.ns());

        // For backwards compatibility with map/reduce, which can access up to 2 sharded collections
        // in a single call, the lack of version for a namespace on the collection must be treated
        // as UNSHARDED
        return connectionShardVersion.value_or(ChunkVersion::UNSHARDED());
    } else {
        // There is no shard version information on either 'opCtx' or 'client'. This means that the
        // operation represented by 'opCtx' is unversioned, and the shard version is always OK for
        // unversioned operations.
        return ChunkVersion::IGNORED();
    }
}

}  // namespace

CollectionShardingState::CollectionShardingState(NamespaceString nss)
    : _stateChangeMutex(nss.toString()), _nss(std::move(nss)) {}

CollectionShardingState* CollectionShardingState::get(OperationContext* opCtx,
                                                      const NamespaceString& nss) {
    // Collection lock must be held to have a reference to the collection's sharding state
    dassert(opCtx->lockState()->isCollectionLockedForMode(nss.ns(), MODE_IS));

    auto& collectionsMap = CollectionShardingStateMap::get(opCtx->getServiceContext());
    return &collectionsMap->getOrCreate(nss);
}

void CollectionShardingState::report(OperationContext* opCtx, BSONObjBuilder* builder) {
    auto& collectionsMap = CollectionShardingStateMap::get(opCtx->getServiceContext());
    collectionsMap->report(opCtx, builder);
}

ScopedCollectionMetadata CollectionShardingState::getMetadataForOperation(OperationContext* opCtx) {
    const auto receivedShardVersion = getOperationReceivedVersion(opCtx, _nss);

    if (ChunkVersion::isIgnoredVersion(receivedShardVersion)) {
        return {kUnshardedCollection};
    }

    const auto atClusterTime = repl::ReadConcernArgs::get(opCtx).getArgsAtClusterTime();
    auto optMetadata = _getMetadata(atClusterTime);

    if (!optMetadata)
        return {kUnshardedCollection};

    return {std::move(*optMetadata)};
}

ScopedCollectionMetadata CollectionShardingState::getCurrentMetadata() {
    auto optMetadata = _getMetadata(boost::none);

    if (!optMetadata)
        return {kUnshardedCollection};

    return {std::move(*optMetadata)};
}

boost::optional<ScopedCollectionMetadata> CollectionShardingState::getCurrentMetadataIfKnown() {
    return _getMetadata(boost::none);
}

boost::optional<ChunkVersion> CollectionShardingState::getCurrentShardVersionIfKnown() {
    const auto optMetadata = _getMetadata(boost::none);
    if (!optMetadata)
        return boost::none;

    const auto& metadata = *optMetadata;
    if (!metadata->isSharded())
        return ChunkVersion::UNSHARDED();

    return metadata->getCollVersion();
}

void CollectionShardingState::checkShardVersionOrThrow(OperationContext* opCtx) {
    const auto receivedShardVersion = getOperationReceivedVersion(opCtx, _nss);

    if (ChunkVersion::isIgnoredVersion(receivedShardVersion)) {
        return;
    }

    // An operation with read concern 'available' should never have shardVersion set.
    invariant(repl::ReadConcernArgs::get(opCtx).getLevel() !=
              repl::ReadConcernLevel::kAvailableReadConcern);

    const auto metadata = getMetadataForOperation(opCtx);
    const auto wantedShardVersion =
        metadata->isSharded() ? metadata->getShardVersion() : ChunkVersion::UNSHARDED();

    auto criticalSectionSignal = [&] {
        auto csrLock = CSRLock::lock(opCtx, this);
        return _critSec.getSignal(opCtx->lockState()->isWriteLocked()
                                      ? ShardingMigrationCriticalSection::kWrite
                                      : ShardingMigrationCriticalSection::kRead);
    }();

    if (criticalSectionSignal) {
        // Set migration critical section on operation sharding state: operation will wait for the
        // migration to finish before returning failure and retrying.
        auto& oss = OperationShardingState::get(opCtx);
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

void CollectionShardingState::enterCriticalSectionCatchUpPhase(OperationContext* opCtx, CSRLock&) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(_nss.ns(), MODE_X));
    _critSec.enterCriticalSectionCatchUpPhase();
}

void CollectionShardingState::enterCriticalSectionCommitPhase(OperationContext* opCtx, CSRLock&) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(_nss.ns(), MODE_X));
    _critSec.enterCriticalSectionCommitPhase();
}

void CollectionShardingState::exitCriticalSection(OperationContext* opCtx, CSRLock&) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(_nss.ns(), MODE_IX));
    _critSec.exitCriticalSection();
}

void CollectionShardingStateFactory::set(ServiceContext* service,
                                         std::unique_ptr<CollectionShardingStateFactory> factory) {
    auto& collectionsMap = CollectionShardingStateMap::get(service);
    invariant(!collectionsMap);
    invariant(factory);
    collectionsMap.emplace(std::move(factory));
}

void CollectionShardingStateFactory::clear(ServiceContext* service) {
    auto& collectionsMap = CollectionShardingStateMap::get(service);
    collectionsMap.reset();
}

}  // namespace mongo
