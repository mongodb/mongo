// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace [[MONGO_MOD_PUBLIC]] mongo {

constexpr inline std::string_view kIsInternalOperationFieldName{"isInternalOperation"};

/**
 * Marks operations doing work owned by a server subsystem (e.g. resharding, chunk migration), so
 * user-facing metrics such as $queryStats can tell them apart from work done for a client command.
 */
bool isInternalOperation(const OperationContext*);
void markInternalOperation(OperationContext*);

}  // namespace mongo
