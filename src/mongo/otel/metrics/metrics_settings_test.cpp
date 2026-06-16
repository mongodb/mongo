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


#include "mongo/otel/metrics/metrics_settings.h"

#include "mongo/otel/metrics/metrics_settings_gen.h"
#include "mongo/unittest/unittest.h"

namespace mongo::otel::metrics {
namespace {

using testing::ElementsAre;
using testing::HasSubstr;
using testing::IsEmpty;
using testing::Pair;
using testing::SizeIs;
using unittest::match::StatusIs;

class OtelMetricsHttpExportHeadersTest : public unittest::Test {
public:
    OpenTelemetryMetricsHttpExportHeaders headers{"openTelemetryMetricsHttpExportHeaders",
                                                  ServerParameterType::kStartupOnly};

    void setUp() override {
        ASSERT_OK(setHeaders(BSONObj{}));
    }

    void tearDown() override {
        ASSERT_OK(setHeaders(BSONObj{}));
    }

    Status setHeaders(BSONObj doc) {
        auto storage = BSON("v" << doc);
        return headers.set(storage.firstElement(), boost::none);
    }
};

TEST_F(OtelMetricsHttpExportHeadersTest, HttpExportHeadersSetFromBson) {
    // Verify headers can be set and are stored
    ASSERT_OK(setHeaders(BSON("Authorization" << "Bearer tok")));
    EXPECT_THAT(getMetricsHttpExportHeaders(), SizeIs(1));

    // Verify headers can be updated
    ASSERT_OK(setHeaders(
        BSON("Authorization" << "Bearer tok" << "X-Tenant-ID" << "acme" << "X-Empty-Value" << "")));
    EXPECT_THAT(getMetricsHttpExportHeaders(), SizeIs(3));

    // Verify that an empty object causes headers to be cleared
    ASSERT_OK(setHeaders(BSONObj{}));
    EXPECT_THAT(getMetricsHttpExportHeaders(), IsEmpty());
}

TEST_F(OtelMetricsHttpExportHeadersTest, HttpExportHeadersSetFromString) {
    // Verify setFromString behavior
    ASSERT_OK(headers.setFromString("{\"Authorization\": \"Bearer tok\"}", boost::none));
    EXPECT_THAT(getMetricsHttpExportHeaders(), SizeIs(1));
    EXPECT_THAT(getMetricsHttpExportHeaders(),
                ElementsAre(Pair("Authorization", ElementsAre("Bearer tok"))));

    EXPECT_THAT(headers.setFromString("{\"BadBson\":", boost::none),
                StatusIs(ErrorCodes::BadValue, HasSubstr("convert string to BSON")));
}

TEST_F(OtelMetricsHttpExportHeadersTest, HttpExportHeadersFailurePreservesPreviousValue) {
    ASSERT_OK(setHeaders(BSON("Authorization" << "Bearer tok")));
    EXPECT_THAT(getMetricsHttpExportHeaders(),
                ElementsAre(Pair("Authorization", ElementsAre("Bearer tok"))));

    // BSON allows empty field names; the parameter must reject them.
    EXPECT_THAT(setHeaders(BSON("" << "value")),
                StatusIs(ErrorCodes::BadValue, HasSubstr("empty key")));
    // BSON allows duplicate field names; the parameter must reject them.
    EXPECT_THAT(setHeaders(BSON("X-Same" << "first" << "X-Same" << "second")),
                StatusIs(ErrorCodes::BadValue, HasSubstr("duplicate key")));

    // Backing store must be unchanged after a failed set().
    EXPECT_THAT(getMetricsHttpExportHeaders(),
                ElementsAre(Pair("Authorization", ElementsAre("Bearer tok"))));
}
}  // namespace
}  // namespace mongo::otel::metrics
