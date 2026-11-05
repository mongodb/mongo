// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/tenant_id.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/modules.h"

#include <boost/optional/optional.hpp>

namespace mongo {
namespace repl {

/**
 * True when oplogApplicationEnforcesSteadyStateConstraints has not been explicitly set by the user
 * via --setParameter or setParameter command.
 */
[[MONGO_MOD_PUBLIC]] extern Atomic<bool>
    oplogApplicationEnforcesSteadyStateConstraintsInitializedUsingDefault;

[[MONGO_MOD_PARENT_PRIVATE]] Status onOplogApplicationEnforcesSteadyStateConstraintsUpdate(
    bool newValue);

[[MONGO_MOD_PARENT_PRIVATE]] Status validateDisableContinuousInternodeValidationPerCollection(
    const bool& newValue, const boost::optional<TenantId>& tenantId);

}  // namespace repl
}  // namespace mongo
