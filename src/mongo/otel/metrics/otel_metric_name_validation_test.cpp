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

#include "mongo/otel/metrics/otel_metric_name_validation.h"

#include "mongo/otel/metrics/metric_names.h"
#include "mongo/unittest/unittest.h"

#include <string>

namespace mongo::otel::metrics {

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
    ASSERT_OK(validateOtelMetricName(MetricNames::kOpenConnections.getName()));
    ASSERT_OK(validateOtelMetricName(MetricNames::kTest1.getName()));
    ASSERT_OK(validateOtelMetricName(MetricNames::kPrometheusFileExporterWrites.getName()));
}

TEST(OtelMetricNameValidation, RejectsCamelCaseSegments) {
    ASSERT_NOT_OK(validateOtelMetricName("network.openConnections"));
    ASSERT_NOT_OK(validateOtelMetricName("ingressNetwork.connections"));
}

TEST(OtelMetricNameValidation, RejectsUppercaseLetters) {
    ASSERT_NOT_OK(validateOtelMetricName("network.Open_connections"));
    ASSERT_NOT_OK(validateOtelMetricName("Network.open_connections"));
}

TEST(OtelMetricNameValidation, RejectsLeadingDigitInSegment) {
    ASSERT_NOT_OK(validateOtelMetricName("network.2connections"));
    ASSERT_NOT_OK(validateOtelMetricName("2network.connections"));
    ASSERT_NOT_OK(validateOtelMetricName("2network"));
}

TEST(OtelMetricNameValidation, RejectsSpace) {
    ASSERT_NOT_OK(validateOtelMetricName("network.open_ connections"));
}

TEST(OtelMetricNameValidation, RejectsHyphenInSegment) {
    ASSERT_NOT_OK(validateOtelMetricName("network.open-connections"));
}

TEST(OtelMetricNameValidation, RejectsPunctuationInSegment) {
    ASSERT_NOT_OK(validateOtelMetricName("network.open!connections"));
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

}  // namespace mongo::otel::metrics
