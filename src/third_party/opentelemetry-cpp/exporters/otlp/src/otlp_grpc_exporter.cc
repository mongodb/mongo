// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include <memory>
#include <mutex>

#include "opentelemetry/common/macros.h"
#include "opentelemetry/exporters/otlp/otlp_grpc_exporter.h"

#include "opentelemetry/exporters/otlp/otlp_grpc_client.h"
#include "opentelemetry/exporters/otlp/otlp_recordable.h"
#include "opentelemetry/exporters/otlp/otlp_recordable_utils.h"
#include "opentelemetry/sdk_config.h"

#include "opentelemetry/exporters/otlp/otlp_grpc_utils.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace exporter
{
namespace otlp
{
// -------------------------------- Constructors --------------------------------

OtlpGrpcExporter::OtlpGrpcExporter() : OtlpGrpcExporter(OtlpGrpcExporterOptions()) {}

OtlpGrpcExporter::OtlpGrpcExporter(const OtlpGrpcExporterOptions &options)
    : options_(options),
#ifdef ENABLE_ASYNC_EXPORT
      client_(std::make_shared<OtlpGrpcClient>()),
#endif
      trace_service_stub_(OtlpGrpcClient::MakeTraceServiceStub(options))
{}

OtlpGrpcExporter::OtlpGrpcExporter(
    std::unique_ptr<proto::collector::trace::v1::TraceService::StubInterface> stub)
    : options_(OtlpGrpcExporterOptions()),
#ifdef ENABLE_ASYNC_EXPORT
      client_(std::make_shared<OtlpGrpcClient>()),
#endif
      trace_service_stub_(std::move(stub))
{}

// ----------------------------- Exporter methods ------------------------------

std::unique_ptr<sdk::trace::Recordable> OtlpGrpcExporter::MakeRecordable() noexcept
{
  return std::unique_ptr<sdk::trace::Recordable>(new OtlpRecordable);
}

sdk::common::ExportResult OtlpGrpcExporter::Export(
    const nostd::span<std::unique_ptr<sdk::trace::Recordable>> &spans) noexcept
{
  if (isShutdown())
  {
    OTEL_INTERNAL_LOG_ERROR("[OTLP gRPC] Exporting " << spans.size()
                                                     << " span(s) failed, exporter is shutdown");
    return sdk::common::ExportResult::kFailure;
  }
  if (spans.empty())
  {
    return sdk::common::ExportResult::kSuccess;
  }

  google::protobuf::ArenaOptions arena_options;
  // It's easy to allocate datas larger than 1024 when we populate basic resource and attributes
  arena_options.initial_block_size = 1024;
  // When in batch mode, it's easy to export a large number of spans at once, we can alloc a lager
  // block to reduce memory fragments.
  arena_options.max_block_size = 65536;
  std::unique_ptr<google::protobuf::Arena> arena{new google::protobuf::Arena{arena_options}};

  proto::collector::trace::v1::ExportTraceServiceRequest *request =
      google::protobuf::Arena::Create<proto::collector::trace::v1::ExportTraceServiceRequest>(
          arena.get());
  OtlpRecordableUtils::PopulateRequest(spans, request);

  auto context = OtlpGrpcClient::MakeClientContext(options_);
  proto::collector::trace::v1::ExportTraceServiceResponse *response =
      google::protobuf::Arena::Create<proto::collector::trace::v1::ExportTraceServiceResponse>(
          arena.get());

#ifdef ENABLE_ASYNC_EXPORT
  if (options_.max_concurrent_requests > 1)
  {
    return client_->DelegateAsyncExport(
        options_, trace_service_stub_.get(), std::move(context), std::move(arena),
        std::move(*request),
        [](opentelemetry::sdk::common::ExportResult result,
           std::unique_ptr<google::protobuf::Arena> &&,
           const proto::collector::trace::v1::ExportTraceServiceRequest &request,
           proto::collector::trace::v1::ExportTraceServiceResponse *) {
          if (result != opentelemetry::sdk::common::ExportResult::kSuccess)
          {
            OTEL_INTERNAL_LOG_ERROR("[OTLP TRACE GRPC Exporter] ERROR: Export "
                                    << request.resource_spans_size()
                                    << " trace span(s) error: " << static_cast<int>(result));
          }
          else
          {
            OTEL_INTERNAL_LOG_DEBUG("[OTLP TRACE GRPC Exporter] Export "
                                    << request.resource_spans_size() << " trace span(s) success");
          }
          return true;
        });
  }
  else
  {
#endif
    grpc::Status status =
        OtlpGrpcClient::DelegateExport(trace_service_stub_.get(), std::move(context),
                                       std::move(arena), std::move(*request), response);
    if (!status.ok())
    {
      OTEL_INTERNAL_LOG_ERROR("[OTLP TRACE GRPC Exporter] Export() failed with status_code: \""
                              << grpc_utils::grpc_status_code_to_string(status.error_code())
                              << "\" error_message: \"" << status.error_message() << "\"");
      return sdk::common::ExportResult::kFailure;
    }
#ifdef ENABLE_ASYNC_EXPORT
  }
#endif
  return sdk::common::ExportResult::kSuccess;
}

bool OtlpGrpcExporter::ForceFlush(
    OPENTELEMETRY_MAYBE_UNUSED std::chrono::microseconds timeout) noexcept
{
#ifdef ENABLE_ASYNC_EXPORT
  return client_->ForceFlush(timeout);
#else
  return true;
#endif
}

bool OtlpGrpcExporter::Shutdown(
    OPENTELEMETRY_MAYBE_UNUSED std::chrono::microseconds timeout) noexcept
{
  is_shutdown_ = true;
#ifdef ENABLE_ASYNC_EXPORT
  return client_->Shutdown(timeout);
#else
  return true;
#endif
}

bool OtlpGrpcExporter::isShutdown() const noexcept
{
  return is_shutdown_;
}

}  // namespace otlp
}  // namespace exporter
OPENTELEMETRY_END_NAMESPACE
