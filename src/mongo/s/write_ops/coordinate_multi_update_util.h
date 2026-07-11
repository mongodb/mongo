// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/s/transaction_router.h"
#include "mongo/s/write_ops/batch_write_op.h"
#include "mongo/s/write_ops/bulk_write_exec.h"
#include "mongo/util/modules.h"

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
