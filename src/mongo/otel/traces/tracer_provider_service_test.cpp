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

#include "mongo/otel/traces/tracer_provider_service.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/otel/traces/trace_settings.h"
#include "mongo/platform/process_id.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"

#include <gmock/gmock.h>
#include <opentelemetry/nostd/variant.h>
#include <opentelemetry/sdk/resource/resource.h>
#include <opentelemetry/sdk/trace/tracer_provider.h>

namespace mongo::otel::traces {
namespace {

using mongo::unittest::TempDir;

constexpr auto kServiceName = "mongod";
constexpr auto kHttpEndpoint = "http://localhost:4318/v1/traces";

/**
 * Returns the OTel SDK-level TracerProvider, or nullptr if the service holds a
 * non-SDK provider (e.g. noop).
 */
opentelemetry::sdk::trace::TracerProvider* getSdkProvider(TracerProviderService& svc) {
    return dynamic_cast<opentelemetry::sdk::trace::TracerProvider*>(svc.getTracerProvider().get());
}

/**
 * Extracts a string resource attribute by key. Returns "" if absent or the wrong type.
 * Note: "" is also a valid attribute value, so absence and empty-string are not distinguished.
 */
std::string getStringAttr(const opentelemetry::sdk::resource::Resource& resource,
                          const std::string& key) {
    const auto& attrs = resource.GetAttributes();
    auto it = attrs.find(key);
    if (it == attrs.end()) {
        return {};
    }
    const auto* s = opentelemetry::nostd::get_if<std::string>(&it->second);
    return s ? *s : std::string{};
}

TEST(HttpInitTest, ResourceHasServiceName) {
    std::unique_ptr<TracerProviderService> service = TracerProviderService::create();
    ASSERT_OK(service->initializeHttp(kServiceName, kHttpEndpoint));

    auto* sdkProvider = getSdkProvider(*service);
    ASSERT_NE(sdkProvider, nullptr);
    EXPECT_EQ(getStringAttr(sdkProvider->GetResource(), "service.name"), kServiceName);
}

TEST(HttpInitTest, ResourceHasServiceInstanceId) {
    std::unique_ptr<TracerProviderService> service = TracerProviderService::create();
    ASSERT_OK(service->initializeHttp(kServiceName, kHttpEndpoint));

    auto* sdkProvider = getSdkProvider(*service);
    ASSERT_NE(sdkProvider, nullptr);
    EXPECT_EQ(getStringAttr(sdkProvider->GetResource(), "service.instance.id"),
              ProcessId::getCurrent().toString());
}

TEST(HttpInitTest, UserResourceAttributesAppearInProvider) {
    std::unique_ptr<TracerProviderService> service = TracerProviderService::create();
    unittest::ServerParameterGuard attrsParam{
        "openTelemetryTracingResourceAttributes",
        BSON("deployment.environment" << "staging" << "custom.key" << "custom-val")};

    ASSERT_OK(service->initializeHttp(kServiceName, kHttpEndpoint));

    auto* sdkProvider = getSdkProvider(*service);
    ASSERT_NE(sdkProvider, nullptr);

    const auto& resource = sdkProvider->GetResource();
    EXPECT_EQ(getStringAttr(resource, "deployment.environment"), "staging");
    EXPECT_EQ(getStringAttr(resource, "custom.key"), "custom-val");
}

TEST(HttpInitTest, UserAttributeOverrideServiceNameAndPidOverride) {
    std::unique_ptr<TracerProviderService> service = TracerProviderService::create();
    unittest::ServerParameterGuard attrsParam{
        "openTelemetryTracingResourceAttributes",
        BSON("service.name" << "user-supplied-name" << "service.instance.id" << "user-pid"
                            << "custom.key" << "custom-val")};

    ASSERT_OK(service->initializeHttp(kServiceName, kHttpEndpoint));

    auto* sdkProvider = getSdkProvider(*service);
    ASSERT_NE(sdkProvider, nullptr);
    EXPECT_EQ(getStringAttr(sdkProvider->GetResource(), "service.name"), "user-supplied-name");
    EXPECT_EQ(getStringAttr(sdkProvider->GetResource(), "service.instance.id"), "user-pid");
    EXPECT_EQ(getStringAttr(sdkProvider->GetResource(), "custom.key"), "custom-val");
}

TEST(FileInitTest, ResourceHasServiceName) {
    std::unique_ptr<TracerProviderService> service = TracerProviderService::create();
    TempDir dir("tmp");
    ASSERT_OK(service->initializeFile(kServiceName, dir.path()));

    auto* sdkProvider = getSdkProvider(*service);
    ASSERT_NE(sdkProvider, nullptr);
    EXPECT_EQ(getStringAttr(sdkProvider->GetResource(), "service.name"), kServiceName);
}

TEST(FileInitTest, ResourceHasServiceInstanceId) {
    std::unique_ptr<TracerProviderService> service = TracerProviderService::create();
    TempDir dir("tmp");
    ASSERT_OK(service->initializeFile(kServiceName, dir.path()));

    auto* sdkProvider = getSdkProvider(*service);
    ASSERT_NE(sdkProvider, nullptr);
    EXPECT_EQ(getStringAttr(sdkProvider->GetResource(), "service.instance.id"),
              ProcessId::getCurrent().toString());
}

TEST(FileInitTest, UserResourceAttributesAppearInProvider) {
    std::unique_ptr<TracerProviderService> service = TracerProviderService::create();
    unittest::ServerParameterGuard attrsParam{"openTelemetryTracingResourceAttributes",
                                              BSON("deployment.environment" << "production")};

    TempDir dir("tmp");
    ASSERT_OK(service->initializeFile(kServiceName, dir.path()));

    auto* sdkProvider = getSdkProvider(*service);
    ASSERT_NE(sdkProvider, nullptr);
    EXPECT_EQ(getStringAttr(sdkProvider->GetResource(), "deployment.environment"), "production");
}

TEST(FileInitTest, UserAttributeOverrideServiceNameAndPidOverride) {
    std::unique_ptr<TracerProviderService> service = TracerProviderService::create();
    unittest::ServerParameterGuard attrsParam{
        "openTelemetryTracingResourceAttributes",
        BSON("service.name" << "user-supplied-name" << "service.instance.id" << "user-pid"
                            << "custom.key" << "custom-val")};

    TempDir dir("tmp");
    ASSERT_OK(service->initializeFile(kServiceName, dir.path()));

    auto* sdkProvider = getSdkProvider(*service);
    ASSERT_NE(sdkProvider, nullptr);
    EXPECT_EQ(getStringAttr(sdkProvider->GetResource(), "service.name"), "user-supplied-name");
    EXPECT_EQ(getStringAttr(sdkProvider->GetResource(), "service.instance.id"), "user-pid");
    EXPECT_EQ(getStringAttr(sdkProvider->GetResource(), "custom.key"), "custom-val");
}

}  // namespace
}  // namespace mongo::otel::traces
