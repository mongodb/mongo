// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace [[MONGO_MOD_PUBLIC]] mongo {

constexpr inline std::string_view kIsDirectSystemBucketsAccessFieldName{
    "isDirectSystemBucketsAccess"};

/**
 * Returns a settable boolean indicating whether the given operation context originated from a user
 * directly targeting a system.buckets namespace through the router.
 *
 * This flag is propagated to shards so they can enforce the BlockDirectSystemBucketsAccess
 * feature flag. Enforcement cannot happen on the router (mongos) because mongos lacks FCV
 * awareness: it always evaluates feature flags against the latest FCV rather than the cluster's
 * actual FCV, so it cannot reliably determine whether BlockDirectSystemBucketsAccess is enabled
 * for the current cluster version.
 *
 */
bool& isDirectSystemBucketsAccess(OperationContext*);

}  // namespace mongo
