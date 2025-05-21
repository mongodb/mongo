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

#include "mongo/tracing/tracing.h"

#ifdef MONGO_CONFIG_OTEL

#include <opentelemetry/trace/provider.h>
#include <opentelemetry/trace/span.h>
#include <opentelemetry/trace/span_context.h>

namespace mongo {
namespace tracing {

static constexpr StringData errorCodeKey = "errorCode"_sd;
static constexpr StringData errorCodeStringKey = "errorCodeString"_sd;

using ScopedSpan = opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span>;
using OtelSpanContext = opentelemetry::trace::SpanContext;

opentelemetry::nostd::string_view view(StringData data) {
    return opentelemetry::nostd::string_view{data.data(), data.length()};
}

class TracingContext : public Traceable::SpanContext {
public:
    TracingContext(OtelSpanContext ctx) : _ctx(std::move(ctx)) {}
    OtelSpanContext _ctx;
};

class Span::SpanImpl {
public:
    SpanImpl(Traceable* traceable, ScopedSpan span, std::shared_ptr<Traceable::SpanContext> parent)
        : _traceable(traceable), _span(std::move(span)), _parent(std::move(parent)) {
        _traceable->setActiveContext(std::make_shared<TracingContext>(_span->GetContext()));
    }

    ~SpanImpl() {
        _traceable->setActiveContext(_parent);

        if (!std::uncaught_exceptions() && !_error) {
            _span->SetStatus(opentelemetry::trace::StatusCode::kOk);
            _span->End();
        } else {
            _span->SetStatus(opentelemetry::trace::StatusCode::kError);
            _span->End();
        }
    }

    void setAttribute(StringData key, int value) {
        _span->SetAttribute(key.toString(), value);
    }

    void setStatus(const Status& status) {
        _span->SetAttribute(view(errorCodeKey), status.code());
        _error = !status.isOK();
    }

private:
    Traceable* _traceable;
    ScopedSpan _span;
    std::shared_ptr<Traceable::SpanContext> _parent;
    bool _error = false;
};

Span::Span() {}

Span::Span(std::unique_ptr<Span::SpanImpl> impl) : _impl(std::move(impl)) {}

Span::~Span() {}

Span& Span::operator=(Span&& other) {
    _impl = std::move(other._impl);

    return *this;
}

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

Span Span::start(Traceable* traceable, const std::string& name) {
    auto provider = opentelemetry::trace::Provider::GetTracerProvider();
    if (!provider || !traceable) {
        return Span{};
    }

    auto tracer = provider->GetTracer("mongodb");
    if (!tracer) {
        return Span{};
    }

    auto trCtx = traceable->getActiveContext();
    opentelemetry::trace::StartSpanOptions opts;
    if (trCtx) {
        if (auto spanCtxt = dynamic_cast<TracingContext*>(trCtx.get())) {
            opts.parent = spanCtxt->_ctx;
        }
    }

    return Span(std::make_unique<Span::SpanImpl>(traceable, tracer->StartSpan(name, opts), trCtx));
}

}  // namespace tracing
}  // namespace mongo

#endif
