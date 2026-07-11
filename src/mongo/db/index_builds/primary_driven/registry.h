// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/database_name.h"
#include "mongo/db/index_builds/index_builds_common.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <mutex>
#include <string>
#include <vector>

#include <boost/optional.hpp>

[[MONGO_MOD_PUBLIC]];
namespace mongo::index_builds::primary_driven {

class Registry {
public:
    struct Entry {
        DatabaseName dbName;
        UUID collectionUUID;
        std::vector<IndexBuildInfo> indexes;
        boost::optional<std::string> indexBuildIdent;
    };

    void add(UUID buildUUID,
             DatabaseName dbName,
             UUID collectionUUID,
             std::vector<IndexBuildInfo> indexes,
             boost::optional<std::string> indexBuildIdent);

    void remove(UUID buildUUID);

    void clear();

    std::vector<std::pair<UUID, Entry>> all() const;

private:
    mutable std::mutex _mutex;
    stdx::unordered_map<UUID, Entry, UUID::Hash> _entries;  // Keyed by index build UUID.
};

}  // namespace mongo::index_builds::primary_driven
