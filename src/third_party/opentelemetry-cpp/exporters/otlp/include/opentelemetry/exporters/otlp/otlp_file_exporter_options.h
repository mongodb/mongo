// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "opentelemetry/version.h"

#include "opentelemetry/exporters/otlp/otlp_file_client_options.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace exporter
{
namespace otlp
{

/**
 * Struct to hold OTLP File traces exporter options.
 *
 * See
 * https://github.com/open-telemetry/opentelemetry-specification/blob/main/specification/protocol/file-exporter.md
 */
struct OPENTELEMETRY_EXPORT OtlpFileExporterOptions : public OtlpFileClientOptions
{
  OtlpFileExporterOptions();
  OtlpFileExporterOptions(const OtlpFileExporterOptions &)            = default;
  OtlpFileExporterOptions(OtlpFileExporterOptions &&)                 = default;
  OtlpFileExporterOptions &operator=(const OtlpFileExporterOptions &) = default;
  OtlpFileExporterOptions &operator=(OtlpFileExporterOptions &&)      = default;
  ~OtlpFileExporterOptions() override;
};

}  // namespace otlp
}  // namespace exporter
OPENTELEMETRY_END_NAMESPACE
