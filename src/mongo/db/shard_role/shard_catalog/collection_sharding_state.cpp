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

#include "mongo/db/shard_role/shard_catalog/collection_sharding_state.h"

#include "mongo/db/server_options.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/platform/rwmutex.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"

#include <boost/none.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

class CollectionShardingStateMap {
    CollectionShardingStateMap(const CollectionShardingStateMap&) = delete;
    CollectionShardingStateMap& operator=(const CollectionShardingStateMap&) = delete;

public:
    static const ServiceContext::Decoration<boost::optional<CollectionShardingStateMap>> get;

    CollectionShardingStateMap(std::unique_ptr<CollectionShardingStateFactory> factory)
        : _factory(std::move(factory)) {}

    class CSSAndLock {
    public:
        CSSAndLock(std::unique_ptr<CollectionShardingState> css) : css(std::move(css)) {}

        mutable std::shared_mutex cssMutex;  // NOLINT
        std::unique_ptr<CollectionShardingState> css;
    };

    CSSAndLock* getOrCreate(const NamespaceString& nss) {
        std::shared_lock readLock(_mutex);  // NOLINT
        if (auto it = _collections.find(nss); MONGO_likely(it != _collections.end())) {
            return &it->second;
        }
        readLock.unlock();
        stdx::lock_guard writeLock(_mutex);
        auto [it, _] = _collections.emplace(nss, _factory->make(nss));
        return &it->second;
    }

    void appendInfoForShardingStateCommand(BSONObjBuilder* builder) const {
        std::vector<const CSSAndLock*> cssAndLocks;
        {
            std::shared_lock lk(_mutex);  // NOLINT
            cssAndLocks.reserve(_collections.size());
            for (const auto& [_, cssAndLock] : _collections) {
                cssAndLocks.emplace_back(&cssAndLock);
            }
        }

        BSONObjBuilder versionB(builder->subobjStart("versions"));
        for (auto cssAndLock : cssAndLocks) {
            CollectionShardingState::ScopedCollectionShardingState scopedCss(
                std::shared_lock(cssAndLock->cssMutex), cssAndLock->css.get());
            scopedCss->appendShardVersion(builder);
        }
        versionB.done();
    }

    std::vector<NamespaceString> getCollectionNames() const {
        std::shared_lock lk(_mutex);  // NOLINT
        std::vector<NamespaceString> result;
        result.reserve(_collections.size());
        for (const auto& [nss, _] : _collections) {
            result.emplace_back(nss);
        }
        return result;
    }

    const StaleShardCollectionMetadataHandler& getStaleShardExceptionHandler() {
        return _factory->getStaleShardExceptionHandler();
    }

private:
    std::unique_ptr<CollectionShardingStateFactory> _factory;

    // Adding entries to `_collections` is expected to be very infrequent and far apart (collection
    // creation), so the majority of accesses to this map are read-only and benefit from using a
    // shared mutex type for synchronization.
    mutable RWMutex _mutex;

    // Entries of the _collections map must never be deleted or replaced. This is to guarantee that
    // a 'nss' is always associated to the same 'cssMutex'.
    using CollectionsMap = stdx::unordered_map<NamespaceString, CSSAndLock>;
    CollectionsMap _collections;
};

const ServiceContext::Decoration<boost::optional<CollectionShardingStateMap>>
    CollectionShardingStateMap::get =
        ServiceContext::declareDecoration<boost::optional<CollectionShardingStateMap>>();

}  // namespace

CollectionShardingState::ScopedCollectionShardingState::ScopedCollectionShardingState(
    std::shared_lock<std::shared_mutex> lock, CollectionShardingState* css)
    : _lock(std::move(lock)), _css(css) {}

CollectionShardingState::ScopedCollectionShardingState::ScopedCollectionShardingState(
    CollectionShardingState* css)
    : _lock(boost::none), _css(css) {}

CollectionShardingState::ScopedCollectionShardingState::ScopedCollectionShardingState(
    ScopedCollectionShardingState&& other)
    : _lock(std::move(other._lock)), _css(other._css) {
    other._css = nullptr;
}

CollectionShardingState::ScopedCollectionShardingState::~ScopedCollectionShardingState() = default;

