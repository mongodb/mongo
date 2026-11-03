// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/query/query_shape/delete_cmd_shape.h"
#include "mongo/db/query/query_shape/query_shape.h"
#include "mongo/db/query/query_shape/update_cmd_shape.h"
#include "mongo/db/query/write_ops/delete_request_gen.h"
#include "mongo/db/query/write_ops/update_request.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/util/modules.h"

#include <memory>

namespace mongo::query_shape {

/**
 * Builds the per-op request for the op at 'opIdx' of a standalone update/delete command, the way
 * the write path does.
 */
UpdateRequest makeUpdateRequest(const write_ops::UpdateCommandRequest& cmd, size_t opIdx = 0);
DeleteRequest makeDeleteRequest(const write_ops::DeleteCommandRequest& cmd, size_t opIdx = 0);

/**
 * Parses 'request' and builds its shape. The shape does not reference 'request' afterwards.
 */
std::unique_ptr<UpdateCmdShape> makeUpdateShape(OperationContext* opCtx,
                                                const UpdateRequest& request);
std::unique_ptr<DeleteCmdShape> makeDeleteShape(OperationContext* opCtx,
                                                const DeleteRequest& request);

}  // namespace mongo::query_shape
