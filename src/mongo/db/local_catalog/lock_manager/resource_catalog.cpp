/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/local_catalog/lock_manager/resource_catalog.h"

#include "mongo/util/assert_util.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/static_immortal.h"

#include <mutex>
#include <new>
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
    _add(id, std::string{StringData(resourceName)});
}

void ResourceCatalog::_add(ResourceId id, std::string name) {
    stdx::lock_guard<stdx::mutex> lk{_mutex};
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
    _remove(id, std::string{StringData(resourceName)});
}

ResourceId ResourceCatalog::newResourceIdForMutex(std::string resourceLabel) {
    stdx::lock_guard<stdx::mutex> lk(_mutexResourceIdLabelsMutex);
    _mutexResourceIdLabels.emplace_back(std::move(resourceLabel));

    return ResourceId(
        ResourceId::fullHash(ResourceType::RESOURCE_MUTEX, _mutexResourceIdLabels.size() - 1));
}

void ResourceCatalog::_remove(ResourceId id, const std::string& name) {
    stdx::lock_guard<stdx::mutex> lk{_mutex};

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
    stdx::lock_guard<stdx::mutex> lk{_mutex};
    _resources.clear();
}

boost::optional<std::string> ResourceCatalog::name(ResourceId id) const {
    const auto& resType = id.getType();
    switch (resType) {
        case RESOURCE_DATABASE:
        case RESOURCE_COLLECTION:
        case RESOURCE_DDL_DATABASE:
        case RESOURCE_DDL_COLLECTION: {
            stdx::lock_guard<stdx::mutex> lk{_mutex};

            auto it = _resources.find(id);
            return it == _resources.end() || it->second.size() > 1
                ? boost::none
                : boost::make_optional(*it->second.begin());
        }
        case RESOURCE_MUTEX: {
            stdx::lock_guard<stdx::mutex> lk{_mutexResourceIdLabelsMutex};
            return _mutexResourceIdLabels.at(id.getHashId());
        }
        default:
            return boost::none;
    }
    MONGO_UNREACHABLE_TASSERT(10083514);
}

}  // namespace mongo
