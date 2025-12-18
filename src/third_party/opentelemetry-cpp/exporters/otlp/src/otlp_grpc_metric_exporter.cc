// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include <grpcpp/client_context.h>
#include <grpcpp/support/status.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <new>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "opentelemetry/exporters/otlp/otlp_grpc_client.h"
#include "opentelemetry/exporters/otlp/otlp_grpc_client_factory.h"
#include "opentelemetry/exporters/otlp/otlp_grpc_metric_exporter.h"
#include "opentelemetry/exporters/otlp/otlp_grpc_metric_exporter_options.h"
#include "opentelemetry/exporters/otlp/otlp_metric_utils.h"
#include "opentelemetry/sdk/common/exporter_utils.h"
#include "opentelemetry/sdk/common/global_log_handler.h"
#include "opentelemetry/sdk/metrics/export/metric_producer.h"
#include "opentelemetry/sdk/metrics/instruments.h"
#include "opentelemetry/version.h"

// clang-format off
#include "opentelemetry/exporters/otlp/protobuf_include_prefix.h" // IWYU pragma: keep
#include "google/protobuf/arena.h"
#include "opentelemetry/proto/collector/metrics/v1/metrics_service.grpc.pb.h"
#include "opentelemetry/proto/collector/metrics/v1/metrics_service.pb.h"
#include "opentelemetry/exporters/otlp/protobuf_include_suffix.h" // IWYU pragma: keep
// clang-format on

#ifdef ENABLE_ASYNC_EXPORT
#  include <functional>
#endif

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
      aggregation_temporality_selector_{
          OtlpMetricUtils::ChooseTemporalitySelector(options_.aggregation_temporality)}
{
  client_                 = OtlpGrpcClientFactory::Create(options_);
  client_reference_guard_ = OtlpGrpcClientFactory::CreateReferenceGuard();
  client_->AddReference(*client_reference_guard_, options_);

  metrics_service_stub_ = client_->MakeMetricsServiceStub();
}

OtlpGrpcMetricExporter::OtlpGrpcMetricExporter(
    std::unique_ptr<proto::collector::metrics::v1::MetricsService::StubInterface> stub)
    : options_(OtlpGrpcMetricExporterOptions()),
      aggregation_temporality_selector_{
          OtlpMetricUtils::ChooseTemporalitySelector(options_.aggregation_temporality)},
      metrics_service_stub_(std::move(stub))
{
  client_                 = OtlpGrpcClientFactory::Create(options_);
  client_reference_guard_ = OtlpGrpcClientFactory::CreateReferenceGuard();
  client_->AddReference(*client_reference_guard_, options_);
}

OtlpGrpcMetricExporter::OtlpGrpcMetricExporter(const OtlpGrpcMetricExporterOptions &options,
                                               const std::shared_ptr<OtlpGrpcClient> &client)
    : options_(options),
      client_(client),
      client_reference_guard_(OtlpGrpcClientFactory::CreateReferenceGuard()),
      aggregation_temporality_selector_{
          OtlpMetricUtils::ChooseTemporalitySelector(options_.aggregation_temporality)}
{
  client_->AddReference(*client_reference_guard_, options_);

  metrics_service_stub_ = client_->MakeMetricsServiceStub();
}

OtlpGrpcMetricExporter::OtlpGrpcMetricExporter(
    std::unique_ptr<proto::collector::metrics::v1::MetricsService::StubInterface> stub,
    const std::shared_ptr<OtlpGrpcClient> &client)
    : options_(OtlpGrpcMetricExporterOptions()),
      client_(client),
      client_reference_guard_(OtlpGrpcClientFactory::CreateReferenceGuard()),
      aggregation_temporality_selector_{
          OtlpMetricUtils::ChooseTemporalitySelector(options_.aggregation_temporality)},
      metrics_service_stub_(std::move(stub))
{
  client_->AddReference(*client_reference_guard_, options_);
}

OtlpGrpcMetricExporter::~OtlpGrpcMetricExporter()
{
  if (client_)
  {
    client_->RemoveReference(*client_reference_guard_);
  }
}

// ----------------------------- Exporter methods ------------------------------

sdk::metrics::AggregationTemporality OtlpGrpcMetricExporter::GetAggregationTemporality(
    sdk::metrics::InstrumentType instrument_type) const noexcept
{
  return aggregation_temporality_selector_(instrument_type);
}

opentelemetry::sdk::common::ExportResult OtlpGrpcMetricExporter::Export(
    const opentelemetry::sdk::metrics::ResourceMetrics &data) noexcept
{
  std::shared_ptr<OtlpGrpcClient> client = client_;
  if (isShutdown() || !client)
  {
    OTEL_INTERNAL_LOG_ERROR("[OTLP METRICS gRPC] Exporting "
                            << data.scope_metric_data_.size()
                            << " metric(s) failed, exporter is shutdown");
    return sdk::common::ExportResult::kFailure;
  }

  if (!metrics_service_stub_)
  {
    OTEL_INTERNAL_LOG_ERROR("[OTLP gRPC] Exporting "
                            << data.scope_metric_data_.size()
                            << " metric(s) failed, service stub unavailable");
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
    return client->DelegateAsyncExport(
        options_, metrics_service_stub_.get(), std::move(context), std::move(arena),
        std::move(*request),
        // Capture the metrics_service_stub_ to ensure it is not destroyed before the callback is
        // called.
        [metrics_service_stub = metrics_service_stub_](
            opentelemetry::sdk::common::ExportResult result,
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
  // Maybe already shutdown, we need to keep thread-safety here.
  std::shared_ptr<OtlpGrpcClient> client = client_;
  if (!client)
  {
    return true;
  }
  return client->ForceFlush(timeout);
}

bool OtlpGrpcMetricExporter::Shutdown(
    OPENTELEMETRY_MAYBE_UNUSED std::chrono::microseconds timeout) noexcept
{
  is_shutdown_ = true;
  // Maybe already shutdown, we need to keep thread-safety here.
  std::shared_ptr<OtlpGrpcClient> client;
  client.swap(client_);
  if (!client)
  {
    return true;
  }
  return client->Shutdown(*client_reference_guard_, timeout);
}

bool OtlpGrpcMetricExporter::isShutdown() const noexcept
{
  return is_shutdown_;
}

const std::shared_ptr<OtlpGrpcClient> &OtlpGrpcMetricExporter::GetClient() const noexcept
{
  return client_;
}

}  // namespace otlp
}  // namespace exporter
OPENTELEMETRY_END_NAMESPACE
