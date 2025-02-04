/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <smithy/tracing/TracingUtils.h>

using namespace smithy::components::tracing;


const char TracingUtils::COUNT_METRIC_TYPE[] = "Count";
const char TracingUtils::MICROSECOND_METRIC_TYPE[] = "Microseconds";
const char TracingUtils::BYTES_PER_SECOND_METRIC_TYPE[] = "Bytes/Second";
const char TracingUtils::SMITHY_CLIENT_DURATION_METRIC[] = "smithy.client.duration";
const char TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC[] = "smithy.client.resolve_endpoint_duration";
const char TracingUtils::SMITHY_CLIENT_DESERIALIZATION_METRIC[] = "smithy.client.deserialization_duration";
const char TracingUtils::SMITHY_CLIENT_SERIALIZATION_METRIC[] = "smithy.client.serialization_duration";
const char TracingUtils::SMITHY_CLIENT_SERVICE_CALL_METRIC[] = "smithy.client.service_call_duration";
const char TracingUtils::SMITHY_CLIENT_SIGNING_METRIC[] = "smithy.client.auth.signing_duration";
const char TracingUtils::SMITHY_CLIENT_SERVICE_BACKOFF_DELAY_METRIC[] = "smithy.client.backoff_delay";
const char TracingUtils::SMITHY_CLIENT_SERVICE_ATTEMPTS_METRIC[] = "smithy.client.attempts";
const char TracingUtils::SMITHY_METHOD_AWS_VALUE[] = "aws-api";
const char TracingUtils::SMITHY_SERVICE_DIMENSION[] = "rpc.service";
const char TracingUtils::SMITHY_METHOD_DIMENSION[] = "rpc.method";
const char TracingUtils::SMITHY_SYSTEM_DIMENSION[] = "rpc.system";
const char TracingUtils::SMITHY_METRICS_DNS_DURATION[] = "smithy.client.http.dns_duration";
const char TracingUtils::SMITHY_METRICS_CONNECT_DURATION[] = "smithy.client.http.connect_duration";
const char TracingUtils::SMITHY_METRICS_SSL_DURATION[] = "smithy.client.http.ssl_duration";
const char TracingUtils::SMITHY_METRICS_DOWNLOAD_SPEED_METRIC[] = "smithy.client.http.download_speed";
const char TracingUtils::SMITHY_METRICS_UPLOAD_SPEED_METRIC[] = "smithy.client.http.upload_speed";
const char TracingUtils::SMITHY_METRICS_UNKNOWN_METRIC[] = "smithy.client.http.unknown_metric";