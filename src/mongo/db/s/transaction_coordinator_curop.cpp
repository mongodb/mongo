// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/transaction_coordinator_curop.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/transaction_coordinator_service.h"

#include <string>

namespace mongo {

void reportCurrentOpsForTransactionCoordinators(OperationContext* opCtx,
                                                bool includeIdle,
                                                std::vector<BSONObj>* ops) {
    TransactionCoordinatorService::get(opCtx)->reportCoordinators(opCtx, includeIdle, ops);
}

}  // namespace mongo
