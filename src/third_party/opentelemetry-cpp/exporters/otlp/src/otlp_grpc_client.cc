// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include "opentelemetry/exporters/otlp/otlp_grpc_client.h"

#include <grpc/compression.h>
#include <grpcpp/resource_quota.h>
#include <grpcpp/security/credentials.h>
#include <grpcpp/support/channel_arguments.h>
#include <stdint.h>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <utility>

#include "opentelemetry/ext/http/common/url_parser.h"
#include "opentelemetry/sdk/common/global_log_handler.h"

// clang-format off
#include "opentelemetry/exporters/otlp/protobuf_include_prefix.h" // IWYU pragma: keep
#ifdef ENABLE_ASYNC_EXPORT
#  include "google/protobuf/arena.h"
#endif /* ENABLE_ASYNC_EXPORT */
#include "opentelemetry/proto/collector/logs/v1/logs_service.pb.h"
#include "opentelemetry/proto/collector/metrics/v1/metrics_service.pb.h"
#include "opentelemetry/proto/collector/trace/v1/trace_service.pb.h"
#include "opentelemetry/exporters/otlp/protobuf_include_suffix.h" // IWYU pragma: keep
// clang-format on

#ifdef ENABLE_ASYNC_EXPORT
#  include <algorithm>
#  include <condition_variable>
#  include <cstdio>
#  include <mutex>
#  include <ratio>
#  include <unordered_set>

#  include "opentelemetry/common/timestamp.h"
#  include "opentelemetry/nostd/string_view.h"
#endif /* ENABLE_ASYNC_EXPORT */

OPENTELEMETRY_BEGIN_NAMESPACE
namespace exporter
{
namespace otlp
{

#ifdef ENABLE_ASYNC_EXPORT

namespace
{
// When building with -fvisibility=default, we hide the symbols and vtable to ensure we always use
// local version of OtlpGrpcAsyncCallDataBase and OtlpGrpcAsyncCallData. It's used to keep
// compatibility when executables links multiple versions of otel-cpp.
class OPENTELEMETRY_LOCAL_SYMBOL OtlpGrpcAsyncCallDataBase
{
public:
  using GrpcAsyncCallback = bool (*)(OtlpGrpcAsyncCallDataBase *);

  std::unique_ptr<google::protobuf::Arena> arena;
  grpc::Status grpc_status;
  std::unique_ptr<grpc::ClientContext> grpc_context;

  opentelemetry::sdk::common::ExportResult export_result =
      opentelemetry::sdk::common::ExportResult::kFailure;
  GrpcAsyncCallback grpc_async_callback = nullptr;

  OtlpGrpcAsyncCallDataBase()                                             = default;
  virtual ~OtlpGrpcAsyncCallDataBase()                                    = default;
  OtlpGrpcAsyncCallDataBase(const OtlpGrpcAsyncCallDataBase &)            = delete;
  OtlpGrpcAsyncCallDataBase &operator=(const OtlpGrpcAsyncCallDataBase &) = delete;
  OtlpGrpcAsyncCallDataBase(OtlpGrpcAsyncCallDataBase &&)                 = delete;
  OtlpGrpcAsyncCallDataBase &operator=(OtlpGrpcAsyncCallDataBase &&)      = delete;
};

// When building with -fvisibility=default, we hide the symbols and vtable to ensure we always use
// local version of OtlpGrpcAsyncCallDataBase and OtlpGrpcAsyncCallData. It's used to keep
// compatibility when executables links multiple versions of otel-cpp.
template <class GrpcRequestType, class GrpcResponseType>
class OPENTELEMETRY_LOCAL_SYMBOL OtlpGrpcAsyncCallData : public OtlpGrpcAsyncCallDataBase
{
public:
  using RequestType  = GrpcRequestType;
  using ResponseType = GrpcResponseType;

  RequestType *request   = nullptr;
  ResponseType *response = nullptr;

