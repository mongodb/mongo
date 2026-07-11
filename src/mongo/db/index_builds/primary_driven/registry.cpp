// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/index_builds/primary_driven/registry.h"

namespace mongo::index_builds::primary_driven {

void Registry::add(UUID buildUUID,
                   DatabaseName dbName,
                   UUID collectionUUID,
                   std::vector<IndexBuildInfo> indexes,
                   boost::optional<std::string> indexBuildIdent) {
    std::lock_guard lock{_mutex};
    _entries.try_emplace(buildUUID,
                         std::move(dbName),
                         collectionUUID,
                         std::move(indexes),
                         std::move(indexBuildIdent));
}

void Registry::remove(UUID buildUUID) {
    std::lock_guard lock{_mutex};
    _entries.erase(buildUUID);
}

void Registry::clear() {
    std::lock_guard lock{_mutex};
    _entries.clear();
}

std::vector<std::pair<UUID, Registry::Entry>> Registry::all() const {
    std::lock_guard lock{_mutex};
    std::vector<std::pair<UUID, Entry>> entries;
    entries.reserve(_entries.size());
    for (auto&& entry : _entries) {
        entries.push_back(entry);
    }
    return entries;
}

}  // namespace mongo::index_builds::primary_driven
