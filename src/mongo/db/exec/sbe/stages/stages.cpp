// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/stages/stages.h"

#include "mongo/db/memory_tracking/query_memory_load_shedding.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace sbe {

void checkForQueryMemoryLoadShedding(OperationContext* opCtx) {
    uassertStatusOK(queryMemoryCheckLoadShedding(opCtx));
}

}  // namespace sbe
}  // namespace mongo
