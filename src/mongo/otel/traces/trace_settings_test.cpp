// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/otel/traces/trace_settings.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/otel/traces/trace_settings_gen.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

#include <string_view>

#include <boost/optional.hpp>
#include <gmock/gmock.h>

namespace mongo::otel::traces {
namespace {

using namespace std::literals::string_view_literals;

using mongo::unittest::match::BSONObjUnorderedEQ;
using mongo::unittest::match::IsBSONElement;
using mongo::unittest::match::StatusIs;
using mongo::unittest::match::StatusIsOK;
using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::Matcher;
using ::testing::Not;
using ::testing::Pair;
using ::testing::SizeIs;

class HttpHeadersTest : public unittest::Test {
public:
    OpenTelemetryTracingHttpExportHeaders headers{"openTelemetryTracingHttpExportHeaders",
                                                  ServerParameterType::kStartupOnly};

    void setUp() override {
        ASSERT_OK(setHeaders(BSONObj{}));
    }

    void tearDown() override {
        ASSERT_OK(setHeaders(BSONObj{}));
    }

    Status setHeaders(BSONObj doc) {
        auto storage = BSON("v" << doc);
        return headers.set(storage.firstElement(), /*tenantId=*/boost::none);
    }
};

TEST_F(HttpHeadersTest, HttpExportHeadersSetFromBson) {
    // Verify headers can be set and are stored
    ASSERT_OK(setHeaders(BSON("Authorization" << "Bearer tok")));
    EXPECT_THAT(getTracingHttpExportHeaders(), SizeIs(1));

    // Verify headers can be updated
    ASSERT_OK(setHeaders(
        BSON("Authorization" << "Bearer tok" << "X-Tenant-ID" << "acme" << "X-Empty-Value" << "")));
    EXPECT_THAT(getTracingHttpExportHeaders(), SizeIs(3));

    // Verify that an empty object causes headers to be cleared
    ASSERT_OK(setHeaders(BSONObj{}));
    EXPECT_THAT(getTracingHttpExportHeaders(), IsEmpty());
}

TEST_F(HttpHeadersTest, HttpExportHeadersSetFromString) {
    // Verify setFromString behavior
    ASSERT_OK(headers.setFromString("{\"Authorization\": \"Bearer tok\"}", boost::none));
    EXPECT_THAT(getTracingHttpExportHeaders(), SizeIs(1));
    EXPECT_THAT(getTracingHttpExportHeaders(),
                ElementsAre(Pair("Authorization", ElementsAre("Bearer tok"))));

    EXPECT_THAT(headers.setFromString("{\"BadBson\":", boost::none),
                StatusIs(ErrorCodes::BadValue, HasSubstr("convert string to BSON")));
}

TEST_F(HttpHeadersTest, HttpExportHeadersFailurePreservesPreviousValue) {
    ASSERT_OK(setHeaders(BSON("Authorization" << "Bearer tok")));
    EXPECT_THAT(getTracingHttpExportHeaders(),
                ElementsAre(Pair("Authorization", ElementsAre("Bearer tok"))));

    // BSON allows empty field names; the parameter must reject them.
    EXPECT_THAT(setHeaders(BSON("" << "value")),
                StatusIs(ErrorCodes::BadValue, HasSubstr("empty key")));
    // BSON allows duplicate field names; the parameter must reject them.
    EXPECT_THAT(setHeaders(BSON("X-Same" << "first" << "X-Same" << "second")),
                StatusIs(ErrorCodes::BadValue, HasSubstr("duplicate key")));

    // Backing store must be unchanged after a failed set().
    EXPECT_THAT(getTracingHttpExportHeaders(),
                ElementsAre(Pair("Authorization", ElementsAre("Bearer tok"))));
}

class ResourceAttributesTest : public unittest::Test {
public:
    OpenTelemetryTracingResourceAttributes attrs{"openTelemetryTracingResourceAttributes",
                                                 ServerParameterType::kStartupOnly};

    Status setAttrs(BSONObj doc) {
        auto storage = BSON("v" << doc);
        return attrs.set(storage.firstElement(), /*tenantId=*/boost::none);
    }

    Status setAttrs(int v) {
        auto storage = BSON("v" << v);
        return attrs.set(storage.firstElement(), /*tenantId=*/boost::none);
    }

private:
    unittest::ServerParameterGuard _attrsController{"openTelemetryTracingResourceAttributes",
                                                    BSONObj{}};
};

TEST_F(ResourceAttributesTest, EmptyDocumentClearsAttributes) {
    ASSERT_OK(setAttrs(BSON("env" << "prod")));
    EXPECT_THAT(getTracingResourceAttributes(), SizeIs(1u));

    ASSERT_OK(setAttrs(BSONObj{}));
    EXPECT_THAT(getTracingResourceAttributes(), IsEmpty());
}

TEST_F(ResourceAttributesTest, SecondSetReplacesFirstCompletely) {
    ASSERT_OK(setAttrs(BSON("old.key" << "old")));
    ASSERT_OK(setAttrs(BSON("new.key" << "new")));

    const auto& result = getTracingResourceAttributes();
    EXPECT_THAT(result, ElementsAre(Pair("new.key", "new")));
}

TEST_F(ResourceAttributesTest, NonObjectElementFails) {
    EXPECT_THAT(setAttrs(99), Not(StatusIsOK()));
}

TEST_F(ResourceAttributesTest, NonStringValueFails) {
    EXPECT_THAT(setAttrs(BSON("service.version" << 3)), Not(StatusIsOK()));

    // Backing store must be unchanged after a failed set().
    EXPECT_THAT(getTracingResourceAttributes(), IsEmpty());
}

TEST_F(ResourceAttributesTest, DuplicateKeyFails) {
    // BSON allows duplicate field names; the parameter must reject them.
    EXPECT_THAT(setAttrs(BSON("env" << "prod" << "env" << "staging")), Not(StatusIsOK()));

    // Backing store must be unchanged after a failed set().
    EXPECT_THAT(getTracingResourceAttributes(), IsEmpty());
}

TEST_F(ResourceAttributesTest, SetFromStringParsesJson) {
    ASSERT_OK(attrs.setFromString("{\"env\": \"prod\"}", /*tenantId=*/boost::none));
    EXPECT_THAT(getTracingResourceAttributes(), ElementsAre(Pair("env", "prod")));
}

TEST_F(ResourceAttributesTest, SetFromStringRejectsInvalidJson) {
    EXPECT_THAT(attrs.setFromString("{\"BadBson\":", /*tenantId=*/boost::none),
                StatusIs(ErrorCodes::BadValue, HasSubstr("convert string to BSON")));
}

TEST_F(ResourceAttributesTest, SetFromStringRejectsNonStringValues) {
    EXPECT_THAT(attrs.setFromString("{\"service.version\": 3}", /*tenantId=*/boost::none),
                Not(StatusIsOK()));
}

TEST_F(ResourceAttributesTest, AppendRoundTrip) {
    ASSERT_OK(
        setAttrs(BSON("deployment.environment" << "staging" << "service.version" << "2.0.0")));

    BSONObjBuilder out;
    attrs.append(nullptr, &out, "attrs"sv, /*tenantId=*/boost::none);
    BSONObj result = out.obj();

    EXPECT_THAT(result["attrs"],
                IsBSONElement("attrs"sv,
                              BSONType::object,
                              Matcher<BSONObj>(BSONObjUnorderedEQ(BSON("deployment.environment"
                                                                       << "staging"
                                                                       << "service.version"
                                                                       << "2.0.0")))));
}

}  // namespace
}  // namespace mongo::otel::traces
