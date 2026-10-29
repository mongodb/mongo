// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/resharding/resharding_test_util.h"

#include "mongo/unittest/unittest.h"

namespace mongo {
namespace resharding_test_util {

void assertSoon(OperationContext* opCtx,
                std::function<bool()> pred,
                Milliseconds timeout,
                Milliseconds interval) {
    auto startTime = Date_t::now();
    while (Date_t::now() - startTime < timeout) {
        if (pred()) {
            return;
        }
        opCtx->sleepFor(interval);
    }
    ASSERT(false) << "Timed out waiting the predicate to be satisfied";
}

}  // namespace resharding_test_util
}  // namespace mongo
