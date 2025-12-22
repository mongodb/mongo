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

#pragma once

#include "mongo/db/commands/query_cmd/bulk_write_gen.h"
#include "mongo/db/operation_context.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/s/write_ops/bulk_write_reply_info.h"
#include "mongo/s/write_ops/write_command_ref.h"
#include "mongo/util/modules.h"

namespace MONGO_MOD_PUB mongo {
namespace unified_write_executor {

struct FindAndModifyCommandResponse {
    StatusWith<write_ops::FindAndModifyCommandReply> swReply;
    boost::optional<WriteConcernErrorDetail> wce;
};
using WriteCommandResponse = std::variant<BatchedCommandResponse,
                                          bulk_write_exec::BulkWriteReplyInfo,
                                          FindAndModifyCommandResponse>;

/**
 * This function will execute the specified write command and return a response.
 */
WriteCommandResponse executeWriteCommand(OperationContext* opCtx,
                                         WriteCommandRef cmdRef,
                                         BSONObj originalCommand = BSONObj(),
                                         boost::optional<OID> targetEpoch = boost::none);

/**
 * Helper function for executing insert/update/delete commands.
 */
BatchedCommandResponse write(OperationContext* opCtx,
                             const BatchedCommandRequest& request,
                             boost::optional<OID> targetEpoch = boost::none);

/**
 * Helper function for executing bulk commands.
 */
bulk_write_exec::BulkWriteReplyInfo bulkWrite(OperationContext* opCtx,
                                              const BulkWriteCommandRequest& request,
                                              BSONObj originalCommand = BSONObj());

/**
 * Helper function for executing findAndModify commands.
 */
FindAndModifyCommandResponse findAndModify(OperationContext* opCtx,
                                           const write_ops::FindAndModifyCommandRequest& request,
                                           BSONObj originalCommand = BSONObj());

/**
 * Unified write executor query knob check.
 */
bool isEnabled(OperationContext* opCtx);

}  // namespace unified_write_executor
}  // namespace MONGO_MOD_PUB mongo
