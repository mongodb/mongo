// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/index_catalog_entry.h"

#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"

namespace mongo {

std::shared_ptr<const IndexCatalogEntry> IndexCatalogEntryContainer::release(
    const IndexDescriptor* desc) {
    for (auto i = _entries.begin(); i != _entries.end(); ++i) {
        if ((*i)->descriptor() == desc) {
            auto e = std::move(*i);
            _entries.erase(i);
            return e;
        }
    }
    return nullptr;
}

}  // namespace mongo
