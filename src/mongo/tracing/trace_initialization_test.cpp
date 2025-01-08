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

#include "mongo/config.h"

#ifdef MONGO_CONFIG_OTEL

#include <opentelemetry/trace/noop.h>
#include <opentelemetry/trace/provider.h>

#include "mongo/tracing/trace_initialization.h"
#include "mongo/tracing/trace_settings_gen.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class TraceInitializationTest : public unittest::Test {
public:
    void setUp() override {
        opentelemetry::trace::Provider::SetTracerProvider(
            std::make_shared<opentelemetry::trace::NoopTracerProvider>());
        tracing::gOpenTelemetryHttpEndpoint.clear();
        tracing::gOpenTelemetryTraceDirectory.clear();
    }

    void tearDown() override {
        opentelemetry::trace::Provider::SetTracerProvider({});
    }
};

bool isNoop(opentelemetry::trace::TracerProvider* provider) {
    return !!dynamic_cast<opentelemetry::trace::NoopTracerProvider*>(provider);
}

TEST_F(TraceInitializationTest, NoTraceProvider) {
    ASSERT_OK(tracing::initialize("mongod"));

    auto provider = opentelemetry::trace::Provider::GetTracerProvider();
    ASSERT_TRUE(isNoop(provider.get()));
}

TEST_F(TraceInitializationTest, Shutdown) {
    tracing::shutdown();

    ASSERT_FALSE(opentelemetry::trace::Provider::GetTracerProvider());
}

TEST_F(TraceInitializationTest, FileTraceProvider) {
    tracing::gOpenTelemetryTraceDirectory = "/tmp/";
    ASSERT_OK(tracing::initialize("mongod"));

    auto provider = opentelemetry::trace::Provider::GetTracerProvider();
    ASSERT_FALSE(isNoop(provider.get()));
}

TEST_F(TraceInitializationTest, HttpTraceProvider) {
    tracing::gOpenTelemetryHttpEndpoint = "http://localhost:4318/v1/traces";
    ASSERT_OK(tracing::initialize("mongod"));

    auto provider = opentelemetry::trace::Provider::GetTracerProvider();
    ASSERT_FALSE(isNoop(provider.get()));
}

TEST_F(TraceInitializationTest, HttpAndDirectory) {
    tracing::gOpenTelemetryHttpEndpoint = "http://localhost:4318/v1/traces";
    tracing::gOpenTelemetryTraceDirectory = "/tmp/";
    ASSERT_THROWS_CODE(tracing::initialize("mongod"), DBException, ErrorCodes::InvalidOptions);

    auto provider = opentelemetry::trace::Provider::GetTracerProvider();
    ASSERT_TRUE(isNoop(provider.get()));
}

}  // namespace
}  // namespace mongo

#endif
