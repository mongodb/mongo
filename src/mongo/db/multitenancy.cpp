// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/multitenancy.h"

#include "mongo/db/auth/validated_tenancy_scope.h"
#include "mongo/db/tenant_id.h"

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

boost::optional<TenantId> getActiveTenant(OperationContext* opCtx) {
    if (auto token = auth::ValidatedTenancyScope::get(opCtx)) {
        return token->tenantId();
    }

    return boost::none;
}

}  // namespace mongo
