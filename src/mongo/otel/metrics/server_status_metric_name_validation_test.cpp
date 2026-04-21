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
