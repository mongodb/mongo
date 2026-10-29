// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


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
