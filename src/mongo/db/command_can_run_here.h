// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"

#include <string>

namespace [[MONGO_MOD_PUBLIC]] mongo {

bool commandCanRunHere(OperationContext* opCtx,
                       const DatabaseName& dbName,
                       const Command* command,
                       bool inMultiDocumentTransaction);

}  // namespace mongo