CollectionShardingState::ScopedCollectionShardingState
CollectionShardingState::ScopedCollectionShardingState::acquire(OperationContext* opCtx,
                                                                const NamespaceString& nss) {
    CollectionShardingStateMap::CSSAndLock* cssAndLock =
        CollectionShardingStateMap::get(opCtx->getServiceContext())->getOrCreate(nss);

    if (serverGlobalParams.clusterRole.has(ClusterRole::ShardServer)) {
        // First lock the shared_mutex associated to this nss to guarantee stability of the
        // CollectionShardingState* . After that, it is safe to get and store the
        // CollectionShardingState*, as long as the mutex is kept locked.
        return ScopedCollectionShardingState(std::shared_lock(cssAndLock->cssMutex),
                                             cssAndLock->css.get());
    } else {
        // No need to lock the CSSLock on non-shardsvrs. For performance, skip doing it.
        return ScopedCollectionShardingState(cssAndLock->css.get());
    }
}

CollectionShardingState::ScopedCollectionShardingState
CollectionShardingState::assertCollectionLockedAndAcquire(OperationContext* opCtx,
                                                          const NamespaceString& nss) {
    dassert(opCtx->isLockFreeReadsOp() ||
            shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(nss, MODE_IS) ||
            (nss.isOplog() && shard_role_details::getLocker(opCtx)->isReadLocked()));

    return acquire(opCtx, nss);
}

CollectionShardingState::ScopedCollectionShardingState CollectionShardingState::acquire(
    OperationContext* opCtx, const NamespaceString& nss) {
    return ScopedCollectionShardingState::acquire(opCtx, nss);
}

void CollectionShardingState::appendInfoForShardingStateCommand(OperationContext* opCtx,
                                                                BSONObjBuilder* builder) {
    auto& collectionsMap = CollectionShardingStateMap::get(opCtx->getServiceContext());
    collectionsMap->appendInfoForShardingStateCommand(builder);
}

std::vector<NamespaceString> CollectionShardingState::getCollectionNames(OperationContext* opCtx) {
    auto& collectionsMap = CollectionShardingStateMap::get(opCtx->getServiceContext());
    return collectionsMap->getCollectionNames();
}

const StaleShardCollectionMetadataHandler& CollectionShardingState::getStaleShardExceptionHandler(
    OperationContext* opCtx) {
    auto& collectionsMap = CollectionShardingStateMap::get(opCtx->getServiceContext());
    return collectionsMap->getStaleShardExceptionHandler();
}

void CollectionShardingStateFactory::set(ServiceContext* service,
                                         std::unique_ptr<CollectionShardingStateFactory> factory) {
    auto& collectionsMap = CollectionShardingStateMap::get(service);
    invariant(!collectionsMap);
    invariant(factory);
    collectionsMap.emplace(std::move(factory));
}

void CollectionShardingStateFactory::clear(ServiceContext* service) {
    if (auto& collectionsMap = CollectionShardingStateMap::get(service))
        collectionsMap.reset();
}

CollectionShardingState::ScopedExclusiveCollectionShardingState::
    ScopedExclusiveCollectionShardingState(CollectionShardingState* css)
    : _lock(boost::none), _css(css) {}

CollectionShardingState::ScopedExclusiveCollectionShardingState
CollectionShardingState::ScopedExclusiveCollectionShardingState::acquire(
    OperationContext* opCtx, const NamespaceString& nss) {
    CollectionShardingStateMap::CSSAndLock* cssAndLock =
        CollectionShardingStateMap::get(opCtx->getServiceContext())->getOrCreate(nss);

    if (serverGlobalParams.clusterRole.has(ClusterRole::ShardServer)) {
        // First lock the shared_mutex associated to this nss to guarantee stability of the
        // CollectionShardingState* . After that, it is safe to get and store the
        // CollectionShardingState*, as long as the mutex is kept locked.
        return ScopedExclusiveCollectionShardingState(std::unique_lock(cssAndLock->cssMutex),
                                                      cssAndLock->css.get());
    } else {
        // No need to lock the CSSLock on non-shardsvrs. For performance, skip doing it.
        return ScopedExclusiveCollectionShardingState(cssAndLock->css.get());
    }
}

}  // namespace mongo
