// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/otel/traces/tracer_provider_service_factory.h"

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
    auto swService = createHttpTracerProviderService(kServiceName, kHttpEndpoint);
    ASSERT_OK(swService);
    auto service = std::move(swService.getValue());

    auto* sdkProvider = getSdkProvider(*service);
    ASSERT_NE(sdkProvider, nullptr);
    EXPECT_EQ(getStringAttr(sdkProvider->GetResource(), "service.name"), kServiceName);
}

TEST(HttpInitTest, ResourceHasServiceInstanceId) {
    auto swService = createHttpTracerProviderService(kServiceName, kHttpEndpoint);
    ASSERT_OK(swService);
    auto service = std::move(swService.getValue());

    auto* sdkProvider = getSdkProvider(*service);
    ASSERT_NE(sdkProvider, nullptr);
    EXPECT_EQ(getStringAttr(sdkProvider->GetResource(), "service.instance.id"),
              ProcessId::getCurrent().toString());
}

TEST(HttpInitTest, UserResourceAttributesAppearInProvider) {
    unittest::ServerParameterGuard attrsParam{
        "openTelemetryTracingResourceAttributes",
        BSON("deployment.environment" << "staging" << "custom.key" << "custom-val")};

    auto swService = createHttpTracerProviderService(kServiceName, kHttpEndpoint);
    ASSERT_OK(swService);
    auto service = std::move(swService.getValue());

    auto* sdkProvider = getSdkProvider(*service);
    ASSERT_NE(sdkProvider, nullptr);

    const auto& resource = sdkProvider->GetResource();
    EXPECT_EQ(getStringAttr(resource, "deployment.environment"), "staging");
    EXPECT_EQ(getStringAttr(resource, "custom.key"), "custom-val");
}

TEST(HttpInitTest, UserAttributeOverrideServiceNameAndPidOverride) {
    unittest::ServerParameterGuard attrsParam{
        "openTelemetryTracingResourceAttributes",
        BSON("service.name" << "user-supplied-name" << "service.instance.id" << "user-pid"
                            << "custom.key" << "custom-val")};

    auto swService = createHttpTracerProviderService(kServiceName, kHttpEndpoint);
    ASSERT_OK(swService);
    auto service = std::move(swService.getValue());

    auto* sdkProvider = getSdkProvider(*service);
    ASSERT_NE(sdkProvider, nullptr);
    EXPECT_EQ(getStringAttr(sdkProvider->GetResource(), "service.name"), "user-supplied-name");
    EXPECT_EQ(getStringAttr(sdkProvider->GetResource(), "service.instance.id"), "user-pid");
    EXPECT_EQ(getStringAttr(sdkProvider->GetResource(), "custom.key"), "custom-val");
}

TEST(FileInitTest, ResourceHasServiceName) {
    TempDir dir("tmp");
    auto swService = createFileTracerProviderService(kServiceName, dir.path());
    ASSERT_OK(swService);
    auto service = std::move(swService.getValue());

    auto* sdkProvider = getSdkProvider(*service);
    ASSERT_NE(sdkProvider, nullptr);
    EXPECT_EQ(getStringAttr(sdkProvider->GetResource(), "service.name"), kServiceName);
}

TEST(FileInitTest, ResourceHasServiceInstanceId) {
    TempDir dir("tmp");
    auto swService = createFileTracerProviderService(kServiceName, dir.path());
    ASSERT_OK(swService);
    auto service = std::move(swService.getValue());

    auto* sdkProvider = getSdkProvider(*service);
    ASSERT_NE(sdkProvider, nullptr);
    EXPECT_EQ(getStringAttr(sdkProvider->GetResource(), "service.instance.id"),
              ProcessId::getCurrent().toString());
}

TEST(FileInitTest, UserResourceAttributesAppearInProvider) {
    unittest::ServerParameterGuard attrsParam{"openTelemetryTracingResourceAttributes",
                                              BSON("deployment.environment" << "production")};

    TempDir dir("tmp");
    auto swService = createFileTracerProviderService(kServiceName, dir.path());
    ASSERT_OK(swService);
    auto service = std::move(swService.getValue());

    auto* sdkProvider = getSdkProvider(*service);
    ASSERT_NE(sdkProvider, nullptr);
    EXPECT_EQ(getStringAttr(sdkProvider->GetResource(), "deployment.environment"), "production");
}

TEST(FileInitTest, UserAttributeOverrideServiceNameAndPidOverride) {
    unittest::ServerParameterGuard attrsParam{
        "openTelemetryTracingResourceAttributes",
        BSON("service.name" << "user-supplied-name" << "service.instance.id" << "user-pid"
                            << "custom.key" << "custom-val")};

    TempDir dir("tmp");
    auto swService = createFileTracerProviderService(kServiceName, dir.path());
    ASSERT_OK(swService);
    auto service = std::move(swService.getValue());

    auto* sdkProvider = getSdkProvider(*service);
    ASSERT_NE(sdkProvider, nullptr);
    EXPECT_EQ(getStringAttr(sdkProvider->GetResource(), "service.name"), "user-supplied-name");
    EXPECT_EQ(getStringAttr(sdkProvider->GetResource(), "service.instance.id"), "user-pid");
    EXPECT_EQ(getStringAttr(sdkProvider->GetResource(), "custom.key"), "custom-val");
}

}  // namespace
}  // namespace mongo::otel::traces
