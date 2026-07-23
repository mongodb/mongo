// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/otel/traces/traces_test_util.h"

#ifdef MONGO_CONFIG_OTEL
#include "mongo/otel/traces/tracer_provider_service_factory.h"

#include <opentelemetry/sdk/resource/resource.h>
#include <opentelemetry/sdk/trace/simple_processor_factory.h>
#include <opentelemetry/sdk/trace/tracer_provider_factory.h>
#endif

namespace mongo::otel::traces {
namespace {
#ifdef MONGO_CONFIG_OTEL
using traces_test_util_detail::CapturedSpanData;

std::string attributeValueToString(const opentelemetry::common::AttributeValue& val) {
    return std::visit(
        [](auto&& v) -> std::string {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, const char*>) {
                return std::string{v};
            } else if constexpr (std::is_same_v<T, std::string_view>) {
                return std::string{v.data(), v.size()};
            } else {
                return "<unsupported>";
            }
        },
        val);
}

SpanKind fromOtelSpanKind(opentelemetry::trace::SpanKind kind) {
    switch (kind) {
        case opentelemetry::trace::SpanKind::kServer:
            return SpanKind::kServer;
        case opentelemetry::trace::SpanKind::kClient:
            return SpanKind::kClient;
        case opentelemetry::trace::SpanKind::kProducer:
            return SpanKind::kProducer;
        case opentelemetry::trace::SpanKind::kConsumer:
            return SpanKind::kConsumer;
        case opentelemetry::trace::SpanKind::kInternal:
            return SpanKind::kInternal;
    }
    MONGO_UNREACHABLE;
}
#endif
}  // namespace

std::string_view CapturedSpan::name() const {
#ifdef MONGO_CONFIG_OTEL
    if (_data)
        return _data->name;
#endif
    return {};
}

std::string_view CapturedSpan::attribute(std::string_view key) const {
#ifdef MONGO_CONFIG_OTEL
    if (_data) {
        auto it = _data->attributes.find(std::string(key));
        if (it != _data->attributes.end())
            return it->second;
    }
#endif
    return {};
}

const StringMap<std::string>& CapturedSpan::attributes() const {
#ifdef MONGO_CONFIG_OTEL
    if (_data)
        return _data->attributes;
#endif
    static const StringMap<std::string> empty;
    return empty;
}

bool CapturedSpan::isError() const {
#ifdef MONGO_CONFIG_OTEL
    if (_data)
        return _data->isError;
#endif
    return false;
}

SpanKind CapturedSpan::kind() const {
#ifdef MONGO_CONFIG_OTEL
    if (_data)
        return _data->kind;
#endif
    return SpanKind::kInternal;
}

std::string CapturedSpan::parentSpanIdHex() const {
#ifdef MONGO_CONFIG_OTEL
    if (_data && _data->parentSpanId.IsValid()) {
        char buf[16];
        _data->parentSpanId.ToLowerBase16(buf);
        return std::string(buf, sizeof(buf));
    }
#endif
    return {};
}

std::string CapturedSpan::traceIdHex() const {
#ifdef MONGO_CONFIG_OTEL
    if (_data && _data->traceId.IsValid()) {
        char buf[32];
        _data->traceId.ToLowerBase16(buf);
        return std::string(buf, sizeof(buf));
    }
#endif
    return {};
}

std::optional<CapturedSpan> CapturedSpan::parent() const {
#ifdef MONGO_CONFIG_OTEL
    if (_data && _data->parent)
        return CapturedSpan(_data->parent);
#endif
    return std::nullopt;
}

std::vector<CapturedSpan> CapturedSpan::children() const {
#ifdef MONGO_CONFIG_OTEL
    if (_data) {
        std::vector<CapturedSpan> result;
        result.reserve(_data->children.size());
        for (auto* child : _data->children)
            result.emplace_back(CapturedSpan(child));
        return result;
    }
#endif
    return {};
}

OtelTracesCapturer::OtelTracesCapturer() {
#ifdef MONGO_CONFIG_OTEL
    auto uniqueExporter = std::make_unique<MockExporter>();
    _exporter = uniqueExporter.get();

    auto processor =
        opentelemetry::sdk::trace::SimpleSpanProcessorFactory::Create(std::move(uniqueExporter));
    auto provider = opentelemetry::sdk::trace::TracerProviderFactory::Create(std::move(processor));

    auto service = createNoOpTracerProviderService();
    service->setTracerProvider_ForTest(std::move(provider));
    _savedProvider = swapGlobalTracerProviderServiceForTest(std::move(service));

    _samplerGuard = setTraceSamplingFnForTest([](std::string_view, double) { return true; });
#endif
}

OtelTracesCapturer::~OtelTracesCapturer() {
#ifdef MONGO_CONFIG_OTEL
    swapGlobalTracerProviderServiceForTest(std::move(_savedProvider));
#endif
}

bool OtelTracesCapturer::canReadSpans() {
#ifdef MONGO_CONFIG_OTEL
    return true;
#else
    return false;
#endif
}

#ifdef MONGO_CONFIG_OTEL
void OtelTracesCapturer::_rebuild() const {
    const auto& exported = _exporter->getSpans();
    const size_t processed = _baseIndex + _spans.size();


    if (processed == exported.size())
        return;  // nothing new to do.

    // Only process spans that were exported since the last _rebuild.
    for (size_t i = processed; i < exported.size(); ++i) {
        const MockRecordable* rec = exported[i].get();
        auto data = std::make_unique<CapturedSpanData>(CapturedSpanData{
            .name = rec->name,
            .isError = (rec->status == opentelemetry::trace::StatusCode::kError),
            .kind = fromOtelSpanKind(rec->kind),
            .spanId = rec->context.span_id(),
            .parentSpanId = rec->parentId,
            .traceId = rec->context.trace_id(),
        });
        for (auto& [k, v] : rec->attributes)
            data->attributes[k] = attributeValueToString(v);
        _spans.push_back(std::move(data));
    }

    // Reset and re-wire all parent/child links, in case there are any new links.
    for (std::unique_ptr<CapturedSpanData>& spanData : _spans) {
        spanData->parent = nullptr;
        spanData->children.clear();
    }
    for (std::unique_ptr<CapturedSpanData>& spanData : _spans) {
        if (!spanData->parentSpanId.IsValid())
            continue;
        // spanId is not hashable, so just do linear scan.
        for (std::unique_ptr<CapturedSpanData>& potentialParent : _spans) {
            if (potentialParent->spanId == spanData->parentSpanId) {
                spanData->parent = potentialParent.get();
                potentialParent->children.push_back(spanData.get());
                break;
            }
        }
    }
}
#endif

std::vector<CapturedSpan> OtelTracesCapturer::getSpans(SpanName name) const {
    return getSpans(name.getName());
}

std::vector<CapturedSpan> OtelTracesCapturer::getSpans(std::string_view name) const {
#ifdef MONGO_CONFIG_OTEL
    _rebuild();
    std::vector<CapturedSpan> result;
    for (const auto& data : _spans) {
        if (std::string_view(data->name) == name)
            result.emplace_back(CapturedSpan(data.get()));
    }
    return result;
#else
    invariant(false, "OtelTracesCapturer is not available (OTel not compiled)");
    MONGO_UNREACHABLE;
#endif
}

void OtelTracesCapturer::clearSpans() {
#ifdef MONGO_CONFIG_OTEL
    _baseIndex = _exporter->getSpans().size();
    _spans.clear();
#endif
}

}  // namespace mongo::otel::traces
