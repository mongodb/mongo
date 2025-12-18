// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "opentelemetry/version.h"

#include "opentelemetry/exporters/otlp/otlp_file_client_runtime_options.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace exporter
{
namespace otlp
{

/**
 * Struct to hold OTLP File traces exporter runtime options.
 */
struct OPENTELEMETRY_EXPORT OtlpFileExporterRuntimeOptions : public OtlpFileClientRuntimeOptions
{
  OtlpFileExporterRuntimeOptions() = default;
};

}  // namespace otlp
}  // namespace exporter
OPENTELEMETRY_END_NAMESPACE
