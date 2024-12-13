// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "opentelemetry/exporters/otlp/otlp_environment.h"
#include "opentelemetry/exporters/otlp/otlp_http.h"
#include "opentelemetry/ext/http/client/http_client.h"
#include "opentelemetry/nostd/string_view.h"
#include "opentelemetry/nostd/variant.h"
#include "opentelemetry/sdk/common/exporter_utils.h"
#include "opentelemetry/version.h"

// forward declare google::protobuf::Message
namespace google
{
namespace protobuf
{
class Message;
}
}  // namespace google

OPENTELEMETRY_BEGIN_NAMESPACE
namespace exporter
{
namespace otlp
{
// The default URL path to post metric data.
constexpr char kDefaultMetricsPath[] = "/v1/metrics";
// The HTTP header "Content-Type"
constexpr char kHttpJsonContentType[]   = "application/json";
constexpr char kHttpBinaryContentType[] = "application/x-protobuf";

/**
 * Struct to hold OTLP HTTP client options.
 */
struct OtlpHttpClientOptions
{
  std::string url;

  /** SSL options. */
  ext::http::client::HttpSslOptions ssl_options;

  // By default, post binary data
  HttpRequestContentType content_type = HttpRequestContentType::kBinary;

  // If convert bytes into hex. By default, we will convert all bytes but id into base64
  // This option is ignored if content_type is not kJson
  JsonBytesMappingKind json_bytes_mapping = JsonBytesMappingKind::kHexId;

  // By default, do not compress data
  std::string compression = "none";

  // If using the json name of protobuf field to set the key of json. By default, we will use the
  // field name just like proto files.
  bool use_json_name = false;

  // Whether to print the status of the HTTP client in the console
  bool console_debug = false;

  std::chrono::system_clock::duration timeout;

  // Additional HTTP headers
  OtlpHeaders http_headers;

  // Concurrent requests
  std::size_t max_concurrent_requests = 64;

  // Requests per connections
  std::size_t max_requests_per_connection = 8;

  // User agent
  std::string user_agent;

  inline OtlpHttpClientOptions(nostd::string_view input_url,
                               bool input_ssl_insecure_skip_verify,
                               nostd::string_view input_ssl_ca_cert_path,
                               nostd::string_view input_ssl_ca_cert_string,
                               nostd::string_view input_ssl_client_key_path,
                               nostd::string_view input_ssl_client_key_string,
                               nostd::string_view input_ssl_client_cert_path,
                               nostd::string_view input_ssl_client_cert_string,
                               nostd::string_view input_ssl_min_tls,
                               nostd::string_view input_ssl_max_tls,
                               nostd::string_view input_ssl_cipher,
                               nostd::string_view input_ssl_cipher_suite,
                               HttpRequestContentType input_content_type,
                               JsonBytesMappingKind input_json_bytes_mapping,
                               nostd::string_view input_compression,
                               bool input_use_json_name,
                               bool input_console_debug,
                               std::chrono::system_clock::duration input_timeout,
                               const OtlpHeaders &input_http_headers,
                               std::size_t input_concurrent_sessions         = 64,
                               std::size_t input_max_requests_per_connection = 8,
                               nostd::string_view input_user_agent = GetOtlpDefaultUserAgent())
      : url(input_url),
        ssl_options(input_url,
                    input_ssl_insecure_skip_verify,
                    input_ssl_ca_cert_path,
                    input_ssl_ca_cert_string,
                    input_ssl_client_key_path,
                    input_ssl_client_key_string,
                    input_ssl_client_cert_path,
                    input_ssl_client_cert_string,
                    input_ssl_min_tls,
                    input_ssl_max_tls,
                    input_ssl_cipher,
                    input_ssl_cipher_suite),
        content_type(input_content_type),
        json_bytes_mapping(input_json_bytes_mapping),
        compression(input_compression),
        use_json_name(input_use_json_name),
        console_debug(input_console_debug),
        timeout(input_timeout),
        http_headers(input_http_headers),
        max_concurrent_requests(input_concurrent_sessions),
        max_requests_per_connection(input_max_requests_per_connection),
        user_agent(input_user_agent)
  {}
};

/**
 * The OTLP HTTP client exports span data in OpenTelemetry Protocol (OTLP) format.
 */
class OtlpHttpClient
{
public:
  /**
   * Create an OtlpHttpClient using the given options.
   */
  explicit OtlpHttpClient(OtlpHttpClientOptions &&options);

  ~OtlpHttpClient();

  /**
   * Sync export
   * @param message message to export, it should be ExportTraceServiceRequest,
   * ExportMetricsServiceRequest or ExportLogsServiceRequest
   * @return return the status of this operation
   */
  sdk::common::ExportResult Export(const google::protobuf::Message &message) noexcept;

