// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/lock_manager/resource_catalog.h"

#include "mongo/db/database_name_util.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/static_immortal.h"

#include <mutex>
#include <new>
#include <string_view>
#include <utility>

#include <absl/container/flat_hash_set.h>
#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/type_traits/decay.hpp>

namespace mongo {

ResourceCatalog& ResourceCatalog::get() {
    static StaticImmortal<ResourceCatalog> resourceCatalog;
    return resourceCatalog.value();
}

void ResourceCatalog::add(ResourceId id, const NamespaceString& ns) {
    invariant(id.getType() == RESOURCE_COLLECTION);
    _add(id, NamespaceStringUtil::serializeForCatalog(ns));
}

void ResourceCatalog::add(ResourceId id, const DatabaseName& dbName) {
    invariant(id.getType() == RESOURCE_DATABASE);
    _add(id, DatabaseNameUtil::serialize(dbName, SerializationContext::stateCatalog()));
}

void ResourceCatalog::add(ResourceId id, DDLResourceName resourceName) {
    invariant(id.getType() == RESOURCE_DDL_DATABASE || id.getType() == RESOURCE_DDL_COLLECTION);
    _add(id, std::string{std::string_view(resourceName)});
}

void ResourceCatalog::_add(ResourceId id, std::string name) {
    std::lock_guard<std::mutex> lk{_mutex};
    _resources[id].insert(std::move(name));
}

void ResourceCatalog::remove(ResourceId id, const NamespaceString& ns) {
    invariant(id.getType() == RESOURCE_COLLECTION);
    _remove(id, NamespaceStringUtil::serializeForCatalog(ns));
}

void ResourceCatalog::remove(ResourceId id, const DatabaseName& dbName) {
    invariant(id.getType() == RESOURCE_DATABASE);
    _remove(id, DatabaseNameUtil::serialize(dbName, SerializationContext::stateCatalog()));
}

void ResourceCatalog::remove(ResourceId id, DDLResourceName resourceName) {
    invariant(id.getType() == RESOURCE_DDL_DATABASE || id.getType() == RESOURCE_DDL_COLLECTION);
    _remove(id, std::string{std::string_view(resourceName)});
}

ResourceId ResourceCatalog::newResourceIdForMutex(std::string resourceLabel) {
    std::lock_guard<std::mutex> lk(_mutexResourceIdLabelsMutex);
    _mutexResourceIdLabels.emplace_back(std::move(resourceLabel));

    return ResourceId(
        ResourceId::fullHash(ResourceType::RESOURCE_MUTEX, _mutexResourceIdLabels.size() - 1));
}

void ResourceCatalog::_remove(ResourceId id, const std::string& name) {
    std::lock_guard<std::mutex> lk{_mutex};

    auto it = _resources.find(id);
    if (it == _resources.end()) {
        return;
    }

    it->second.erase(name);

    if (it->second.empty()) {
        _resources.erase(it);
    }
}

void ResourceCatalog::clear() {
    std::lock_guard<std::mutex> lk{_mutex};
    _resources.clear();
}

boost::optional<std::string> ResourceCatalog::name(ResourceId id) const {
    const auto& resType = id.getType();
    switch (resType) {
        case RESOURCE_DATABASE:
        case RESOURCE_COLLECTION:
        case RESOURCE_DDL_DATABASE:
        case RESOURCE_DDL_COLLECTION: {
            std::lock_guard<std::mutex> lk{_mutex};

            auto it = _resources.find(id);
            return it == _resources.end() || it->second.size() > 1
                ? boost::none
                : boost::make_optional(*it->second.begin());
        }
        case RESOURCE_MUTEX: {
            std::lock_guard<std::mutex> lk{_mutexResourceIdLabelsMutex};
            return _mutexResourceIdLabels.at(id.getHashId());
        }
        default:
            return boost::none;
    }
    MONGO_UNREACHABLE_TASSERT(10083514);
}

}  // namespace mongo
