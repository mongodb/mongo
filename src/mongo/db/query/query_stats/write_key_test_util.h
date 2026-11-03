// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/query/query_stats/write_key.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>

namespace mongo::query_stats {

/**
 * Builds the query stats key of a standalone update/delete command that must contain exactly one
 * op, or of a standalone insert command.
 */
std::unique_ptr<UpdateKey> makeUpdateKey(
    OperationContext* opCtx,
    std::string_view updateCmd,
    query_shape::CollectionType collectionType = query_shape::CollectionType::kCollection);
std::unique_ptr<DeleteKey> makeDeleteKey(
    OperationContext* opCtx,
    std::string_view deleteCmd,
    query_shape::CollectionType collectionType = query_shape::CollectionType::kCollection);
std::unique_ptr<InsertKey> makeInsertKey(
    OperationContext* opCtx,
    std::string_view insertCmd,
    query_shape::CollectionType collectionType = query_shape::CollectionType::kCollection);

}  // namespace mongo::query_stats
