/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/util/observable_mutex_registry.h"

#include "mongo/util/scoped_unlock.h"
#include "mongo/util/static_immortal.h"

namespace mongo {

ObservableMutexRegistry& ObservableMutexRegistry::get() {
    static StaticImmortal<ObservableMutexRegistry> obj;
    return *obj;
}

void ObservableMutexRegistry::iterate(CollectionCallback cb) {
    std::vector<StringData> tags;
    stdx::unique_lock lk(_mutex);
    tags.reserve(_mutexEntries.size());
    for (const auto& [tag, _] : _mutexEntries) {
        tags.emplace_back(tag);
    }

    for (const auto& tag : tags) {
        auto& entries = _mutexEntries[tag];
        std::list<MutexEntry> entriesSnapshot(entries);

        // Garbage collect invalid entries.
        entries.remove_if([&](auto& entry) { return MONGO_unlikely(!entry.token->isValid()); });

        ScopedUnlock scopedUnlock(lk);
        for (const auto& entry : entriesSnapshot) {
            cb(tag, entry.registrationTime, *entry.token);
        }
    }
}

size_t ObservableMutexRegistry::getNumRegistered_forTest() const {
    stdx::lock_guard lk(_mutex);
    size_t size = 0;
    for (const auto& [_, entries] : _mutexEntries) {
        size += entries.size();
    }
    return size;
}
}  // namespace mongo
