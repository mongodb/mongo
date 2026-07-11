// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/namespace_string.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_state.h"
#include "mongo/db/shard_role/shard_catalog/stale_shard_exception_handler.h"
#include "mongo/util/modules.h"

namespace mongo {

class [[MONGO_MOD_PUBLIC]] CollectionShardingStateFactoryShard final
    : public CollectionShardingStateFactory {
public:
    CollectionShardingStateFactoryShard(ServiceContext* serviceContext);

    std::unique_ptr<CollectionShardingState> make(const NamespaceString& nss) override;

    const StaleShardCollectionMetadataHandler& getStaleShardExceptionHandler() const override;

private:
    // The service context which owns this factory
    ServiceContext* const _serviceContext;

    StaleShardCollectionMetadataHandlerImpl _staleExceptionHandler;
};

}  // namespace mongo
