// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/otel/telemetry_context.h"
#include "mongo/util/modules.h"

#include <memory>

namespace mongo {
namespace otel {

/**
 * TelemetryContextHolder is a decoration on OperationContext that holds the current
 * TelemetryContext. TelemetryContext is a wrapper for OpenTelemetry's Context that is used to
 * propagate parent / child relationships between Spans as well as hold metadata to correlate
 * various telemetry data.
 */
class [[MONGO_MOD_PUBLIC]] TelemetryContextHolder {
public:
    static TelemetryContextHolder& getDecoration(OperationContext* opCtx);

    const std::shared_ptr<TelemetryContext>& getTelemetryContext() {
        return _current;
    }
    void setTelemetryContext(std::shared_ptr<TelemetryContext> context) {
        _current = std::move(context);
    }
    void clearTelemetryContext() {
        _current.reset();
    }
    /**
     * Clones the current TelemetryContext, and returns nullptr if no context is set. See
     * TelemetryContext::clone() for more details. Note that it is safe to call Span::start with the
     * returned value even if it is nullptr.
     */
    std::shared_ptr<TelemetryContext> cloneTelemetryContext() {
        if (!_current) {
            return nullptr;
        }
        return _current->clone();
    }

private:
    std::shared_ptr<TelemetryContext> _current;
};

}  // namespace otel
}  // namespace mongo
