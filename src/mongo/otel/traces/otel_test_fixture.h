// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/service_context_test_fixture.h"
#include "mongo/otel/traces/mock_exporter.h"
#include "mongo/otel/traces/span/span_names.h"
#include "mongo/otel/traces/tracer_provider_service.h"
#include "mongo/otel/traces/tracer_provider_service_factory.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/modules.h"

#include <opentelemetry/sdk/trace/simple_processor_factory.h>
#include <opentelemetry/sdk/trace/tracer_provider_factory.h>
#include <opentelemetry/trace/provider.h>

namespace mongo {
namespace otel {
namespace traces {

/**
 * Test fixture for tests that require OpenTelemetry TracerProvider to be initialized.
 * Sets up a TracerProvider with MockExporter so tests can create and inspect spans.
 */
class [[MONGO_MOD_OPEN]] OtelTestFixture : public ServiceContextTest {
public:
    void setUp() override {
        ServiceContextTest::setUp();
        setProvider();
    }

    void tearDown() override {
        ServiceContextTest::tearDown();
        clearProvider();
    }

    void setProvider() {
        auto uniqueExporter = std::make_unique<MockExporter>();
        _mockExporter = uniqueExporter.get();

        auto processor = opentelemetry::sdk::trace::SimpleSpanProcessorFactory::Create(
            std::move(uniqueExporter));
        auto resourceAttributes = opentelemetry::sdk::resource::ResourceAttributes{
            {"service.name", "test"}, {"service.instance.id", 1}};
        auto resource = opentelemetry::sdk::resource::Resource::Create(resourceAttributes);

        std::shared_ptr<opentelemetry::trace::TracerProvider> provider =
            opentelemetry::sdk::trace::TracerProviderFactory::Create(std::move(processor),
                                                                     resource);
        // Create and set the TracerProviderService
        auto tracerProviderService = createNoOpTracerProviderService();
        tracerProviderService->setTracerProvider_ForTest(provider);

        setGlobalTracerProviderService(std::move(tracerProviderService));
    }

    void clearProvider() {
        _mockExporter = nullptr;
        setGlobalTracerProviderService(nullptr);
    }

    bool isEmpty() const {
        return _mockExporter->getSpans().empty();
    }

    MockRecordable* getSpan(size_t idx, const SpanName& name) {
        const auto& spans = _mockExporter->getSpans();
        ASSERT_GREATER_THAN(spans.size(), idx);

        MockRecordable* mock = dynamic_cast<MockRecordable*>(spans[idx].get());
        ASSERT_TRUE(mock);
        ASSERT_EQ(mock->name, name.getName());

        return mock;
    }

protected:
    MockExporter* _mockExporter;
};

}  // namespace traces
}  // namespace otel
}  // namespace mongo
