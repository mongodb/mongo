// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <memory>

#include "opentelemetry/exporters/otlp/otlp_grpc_exporter_options.h"
#include "opentelemetry/nostd/shared_ptr.h"
#include "opentelemetry/sdk/trace/exporter.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace exporter
{
namespace otlp
{

class OtlpGrpcClient;

/**
 * Factory class for OtlpGrpcExporter.
 */
class OPENTELEMETRY_EXPORT OtlpGrpcExporterFactory
{
public:
  /**
   * Create an OtlpGrpcExporter using all default options.
   */
  static std::unique_ptr<opentelemetry::sdk::trace::SpanExporter> Create();

  /**
   * Create an OtlpGrpcExporter using the given options.
   */
  static std::unique_ptr<opentelemetry::sdk::trace::SpanExporter> Create(
      const OtlpGrpcExporterOptions &options);

  /**
   * Create an OtlpGrpcExporter using the given options and gRPC client.
   */
  static std::unique_ptr<opentelemetry::sdk::trace::SpanExporter> Create(
      const OtlpGrpcExporterOptions &options,
      const std::shared_ptr<OtlpGrpcClient> &client);
};

}  // namespace otlp
}  // namespace exporter
OPENTELEMETRY_END_NAMESPACE
