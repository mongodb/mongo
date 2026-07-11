// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/transaction_coordinator_worker_curop_repository.h"

#include "mongo/base/shim.h"

#include <string>

namespace mongo {

std::shared_ptr<TransactionCoordinatorWorkerCurOpRepository>
getTransactionCoordinatorWorkerCurOpRepository() {
    static auto w = MONGO_WEAK_FUNCTION_DEFINITION(getTransactionCoordinatorWorkerCurOpRepository);
    return w();
}

}  // namespace mongo