  std::function<bool(opentelemetry::sdk::common::ExportResult,
                     std::unique_ptr<google::protobuf::Arena> &&,
                     const RequestType &,
                     ResponseType *)>
      result_callback;

  OtlpGrpcAsyncCallData() = default;
};
}  // namespace
#endif

struct OtlpGrpcClientAsyncData
{

  std::chrono::system_clock::duration export_timeout = std::chrono::seconds{10};

  // The best performance trade-off of gRPC is having numcpu's threads and one completion queue
  // per thread, but this exporter should not cost a lot resource and we don't want to create
  // too many threads in the process. So we use one completion queue and shared context.
  std::shared_ptr<grpc::Channel> channel;
#ifdef ENABLE_ASYNC_EXPORT
  std::mutex running_calls_lock;
  std::unordered_set<std::shared_ptr<OtlpGrpcAsyncCallDataBase>> running_calls;
  // Running requests, this is used to limit the number of concurrent requests.
  std::atomic<std::size_t> running_requests{0};
  // Request counter is used to record ForceFlush.
  std::atomic<std::size_t> start_request_counter{0};
  std::atomic<std::size_t> finished_request_counter{0};
  std::size_t max_concurrent_requests = 64;

  // Condition variable and mutex to control the concurrency count of running requests.
  std::mutex session_waker_lock;
  std::condition_variable session_waker;
#endif

  // Reference count of OtlpGrpcClient
  std::atomic<int64_t> reference_count{0};

