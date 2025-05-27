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

#include "mongo/config.h"
#include "mongo/tracing/mock_exporter.h"
#include "mongo/tracing/traceable.h"
#include "mongo/unittest/unittest.h"

#include <opentelemetry/sdk/trace/simple_processor_factory.h>
#include <opentelemetry/sdk/trace/tracer_provider_factory.h>
#include <opentelemetry/trace/provider.h>

namespace mongo {
namespace tracing {
namespace {

using namespace opentelemetry::sdk::trace;

class SpanTest : public unittest::Test {
public:
    void setProvider() {
        auto uniqueExporter = std::make_unique<MockExporter>();
        _mockExporter = uniqueExporter.get();

        auto processor = opentelemetry::sdk::trace::SimpleSpanProcessorFactory::Create(
            std::move(uniqueExporter));
        auto resourceAttributes = opentelemetry::sdk::resource::ResourceAttributes{
            {"service.name", "test"}, {"service.instance.id", 1}};
        auto resource = opentelemetry::sdk::resource::Resource::Create(resourceAttributes);

        std::shared_ptr<trace_api::TracerProvider> provider =
            opentelemetry::sdk::trace::TracerProviderFactory::Create(std::move(processor),
                                                                     resource);
        trace_api::Provider::SetTracerProvider(std::move(provider));
    }

    void setUp() override {
        setProvider();
    }

    void clearProvider() {
        _mockExporter = nullptr;
        opentelemetry::trace::Provider::SetTracerProvider({});
    }

    void tearDown() override {
        clearProvider();
    }

    bool isEmpty() const {
        return _mockExporter->getSpans().empty();
    }

