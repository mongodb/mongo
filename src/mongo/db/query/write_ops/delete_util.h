// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/query/write_ops/delete_request_gen.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/util/modules.h"

namespace [[MONGO_MOD_PUBLIC]] mongo {
namespace delete_util {

/**
 * Parses the remove branch of a findAndModify command request into a DeleteRequest.
 */
void makeDeleteRequest(OperationContext* opCtx,
                       const write_ops::FindAndModifyCommandRequest& request,
                       bool explain,
                       DeleteRequest* requestOut);

}  // namespace delete_util
}  // namespace mongo
