// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include <memory>
#include <mutex>

#include "opentelemetry/common/macros.h"
#include "opentelemetry/exporters/otlp/otlp_grpc_client.h"
#include "opentelemetry/exporters/otlp/otlp_grpc_metric_exporter.h"
#include "opentelemetry/exporters/otlp/otlp_metric_utils.h"

#include "opentelemetry/sdk_config.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace exporter
{
namespace otlp
{
// -------------------------------- Constructors --------------------------------

OtlpGrpcMetricExporter::OtlpGrpcMetricExporter()
    : OtlpGrpcMetricExporter(OtlpGrpcMetricExporterOptions())
{}

OtlpGrpcMetricExporter::OtlpGrpcMetricExporter(const OtlpGrpcMetricExporterOptions &options)
    : options_(options),
#ifdef ENABLE_ASYNC_EXPORT
      client_(std::make_shared<OtlpGrpcClient>()),
#endif
      aggregation_temporality_selector_{
          OtlpMetricUtils::ChooseTemporalitySelector(options_.aggregation_temporality)},
      metrics_service_stub_(OtlpGrpcClient::MakeMetricsServiceStub(options))
{}

OtlpGrpcMetricExporter::OtlpGrpcMetricExporter(
    std::unique_ptr<proto::collector::metrics::v1::MetricsService::StubInterface> stub)
    : options_(OtlpGrpcMetricExporterOptions()),
#ifdef ENABLE_ASYNC_EXPORT
      client_(std::make_shared<OtlpGrpcClient>()),
#endif
      aggregation_temporality_selector_{
          OtlpMetricUtils::ChooseTemporalitySelector(options_.aggregation_temporality)},
      metrics_service_stub_(std::move(stub))
{}

// ----------------------------- Exporter methods ------------------------------

sdk::metrics::AggregationTemporality OtlpGrpcMetricExporter::GetAggregationTemporality(
    sdk::metrics::InstrumentType instrument_type) const noexcept
{
  return aggregation_temporality_selector_(instrument_type);
}

opentelemetry::sdk::common::ExportResult OtlpGrpcMetricExporter::Export(
    const opentelemetry::sdk::metrics::ResourceMetrics &data) noexcept
{

  if (isShutdown())
  {
    OTEL_INTERNAL_LOG_ERROR("[OTLP METRICS gRPC] Exporting "
                            << data.scope_metric_data_.size()
                            << " metric(s) failed, exporter is shutdown");
    return sdk::common::ExportResult::kFailure;
  }
  if (data.scope_metric_data_.empty())
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

  proto::collector::metrics::v1::ExportMetricsServiceRequest *request =
      google::protobuf::Arena::Create<proto::collector::metrics::v1::ExportMetricsServiceRequest>(
          arena.get());
  OtlpMetricUtils::PopulateRequest(data, request);

  auto context = OtlpGrpcClient::MakeClientContext(options_);
  proto::collector::metrics::v1::ExportMetricsServiceResponse *response =
      google::protobuf::Arena::Create<proto::collector::metrics::v1::ExportMetricsServiceResponse>(
          arena.get());

#ifdef ENABLE_ASYNC_EXPORT
  if (options_.max_concurrent_requests > 1)
  {
    return client_->DelegateAsyncExport(
        options_, metrics_service_stub_.get(), std::move(context), std::move(arena),
        std::move(*request),
        [](opentelemetry::sdk::common::ExportResult result,
           std::unique_ptr<google::protobuf::Arena> &&,
           const proto::collector::metrics::v1::ExportMetricsServiceRequest &request,
           proto::collector::metrics::v1::ExportMetricsServiceResponse *) {
          if (result != opentelemetry::sdk::common::ExportResult::kSuccess)
          {
            OTEL_INTERNAL_LOG_ERROR("[OTLP METRIC GRPC Exporter] ERROR: Export "
                                    << request.resource_metrics_size()
                                    << " metric(s) error: " << static_cast<int>(result));
          }
          else
          {
            OTEL_INTERNAL_LOG_DEBUG("[OTLP METRIC GRPC Exporter] Export "
                                    << request.resource_metrics_size() << " metric(s) success");
          }
          return true;
        });
  }
  else
  {
#endif
    grpc::Status status =
        OtlpGrpcClient::DelegateExport(metrics_service_stub_.get(), std::move(context),
                                       std::move(arena), std::move(*request), response);

    if (!status.ok())
    {
      OTEL_INTERNAL_LOG_ERROR(
          "[OTLP METRIC GRPC Exporter] Export() failed: " << status.error_message());
      return sdk::common::ExportResult::kFailure;
    }
#ifdef ENABLE_ASYNC_EXPORT
  }
#endif
  return opentelemetry::sdk::common::ExportResult::kSuccess;
}

bool OtlpGrpcMetricExporter::ForceFlush(
    OPENTELEMETRY_MAYBE_UNUSED std::chrono::microseconds timeout) noexcept
{
#ifdef ENABLE_ASYNC_EXPORT
  return client_->ForceFlush(timeout);
#else
  return true;
#endif
}

bool OtlpGrpcMetricExporter::Shutdown(
    OPENTELEMETRY_MAYBE_UNUSED std::chrono::microseconds timeout) noexcept
{
  is_shutdown_ = true;
#ifdef ENABLE_ASYNC_EXPORT
  return client_->Shutdown(timeout);
#else
  return true;
#endif
}

bool OtlpGrpcMetricExporter::isShutdown() const noexcept
{
  return is_shutdown_;
}

}  // namespace otlp
}  // namespace exporter
OPENTELEMETRY_END_NAMESPACE
