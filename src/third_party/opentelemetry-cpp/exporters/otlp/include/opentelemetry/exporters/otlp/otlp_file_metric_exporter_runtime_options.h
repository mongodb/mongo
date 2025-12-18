// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "opentelemetry/exporters/otlp/otlp_file_client_runtime_options.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace exporter
{
namespace otlp
{

/**
 * Struct to hold OTLP File metrics exporter runtime options.
 */
struct OPENTELEMETRY_EXPORT OtlpFileMetricExporterRuntimeOptions
    : public OtlpFileClientRuntimeOptions
{
  OtlpFileMetricExporterRuntimeOptions() = default;
};

}  // namespace otlp
}  // namespace exporter
OPENTELEMETRY_END_NAMESPACE
