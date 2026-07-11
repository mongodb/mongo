// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"

namespace mongo {
namespace resharding_test_util {

const Milliseconds kAssertSoonTimeout{5 * 60};
const Milliseconds kAssertSoonInterval{5};

/*
 * Calls the 'pred' function at repeated intervals until either 'pred' returns true
 * or more than 'timeout' milliseconds have elapsed. Throws an exception after timing out.
 */
void assertSoon(OperationContext* opCtx,
                std::function<bool()> pred,
                Milliseconds timeout = kAssertSoonTimeout,
                Milliseconds interval = kAssertSoonInterval);

}  // namespace resharding_test_util
}  // namespace mongo
