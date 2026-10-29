// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/otel/metrics/otel_metric_name_validation.h"

#include "mongo/otel/metrics/metric_names.h"
#include "mongo/unittest/unittest.h"

#include <string>

namespace mongo::otel::metrics {
namespace {

using testing::Not;
using unittest::match::StatusIsOK;

TEST(OtelMetricNameValidation, RejectsEmpty) {
    ASSERT_NOT_OK(validateOtelMetricName({}));
}

TEST(OtelMetricNameValidation, RejectsLeadingOrTrailingDot) {
    ASSERT_NOT_OK(validateOtelMetricName(".network.open_connections"));
    ASSERT_NOT_OK(validateOtelMetricName("network.open_connections."));
}

TEST(OtelMetricNameValidation, RejectsDoubleDot) {
    ASSERT_NOT_OK(validateOtelMetricName("network..open_connections"));
}

TEST(OtelMetricNameValidation, AcceptsSingleSegment) {
    ASSERT_OK(validateOtelMetricName("network"));
    ASSERT_OK(validateOtelMetricName("ingress"));
}

TEST(OtelMetricNameValidation, AcceptsMultipleSegments) {
    ASSERT_OK(validateOtelMetricName("network.connections"));
    ASSERT_OK(validateOtelMetricName("network.ingress.connections"));
}

TEST(OtelMetricNameValidation, AcceptsSnakeCaseSegments) {
    ASSERT_OK(validateOtelMetricName("network.open_connections"));
    ASSERT_OK(validateOtelMetricName("ingress_network.open_connections"));
    ASSERT_OK(validateOtelMetricName("network.open_ingress_connections"));
}

TEST(OtelMetricNameValidation, AcceptsRegistryMetricNameSamples) {
    ASSERT_OK(validateOtelMetricName(MetricNames::kConnectionsCurrent.getName()));
    ASSERT_OK(validateOtelMetricName(MetricNames::kTest1.getName()));
    ASSERT_OK(validateOtelMetricName(MetricNames::kPrometheusFileExporterWrites.getName()));
}

// camelCase interior letters are allowed to support legacy serverStatus-mirroring names
// (e.g. serverStatus.network.numRequests). Prefer lowercase_snake_case for new metrics.
TEST(OtelMetricNameValidation, AcceptsCamelCaseSegments) {
    ASSERT_OK(validateOtelMetricName("network.openConnections"));
    ASSERT_OK(validateOtelMetricName("serverStatus.network.numRequests"));
    ASSERT_OK(validateOtelMetricName(
        "serverStatus.network.ingressRequestRateLimiter.rejectedAdmissions"));
}

// Leading uppercase is still rejected (segments must start with a lowercase letter).
TEST(OtelMetricNameValidation, RejectsLeadingUppercaseInSegment) {
    ASSERT_NOT_OK(validateOtelMetricName("network.Open_connections"));
    ASSERT_NOT_OK(validateOtelMetricName("Network.open_connections"));
}

TEST(OtelMetricNameValidation, RejectsMixedCamelAndSnakeCaseSegments) {
    ASSERT_NOT_OK(validateOtelMetricName("network.openConnections_"));
    ASSERT_NOT_OK(validateOtelMetricName("network.open_Connections"));
}

TEST(OtelMetricNameValidation, RejectsLeadingDigitInSegment) {
    ASSERT_NOT_OK(validateOtelMetricName("network.2connections"));
    ASSERT_NOT_OK(validateOtelMetricName("2network.connections"));
    ASSERT_NOT_OK(validateOtelMetricName("2network"));
}

TEST(OtelMetricNameValidation, RejectsSpace) {
    ASSERT_NOT_OK(validateOtelMetricName("network.open_ connections"));
}

// Interior hyphens are allowed to support serverStatus
// names (e.g. serverStatus.wiredTiger.data-handle....).
// A hyphen must be followed by at least one alphanumeric character.
TEST(OtelMetricNameValidation, AcceptsInteriorHyphenInSegment) {
    ASSERT_OK(validateOtelMetricName("network.open-connections"));
    ASSERT_OK(validateOtelMetricName("serverStatus.wiredTiger.data-handle.count"));
    ASSERT_OK(validateOtelMetricName("network.open-ingress-connections"));
}

TEST(OtelMetricNameValidation, RejectsLeadingTrailingOrConsecutiveHyphens) {
    ASSERT_NOT_OK(validateOtelMetricName("network.-connections"));
    ASSERT_NOT_OK(validateOtelMetricName("network.connections-"));
    ASSERT_NOT_OK(validateOtelMetricName("network.open--connections"));
}

TEST(OtelMetricNameValidation, RejectsPunctuationInSegment) {
    ASSERT_NOT_OK(validateOtelMetricName("network.open!connections"));
}

TEST(OtelMetricNameValidation, RejectsMultipleUnderscores) {
    EXPECT_THAT(validateOtelMetricName("x_____________y"), Not(StatusIsOK()));
    EXPECT_THAT(validateOtelMetricName("a.k.k___fish.b.c.d"), Not(StatusIsOK()));
}
TEST(OtelMetricNameValidation, rejectsTrailingUnderscore) {
    EXPECT_THAT(validateOtelMetricName("x_"), Not(StatusIsOK()));
    EXPECT_THAT(validateOtelMetricName("a.k.k_.b"), Not(StatusIsOK()));
}

TEST(OtelMetricNameValidation, RejectsNonAscii) {
    ASSERT_NOT_OK(validateOtelMetricName("network.\xffopen"));
}

TEST(OtelMetricNameValidation, AcceptsMaxLengthTwoSegments) {
    std::string first(127, 'f');
    std::string second(127, 'o');
    const std::string s = first + '.' + second;
    ASSERT_EQ(s.size(), 255u);
    ASSERT_OK(validateOtelMetricName(s));
}

TEST(OtelMetricNameValidation, AcceptsMaxLengthSingleSegment) {
    std::string s(255, 'a');
    ASSERT_EQ(s.size(), 255u);
    ASSERT_OK(validateOtelMetricName(s));
}

TEST(OtelMetricNameValidation, RejectsOverlongTwoSegments) {
    std::string first(128, 'f');
    std::string second(127, 'o');
    const std::string s = first + '.' + second;
    ASSERT_EQ(s.size(), 256u);
    ASSERT_NOT_OK(validateOtelMetricName(s));
}

TEST(OtelMetricNameValidation, RejectsOverlongSingleSegment) {
    std::string s(256, 'a');
    ASSERT_EQ(s.size(), 256u);
    ASSERT_NOT_OK(validateOtelMetricName(s));
}

}  // namespace
}  // namespace mongo::otel::metrics
