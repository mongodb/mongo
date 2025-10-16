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

#include "mongo/db/service_context_test_fixture.h"
#include "mongo/otel/traces/mock_exporter.h"
#include "mongo/otel/traces/tracer_provider_service.h"

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
class OtelTestFixture : public ServiceContextTest {
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
        auto tracerProviderService = TracerProviderService::create();
        tracerProviderService->setTracerProvider_ForTest(provider);

        TracerProviderService::set(getServiceContext(), std::move(tracerProviderService));
    }

    void clearProvider() {
        _mockExporter = nullptr;
        // Clear the TracerProviderService from ServiceContext
        TracerProviderService::set(getServiceContext(), nullptr);
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

    int getBaseAttributesSize() {
        // We always set the "DROP_SPAN" attribute, so we expect at least one attribute.
        return 1;
    }

protected:
    MockExporter* _mockExporter;
};

}  // namespace traces
}  // namespace otel
}  // namespace mongo
