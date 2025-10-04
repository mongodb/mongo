/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/s/transaction_router.h"
#include "mongo/s/write_ops/batch_write_op.h"
#include "mongo/s/write_ops/bulk_write_exec.h"

namespace mongo {
namespace coordinate_multi_update_util {

class PauseMigrationsDuringMultiUpdatesEnablement {
public:
    bool isEnabled();

private:
    boost::optional<bool> _enabled;
};

// Prevents arguments from the user's original request from being incorrectly included in the
// multi-update's inner command to be executed by the shard(s). As an example, this ensures a
// { w: 0 } write concern specified on the original request will not interfere with the router's
// ability to receive responses from the shard(s).
//
// This currently just drops all of the generic arguments except for rawData. The rawData argument
// needs to be passed along so that the inner command respects the user-provided option.
void filterRequestGenericArguments(GenericArguments& args);

BulkWriteCommandReply parseBulkResponse(const BSONObj& response);

int getWriteOpIndex(const TargetedBatchMap& childBatches);

BatchedCommandResponse executeCoordinateMultiUpdate(OperationContext* opCtx,
                                                    BatchWriteOp& batchOp,
                                                    const TargetedBatchMap& childBatches,
                                                    const BatchedCommandRequest& clientRequest);

BulkWriteCommandReply executeCoordinateMultiUpdate(OperationContext* opCtx,
                                                   TargetedBatchMap& childBatches,
                                                   bulk_write_exec::BulkWriteOp& bulkWriteOp);

BSONObj executeCoordinateMultiUpdate(OperationContext* opCtx,
                                     const NamespaceString& nss,
                                     BSONObj writeCommand);


bool shouldCoordinateMultiWrite(
    OperationContext* opCtx, mongo::PauseMigrationsDuringMultiUpdatesEnablement& pauseMigrations);

}  // namespace coordinate_multi_update_util
}  // namespace mongo
