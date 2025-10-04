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

#include "mongo/db/local_catalog/shard_role_catalog/collection_sharding_state.h"

#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/server_options.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/platform/rwmutex.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"

#include <mutex>
#include <shared_mutex>
#include <utility>

#include <absl/container/flat_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

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

    struct CSSAndLock {
        CSSAndLock(const NamespaceString& nss, std::unique_ptr<CollectionShardingState> css)
            : css(std::move(css)) {}

        std::shared_mutex cssMutex;  // NOLINT
        std::unique_ptr<CollectionShardingState> css;
    };

    CSSAndLock* getOrCreate(const NamespaceString& nss) {
        std::shared_lock lk(_mutex);  // NOLINT
        if (auto it = _collections.find(nss); MONGO_likely(it != _collections.end())) {
            return it->second.get();
        }
        lk.unlock();
        stdx::lock_guard writeLock(_mutex);
        auto [it, _] =
            _collections.emplace(nss, std::make_unique<CSSAndLock>(nss, _factory->make(nss)));
        return it->second.get();
    }

    void appendInfoForShardingStateCommand(BSONObjBuilder* builder) const {
        std::vector<CSSAndLock*> cssAndLocks;
        {
            std::shared_lock lk(_mutex);  // NOLINT
            cssAndLocks.reserve(_collections.size());
            for (const auto& [_, cssAndLock] : _collections) {
                cssAndLocks.emplace_back(cssAndLock.get());
            }
        }

        BSONObjBuilder versionB(builder->subobjStart("versions"));
        for (auto cssAndLock : cssAndLocks) {
            cssAndLock->css->appendShardVersion(builder);
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

private:
    std::unique_ptr<CollectionShardingStateFactory> _factory;

    // Adding entries to `_collections` is expected to be very infrequent and far apart (collection
    // creation), so the majority of accesses to this map are read-only and benefit from using a
    // shared mutex type for synchronization.
    mutable RWMutex _mutex;

    // Entries of the _collections map must never be deleted or replaced. This is to guarantee that
    // a 'nss' is always associated to the same 'ResourceMutex'.
    using CollectionsMap = absl::flat_hash_map<NamespaceString, std::unique_ptr<CSSAndLock>>;
    CollectionsMap _collections;
};

const ServiceContext::Decoration<boost::optional<CollectionShardingStateMap>>
    CollectionShardingStateMap::get =
        ServiceContext::declareDecoration<boost::optional<CollectionShardingStateMap>>();

}  // namespace

CollectionShardingState::ScopedCollectionShardingState::ScopedCollectionShardingState(
    LockType lock, CollectionShardingState* css)
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
CollectionShardingState::ScopedCollectionShardingState::acquireScopedCollectionShardingState(
    OperationContext* opCtx, const NamespaceString& nss, LockMode mode) {
    // Only IS and X modes are supported.
    invariant(mode == MODE_IS || mode == MODE_X);
    const bool shared = mode == MODE_IS;

    CollectionShardingStateMap::CSSAndLock* cssAndLock =
        CollectionShardingStateMap::get(opCtx->getServiceContext())->getOrCreate(nss);

    if (serverGlobalParams.clusterRole.has(ClusterRole::ShardServer)) {
        // First lock the shared_mutex associated to this nss to guarantee stability of the
        // CollectionShardingState* . After that, it is safe to get and store the
        // CollectionShardingState*, as long as the mutex is kept locked.
        if (shared) {
            return ScopedCollectionShardingState(std::shared_lock(cssAndLock->cssMutex),
                                                 cssAndLock->css.get());
        } else {
            return ScopedCollectionShardingState(std::unique_lock(cssAndLock->cssMutex),
                                                 cssAndLock->css.get());
        }
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
    return ScopedCollectionShardingState::acquireScopedCollectionShardingState(opCtx, nss, MODE_IS);
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

}  // namespace mongo
