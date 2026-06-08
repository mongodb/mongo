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

#include "mongo/otel/traces/trace_settings.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/otel/traces/trace_settings_gen.h"
#include "mongo/unittest/unittest.h"

#include <boost/optional.hpp>
#include <gmock/gmock.h>

namespace mongo::otel::traces {
namespace {

using mongo::unittest::match::BSONObjUnorderedEQ;
using mongo::unittest::match::IsBSONElement;
using mongo::unittest::match::StatusIsOK;
using ::testing::ElementsAre;
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

    Status setHeaders(int v) {
        auto storage = BSON("v" << v);
        return headers.set(storage.firstElement(), /*tenantId=*/boost::none);
    }
};

TEST_F(HttpHeadersTest, EmptyDocumentClearsHeaders) {
    // First populate headers, then set empty doc to verify replacement.
    ASSERT_OK(setHeaders(BSON("X-Foo" << "bar")));
    EXPECT_THAT(getTracingHttpExportHeaders(), SizeIs(1u));

    ASSERT_OK(setHeaders(BSONObj{}));
    EXPECT_THAT(getTracingHttpExportHeaders(), IsEmpty());
}

TEST_F(HttpHeadersTest, SecondSetReplacesFirstCompletely) {
    ASSERT_OK(setHeaders(BSON("X-First" << "one")));
    ASSERT_OK(setHeaders(BSON("X-Second" << "two")));

    const auto& result = getTracingHttpExportHeaders();
    EXPECT_THAT(result, ElementsAre(Pair("X-Second", ElementsAre("two"))));
}

TEST_F(HttpHeadersTest, NonObjectElementFails) {
    EXPECT_THAT(setHeaders(42), Not(StatusIsOK()));
}

TEST_F(HttpHeadersTest, NonStringValueFails) {
    EXPECT_THAT(setHeaders(BSON("X-Foo" << 123)), Not(StatusIsOK()));

    // Backing store must be unchanged after a failed set().
    EXPECT_THAT(getTracingHttpExportHeaders(), IsEmpty());
}

TEST_F(HttpHeadersTest, ArrayValue) {
    ASSERT_OK(setHeaders(BSON("X-Multi" << BSON_ARRAY("first" << "second"))));

    EXPECT_THAT(getTracingHttpExportHeaders(),
                ElementsAre(Pair("X-Multi", ElementsAre("first", "second"))));
}

TEST_F(HttpHeadersTest, ArrayWithNonStringElementFails) {
    EXPECT_THAT(setHeaders(BSON("X-Bad" << BSON_ARRAY("good" << 42))), Not(StatusIsOK()));

    // Backing store must be unchanged after a failed set().
    EXPECT_THAT(getTracingHttpExportHeaders(), IsEmpty());
}

TEST_F(HttpHeadersTest, DuplicateKeyFails) {
    // BSON allows duplicate field names; the parameter must reject them.
    EXPECT_THAT(setHeaders(BSON("X-Foo" << "first" << "X-Foo" << "second")), Not(StatusIsOK()));

    // Backing store must be unchanged after a failed set().
    EXPECT_THAT(getTracingHttpExportHeaders(), IsEmpty());
}

TEST_F(HttpHeadersTest, SetFromStringFails) {
    EXPECT_THAT(headers.setFromString("{\"X-Foo\": \"bar\"}", /*tenantId=*/boost::none),
                Not(StatusIsOK()));
}

TEST_F(HttpHeadersTest, AppendRedacts) {
    // openTelemetryTracingHttpExportHeaders has redact:true, so append always outputs "###".
    ASSERT_OK(setHeaders(BSON("Authorization" << "Bearer tok")));

    BSONObjBuilder out;
    headers.append(nullptr, &out, "headers"_sd, /*tenantId=*/boost::none);
    EXPECT_EQ(out.obj()["headers"].str(), "###");
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
    RAIIServerParameterControllerForTest _attrsController{"openTelemetryTracingResourceAttributes",
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

TEST_F(ResourceAttributesTest, SetFromStringFails) {
    EXPECT_THAT(attrs.setFromString("{\"env\": \"prod\"}", /*tenantId=*/boost::none),
                Not(StatusIsOK()));
}

TEST_F(ResourceAttributesTest, AppendRoundTrip) {
    ASSERT_OK(
        setAttrs(BSON("deployment.environment" << "staging" << "service.version" << "2.0.0")));

    BSONObjBuilder out;
    attrs.append(nullptr, &out, "attrs"_sd, /*tenantId=*/boost::none);
    BSONObj result = out.obj();

    EXPECT_THAT(result["attrs"],
                IsBSONElement("attrs"_sd,
                              BSONType::object,
                              Matcher<BSONObj>(BSONObjUnorderedEQ(BSON("deployment.environment"
                                                                       << "staging"
                                                                       << "service.version"
                                                                       << "2.0.0")))));
}

}  // namespace
}  // namespace mongo::otel::traces
