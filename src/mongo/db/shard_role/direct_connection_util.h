// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"

namespace mongo {
namespace direct_connection_util {

/*
 * Checks if the operation is coming from a direct connection and whether the user has the correct
 * permissions to execute a DDL operation directly against the shard.
 *
 * Throws ErrorCodes::Unauthorized if the command is a direct connection and the user does not have
 * sufficient permissions.
 */
[[MONGO_MOD_PUBLIC]] void checkDirectShardDDLAllowed(OperationContext* opCtx,
                                                     const NamespaceString& nss);

/*
 * Checks if the operation is coming from a direct connection and whether the user has the correct
 * permissions to execute any operation directly against the shard (CRUD or DDL).
 *
 * Throws ErrorCodes::Unauthorized if the command is a direct connection and the user does not have
 * sufficient permissions.
 */
[[MONGO_MOD_PUBLIC]] void checkDirectShardOperationAllowed(OperationContext* opCtx,
                                                           const NamespaceString& nss);

}  // namespace direct_connection_util
}  // namespace mongo
