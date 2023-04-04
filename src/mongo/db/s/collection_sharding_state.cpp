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

#include "mongo/db/s/collection_sharding_state.h"

#include "mongo/logv2/log.h"
#include "mongo/s/sharding_feature_flags_gen.h"
#include "mongo/util/string_map.h"

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
        CSSAndLock(std::unique_ptr<CollectionShardingState> css)
            : cssMutex("CSSMutex::" + css->nss().toString()), css(std::move(css)) {}

        const Lock::ResourceMutex cssMutex;
        std::unique_ptr<CollectionShardingState> css;
    };

    /**
     * Joins the factory, waiting for any outstanding tasks using the factory to be finished. Must
     * be called before destruction.
     */
    void join() {
        _factory->join();
    }

    CSSAndLock* getOrCreate(const NamespaceString& nss) noexcept {
        stdx::lock_guard<Latch> lg(_mutex);

        auto it = _collections.find(nss.ns());
        if (it == _collections.end()) {
            auto inserted = _collections.try_emplace(
                nss.ns(), std::make_unique<CSSAndLock>(_factory->make(nss)));
            invariant(inserted.second);
            it = std::move(inserted.first);
        }

        return it->second.get();
    }

    void appendInfoForShardingStateCommand(BSONObjBuilder* builder) {
        BSONObjBuilder versionB(builder->subobjStart("versions"));

        {
            stdx::lock_guard<Latch> lg(_mutex);
            for (const auto& coll : _collections) {
                coll.second->css->appendShardVersion(builder);
            }
        }

        versionB.done();
    }

    void appendInfoForServerStatus(BSONObjBuilder* builder) {
        // (Ignore FCV check): This feature doesn't have any upgrade/downgrade concerns. The feature
        // flag is used to turn on new range deleter on startup.
        if (!mongo::feature_flags::gRangeDeleterService.isEnabledAndIgnoreFCVUnsafe()) {
            auto totalNumberOfRangesScheduledForDeletion = ([this] {
                stdx::lock_guard lg(_mutex);
                return std::accumulate(
                    _collections.begin(),
                    _collections.end(),
                    0LL,
                    [](long long total, const auto& coll) {
                        return total + coll.second->css->numberOfRangesScheduledForDeletion();
                    });
            })();

            builder->appendNumber("rangeDeleterTasks", totalNumberOfRangesScheduledForDeletion);
        }
    }

    std::vector<NamespaceString> getCollectionNames() {
        stdx::lock_guard lg(_mutex);
        std::vector<NamespaceString> result;
        result.reserve(_collections.size());
        for (const auto& [ns, _] : _collections) {
            result.emplace_back(ns);
        }
        return result;
    }

private:
    std::unique_ptr<CollectionShardingStateFactory> _factory;

    Mutex _mutex = MONGO_MAKE_LATCH("CollectionShardingStateMap::_mutex");

    // Entries of the _collections map must never be deleted or replaced. This is to guarantee that
    // a 'nss' is always associated to the same 'ResourceMutex'.
    using CollectionsMap = StringMap<std::unique_ptr<CSSAndLock>>;
    CollectionsMap _collections;
};

const ServiceContext::Decoration<boost::optional<CollectionShardingStateMap>>
    CollectionShardingStateMap::get =
        ServiceContext::declareDecoration<boost::optional<CollectionShardingStateMap>>();

}  // namespace

CollectionShardingState::ScopedCollectionShardingState::ScopedCollectionShardingState(
    Lock::ResourceLock lock, CollectionShardingState* css)
    : _lock(std::move(lock)), _css(css) {}

CollectionShardingState::ScopedCollectionShardingState::ScopedCollectionShardingState(
    ScopedCollectionShardingState&& other)
    : _lock(std::move(other._lock)), _css(other._css) {
    other._css = nullptr;
}

CollectionShardingState::ScopedCollectionShardingState::~ScopedCollectionShardingState() = default;

CollectionShardingState::ScopedCollectionShardingState
CollectionShardingState::ScopedCollectionShardingState::acquireScopedCollectionShardingState(
    OperationContext* opCtx, const NamespaceString& nss, LockMode mode) {
    CollectionShardingStateMap::CSSAndLock* cssAndLock =
        CollectionShardingStateMap::get(opCtx->getServiceContext())->getOrCreate(nss);

    // First lock the RESOURCE_MUTEX associated to this nss to guarantee stability of the
    // CollectionShardingState* . After that, it is safe to get and store the
    // CollectionShadingState*, as long as the RESOURCE_MUTEX is kept locked.
    Lock::ResourceLock lock(opCtx->lockState(), cssAndLock->cssMutex.getRid(), mode);
    return ScopedCollectionShardingState(std::move(lock), cssAndLock->css.get());
}

CollectionShardingState::ScopedCollectionShardingState
CollectionShardingState::assertCollectionLockedAndAcquire(OperationContext* opCtx,
                                                          const NamespaceString& nss) {
    dassert(opCtx->lockState()->isCollectionLockedForMode(nss, MODE_IS));

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

void CollectionShardingState::appendInfoForServerStatus(OperationContext* opCtx,
                                                        BSONObjBuilder* builder) {
    auto& collectionsMap = CollectionShardingStateMap::get(opCtx->getServiceContext());
    collectionsMap->appendInfoForServerStatus(builder);
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
    auto& collectionsMap = CollectionShardingStateMap::get(service);
    if (collectionsMap) {
        collectionsMap->join();
        collectionsMap.reset();
    }
}

}  // namespace mongo
