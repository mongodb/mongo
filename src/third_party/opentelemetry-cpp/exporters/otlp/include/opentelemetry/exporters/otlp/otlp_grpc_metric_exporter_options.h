// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "opentelemetry/exporters/otlp/otlp_grpc_client_options.h"
#include "opentelemetry/exporters/otlp/otlp_preferred_temporality.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace exporter
{
namespace otlp
{

/**
 * Struct to hold OTLP GRPC metrics exporter options.
 *
 * See
 * https://github.com/open-telemetry/opentelemetry-proto/blob/main/docs/specification.md#otlpgrpc
 *
 * See
 * https://github.com/open-telemetry/opentelemetry-specification/blob/main/specification/protocol/exporter.md
 */
struct OPENTELEMETRY_EXPORT OtlpGrpcMetricExporterOptions : public OtlpGrpcClientOptions
{
  /** Lookup environment variables. */
  OtlpGrpcMetricExporterOptions();
  /** No defaults. */
  OtlpGrpcMetricExporterOptions(void *);
  OtlpGrpcMetricExporterOptions(const OtlpGrpcMetricExporterOptions &)            = default;
  OtlpGrpcMetricExporterOptions(OtlpGrpcMetricExporterOptions &&)                 = default;
  OtlpGrpcMetricExporterOptions &operator=(const OtlpGrpcMetricExporterOptions &) = default;
  OtlpGrpcMetricExporterOptions &operator=(OtlpGrpcMetricExporterOptions &&)      = default;
  ~OtlpGrpcMetricExporterOptions() override;

  /** Preferred Aggregation Temporality. */
  PreferredAggregationTemporality aggregation_temporality;
};

}  // namespace otlp
}  // namespace exporter
OPENTELEMETRY_END_NAMESPACE
