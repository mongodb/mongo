// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/database_holder.h"

#include "mongo/util/decorable.h"

#include <utility>

namespace mongo {

namespace {

const auto getDatabaseHolderFromServiceContext =
    ServiceContext::declareDecoration<std::unique_ptr<DatabaseHolder>>();

}  // namespace

DatabaseHolder* DatabaseHolder::get(ServiceContext* service) {
    return getDatabaseHolderFromServiceContext(service).get();
}

DatabaseHolder* DatabaseHolder::get(ServiceContext& service) {
    return getDatabaseHolderFromServiceContext(service).get();
}

DatabaseHolder* DatabaseHolder::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

void DatabaseHolder::set(ServiceContext* service, std::unique_ptr<DatabaseHolder> databaseHolder) {
    auto& holder = getDatabaseHolderFromServiceContext(service);
    holder = std::move(databaseHolder);
}

}  // namespace mongo
