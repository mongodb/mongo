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
#include "mongo/s/stale_exception.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/log.h"
#include "mongo/util/string_map.h"

namespace mongo {

namespace {

class CollectionShardingStateMap {
    CollectionShardingStateMap(const CollectionShardingStateMap&) = delete;
    CollectionShardingStateMap& operator=(const CollectionShardingStateMap&) = delete;

public:
    static const ServiceContext::Decoration<boost::optional<CollectionShardingStateMap>> get;

    CollectionShardingStateMap(std::unique_ptr<CollectionShardingStateFactory> factory)
        : _factory(std::move(factory)) {}

    CollectionShardingState& getOrCreate(const NamespaceString& nss) {
        stdx::lock_guard<Latch> lg(_mutex);

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
            stdx::lock_guard<Latch> lg(_mutex);

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

    Mutex _mutex = MONGO_MAKE_LATCH("CollectionShardingStateMap::_mutex");
    CollectionsMap _collections;
};

const ServiceContext::Decoration<boost::optional<CollectionShardingStateMap>>
    CollectionShardingStateMap::get =
        ServiceContext::declareDecoration<boost::optional<CollectionShardingStateMap>>();

}  // namespace

CollectionShardingState* CollectionShardingState::get(OperationContext* opCtx,
                                                      const NamespaceString& nss) {
    // Collection lock must be held to have a reference to the collection's sharding state
    dassert(opCtx->lockState()->isCollectionLockedForMode(nss, MODE_IS));

    auto& collectionsMap = CollectionShardingStateMap::get(opCtx->getServiceContext());
    return &collectionsMap->getOrCreate(nss);
}

CollectionShardingState* CollectionShardingState::get_UNSAFE(ServiceContext* svcCtx,
                                                             const NamespaceString& nss) {
    auto& collectionsMap = CollectionShardingStateMap::get(svcCtx);
    return &collectionsMap->getOrCreate(nss);
}

void CollectionShardingState::report(OperationContext* opCtx, BSONObjBuilder* builder) {
    auto& collectionsMap = CollectionShardingStateMap::get(opCtx->getServiceContext());
    collectionsMap->report(opCtx, builder);
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
