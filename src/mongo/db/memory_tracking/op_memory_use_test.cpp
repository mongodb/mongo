/**
 *    Copyright (C) 2025-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/memory_tracking/op_memory_use.h"

#include "mongo/db/memory_tracking/operation_memory_usage_tracker.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/idl/server_parameter_test_util.h"

namespace mongo {
namespace {

ServiceContext::UniqueOperationContext makeOpCtx() {
    return cc().makeOperationContext();
}

TEST_F(ServiceContextTest, OpMemoryUseCanAttachToOpCtx) {
    RAIIServerParameterControllerForTest featureFlagController("featureFlagQueryMemoryTracking",
                                                               true);

    auto opCtx = makeOpCtx();

    auto tracker = std::make_unique<OperationMemoryUsageTracker>(opCtx.get());
    tracker->add(15);
    tracker->add(-5);

    OpMemoryUse::operationMemoryAggregator(opCtx.get()) = std::move(tracker);
    OperationMemoryUsageTracker* memoryTracker =
        OpMemoryUse::operationMemoryAggregator(opCtx.get()).get();
    ASSERT(memoryTracker);
    ASSERT_EQ(memoryTracker->currentMemoryBytes(), 10);
    ASSERT_EQ(memoryTracker->maxMemoryBytes(), 15);
}

}  // namespace
}  // namespace mongo
