// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/index_builds/primary_driven/registry.h"

namespace mongo::index_builds::primary_driven {

void Registry::add(UUID buildUUID,
                   DatabaseName dbName,
                   UUID collectionUUID,
                   std::vector<IndexBuildInfo> indexes,
                   boost::optional<std::string> indexBuildIdent) {
    {
        std::lock_guard lock{_mutex};
        _entries.try_emplace(buildUUID,
                             std::move(dbName),
                             collectionUUID,
                             std::move(indexes),
                             std::move(indexBuildIdent));
    }
    _onChange();
}

void Registry::remove(UUID buildUUID) {
    {
        std::lock_guard lock{_mutex};
        _entries.erase(buildUUID);
    }
    _onChange();
}

void Registry::clear() {
    {
        std::lock_guard lock{_mutex};
        _entries.clear();
    }
    _onChange();
}

void Registry::setOnChangeHandler(std::function<void()> handler) {
    std::lock_guard lock{_mutex};
    _onChangeHandler = std::move(handler);
}

void Registry::_onChange() const noexcept {
    auto handler = [&] {
        std::lock_guard lock{_mutex};
        return _onChangeHandler;
    }();
    if (handler) {
        handler();
    }
}

bool Registry::contains(const UUID& buildUUID) const {
    std::lock_guard lock{_mutex};
    return _entries.contains(buildUUID);
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
