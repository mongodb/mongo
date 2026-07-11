// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"

#include "mongo/base/shim.h"

namespace mongo {

std::shared_ptr<MongoProcessInterface> MongoProcessInterface::create(OperationContext* opCtx) {
    static auto w = MONGO_WEAK_FUNCTION_DEFINITION(MongoProcessInterface::create);
    return w(opCtx);
}

}  // namespace mongo