    MockRecordable* getSpan(size_t idx, const std::string& name) {
        const auto& spans = _mockExporter->getSpans();
        ASSERT_GREATER_THAN(spans.size(), idx);

        MockRecordable* mock = dynamic_cast<MockRecordable*>(spans[idx].get());
        ASSERT_TRUE(mock);
        ASSERT_EQ(mock->name, name);

        return mock;
    }

protected:
    MockExporter* _mockExporter;
};

TEST_F(SpanTest, NoTraceableStartSpan) {
    {
        auto span = tracing::Span::start(nullptr, "firstSpan");
        TRACING_SPAN_ATTR(span, "test", 1);
        ASSERT_TRUE(isEmpty());
    }
    ASSERT_TRUE(isEmpty());
}

TEST_F(SpanTest, NoTracerStartSpan) {
    clearProvider();
    {
        auto span = tracing::Span::start(nullptr, "secondSpan");
        TRACING_SPAN_ATTR(span, "test", 1);
    }
    // Note : this test checks that no crash happens if there's no trace provider. We can't call
    // `isEmpty` as this uses the trace provider to retrieve spans.
}

TEST_F(SpanTest, ExporterSingleSpan) {
    tracing::Traceable traceable;
    {
        auto span = tracing::Span::start(&traceable, "firstSpan");
        ASSERT_TRUE(isEmpty());
    }

    ASSERT_FALSE(isEmpty());
    auto span = getSpan(0, "firstSpan");
    ASSERT_EQ(span->parentId, opentelemetry::trace::SpanId());
}

TEST_F(SpanTest, ParentSpan) {
    tracing::Traceable traceable;
    {
        auto span = tracing::Span::start(&traceable, "firstSpan");

        auto secondSpan = tracing::Span::start(&traceable, "secondSpan");
        ASSERT_TRUE(isEmpty());
    }
    ASSERT_FALSE(isEmpty());

    {
        auto span = tracing::Span::start(&traceable, "thirdSpan");
    }

    auto firstRecord = getSpan(1, "firstSpan");
    ASSERT_EQ(firstRecord->parentId, opentelemetry::trace::SpanId());

    auto secondRecord = getSpan(0, "secondSpan");
    ASSERT_EQ(secondRecord->parentId, firstRecord->context.span_id());

    auto thirdRecord = getSpan(2, "thirdSpan");
    ASSERT_EQ(thirdRecord->parentId, opentelemetry::trace::SpanId());
}

TEST_F(SpanTest, SpanDepthThree) {
    tracing::Traceable traceable;
    {
        auto span = tracing::Span::start(&traceable, "firstSpan");
        auto secondSpan = tracing::Span::start(&traceable, "secondSpan");
        auto thirdSpan = tracing::Span::start(&traceable, "thirdSpan");

        ASSERT_TRUE(isEmpty());
    }

    auto firstRecord = getSpan(2, "firstSpan");
    ASSERT_EQ(firstRecord->parentId, opentelemetry::trace::SpanId());

    auto secondRecord = getSpan(1, "secondSpan");
    ASSERT_NE(secondRecord->parentId, opentelemetry::trace::SpanId());
    ASSERT_EQ(secondRecord->parentId, firstRecord->context.span_id());

    auto thirdRecord = getSpan(0, "thirdSpan");
    ASSERT_EQ(thirdRecord->parentId, secondRecord->context.span_id());
}

TEST_F(SpanTest, ParallelSpan) {
    tracing::Traceable traceable;
    {
        auto span = tracing::Span::start(&traceable, "firstSpan");

        tracing::Traceable traceable2{traceable};
        tracing::Traceable traceable3{traceable};

        auto secondSpan = tracing::Span::start(&traceable2, "secondSpan");
        auto thirdSpan = tracing::Span::start(&traceable3, "thirdSpan");

        ASSERT_TRUE(isEmpty());
    }

    auto firstRecord = getSpan(2, "firstSpan");
    ASSERT_EQ(firstRecord->parentId, opentelemetry::trace::SpanId());

    auto secondRecord = getSpan(1, "secondSpan");
    ASSERT_EQ(secondRecord->parentId, firstRecord->context.span_id());

    auto thirdRecord = getSpan(0, "thirdSpan");
    ASSERT_EQ(thirdRecord->parentId, firstRecord->context.span_id());
}

TEST_F(SpanTest, SetIntAttribute) {
    Traceable t;
    {
        auto span = tracing::Span::start(&t, "firstSpan");
        TRACING_SPAN_ATTR(span, "value1", 15);
        TRACING_SPAN_ATTR(span, "value2", 32);
    }

    auto firstRecord = getSpan(0, "firstSpan");
    ASSERT_EQ(firstRecord->parentId, opentelemetry::trace::SpanId());
    ASSERT_EQ(firstRecord->attributes.size(), 2);
    ASSERT_EQ(firstRecord->status, opentelemetry::trace::StatusCode::kOk);

    auto value1 = firstRecord->attributes.find("value1");
    ASSERT_NE(value1, firstRecord->attributes.end());
    ASSERT_TRUE(absl::holds_alternative<int32_t>(value1->second));
    ASSERT_EQ(static_cast<int>(absl::get<int32_t>(value1->second)), 15);

    auto value2 = firstRecord->attributes.find("value2");
    ASSERT_NE(value2, firstRecord->attributes.end());
    ASSERT_TRUE(absl::holds_alternative<int32_t>(value2->second));
    ASSERT_EQ(static_cast<int>(absl::get<int32_t>(value2->second)), 32);
}

TEST_F(SpanTest, ErrorCode) {
    Traceable t;
    {
        auto span = tracing::Span::start(&t, "firstSpan");
        span.setStatus(Status{ErrorCodes::InternalError, "failed"});
    }

    auto firstRecord = getSpan(0, "firstSpan");
    ASSERT_EQ(firstRecord->parentId, opentelemetry::trace::SpanId());
    ASSERT_EQ(firstRecord->attributes.size(), 1);
    ASSERT_EQ(firstRecord->status, opentelemetry::trace::StatusCode::kError);

    auto value1 = firstRecord->attributes.find("errorCode");
    ASSERT_NE(value1, firstRecord->attributes.end());
    ASSERT_TRUE(absl::holds_alternative<int32_t>(value1->second));
    ASSERT_EQ(static_cast<int>(absl::get<int32_t>(value1->second)), ErrorCodes::InternalError);
}

TEST_F(SpanTest, SpanDuringException) {
    Traceable t;
    try {
        auto span = tracing::Span::start(&t, "firstSpan");
        throw std::runtime_error{"testing"};
    } catch (const std::exception&) {
    }

    auto firstRecord = getSpan(0, "firstSpan");
    ASSERT_EQ(firstRecord->parentId, opentelemetry::trace::SpanId());
    ASSERT_EQ(firstRecord->attributes.size(), 0);
    ASSERT_EQ(firstRecord->status, opentelemetry::trace::StatusCode::kError);
}

}  // namespace
}  // namespace tracing
}  // namespace mongo
