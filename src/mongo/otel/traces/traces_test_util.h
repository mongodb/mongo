/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/config.h"
#include "mongo/otel/traces/span/span_names.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/modules.h"
#include "mongo/util/string_map.h"

#ifdef MONGO_CONFIG_OTEL
#include "mongo/otel/traces/mock_exporter.h"
#include "mongo/otel/traces/sampler/sampler.h"
#include "mongo/otel/traces/tracer_provider_service.h"
#endif

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mongo::otel::traces {

namespace traces_test_util_detail {
#ifdef MONGO_CONFIG_OTEL
/**
 * Internal storage for a captured span. Parent/child raw pointers are into storage owned by
 * OtelTracesCapturer and are valid for the capturer's lifetime (or until clearSpans()).
 */
struct CapturedSpanData {
    std::string name;
    StringMap<std::string> attributes;
    bool isError = false;
    opentelemetry::trace::SpanId spanId;
    opentelemetry::trace::SpanId parentSpanId;
    opentelemetry::trace::TraceId traceId;
    CapturedSpanData* parent = nullptr;
    std::vector<CapturedSpanData*> children;
};
#endif

inline std::string_view toStringView(std::string_view s) {
    return s;
}
inline std::string_view toStringView(const SpanName& s) {
    return s.getName();
}
}  // namespace traces_test_util_detail

/**
 * Lightweight view of a captured OTel span. Methods are no-ops when OTel is not compiled.  Valid
 * until OtelTracesCapturer::clearSpans() or the capturer is destroyed.
 */
class MONGO_MOD_PUBLIC CapturedSpan {
public:
#ifdef MONGO_CONFIG_OTEL
    /** `data` must not be null and must outlive the lifetime of this class. */
    explicit CapturedSpan(const traces_test_util_detail::CapturedSpanData* data) : _data(data) {
        invariant(data != nullptr);
    }
#endif
    std::string_view name() const;
    /**
     * Only string-valued attributes are supported currently.
     * TODO(SERVER-129450): Support other attribute types.
     */
    std::string_view attribute(std::string_view key) const;
    const StringMap<std::string>& attributes() const;
    bool isError() const;

    std::optional<CapturedSpan> parent() const;
    std::vector<CapturedSpan> children() const;

    /**
     * Returns the hex-encoded parent span ID (16 lowercase hex chars), or empty string if there is
     * no parent span ID set. A non-empty value with no parent() indicates a remote parent.
     */
    std::string parentSpanIdHex() const;

    /** Returns the hex-encoded trace ID (32 lowercase hex chars), or empty string if not set. */
    std::string traceIdHex() const;

private:
    friend class OtelTracesCapturer;
#ifdef MONGO_CONFIG_OTEL
    const traces_test_util_detail::CapturedSpanData* _data = nullptr;
#endif
};

/**
 * Utility for testing span creation. Installs an in-memory span exporter for the duration of its
 * scope, capturing all spans started via Span::start (i.e., spans are always recorded).
 *
 * Usage:
 *   OtelTracesCapturer capturer;
 *
 *   doTheOperation();
 *
 *   if (OtelTracesCapturer::canReadSpans()) {
 *     auto spans = capturer.getSpans(SpanNames::kMySpan);
 *     ASSERT_THAT(spans, ElementsAre(HasSpanName("mySpan")));
 *   }
 *
 * Or if the purpose of the test case is just to test span creation:
 *   OtelTracesCapturer capturer;
 *   if (!OtelTracesCapturer::canReadSpans()) GTEST_SKIP() << "OTel not configured";
 *
 *   doTheOperation();
 *
 *   auto spans = capturer.getSpans(SpanNames::kMySpan);
 *   ASSERT_THAT(spans, ElementsAre(HasSpanName("mySpan")));
 */
class MONGO_MOD_PUBLIC OtelTracesCapturer {
public:
    OtelTracesCapturer();
    ~OtelTracesCapturer();

    OtelTracesCapturer(const OtelTracesCapturer&) = delete;
    OtelTracesCapturer& operator=(const OtelTracesCapturer&) = delete;

    /** Returns false on platforms where OTel is not compiled (e.g. Windows). */
    static bool canReadSpans();

    /**
     * Returns all captured spans with the given name. Rebuilds the span tree from the in-memory
     * exporter on each call; previously returned CapturedSpans remain valid until the next
     * getSpans() call, clearSpans(), or capturer destruction. Note that this will *not* return
     * spans that were started before the capturer was created or started but not completed before
     * this call.
     *
     * Asserts canReadSpans().
     */
    std::vector<CapturedSpan> getSpans(SpanName name) const;
    std::vector<CapturedSpan> getSpans(std::string_view name) const;

    /**
     * Clears all captured spans and invalidates any previously returned CapturedSpans. Spans
     * created before this call will not appear in future getSpans() results.
     */
    void clearSpans();

private:
#ifdef MONGO_CONFIG_OTEL
    void _rebuild() const;

    // The global TracerProviderService that was in place before the capturer was created, and will
    // be restored upon capturer destruction.
    std::unique_ptr<TracerProviderService> _savedProvider;
    // Allows capturing all created spans, regardless of sampling configuration.
    ScopedSamplerOverride _samplerGuard;
    unittest::ServerParameterGuard _samplingFlag{"featureFlagOtelTraceSampling", true};
    MockExporter* _exporter = nullptr;  // owned by the TracerProvider's processor

    // Spans before _baseIndex in the exporter are ignored (cleared by clearSpans()).
    mutable size_t _baseIndex = 0;
    // Owned span data; CapturedSpan views hold raw pointers into this storage.
    mutable std::vector<std::unique_ptr<traces_test_util_detail::CapturedSpanData>> _spans;
#endif
};

// ---------------------------------------------------------------------------
// GTest matchers
// ---------------------------------------------------------------------------

/** Matches a CapturedSpan whose name() equals the given string or SpanName. */
MATCHER_P(HasSpanName, name, "") {
    return arg.name() == traces_test_util_detail::toStringView(name);
}

/** Matches a CapturedSpan that has an attribute with the given key and value. */
MATCHER_P2(HasAttribute, key, value, "") {
    auto it = arg.attributes().find(std::string_view(key));
    if (it == arg.attributes().end()) {
        *result_listener << "has no attribute with key " << key;
        return false;
    }
    return it->second == std::string_view(value);
}

/**
 * Matches a CapturedSpan whose parent() satisfies the inner matcher. Fails if there is no parent.
 */
MATCHER_P(Parent, inner, "") {
    auto p = arg.parent();
    if (!p) {
        *result_listener << "has no parent";
        return false;
    }
    return testing::ExplainMatchResult(inner, *p, result_listener);
}

/** Matches a CapturedSpan whose children() satisfy the inner container matcher. */
MATCHER_P(Children, inner, "") {
    return testing::ExplainMatchResult(inner, arg.children(), result_listener);
}

}  // namespace mongo::otel::traces
