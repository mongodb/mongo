// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstddef>
#include <memory>
#include <new>
#include <ostream>

#include "opentelemetry/exporters/otlp/otlp_file_client.h"
#include "opentelemetry/exporters/otlp/otlp_file_client_options.h"
#include "opentelemetry/exporters/otlp/otlp_file_exporter.h"
#include "opentelemetry/exporters/otlp/otlp_file_exporter_options.h"
#include "opentelemetry/exporters/otlp/otlp_file_exporter_runtime_options.h"
#include "opentelemetry/exporters/otlp/otlp_recordable.h"
#include "opentelemetry/exporters/otlp/otlp_recordable_utils.h"
#include "opentelemetry/nostd/span.h"
#include "opentelemetry/sdk/common/exporter_utils.h"
#include "opentelemetry/sdk/common/global_log_handler.h"
#include "opentelemetry/sdk/trace/recordable.h"
#include "opentelemetry/version.h"

// clang-format off
#include "opentelemetry/exporters/otlp/protobuf_include_prefix.h" // IWYU pragma: keep
#include "google/protobuf/arena.h"
#include "opentelemetry/proto/collector/trace/v1/trace_service.pb.h"
#include "opentelemetry/exporters/otlp/protobuf_include_suffix.h" // IWYU pragma: keep
// clang-format on

OPENTELEMETRY_BEGIN_NAMESPACE
namespace exporter
{
namespace otlp
{

OtlpFileExporter::OtlpFileExporter() : OtlpFileExporter(OtlpFileExporterOptions()) {}

OtlpFileExporter::OtlpFileExporter(const OtlpFileExporterOptions &options)
    : OtlpFileExporter(options, OtlpFileExporterRuntimeOptions())
{}

OtlpFileExporter::OtlpFileExporter(const OtlpFileExporterOptions &options,
                                   const OtlpFileExporterRuntimeOptions &runtime_options)
    : options_(options),
      runtime_options_(runtime_options),
      file_client_(new OtlpFileClient(OtlpFileClientOptions(options),
                                      OtlpFileExporterRuntimeOptions(runtime_options)))
{}

// ----------------------------- Exporter methods ------------------------------

std::unique_ptr<opentelemetry::sdk::trace::Recordable> OtlpFileExporter::MakeRecordable() noexcept
{
  return std::unique_ptr<opentelemetry::sdk::trace::Recordable>(
      new exporter::otlp::OtlpRecordable());
}

opentelemetry::sdk::common::ExportResult OtlpFileExporter::Export(
    const nostd::span<std::unique_ptr<opentelemetry::sdk::trace::Recordable>> &spans) noexcept
{
  if (file_client_->IsShutdown())
  {
    std::size_t span_count = spans.size();
    OTEL_INTERNAL_LOG_ERROR("[OTLP TRACE FILE Exporter] ERROR: Export "
                            << span_count << " trace span(s) failed, exporter is shutdown");
    return opentelemetry::sdk::common::ExportResult::kFailure;
  }

  if (spans.empty())
  {
    return opentelemetry::sdk::common::ExportResult::kSuccess;
  }

  google::protobuf::ArenaOptions arena_options;
  // It's easy to allocate datas larger than 1024 when we populate basic resource and attributes
  arena_options.initial_block_size = 1024;
  // When in batch mode, it's easy to export a large number of spans at once, we can alloc a lager
  // block to reduce memory fragments.
  arena_options.max_block_size = 65536;
  google::protobuf::Arena arena{arena_options};

  proto::collector::trace::v1::ExportTraceServiceRequest *service_request =
      google::protobuf::Arena::Create<proto::collector::trace::v1::ExportTraceServiceRequest>(
          &arena);
  OtlpRecordableUtils::PopulateRequest(spans, service_request);
  std::size_t span_count = spans.size();
  opentelemetry::sdk::common::ExportResult result =
      file_client_->Export(*service_request, span_count);
  if (result != opentelemetry::sdk::common::ExportResult::kSuccess)
  {
    OTEL_INTERNAL_LOG_ERROR("[OTLP TRACE FILE Exporter] ERROR: Export "
                            << span_count << " trace span(s) error: " << static_cast<int>(result));
  }
  else
  {
    OTEL_INTERNAL_LOG_DEBUG("[OTLP TRACE FILE Exporter] Export " << span_count
                                                                 << " trace span(s) success");
  }
  return result;
}

bool OtlpFileExporter::ForceFlush(std::chrono::microseconds timeout) noexcept
{
  return file_client_->ForceFlush(timeout);
}

bool OtlpFileExporter::Shutdown(std::chrono::microseconds timeout) noexcept
{
  return file_client_->Shutdown(timeout);
}

}  // namespace otlp
}  // namespace exporter
OPENTELEMETRY_END_NAMESPACE
