// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/otel/telemetry_context_holder.h"

#include "mongo/unittest/unittest.h"

namespace mongo {
namespace otel {
namespace {

TEST(TelemetryContextHolderTest, CloneTelemetryContextReturnsNullWhenNoContext) {
    TelemetryContextHolder holder;
    EXPECT_EQ(holder.cloneTelemetryContext(), nullptr);
}

TEST(TelemetryContextHolderTest, CloneTelemetryContextReturnsDifferentInstance) {
    TelemetryContextHolder holder;
    holder.setTelemetryContext(std::make_shared<TelemetryContext>());
    auto clone = holder.cloneTelemetryContext();
    EXPECT_NE(clone.get(), holder.getTelemetryContext().get());
}

}  // namespace
}  // namespace otel
}  // namespace mongo
