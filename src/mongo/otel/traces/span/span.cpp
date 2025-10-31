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

#include "mongo/otel/traces/span/span.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/otel/telemetry_context_holder.h"
#include "mongo/otel/traces/span/span_telemetry_context_impl.h"
#include "mongo/otel/traces/tracer_provider_service.h"
#include "mongo/otel/traces/tracing_utils.h"
#include "mongo/util/assert_util.h"

#include <memory>

#include <opentelemetry/trace/provider.h>
#include <opentelemetry/trace/span.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

namespace mongo {
namespace otel {
namespace traces {

static constexpr OtelStringView errorCodeKey = "errorCode";
static constexpr OtelStringView errorCodeStringKey = "errorCodeString";
static constexpr OtelStringView dropSpanAttributeName = "DROP_SPAN";

using ScopedSpan = opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span>;

class Span::SpanImpl {
public:
    SpanImpl(ScopedSpan span,
             ScopedSpan parent,
             std::shared_ptr<SpanTelemetryContextImpl> spanCtx,
             bool keepSpanParent,
             bool keepSpan)
        : _span(std::move(span)),
          _parent(std::move(parent)),
          _spanCtx(std::move(spanCtx)),
          _keepSpanParent(keepSpanParent),
          _keepSpan(keepSpan) {
        _spanCtx->setSpan(_span);
        _spanCtx->keepSpan(keepSpan || keepSpanParent);
        _span->SetAttribute(dropSpanAttributeName, !(_keepSpan || _keepSpanParent));
    }

    ~SpanImpl() {

        _spanCtx->setSpan(std::move(_parent));
        _spanCtx->keepSpan(_keepSpanParent);

        if (!std::uncaught_exceptions() && !_error) {
            _span->SetStatus(opentelemetry::trace::StatusCode::kOk);
            _span->End();
        } else {
            _span->SetStatus(opentelemetry::trace::StatusCode::kError);
            _span->End();
        }
    }

    void setAttribute(StringData key, int value) {
        _span->SetAttribute(std::string{key}, value);
    }

    void setStatus(const Status& status) {
        _span->SetAttribute(errorCodeKey, status.code());
        _error = !status.isOK();
    }

private:
    ScopedSpan _span;
    ScopedSpan _parent;
    std::shared_ptr<SpanTelemetryContextImpl> _spanCtx;
    bool _keepSpanParent;
    bool _keepSpan;
    bool _error = false;
};

Span::Span() {}

Span::Span(std::unique_ptr<Span::SpanImpl> impl) : _impl(std::move(impl)) {}

Span::~Span() {}

Span& Span::operator=(Span&& other) {
    _impl = std::move(other._impl);
    return *this;
}

Span::Span(Span&& other) : _impl(std::move(other._impl)) {}

void Span::setAttribute(StringData key, int value) {
    if (_impl) {
        _impl->setAttribute(key, value);
    }
}

void Span::setStatus(const Status& status) {
    if (_impl) {
        _impl->setStatus(status);
    }
}

Span Span::start(OperationContext* opCtx, const std::string& name, bool keepSpan) {
    if (opCtx == nullptr) {
        return Span{};
    }

    auto& telemetryCtxHolder = TelemetryContextHolder::get(opCtx);
    auto telemetryCtx = telemetryCtxHolder.get();

    if (!telemetryCtx) {
        telemetryCtx = std::make_shared<SpanTelemetryContextImpl>();
        telemetryCtxHolder.set(telemetryCtx);
    }

    return start(telemetryCtx, name, keepSpan);
}

Span Span::startIfExistingTraceParent(OperationContext* opCtx,
                                      const std::string& name,
                                      bool keepSpan) {
    if (opCtx == nullptr) {
        return Span{};
    }

    auto telemetryCtx = TelemetryContextHolder::get(opCtx).get();

    if (!telemetryCtx) {
        return Span{};
    }

    return start(telemetryCtx, name, keepSpan);
}

Span Span::start(std::shared_ptr<TelemetryContext>& telemetryCtx,
                 const std::string& name,
                 bool keepSpan) {
    // Get ServiceContext from global context
    ServiceContext* serviceContext = getGlobalServiceContext();
    if (!serviceContext) {
        return Span{};
    }

    // Get the TracerProviderService from ServiceContext decoration
    auto tracerProviderService = TracerProviderService::get(serviceContext);
    if (!tracerProviderService || !tracerProviderService->isEnabled()) {
        return Span{};
    }

    auto tracerProvider = tracerProviderService->getTracerProvider();
    if (!tracerProvider) {
        return Span{};
    }

    auto tracer = tracerProvider->GetTracer("mongodb");
    if (!tracer) {
        return Span{};
    }

    if (!telemetryCtx) {
        telemetryCtx = std::make_shared<SpanTelemetryContextImpl>();
    }

    auto spanCtx = std::dynamic_pointer_cast<SpanTelemetryContextImpl>(telemetryCtx);
    if (!spanCtx) {
        LOGV2(10011700,
              "TelemetryContext is not of type SpanTelemetryContextImpl",
              "context_type"_attr = telemetryCtx->type());
        return Span{};
    }

    ScopedSpan parentSpan = spanCtx->getSpan();

    opentelemetry::trace::StartSpanOptions opts;
    opts.parent = parentSpan->GetContext();

    auto keepSpanParent = spanCtx->shouldKeepSpan();

    return Span(std::make_unique<Span::SpanImpl>(tracer->StartSpan(name, opts),
                                                 std::move(parentSpan),
                                                 std::move(spanCtx),
                                                 keepSpanParent,
                                                 keepSpan));
}

std::shared_ptr<TelemetryContext> Span::createTelemetryContext() {
    return std::make_shared<SpanTelemetryContextImpl>();
}

}  // namespace traces
}  // namespace otel
}  // namespace mongo
