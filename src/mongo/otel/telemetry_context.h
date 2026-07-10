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
