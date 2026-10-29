// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <memory>
#include <string_view>

namespace mongo {
namespace otel {

/**
 * TelemetryContext is an interface that wraps OpenTelemetry's Context to allow for propagation of
 * state across OpenTelemetry functionality.
 */
class [[MONGO_MOD_PUBLIC]] TelemetryContext {
public:
    virtual ~TelemetryContext() = default;
    virtual std::string_view type() const {
        return "TelemetryContext";
    };
    virtual bool hasActiveTrace() const {
        return false;
    }

    /**
     * Creates a new TelemetryContext from this one such that any spans started on the new
     * TelemetryContext will have as parent the span currently active on this TelemetryContext. This
     * is intended for cases where multiple spans will be started concurrently - by creating clones,
     * new spans are tied to the appropriate parent span. Note that if a cloned context outlives the
     * current span, new spans on the clone will still have that span as parent.
     *
     * When `opCtx` is null or has no existing telemetry context, returns nullptr. It is ok to
     * attempt to create new spans on a null context.
     *
     * As an example, the following code:
     *
     * auto telemetryCtx = Span::createTelemetryContext();
     * auto span1 = Span::start(telemetryCtx, kName1);
     * auto span2 = Span::start(telemetryCtx, kName2);
     * auto span3 = Span::start(telemetryCtx, kName3);
     *
     * results in the span tree:
     *
     * kName1
     *  └── kName2
     *       └── kName3
     *
     * But creating child contexts by:
     *
     * auto telemetryCtx = Span::createTelemetryContext();
     * auto span1 = Span::start(telemetryCtx, kName1);
     * auto childCtx1 = telemetryCtx->clone();
     * auto span2 = Span::start(childCtx1, kName2);
     * auto childCtx2 = telemetryCtx->clone();
     * auto span3 = Span::start(childCtx2, kName3);
     *
     * results in the span tree:
     *
     * kName1
     *  └── kName2
     *  └── kName3
     */
    [[nodiscard]] virtual std::shared_ptr<TelemetryContext> clone() const {
        return std::make_shared<TelemetryContext>();
    }
};

}  // namespace otel
}  // namespace mongo