  /**
   * Async export
   * @param message message to export, it should be ExportTraceServiceRequest,
   * ExportMetricsServiceRequest or ExportLogsServiceRequest
   * @param result_callback callback to call when the exporting is done
   * @return return the status of this operation
   */
  sdk::common::ExportResult Export(
      const google::protobuf::Message &message,
      std::function<bool(opentelemetry::sdk::common::ExportResult)> &&result_callback) noexcept;

  /**
   * Async export
   * @param message message to export, it should be ExportTraceServiceRequest,
   * ExportMetricsServiceRequest or ExportLogsServiceRequest
   * @param result_callback callback to call when the exporting is done
   * @param max_running_requests wait for at most max_running_requests running requests
   * @return return the status of this operation
   */
  sdk::common::ExportResult Export(
      const google::protobuf::Message &message,
      std::function<bool(opentelemetry::sdk::common::ExportResult)> &&result_callback,
      std::size_t max_running_requests) noexcept;

  /**
   * Force flush the HTTP client.
   */
  bool ForceFlush(std::chrono::microseconds timeout = (std::chrono::microseconds::max)()) noexcept;

  /**
   * Shut down the HTTP client.
   * @param timeout an optional timeout, the default timeout of 0 means that no
   * timeout is applied.
   * @return return the status of this operation
   */
  bool Shutdown(std::chrono::microseconds timeout = std::chrono::microseconds(0)) noexcept;

  /**
   * @brief Release the lifetime of specify session.
   *
   * @param session the session to release
   */
  void ReleaseSession(const opentelemetry::ext::http::client::Session &session) noexcept;

  /**
   * Get options of current OTLP http client.
   * @return options of current OTLP http client.
   */
  inline const OtlpHttpClientOptions &GetOptions() const noexcept { return options_; }

  /**
   * Get if this OTLP http client is shutdown.
   * @return return true after Shutdown is called.
   */
  bool IsShutdown() const noexcept;

private:
  struct HttpSessionData
  {
    std::shared_ptr<opentelemetry::ext::http::client::Session> session;
    std::shared_ptr<opentelemetry::ext::http::client::EventHandler> event_handle;

    inline HttpSessionData() = default;

    inline explicit HttpSessionData(
        std::shared_ptr<opentelemetry::ext::http::client::Session> &&input_session,
        std::shared_ptr<opentelemetry::ext::http::client::EventHandler> &&input_handle)
    {
      session.swap(input_session);
      event_handle.swap(input_handle);
    }

    inline HttpSessionData(HttpSessionData &&other)
    {
      session.swap(other.session);
      event_handle.swap(other.event_handle);
    }

    inline HttpSessionData &operator=(HttpSessionData &&other) noexcept
    {
      session.swap(other.session);
      event_handle.swap(other.event_handle);
      return *this;
    }
  };

  /**
   * @brief Create a Session object or return a error result
   *
   * @param message The message to send
   */
  nostd::variant<sdk::common::ExportResult, HttpSessionData> createSession(
      const google::protobuf::Message &message,
      std::function<bool(opentelemetry::sdk::common::ExportResult)> &&result_callback) noexcept;

  /**
   * Add http session and hold it's lifetime.
   * @param session_data the session to add
   */
  void addSession(HttpSessionData &&session_data) noexcept;

  /**
   * @brief Real delete all sessions and event handles.
   * @note This function is called in the same thread where we create sessions and handles
   *
   * @return return true if there are more sessions to delete
   */
  bool cleanupGCSessions() noexcept;

  // For testing
  friend class OtlpHttpExporterTestPeer;
  friend class OtlpHttpLogRecordExporterTestPeer;
  friend class OtlpHttpMetricExporterTestPeer;

  /**
   * Create an OtlpHttpClient using the specified http client.
   * Only tests can call this constructor directly.
   * @param options the Otlp http client options to be used for exporting
   * @param http_client the http client to be used for exporting
   */
  OtlpHttpClient(OtlpHttpClientOptions &&options,
                 std::shared_ptr<ext::http::client::HttpClient> http_client);

  // Stores if this HTTP client had its Shutdown() method called
  std::atomic<bool> is_shutdown_;

  // The configuration options associated with this HTTP client.
  const OtlpHttpClientOptions options_;

  // Object that stores the HTTP sessions that have been created
  std::shared_ptr<ext::http::client::HttpClient> http_client_;

  // Cached parsed URI
  std::string http_uri_;

  // Running sessions and event handles
  std::unordered_map<const opentelemetry::ext::http::client::Session *, HttpSessionData>
      running_sessions_;
  // Sessions and event handles that are waiting to be deleted
  std::list<HttpSessionData> gc_sessions_;
  // Lock for running_sessions_, gc_sessions_ and http_client_
  std::recursive_mutex session_manager_lock_;
  // Condition variable and mutex to control the concurrency count of running sessions
  std::mutex session_waker_lock_;
  std::condition_variable session_waker_;
  std::atomic<size_t> start_session_counter_;
  std::atomic<size_t> finished_session_counter_;
};
}  // namespace otlp
}  // namespace exporter
OPENTELEMETRY_END_NAMESPACE
