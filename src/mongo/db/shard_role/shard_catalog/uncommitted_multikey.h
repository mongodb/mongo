// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/auth/validated_tenancy_scope.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/shard_role/shard_catalog/durable_catalog_entry_metadata.h"
#include "mongo/util/modules.h"

#include <map>
#include <memory>

namespace mongo {
class Collection;
class UncommittedMultikey {
public:
    /**
     * Wrapper class for the resources used by UncommittedMultikey
     * Store uncommitted multikey updates as a decoration on the OperationContext. We can use the
     * raw Collection pointer as a key as there cannot be any concurrent MODE_X writer that clones
     * the Collection into a new instance.
     */
    using MultikeyMap = std::map<const Collection*, durable_catalog::CatalogEntryMetaData>;

    static UncommittedMultikey& get(OperationContext* opCtx);

    std::shared_ptr<MultikeyMap>& resources() {
        return _resourcesPtr;
    }

private:
    std::shared_ptr<MultikeyMap> _resourcesPtr;
};
}  // namespace mongo
