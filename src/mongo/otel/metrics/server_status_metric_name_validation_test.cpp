// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/otel/metrics/server_status_metric_name_validation.h"

#include "mongo/unittest/unittest.h"

#include <string>

namespace mongo::otel::metrics {

TEST(ServerStatusMetricNameValidation, RejectsEmpty) {
    ASSERT_NOT_OK(validateServerStatusMetricPath({}));
}

TEST(ServerStatusMetricNameValidation, RejectsLeadingOrTrailingDot) {
    ASSERT_NOT_OK(validateServerStatusMetricPath(".network.openConnections"));
    ASSERT_NOT_OK(validateServerStatusMetricPath("network.openConnections."));
}

TEST(ServerStatusMetricNameValidation, RejectsDoubleDot) {
    ASSERT_NOT_OK(validateServerStatusMetricPath("network..openConnections"));
}

TEST(ServerStatusMetricNameValidation, AcceptsSingleSegment) {
    ASSERT_OK(validateServerStatusMetricPath("network"));
}

TEST(ServerStatusMetricNameValidation, AcceptsMultipleSegments) {
    ASSERT_OK(validateServerStatusMetricPath("network.openConnections"));
    ASSERT_OK(validateServerStatusMetricPath("network.ingress.connections"));
}

TEST(ServerStatusMetricNameValidation, AcceptsCamelCaseSegments) {
    ASSERT_OK(validateServerStatusMetricPath("network.openConnections"));
    ASSERT_OK(validateServerStatusMetricPath("ingressNetwork.connections"));
}

TEST(ServerStatusMetricNameValidation, RejectsSnakeCaseSegments) {
    ASSERT_NOT_OK(validateServerStatusMetricPath("network.open_connections"));
    ASSERT_NOT_OK(validateServerStatusMetricPath("ingress_network.open_connections"));
    ASSERT_NOT_OK(validateServerStatusMetricPath("network.open_ingress_connections"));
}

TEST(ServerStatusMetricNameValidation, RejectsUppercaseLetters) {
    ASSERT_NOT_OK(validateServerStatusMetricPath("network.OpenConnections"));
    ASSERT_NOT_OK(validateServerStatusMetricPath("Network.openConnections"));
}

TEST(ServerStatusMetricNameValidation, RejectsLeadingDigitInSegment) {
    ASSERT_NOT_OK(validateServerStatusMetricPath("network.2connections"));
    ASSERT_NOT_OK(validateServerStatusMetricPath("2network.openConnections"));
    ASSERT_NOT_OK(validateServerStatusMetricPath("2network"));
}

TEST(ServerStatusMetricNameValidation, RejectsSpace) {
    ASSERT_NOT_OK(validateServerStatusMetricPath("network.open Connections"));
}

TEST(ServerStatusMetricNameValidation, RejectsHyphenInSegment) {
    ASSERT_NOT_OK(validateServerStatusMetricPath("network.open-Connections"));
}

TEST(ServerStatusMetricNameValidation, RejectsPunctuationInSegment) {
    ASSERT_NOT_OK(validateServerStatusMetricPath("network.open!Connections"));
}

TEST(ServerStatusMetricNameValidation, RejectsNonAscii) {
    ASSERT_NOT_OK(
        validateServerStatusMetricPath("network.open\xFF"
                                       "Connections"));
}

TEST(ServerStatusMetricNameValidation, RejectsMetricsPrefix) {
    ASSERT_NOT_OK(validateServerStatusMetricPath("metrics"));
    ASSERT_NOT_OK(validateServerStatusMetricPath("metrics.openConnections"));
    ASSERT_NOT_OK(validateServerStatusMetricPath("metrics.network.openConnections"));
    ASSERT_OK(validateServerStatusMetricPath("metricsNetwork"));
    ASSERT_OK(validateServerStatusMetricPath("myMetrics.openConnections"));
}

TEST(ServerStatusMetricNameValidation, AcceptsLongSingleSegment) {
    std::string s = "a";
    s.append(10000, 'b');
    ASSERT_OK(validateServerStatusMetricPath(s));
}

}  // namespace mongo::otel::metrics
