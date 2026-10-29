// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/otel/telemetry_context_holder.h"

namespace mongo {
namespace otel {

const mongo::OperationContext::Decoration<TelemetryContextHolder> handle =
    mongo::OperationContext::declareDecoration<TelemetryContextHolder>();
TelemetryContextHolder& TelemetryContextHolder::getDecoration(OperationContext* opCtx) {
    return handle(opCtx);
}

}  // namespace otel
}  // namespace mongo
