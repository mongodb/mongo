// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/modules.h"
#include "mongo/util/string_map.h"

#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <boost/optional/optional.hpp>
#include <boost/serialization/strong_typedef.hpp>

[[MONGO_MOD_PUBLIC]];

namespace mongo {

BOOST_STRONG_TYPEDEF(std::string_view, DDLResourceName);

class ResourceCatalog {
public:
    static ResourceCatalog& get();

    void add(ResourceId id, const NamespaceString& ns);
    void add(ResourceId id, const DatabaseName& dbName);
    void add(ResourceId id, DDLResourceName resourceName);

    void remove(ResourceId id, const NamespaceString& ns);
    void remove(ResourceId id, const DatabaseName& dbName);
    void remove(ResourceId id, DDLResourceName resourceName);

    ResourceId newResourceIdForMutex(std::string resourceLabel);

    void clear();

    /**
     * Returns the name of a resource by its id. If the id is not found or it maps to multiple
     * resources, returns boost::none.
     */
    boost::optional<std::string> name(ResourceId id) const;

private:
    void _add(ResourceId id, std::string name);

    void _remove(ResourceId id, const std::string& name);

    mutable std::mutex _mutex;
    stdx::unordered_map<ResourceId, StringSet> _resources;

    mutable std::mutex _mutexResourceIdLabelsMutex;
    std::vector<std::string> _mutexResourceIdLabels;
};

}  // namespace mongo
