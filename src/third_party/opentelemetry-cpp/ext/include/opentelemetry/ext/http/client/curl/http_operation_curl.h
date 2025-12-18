// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "opentelemetry/ext/http/client/http_client.h"
#include "opentelemetry/version.h"

#if defined(_MSC_VER)
#  pragma warning(suppress : 5204)
#  include <future>
#else
#  include <future>
#endif

#include <cstdint>
#include <functional>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#  include <io.h>
#  include <winsock2.h>
#else
#  include <poll.h>
#  include <unistd.h>
#endif
#include "curl/curl.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace ext
{
namespace http
{
namespace client
{
namespace curl
{
const std::chrono::milliseconds kDefaultHttpConnTimeout(5000);  // ms
const std::string kHttpStatusRegexp = "HTTP\\/\\d\\.\\d (\\d+)\\ .*";
const std::string kHttpHeaderRegexp = "(.*)\\: (.*)\\n*";

class HttpClient;
class Session;

struct HttpCurlEasyResource
{
  CURL *easy_handle;
  curl_slist *headers_chunk;

  HttpCurlEasyResource(CURL *curl = nullptr, curl_slist *headers = nullptr)
      : easy_handle{curl}, headers_chunk{headers}
  {}

  HttpCurlEasyResource(HttpCurlEasyResource &&other)
      : easy_handle{other.easy_handle}, headers_chunk{other.headers_chunk}
  {
    other.easy_handle   = nullptr;
    other.headers_chunk = nullptr;
  }

  HttpCurlEasyResource &operator=(HttpCurlEasyResource &&other)
  {
    using std::swap;
    swap(easy_handle, other.easy_handle);
    swap(headers_chunk, other.headers_chunk);

    return *this;
  }

  HttpCurlEasyResource(const HttpCurlEasyResource &other)            = delete;
  HttpCurlEasyResource &operator=(const HttpCurlEasyResource &other) = delete;
};

class HttpOperation
{
private:
  /**
   * Old-school memory allocator
   *
   * @param contents Pointer to the data received from the server.
   * @param size Size of each data element.
   * @param nmemb Number of data elements.
   * @param userp Pointer to the user-defined data structure for storing the received data.
   * @return The number of bytes actually taken care of. If this differs from size * nmemb, it
   * signals an error to libcurl.
   */
  static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp);

  /**
   * C++ STL std::vector allocator
   *
   * @param ptr Pointer to the data received from the server.
   * @param size Size of each data element.
   * @param nmemb Number of data elements.
   * @param userp Pointer to the user-defined data structure for storing the received data.
   * @return The number of bytes actually taken care of. If this differs from size * nmemb, it
   * signals an error to libcurl.
   */
  static size_t WriteVectorHeaderCallback(void *ptr, size_t size, size_t nmemb, void *userp);
  static size_t WriteVectorBodyCallback(void *ptr, size_t size, size_t nmemb, void *userp);

  static size_t ReadMemoryCallback(char *buffer, size_t size, size_t nitems, void *userp);

  static int CurlLoggerCallback(const CURL * /* handle */,
                                curl_infotype type,
                                const char *data,
                                size_t size,
                                void * /* clientp */) noexcept;

#if LIBCURL_VERSION_NUM >= 0x075000
  static int PreRequestCallback(void *clientp,
                                char *conn_primary_ip,
                                char *conn_local_ip,
                                int conn_primary_port,
                                int conn_local_port);
#endif

#if LIBCURL_VERSION_NUM >= 0x072000
  static int OnProgressCallback(void *clientp,
                                curl_off_t dltotal,
                                curl_off_t dlnow,
                                curl_off_t ultotal,
                                curl_off_t ulnow);
#else
  static int OnProgressCallback(void *clientp,
                                double dltotal,
                                double dlnow,
                                double ultotal,
                                double ulnow);
#endif
public:
  void DispatchEvent(opentelemetry::ext::http::client::SessionState type,
                     const std::string &reason = "");

  /**
   * Create local CURL instance for url and body
   * @param method   HTTP Method
   * @param url   HTTP URL
   * @param request_headers   Request Headers
   * @param request_body   Request Body
   * @param is_raw_response   Whether to parse the response
   * @param http_conn_timeout   HTTP connection timeout in seconds
   * @param reuse_connection   Whether connection should be reused or closed
   * @param is_log_enabled   To intercept some information from cURL request
   * @param retry_policy   Retry policy for select failure status codes
   */
  HttpOperation(opentelemetry::ext::http::client::Method method,
                std::string url,
                const opentelemetry::ext::http::client::HttpSslOptions &ssl_options,
                opentelemetry::ext::http::client::EventHandler *event_handle,
                // Default empty headers and empty request body
                const opentelemetry::ext::http::client::Headers &request_headers =
                    opentelemetry::ext::http::client::Headers(),
                const opentelemetry::ext::http::client::Body &request_body =
                    opentelemetry::ext::http::client::Body(),
                const opentelemetry::ext::http::client::Compression &compression =
                    opentelemetry::ext::http::client::Compression::kNone,
                // Default connectivity and response size options
                bool is_raw_response                        = false,
                std::chrono::milliseconds http_conn_timeout = kDefaultHttpConnTimeout,
                bool reuse_connection                       = false,
                bool is_log_enabled                         = false,
                const opentelemetry::ext::http::client::RetryPolicy &retry_policy = {});

  /**
   * Destroy CURL instance
   */
  virtual ~HttpOperation();

  /**
   * Finish CURL instance
   */
  virtual void Finish();

  /**
   * Cleanup all resource of curl
   */
  void Cleanup();

  /**
   * Determine if operation is retryable
   */
  bool IsRetryable();

  /**
   * Calculate next time to retry request
   */
  std::chrono::system_clock::time_point NextRetryTime();

  /**
   * Setup request
   */
  CURLcode Setup();

  /**
   * Send request synchronously
   */
  CURLcode Send();

  /**
   * Send request asynchronously
   * @param session This operator must be binded to a Session
   * @param callback callback when start async request success and got response
   */
  CURLcode SendAsync(Session *session, std::function<void(HttpOperation &)> callback = nullptr);

  inline void SendSync() { Send(); }

  /**
   * Get HTTP response code. This function returns 0.
   */
  inline StatusCode GetResponseCode() const noexcept
  {
    return static_cast<StatusCode>(response_code_);
  }

  CURLcode GetLastResultCode() { return last_curl_result_; }

  /**
   * Get last session state.
   */
  opentelemetry::ext::http::client::SessionState GetSessionState() { return session_state_; }

  /**
   * Get whether or not response was programmatically aborted
   */
  bool WasAborted() { return is_aborted_.load(std::memory_order_acquire); }

  /**
   * Return a copy of response headers
   */
  Headers GetResponseHeaders();

  /**
   * Return a copy of response body
   */
  inline const std::vector<uint8_t> &GetResponseBody() const noexcept { return response_body_; }

  /**
   * Return a raw copy of response headers+body
   */
  inline const std::vector<uint8_t> &GetRawResponse() const noexcept { return raw_response_; }

  /**
   * Release memory allocated for response
   */
  void ReleaseResponse();

  /**
   * Abort request in connecting or reading state.
   */
  void Abort();

  /**
   * Perform curl message, this function only can be called in the polling thread and it can only
   * be called when got a CURLMSG_DONE.
   *
   * @param code CURLcode
   */
  void PerformCurlMessage(CURLcode code);

  inline CURL *GetCurlEasyHandle() noexcept { return curl_resource_.easy_handle; }

private:
  CURLcode SetCurlPtrOption(CURLoption option, void *value);

  CURLcode SetCurlStrOption(CURLoption option, const char *str)
  {
    void *ptr = const_cast<char *>(str);
    return SetCurlPtrOption(option, ptr);
  }

  CURLcode SetCurlBlobOption(CURLoption option, struct curl_blob *blob)
  {
    return SetCurlPtrOption(option, blob);
  }

  CURLcode SetCurlListOption(CURLoption option, struct curl_slist *list)
  {
    return SetCurlPtrOption(option, list);
  }

  CURLcode SetCurlLongOption(CURLoption option, long value);

  CURLcode SetCurlOffOption(CURLoption option, curl_off_t value);

  const char *GetCurlErrorMessage(CURLcode code);

  std::atomic<bool> is_aborted_{false};   // Set to 'true' when async callback is aborted
  std::atomic<bool> is_finished_{false};  // Set to 'true' when async callback is finished.
  std::atomic<bool> is_cleaned_{false};   // Set to 'true' when async callback is cleaned.
  const bool is_raw_response_;            // Do not split response headers from response body
  const bool reuse_connection_;           // Reuse connection
  const std::chrono::milliseconds http_conn_timeout_;  // Timeout for connect.  Default: 5000ms

  char curl_error_message_[CURL_ERROR_SIZE];
  HttpCurlEasyResource curl_resource_;
  CURLcode last_curl_result_;  // Curl result OR HTTP status code if successful

  opentelemetry::ext::http::client::EventHandler *event_handle_;

  // Request values
  opentelemetry::ext::http::client::Method method_;
  std::string url_;

  const opentelemetry::ext::http::client::HttpSslOptions &ssl_options_;

  const Headers &request_headers_;
  const opentelemetry::ext::http::client::Body &request_body_;
  size_t request_nwrite_;
  opentelemetry::ext::http::client::SessionState session_state_;

  const opentelemetry::ext::http::client::Compression &compression_;

  const bool is_log_enabled_;

  const RetryPolicy retry_policy_;
  decltype(RetryPolicy::max_attempts) retry_attempts_;
  std::chrono::system_clock::time_point last_attempt_time_;

  // Processed response headers and body
  long response_code_;
  std::vector<uint8_t> response_headers_;
  std::vector<uint8_t> response_body_;
  std::vector<uint8_t> raw_response_;

  struct AsyncData
  {
    Session *session;  // Owner Session

    std::thread::id callback_thread;
    std::function<void(HttpOperation &)> callback;
    std::atomic<bool> is_promise_running{false};
    std::promise<CURLcode> result_promise;
    std::future<CURLcode> result_future;
  };
  friend class HttpOperationAccessor;
  std::unique_ptr<AsyncData> async_data_;
};
}  // namespace curl
}  // namespace client
}  // namespace http
}  // namespace ext
OPENTELEMETRY_END_NAMESPACE
