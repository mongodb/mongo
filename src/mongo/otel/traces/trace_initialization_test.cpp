/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/otel/traces/trace_initialization.h"

#include "mongo/config.h"
#include "mongo/otel/traces/trace_settings_gen.h"
#include "mongo/unittest/unittest.h"

#include <opentelemetry/trace/noop.h>
#include <opentelemetry/trace/provider.h>

namespace mongo {
namespace {

class TraceInitializationTest : public unittest::Test {
public:
    void setUp() override {
        opentelemetry::trace::Provider::SetTracerProvider(
            std::make_shared<opentelemetry::trace::NoopTracerProvider>());
        otel::traces::gOpenTelemetryHttpEndpoint.clear();
        otel::traces::gOpenTelemetryTraceDirectory.clear();
    }

    void tearDown() override {
        opentelemetry::trace::Provider::SetTracerProvider({});
    }
};

bool isNoop(opentelemetry::trace::TracerProvider* provider) {
    return !!dynamic_cast<opentelemetry::trace::NoopTracerProvider*>(provider);
}

TEST_F(TraceInitializationTest, NoTraceProvider) {
    ASSERT_OK(otel::traces::initialize("mongod"));

    auto provider = opentelemetry::trace::Provider::GetTracerProvider();
    ASSERT_FALSE(provider);
}

TEST_F(TraceInitializationTest, Shutdown) {
    otel::traces::shutdown();

    ASSERT_TRUE(isNoop(opentelemetry::trace::Provider::GetTracerProvider().get()));

    otel::traces::gOpenTelemetryTraceDirectory = "/tmp/";
    otel::traces::shutdown();

    // After calling shutdown, the NoOpTraceProvider is removed.
    ASSERT_FALSE(opentelemetry::trace::Provider::GetTracerProvider());
}

TEST_F(TraceInitializationTest, FileTraceProvider) {
    otel::traces::gOpenTelemetryTraceDirectory = "/tmp/";
    ASSERT_OK(otel::traces::initialize("mongod"));

    auto provider = opentelemetry::trace::Provider::GetTracerProvider();
    ASSERT_FALSE(isNoop(provider.get()));
}

TEST_F(TraceInitializationTest, HttpTraceProvider) {
    otel::traces::gOpenTelemetryHttpEndpoint = "http://localhost:4318/v1/traces";
    ASSERT_OK(otel::traces::initialize("mongod"));

    auto provider = opentelemetry::trace::Provider::GetTracerProvider();
    ASSERT_FALSE(isNoop(provider.get()));
}

TEST_F(TraceInitializationTest, HttpAndDirectory) {
    otel::traces::gOpenTelemetryHttpEndpoint = "http://localhost:4318/v1/traces";
    otel::traces::gOpenTelemetryTraceDirectory = "/tmp/";
    ASSERT_THROWS_CODE(otel::traces::initialize("mongod"), DBException, ErrorCodes::InvalidOptions);

    auto provider = opentelemetry::trace::Provider::GetTracerProvider();
    ASSERT_TRUE(isNoop(provider.get()));
}

}  // namespace
}  // namespace mongo
