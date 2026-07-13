// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"

#include <boost/optional.hpp>

namespace mongo {

/**
 * Returns a settable, optional counter tracking how many times the router loop has retried this
 * operation on a StaleConfig error.
 *
 * Semantics:
 *  - Unset: the operation is not being tracked by a router loop that understands this protocol
 *    (e.g. an old/FCV-disabled router, or a non-routed operation).
 *  - 0: the router has armed retry tracking and this is the first attempt.
 *  - >0: the router has already retried a StaleConfig at least once.
 */
[[MONGO_MOD_PUBLIC]] boost::optional<int>& staleConfigRetryAttempt(OperationContext*);

}  // namespace mongo