  // Do not use OtlpGrpcClientAsyncData() = default; here, some versions of GCC&Clang have BUGs
  // and may not initialize the member correctly. See also
  // https://stackoverflow.com/questions/53408962/try-to-understand-compiler-error-message-default-member-initializer-required-be
  OtlpGrpcClientAsyncData() {}
};

namespace
{
// ----------------------------- Helper functions ------------------------------
static std::string GetFileContents(const char *fpath)
{
  std::ifstream finstream(fpath);
  std::string contents;
  contents.assign((std::istreambuf_iterator<char>(finstream)), std::istreambuf_iterator<char>());
  finstream.close();
  return contents;
}

// If the file path is non-empty, returns the contents of the file. Otherwise returns contents.
static std::string GetFileContentsOrInMemoryContents(const std::string &file_path,
                                                     const std::string &contents)
{
  if (!file_path.empty())
  {
    return GetFileContents(file_path.c_str());
  }
  return contents;
}

#ifdef ENABLE_ASYNC_EXPORT
template <class StubType, class RequestType, class ResponseType>
static sdk::common::ExportResult InternalDelegateAsyncExport(
    const std::shared_ptr<OtlpGrpcClientAsyncData> &async_data,
    StubType *stub,
    std::unique_ptr<grpc::ClientContext> &&context,
    std::unique_ptr<google::protobuf::Arena> &&arena,
    RequestType &&request,
    std::function<bool(opentelemetry::sdk::common::ExportResult,
                       std::unique_ptr<google::protobuf::Arena> &&,
                       const RequestType &,
                       ResponseType *)> &&result_callback,
    int32_t export_data_count,
    const char *export_data_name) noexcept
{
  if (async_data->running_requests.load(std::memory_order_acquire) >=
      async_data->max_concurrent_requests)
  {
    OTEL_INTERNAL_LOG_ERROR("[OTLP GRPC Client] ERROR: Export "
                            << export_data_count << " " << export_data_name
                            << " failed, exporter queue is full");
    if (result_callback)
    {
      result_callback(opentelemetry::sdk::common::ExportResult::kFailureFull, std::move(arena),
                      request, nullptr);
    }
    return opentelemetry::sdk::common::ExportResult::kFailureFull;
  }

  std::shared_ptr<OtlpGrpcAsyncCallData<RequestType, ResponseType>> call_data =
      std::make_shared<OtlpGrpcAsyncCallData<RequestType, ResponseType>>();
  call_data->arena.swap(arena);
  call_data->result_callback.swap(result_callback);

  call_data->request = google::protobuf::Arena::Create<RequestType>(
      call_data->arena.get(), std::forward<RequestType>(request));
  call_data->response = google::protobuf::Arena::Create<ResponseType>(call_data->arena.get());

  if (call_data->request == nullptr || call_data->response == nullptr)
  {
    OTEL_INTERNAL_LOG_ERROR("[OTLP GRPC Client] ERROR: Export "
                            << export_data_count << " " << export_data_name
                            << " failed, create gRPC request/response failed");

    if (call_data->result_callback)
    {
      call_data->result_callback(
          opentelemetry::sdk::common::ExportResult::kFailure, std::move(call_data->arena),
          nullptr == call_data->request ? request : *call_data->request, call_data->response);
    }

    return opentelemetry::sdk::common::ExportResult::kFailure;
  }
  call_data->grpc_context.swap(context);

  call_data->grpc_async_callback = [](OtlpGrpcAsyncCallDataBase *base_call_data) {
    OtlpGrpcAsyncCallData<RequestType, ResponseType> *real_call_data =
        static_cast<OtlpGrpcAsyncCallData<RequestType, ResponseType> *>(base_call_data);
    if (real_call_data->result_callback)
    {
      return real_call_data->result_callback(real_call_data->export_result,
                                             std::move(real_call_data->arena),
                                             *real_call_data->request, real_call_data->response);
    }

    return true;
  };

  ++async_data->start_request_counter;
  ++async_data->running_requests;
  {
    std::lock_guard<std::mutex> lock{async_data->running_calls_lock};
    async_data->running_calls.insert(
        std::static_pointer_cast<OtlpGrpcAsyncCallDataBase>(call_data));
  }
  // Some old toolchains can only use gRPC 1.33 and it's experimental.
#  if defined(GRPC_CPP_VERSION_MAJOR) && \
      (GRPC_CPP_VERSION_MAJOR * 1000 + GRPC_CPP_VERSION_MINOR) >= 1039
  stub->async()
#  else
  stub->experimental_async()
#  endif
      ->Export(call_data->grpc_context.get(), call_data->request, call_data->response,
               [call_data, async_data, export_data_name](const ::grpc::Status &grpc_status) {
                 {
                   std::lock_guard<std::mutex> lock{async_data->running_calls_lock};
                   async_data->running_calls.erase(
                       std::static_pointer_cast<OtlpGrpcAsyncCallDataBase>(call_data));
                 }

                 --async_data->running_requests;
                 ++async_data->finished_request_counter;

                 call_data->grpc_status = grpc_status;
                 if (call_data->grpc_status.ok())
                 {
                   call_data->export_result = opentelemetry::sdk::common::ExportResult::kSuccess;
                 }
                 else
                 {
                   OTEL_INTERNAL_LOG_ERROR("[OTLP GRPC Client] ERROR: Export "
                                           << export_data_name << " failed with status_code: \""
                                           << grpc_status.error_code() << "\" error_message: \""
                                           << grpc_status.error_message() << "\"");
                 }

                 if (call_data->grpc_async_callback)
                 {
                   call_data->grpc_async_callback(call_data.get());
                 }

                 // Maybe wake up blocking DelegateAsyncExport() call
                 async_data->session_waker.notify_all();
               });

  // Can not cancel when start the request
  {
    std::unique_lock<std::mutex> lock{async_data->session_waker_lock};
    async_data->session_waker.wait_for(lock, async_data->export_timeout, [async_data]() {
      return async_data->running_requests.load(std::memory_order_acquire) <=
             async_data->max_concurrent_requests;
    });
  }

  return opentelemetry::sdk::common::ExportResult::kSuccess;
}
#endif
}  // namespace

OtlpGrpcClientReferenceGuard::OtlpGrpcClientReferenceGuard() noexcept : has_value_{false} {}

OtlpGrpcClientReferenceGuard::~OtlpGrpcClientReferenceGuard() noexcept {}

OtlpGrpcClient::OtlpGrpcClient(const OtlpGrpcClientOptions &options) : is_shutdown_(false)
{
  std::shared_ptr<OtlpGrpcClientAsyncData> async_data = MutableAsyncData(options);
  async_data->channel                                 = MakeChannel(options);
}

OtlpGrpcClient::~OtlpGrpcClient()
{
  std::shared_ptr<OtlpGrpcClientAsyncData> async_data;
  async_data.swap(async_data_);

#ifdef ENABLE_ASYNC_EXPORT
  if (async_data)
  {
    std::unordered_set<std::shared_ptr<OtlpGrpcAsyncCallDataBase>> running_calls;
    {
      std::lock_guard<std::mutex> lock(async_data->running_calls_lock);
      running_calls = async_data->running_calls;
    }
    for (auto &call_data : running_calls)
    {
      if (call_data && call_data->grpc_context)
      {
        call_data->grpc_context->TryCancel();
      }
    }
  }

  while (async_data && async_data->running_requests.load(std::memory_order_acquire) > 0)
  {
    std::unique_lock<std::mutex> lock{async_data->session_waker_lock};
    async_data->session_waker.wait_for(lock, async_data->export_timeout, [async_data]() {
      return async_data->running_requests.load(std::memory_order_acquire) == 0;
    });
  }
#endif
}

std::string OtlpGrpcClient::GetGrpcTarget(const std::string &endpoint)
{
  //
  // Scheme is allowed in OTLP endpoint definition, but is not allowed for creating gRPC
  // channel. Passing URI with scheme to grpc::CreateChannel could resolve the endpoint to some
  // unexpected address.
  //
  ext::http::common::UrlParser url(endpoint);
  if (!url.success_)
  {
    OTEL_INTERNAL_LOG_ERROR("[OTLP GRPC Client] invalid endpoint: " << endpoint);
    return "";
  }

  std::string grpc_target;
  if (url.scheme_ == "unix")
  {
    grpc_target = "unix:" + url.path_;
  }
  else
  {
    grpc_target = url.host_ + ":" + std::to_string(static_cast<int>(url.port_));
  }
  return grpc_target;
}

std::shared_ptr<grpc::Channel> OtlpGrpcClient::MakeChannel(const OtlpGrpcClientOptions &options)
{

  if (options.endpoint.empty())
  {
    OTEL_INTERNAL_LOG_ERROR("[OTLP GRPC Client] empty endpoint");

    return nullptr;
  }

  std::shared_ptr<grpc::Channel> channel;
  std::string grpc_target = GetGrpcTarget(options.endpoint);

  if (grpc_target.empty())
  {
    OTEL_INTERNAL_LOG_ERROR("[OTLP GRPC Client] invalid endpoint: " << options.endpoint);
    return nullptr;
  }

  grpc::ChannelArguments grpc_arguments;
  grpc_arguments.SetUserAgentPrefix(options.user_agent);

  if (options.max_threads > 0)
  {
    grpc::ResourceQuota quota;
    quota.SetMaxThreads(static_cast<int>(options.max_threads));
    grpc_arguments.SetResourceQuota(quota);
  }

  if (options.compression == "gzip")
  {
    grpc_arguments.SetCompressionAlgorithm(GRPC_COMPRESS_GZIP);
  }

#ifdef ENABLE_OTLP_RETRY_PREVIEW
  if (options.retry_policy_max_attempts > 0U &&
      options.retry_policy_initial_backoff > std::chrono::duration<float>::zero() &&
      options.retry_policy_max_backoff > std::chrono::duration<float>::zero() &&
      options.retry_policy_backoff_multiplier > 0.0f)
  {
    static const auto kServiceConfigJson = opentelemetry::nostd::string_view{R"(
    {
      "methodConfig": [
        {
          "name": [{}],
          "retryPolicy": {
            "maxAttempts": %0000000000u,
            "initialBackoff": "%0000000000.1fs",
            "maxBackoff": "%0000000000.1fs",
            "backoffMultiplier": %0000000000.1f,
            "retryableStatusCodes": [
              "CANCELLED",
              "DEADLINE_EXCEEDED",
              "ABORTED",
              "OUT_OF_RANGE",
              "DATA_LOSS",
              "UNAVAILABLE"
            ]
          }
        }
      ]
    })"};

    // Allocate string with buffer large enough to hold the formatted json config
    auto service_config = std::string(kServiceConfigJson.size(), '\0');
    // Prior to C++17, need to explicitly cast away constness from `data()` buffer
    std::snprintf(
        const_cast<decltype(service_config)::value_type *>(service_config.data()),
        service_config.size(), kServiceConfigJson.data(), options.retry_policy_max_attempts,
        std::min(std::max(options.retry_policy_initial_backoff.count(), 0.f), 999999999.f),
        std::min(std::max(options.retry_policy_max_backoff.count(), 0.f), 999999999.f),
        std::min(std::max(options.retry_policy_backoff_multiplier, 0.f), 999999999.f));

    grpc_arguments.SetServiceConfigJSON(service_config);
  }
#endif  // ENABLE_OTLP_RETRY_PREVIEW

  if (options.use_ssl_credentials)
  {
    grpc::SslCredentialsOptions ssl_opts;
    ssl_opts.pem_root_certs = GetFileContentsOrInMemoryContents(
        options.ssl_credentials_cacert_path, options.ssl_credentials_cacert_as_string);
#ifdef ENABLE_OTLP_GRPC_SSL_MTLS_PREVIEW
    ssl_opts.pem_private_key = GetFileContentsOrInMemoryContents(options.ssl_client_key_path,
                                                                 options.ssl_client_key_string);
    ssl_opts.pem_cert_chain  = GetFileContentsOrInMemoryContents(options.ssl_client_cert_path,
                                                                 options.ssl_client_cert_string);

#endif
    channel =
        grpc::CreateCustomChannel(grpc_target, grpc::SslCredentials(ssl_opts), grpc_arguments);
  }
  else
  {
    channel =
        grpc::CreateCustomChannel(grpc_target, grpc::InsecureChannelCredentials(), grpc_arguments);
  }

#ifdef ENABLE_OTLP_GRPC_CREDENTIAL_PREVIEW
  if (options.credentials)
  {
    if (options.use_ssl_credentials)
    {
      OTEL_INTERNAL_LOG_WARN(
          "[OTLP GRPC Client] Both 'credentials' and 'use_ssl_credentials' options are set. "
          "The former takes priority.");
    }
    channel = grpc::CreateCustomChannel(grpc_target, options.credentials, grpc_arguments);
  }
#endif  // ENABLE_OTLP_GRPC_CREDENTIAL_PREVIEW

  return channel;
}

std::unique_ptr<grpc::ClientContext> OtlpGrpcClient::MakeClientContext(
    const OtlpGrpcClientOptions &options)
{
  std::unique_ptr<grpc::ClientContext> context{new grpc::ClientContext()};
  if (!context)
  {
    return context;
  }

  if (options.timeout.count() > 0)
  {
    context->set_deadline(std::chrono::system_clock::now() + options.timeout);
  }

  for (auto &header : options.metadata)
  {
    context->AddMetadata(header.first,
                         opentelemetry::ext::http::common::UrlDecoder::Decode(header.second));
  }

  return context;
}

std::unique_ptr<proto::collector::trace::v1::TraceService::StubInterface>
OtlpGrpcClient::MakeTraceServiceStub()
{
  if (!async_data_ || !async_data_->channel)
  {
    return nullptr;
  }
  return proto::collector::trace::v1::TraceService::NewStub(async_data_->channel);
}

std::unique_ptr<proto::collector::metrics::v1::MetricsService::StubInterface>
OtlpGrpcClient::MakeMetricsServiceStub()
{
  if (!async_data_ || !async_data_->channel)
  {
    return nullptr;
  }
  return proto::collector::metrics::v1::MetricsService::NewStub(async_data_->channel);
}

std::unique_ptr<proto::collector::logs::v1::LogsService::StubInterface>
OtlpGrpcClient::MakeLogsServiceStub()
{
  if (!async_data_ || !async_data_->channel)
  {
    return nullptr;
  }
  return proto::collector::logs::v1::LogsService::NewStub(async_data_->channel);
}

grpc::Status OtlpGrpcClient::DelegateExport(
    proto::collector::trace::v1::TraceService::StubInterface *stub,
    std::unique_ptr<grpc::ClientContext> &&context,
    std::unique_ptr<google::protobuf::Arena> && /*arena*/,
    proto::collector::trace::v1::ExportTraceServiceRequest &&request,
    proto::collector::trace::v1::ExportTraceServiceResponse *response)
{
  return stub->Export(context.get(), request, response);
}

grpc::Status OtlpGrpcClient::DelegateExport(
    proto::collector::metrics::v1::MetricsService::StubInterface *stub,
    std::unique_ptr<grpc::ClientContext> &&context,
    std::unique_ptr<google::protobuf::Arena> && /*arena*/,
    proto::collector::metrics::v1::ExportMetricsServiceRequest &&request,
    proto::collector::metrics::v1::ExportMetricsServiceResponse *response)
{
  return stub->Export(context.get(), request, response);
}

grpc::Status OtlpGrpcClient::DelegateExport(
    proto::collector::logs::v1::LogsService::StubInterface *stub,
    std::unique_ptr<grpc::ClientContext> &&context,
    std::unique_ptr<google::protobuf::Arena> && /*arena*/,
    proto::collector::logs::v1::ExportLogsServiceRequest &&request,
    proto::collector::logs::v1::ExportLogsServiceResponse *response)
{
  return stub->Export(context.get(), request, response);
}

void OtlpGrpcClient::AddReference(OtlpGrpcClientReferenceGuard &guard,
                                  const OtlpGrpcClientOptions &options) noexcept
{
  if (false == guard.has_value_.exchange(true, std::memory_order_acq_rel))
  {
    MutableAsyncData(options)->reference_count.fetch_add(1, std::memory_order_acq_rel);
  }
}

bool OtlpGrpcClient::RemoveReference(OtlpGrpcClientReferenceGuard &guard) noexcept
{
  auto async_data = async_data_;
  if (true == guard.has_value_.exchange(false, std::memory_order_acq_rel))
  {
    if (async_data)
    {
      int64_t left = async_data->reference_count.fetch_sub(1, std::memory_order_acq_rel);
      return left <= 1;
    }
  }

  if (async_data)
  {
    return async_data->reference_count.load(std::memory_order_acquire) <= 0;
  }

  return true;
}

#ifdef ENABLE_ASYNC_EXPORT

/**
 * Async export
 * @param options Options used to message to create gRPC context and stub(if necessary)
 * @param arena Protobuf arena to hold lifetime of all messages
 * @param request Request for this RPC
 * @param result_callback callback to call when the exporting is done
 * @return return the status of this operation
 */
sdk::common::ExportResult OtlpGrpcClient::DelegateAsyncExport(
    const OtlpGrpcClientOptions &options,
    proto::collector::trace::v1::TraceService::StubInterface *stub,
    std::unique_ptr<grpc::ClientContext> &&context,
    std::unique_ptr<google::protobuf::Arena> &&arena,
    proto::collector::trace::v1::ExportTraceServiceRequest &&request,
    std::function<bool(opentelemetry::sdk::common::ExportResult,
                       std::unique_ptr<google::protobuf::Arena> &&,
                       const proto::collector::trace::v1::ExportTraceServiceRequest &,
                       proto::collector::trace::v1::ExportTraceServiceResponse *)>
        &&result_callback) noexcept
{
  auto span_count = request.resource_spans_size();
  if (is_shutdown_.load(std::memory_order_acquire))
  {
    OTEL_INTERNAL_LOG_ERROR("[OTLP GRPC Client] ERROR: Export "
                            << span_count << " trace span(s) failed, exporter is shutdown");
    if (result_callback)
    {
      result_callback(opentelemetry::sdk::common::ExportResult::kFailure, std::move(arena), request,
                      nullptr);
    }
    return opentelemetry::sdk::common::ExportResult::kFailure;
  }

  std::shared_ptr<OtlpGrpcClientAsyncData> async_data = MutableAsyncData(options);
  return InternalDelegateAsyncExport(async_data, stub, std::move(context), std::move(arena),
                                     std::move(request), std::move(result_callback), span_count,
                                     "trace span(s)");
}

sdk::common::ExportResult OtlpGrpcClient::DelegateAsyncExport(
    const OtlpGrpcClientOptions &options,
    proto::collector::metrics::v1::MetricsService::StubInterface *stub,
    std::unique_ptr<grpc::ClientContext> &&context,
    std::unique_ptr<google::protobuf::Arena> &&arena,
    proto::collector::metrics::v1::ExportMetricsServiceRequest &&request,
    std::function<bool(opentelemetry::sdk::common::ExportResult,
                       std::unique_ptr<google::protobuf::Arena> &&,
                       const proto::collector::metrics::v1::ExportMetricsServiceRequest &,
                       proto::collector::metrics::v1::ExportMetricsServiceResponse *)>
        &&result_callback) noexcept
{
  auto metrics_count = request.resource_metrics_size();
  if (is_shutdown_.load(std::memory_order_acquire))
  {
    OTEL_INTERNAL_LOG_ERROR("[OTLP GRPC Client] ERROR: Export "
                            << metrics_count << " metric(s) failed, exporter is shutdown");
    if (result_callback)
    {
      result_callback(opentelemetry::sdk::common::ExportResult::kFailure, std::move(arena), request,
                      nullptr);
    }
    return opentelemetry::sdk::common::ExportResult::kFailure;
  }

  std::shared_ptr<OtlpGrpcClientAsyncData> async_data = MutableAsyncData(options);
  return InternalDelegateAsyncExport(async_data, stub, std::move(context), std::move(arena),
                                     std::move(request), std::move(result_callback), metrics_count,
                                     "metric(s)");
}

sdk::common::ExportResult OtlpGrpcClient::DelegateAsyncExport(
    const OtlpGrpcClientOptions &options,
    proto::collector::logs::v1::LogsService::StubInterface *stub,
    std::unique_ptr<grpc::ClientContext> &&context,
    std::unique_ptr<google::protobuf::Arena> &&arena,
    proto::collector::logs::v1::ExportLogsServiceRequest &&request,
    std::function<bool(opentelemetry::sdk::common::ExportResult,
                       std::unique_ptr<google::protobuf::Arena> &&,
                       const proto::collector::logs::v1::ExportLogsServiceRequest &,
                       proto::collector::logs::v1::ExportLogsServiceResponse *)>
        &&result_callback) noexcept
{
  auto logs_count = request.resource_logs_size();
  if (is_shutdown_.load(std::memory_order_acquire))
  {
    OTEL_INTERNAL_LOG_ERROR("[OTLP GRPC Client] ERROR: Export "
                            << logs_count << " log(s) failed, exporter is shutdown");
    if (result_callback)
    {
      result_callback(opentelemetry::sdk::common::ExportResult::kFailure, std::move(arena), request,
                      nullptr);
    }
    return opentelemetry::sdk::common::ExportResult::kFailure;
  }

  std::shared_ptr<OtlpGrpcClientAsyncData> async_data = MutableAsyncData(options);
  return InternalDelegateAsyncExport(async_data, stub, std::move(context), std::move(arena),
                                     std::move(request), std::move(result_callback), logs_count,
                                     "log(s)");
}

#endif

std::shared_ptr<OtlpGrpcClientAsyncData> OtlpGrpcClient::MutableAsyncData(
    const OtlpGrpcClientOptions &options)
{
  if (!async_data_)
  {
    async_data_                 = std::make_shared<OtlpGrpcClientAsyncData>();
    async_data_->export_timeout = options.timeout;
#ifdef ENABLE_ASYNC_EXPORT
    async_data_->max_concurrent_requests = options.max_concurrent_requests;
#endif
  }

  return async_data_;
}

bool OtlpGrpcClient::IsShutdown() const noexcept
{
  return is_shutdown_.load(std::memory_order_acquire);
}

bool OtlpGrpcClient::ForceFlush(
    OPENTELEMETRY_MAYBE_UNUSED std::chrono::microseconds timeout) noexcept
{
  if (!async_data_)
  {
    return true;
  }

#ifdef ENABLE_ASYNC_EXPORT
  std::size_t request_counter = async_data_->start_request_counter.load(std::memory_order_acquire);
  if (request_counter <= async_data_->finished_request_counter.load(std::memory_order_acquire))
  {
    return true;
  }

  // ASAN will report chrono: runtime error: signed integer overflow: A + B cannot be represented
  //   in type 'long int' here. So we reset timeout to meet signed long int limit here.
  timeout = opentelemetry::common::DurationUtil::AdjustWaitForTimeout(
      timeout, std::chrono::microseconds::zero());

  // Wait for all the sessions to finish
  std::unique_lock<std::mutex> lock(async_data_->session_waker_lock);

  std::chrono::steady_clock::duration timeout_steady =
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(timeout);
  if (timeout_steady <= std::chrono::steady_clock::duration::zero())
  {
    timeout_steady = std::chrono::steady_clock::duration::max();
  }

  while (timeout_steady > std::chrono::steady_clock::duration::zero() &&
         request_counter > async_data_->finished_request_counter.load(std::memory_order_acquire))
  {
    // When changes of running_sessions_ and notify_one/notify_all happen between predicate
    // checking and waiting, we should not wait forever.We should cleanup gc sessions here as soon
    // as possible to call FinishSession() and cleanup resources.
    std::chrono::steady_clock::time_point start_timepoint = std::chrono::steady_clock::now();
    if (std::cv_status::timeout !=
        async_data_->session_waker.wait_for(lock, async_data_->export_timeout))
    {
      break;
    }

    timeout_steady -= std::chrono::steady_clock::now() - start_timepoint;
  }

  return timeout_steady > std::chrono::steady_clock::duration::zero();
#else
  return true;
#endif
}

bool OtlpGrpcClient::Shutdown(OtlpGrpcClientReferenceGuard &guard,
                              OPENTELEMETRY_MAYBE_UNUSED std::chrono::microseconds timeout) noexcept
{
  if (!async_data_)
  {
    return true;
  }

  bool last_reference_removed = RemoveReference(guard);
  bool force_flush_result{};
  if (last_reference_removed && false == is_shutdown_.exchange(true, std::memory_order_acq_rel))
  {
    OTEL_INTERNAL_LOG_DEBUG("[OTLP GRPC Client] DEBUG: OtlpGrpcClient start to shutdown");
    force_flush_result = ForceFlush(timeout);

#ifdef ENABLE_ASYNC_EXPORT
    std::unordered_set<std::shared_ptr<OtlpGrpcAsyncCallDataBase>> running_calls;
    {
      std::lock_guard<std::mutex> lock(async_data_->running_calls_lock);
      running_calls = async_data_->running_calls;
    }
    if (!running_calls.empty())
    {
      OTEL_INTERNAL_LOG_WARN(
          "[OTLP GRPC Client] WARN: OtlpGrpcClient shutdown timeout, try to cancel "
          << running_calls.size() << " running calls.");
    }
    for (auto &call_data : running_calls)
    {
      if (call_data && call_data->grpc_context)
      {
        call_data->grpc_context->TryCancel();
      }
    }
#endif
  }
  else
  {
    force_flush_result = ForceFlush(timeout);
  }

  return force_flush_result;
}

}  // namespace otlp
}  // namespace exporter
OPENTELEMETRY_END_NAMESPACE
