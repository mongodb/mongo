// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/tenant_id.h"
#include "mongo/util/modules.h"

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * Extract the active TenantId for this OperationContext.
 */
boost::optional<TenantId> getActiveTenant(OperationContext* opCtx);

}  // namespace mongo
