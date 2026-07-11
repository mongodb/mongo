// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/process_interface/mongos_process_interface.h"
#include "mongo/util/modules.h"

#include <vector>

namespace mongo {

[[MONGO_MOD_PUBLIC]] void reportCurrentOpsForTransactionCoordinators(OperationContext* opCtx,
                                                                     bool includeIdle,
                                                                     std::vector<BSONObj>* ops);

}  // namespace mongo
